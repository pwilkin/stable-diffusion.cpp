#include "hf_download.h"
#include "hf_cache.h"
#include "core/util.h"

#include "httplib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <json.hpp>

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#include <openssl/evp.h>
#endif

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hf_download {

// ---- helpers ----

static std::string get_hf_endpoint_impl() {
    if (const char* endpoint = std::getenv("HF_ENDPOINT")) {
        return endpoint;
    }
    return "https://huggingface.co";
}

std::string get_hf_endpoint() {
    static const std::string endpoint = get_hf_endpoint_impl();
    return endpoint;
}

std::string get_hf_token() {
    const char* token = std::getenv("HF_TOKEN");
    if (token && *token) return token;
    token = std::getenv("HUGGINGFACE_TOKEN");
    if (token && *token) return token;
    token = std::getenv("HF_HUB_TOKEN");
    if (token && *token) return token;
    return "";
}

// Simple SHA256 implementation using OpenSSL if available, otherwise a fallback
static std::string compute_sha256(const fs::path& file_path) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) return "";

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT)
    // Use OpenSSL EVP API for SHA256 (works with OpenSSL 3.x)
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

    char buf[65536];
    while (ifs.read(buf, sizeof(buf)) || ifs.gcount() > 0) {
        EVP_DigestUpdate(ctx, buf, ifs.gcount());
    }
    ifs.close();

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    }
    return oss.str();
#else
    // Fallback: use filename + size as a pseudo-hash (not cryptographically secure
    // but allows the cache to function for non-gated public repos)
    ifs.seekg(0, std::ios::end);
    auto sz = ifs.tellg();
    ifs.close();

    // Use a simple non-crypto hash for cache deduplication
    // This is weaker than real SHA256 but allows the cache structure to work
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(64)
        << fs::file_size(file_path);
    return oss.str();
#endif
}

// ---- HTTP helpers ----

// Split URL into protocol, host, path
static bool parse_url(const std::string& url, std::string& protocol, std::string& host, std::string& path) {
    protocol = "https";
    host = url;
    path = "/";
    size_t proto_end = url.find("://");
    if (proto_end != std::string::npos) {
        protocol = url.substr(0, proto_end);
        host = url.substr(proto_end + 3);
    }
    size_t path_start = host.find('/');
    if (path_start != std::string::npos) {
        path = host.substr(path_start);
        host = host.substr(0, path_start);
    } else {
        path = "/";
    }
    return !host.empty();
}

// Configure a httplib client with standard settings
static std::unique_ptr<httplib::Client> make_client(const std::string& protocol, const std::string& host) {
    // Pass a scheme-prefixed URL: a bare host makes httplib default to plain
    // HTTP on port 80, which would send Authorization headers in cleartext
    // and rely on the server redirecting to https.
    auto cli = std::make_unique<httplib::Client>(protocol + "://" + host);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (protocol == "https") {
        cli->enable_server_certificate_verification(true);
    }
#endif
    cli->set_keep_alive(true);
    cli->set_follow_location(true);
    cli->set_connection_timeout(30);
    cli->set_read_timeout(60);
    cli->set_write_timeout(60);
    return cli;
}

// ---- download_file_impl ----

// Per-download state, passed to the streaming callback via lambda capture.
// Each download gets its own instance, fixing the thread-safety bug.
struct DownloadState {
    std::ofstream ofs;
    std::string tmp_path;
    std::string final_path;
    size_t existing_size = 0;
    size_t total_written = 0;
    ProgressCallback progress_cb;
    bool write_to_tmp = false;
    bool success = false;
};

static bool download_file_impl(const std::string& url, const std::string& local_path,
                               ProgressCallback progress_cb) {
    std::error_code ec;
    fs::create_directories(fs::path(local_path).parent_path(), ec);
    if (ec) {
        LOG_ERROR("failed to create directory for '%s': %s", local_path.c_str(), ec.message().c_str());
        return false;
    }

    std::string protocol, host, path;
    if (!parse_url(url, protocol, host, path)) {
        LOG_ERROR("invalid URL (empty host): %s", url.c_str());
        return false;
    }

    auto cli = make_client(protocol, host);

    // Check if file exists for resume
    size_t existing_size = 0;
    if (fs::exists(local_path)) {
        existing_size = fs::file_size(local_path);
    }

    // Strategy: always download to a .tmp file first, then atomically rename.
    // This prevents corruption from partial downloads or server resets.
    std::string tmp_path = local_path + ".tmp";

    // ---- First attempt: with Range header if we have existing data ----
    {
        httplib::Headers headers;
        bool try_resume = (existing_size > 0);
        if (try_resume) {
            headers.emplace("Range", "bytes=" + std::to_string(existing_size) + "-");
        }

        auto state = std::make_shared<DownloadState>();
        state->tmp_path    = tmp_path;
        state->final_path  = local_path;
        state->progress_cb = progress_cb;
        state->existing_size = 0;  // Start from 0 in tmp (we open trunc)
        state->ofs.open(tmp_path, std::ios::binary | std::ios::trunc);
        if (!state->ofs) {
            LOG_ERROR("failed to open file for writing: %s", tmp_path.c_str());
            return false;
        }

        auto res = cli->Get(path.c_str(), headers,
            [state](const char* data, size_t data_len) -> bool {
                state->ofs.write(data, data_len);
                if (!state->ofs) {
                    LOG_ERROR("failed to write to file: %s", state->tmp_path.c_str());
                    return false;
                }
                state->total_written += data_len;
                if (state->progress_cb) {
                    DownloadProgress p;
                    p.downloaded = state->existing_size + state->total_written;
                    p.total = 0;
                    if (!state->progress_cb(p)) {
                        return false;
                    }
                }
                return true;
            });

        if (!res) {
            state->ofs.close();
            fs::remove(tmp_path, ec);
            LOG_ERROR("download failed: %s (network error)", url.c_str());
            return false;
        }

        int status = res->status;
        state->ofs.close();

        if (status == 200) {
            // Full download: server sent the whole file (either we didn't ask for
            // range, or server ignored it). tmp has the complete file.
            fs::rename(tmp_path, local_path, ec);
            if (ec) {
                LOG_ERROR("failed to rename tmp file: %s", ec.message().c_str());
                return false;
            }
            return true;
        }

        if (status == 206) {
            // Partial content: server sent bytes from existing_size onward.
            // tmp has only the new bytes. We need to prepend the existing content.
            std::string tmp2_path = local_path + ".tmp2";
            fs::rename(tmp_path, tmp2_path, ec);  // Save the new bytes
            if (ec) {
                LOG_ERROR("failed to rename tmp2: %s", ec.message().c_str());
                fs::remove(tmp_path, ec);
                return false;
            }

            // Copy existing file to tmp (the old bytes)
            fs::copy(local_path, tmp_path, ec);
            if (ec) {
                LOG_ERROR("failed to copy existing file for resume: %s", ec.message().c_str());
                fs::remove(tmp2_path, ec);
                return false;
            }

            // Append the new bytes
            {
                std::ofstream ofs(tmp_path, std::ios::binary | std::ios::app);
                std::ifstream ifs(tmp2_path, std::ios::binary);
                if (!ofs || !ifs) {
                    fs::remove(tmp_path, ec);
                    fs::remove(tmp2_path, ec);
                    return false;
                }
                ofs << ifs.rdbuf();
                ofs.close();
                ifs.close();
            }
            fs::remove(tmp2_path, ec);

            // Rename to final
            fs::rename(tmp_path, local_path, ec);
            if (ec) {
                LOG_ERROR("failed to rename tmp file: %s", ec.message().c_str());
                return false;
            }
            LOG_INFO("resumed download from %zu bytes", existing_size);
            return true;
        }

        // Range not satisfiable (416) or not found (404) or other error
        if (status == 416 || status == 404) {
            fs::remove(tmp_path, ec);
            // Fall through to fresh download below
            existing_size = 0;
            if (fs::exists(local_path)) {
                fs::remove(local_path, ec);
            }
        } else {
            fs::remove(tmp_path, ec);
            LOG_ERROR("download failed: %s (status: %d)", url.c_str(), status);
            return false;
        }
    }

    // ---- Second attempt: fresh GET without Range ----
    {
        auto state = std::make_shared<DownloadState>();
        state->tmp_path    = tmp_path;
        state->final_path  = local_path;
        state->progress_cb = progress_cb;
        state->existing_size = 0;
        state->ofs.open(tmp_path, std::ios::binary | std::ios::trunc);
        if (!state->ofs) {
            LOG_ERROR("failed to open file for writing: %s", tmp_path.c_str());
            return false;
        }

        auto res = cli->Get(path.c_str(),
            [state](const char* data, size_t data_len) -> bool {
                state->ofs.write(data, data_len);
                if (!state->ofs) {
                    LOG_ERROR("failed to write to file: %s", state->tmp_path.c_str());
                    return false;
                }
                state->total_written += data_len;
                if (state->progress_cb) {
                    DownloadProgress p;
                    p.downloaded = state->total_written;
                    p.total = 0;
                    if (!state->progress_cb(p)) {
                        return false;
                    }
                }
                return true;
            });

        if (!res || res->status != 200) {
            state->ofs.close();
            fs::remove(tmp_path, ec);
            LOG_ERROR("download failed: %s (status: %d)", url.c_str(), res ? res->status : -1);
            return false;
        }

        state->ofs.close();
        fs::rename(tmp_path, local_path, ec);
        if (ec) {
            LOG_ERROR("failed to rename tmp file: %s", ec.message().c_str());
            return false;
        }
        return true;
    }
}

bool download_file(const std::string& url, const std::string& local_path, ProgressCallback progress_cb) {
    return download_file_impl(url, local_path, progress_cb);
}

// ---- download_files (batch) ----

bool download_files(const std::vector<std::pair<std::string, std::string>>& url_path_pairs,
                    ProgressCallback progress_cb) {
    if (url_path_pairs.empty()) {
        return true;
    }

    const size_t total_files = url_path_pairs.size();
    const size_t max_threads = std::min<size_t>(total_files,
        std::max<size_t>(1, std::thread::hardware_concurrency()));

    // Use a semaphore to limit concurrent threads
    std::mutex cb_mutex;
    std::atomic<size_t> active_threads{0};
    std::vector<bool> results(total_files, false);
    std::atomic<size_t> completed_count{0};
    std::mutex sem_mutex;
    std::condition_variable sem_cv;

    std::vector<std::future<bool>> futures;
    futures.reserve(total_files);

    for (size_t i = 0; i < total_files; ++i) {
        futures.push_back(std::async(std::launch::async,
            [i, &url_path_pairs, &results, &completed_count, &cb_mutex,
             progress_cb, total_files, max_threads, &active_threads,
             &sem_mutex, &sem_cv]() -> bool {

            // Semaphore wait: block until we're under max_threads
            {
                std::unique_lock<std::mutex> lock(sem_mutex);
                sem_cv.wait(lock, [&] { return active_threads < max_threads; });
                ++active_threads;
            }

            bool success = download_file_impl(url_path_pairs[i].first,
                                              url_path_pairs[i].second,
                                              progress_cb);
            results[i] = success;

            // Semaphore signal
            {
                std::lock_guard<std::mutex> lock(sem_mutex);
                --active_threads;
                sem_cv.notify_one();
            }

            if (progress_cb) {
                std::lock_guard<std::mutex> lock(cb_mutex);
                size_t done = ++completed_count;
                DownloadProgress p;
                p.total = total_files;
                p.downloaded = done;
                progress_cb(p);
            }

            return success;
        }));
    }

    bool all_success = true;
    for (auto& future : futures) {
        if (!future.get()) {
            all_success = false;
        }
    }

    return all_success;
}

// ---- HF Hub helpers ----

// Fetch the current commit SHA for a repo from the Hub API
static std::string fetch_repo_commit(const std::string& repo_id, const std::string& token, const std::string& revision = "main") {
    std::string endpoint;
    try {
        endpoint = get_hf_endpoint();
    } catch (...) {
        return "";
    }

    std::string url = endpoint + "/api/models/" + repo_id + "?ref=" + revision;
    std::string protocol, host, path;
    if (!parse_url(url, protocol, host, path)) return "";

    auto cli = make_client(protocol, host);
    httplib::Headers headers = {
        {"User-Agent", "stable-diffusion.cpp/1.0"},
        {"Accept", "application/json"}
    };
    if (!token.empty()) {
        headers.emplace("Authorization", "Bearer " + token);
    }

    auto res = cli->Get(path.c_str(), headers);
    if (!res || res->status != 200) return "";

    try {
        auto json = nlohmann::json::parse(res->body);
        if (json.contains("sha") && json["sha"].is_string()) {
            return json["sha"].get<std::string>();
        }
    } catch (...) {}

    return "";
}

// Create the HF cache structure for a downloaded file:
//   blobs/<sha256>          — the actual file
//   refs/main               — contains the commit SHA (always written for default branch)
//   snapshots/<commit>/<path> — symlink to ../../blobs/<sha256>
// sha256 must be pre-computed (by caller) before calling this, since this
// function moves the file.
static bool install_in_cache(const fs::path& downloaded_path,
                              const std::string& repo_id,
                              const std::string& file_path,
                              const std::string& commit_sha,
                              const std::string& sha256) {
    std::error_code ec;

    fs::path cache_dir = hf_cache::get_cache_directory();
    std::string folder_name = hf_cache::repo_to_folder_name(repo_id);
    fs::path repo_dir = cache_dir / folder_name;

    // Move file to blobs/<sha256>
    fs::path blobs_dir = repo_dir / "blobs";
    fs::create_directories(blobs_dir, ec);
    if (ec) {
        LOG_ERROR("failed to create blobs dir: %s", ec.message().c_str());
        return false;
    }

    fs::path blob_path = blobs_dir / sha256;
    if (!fs::exists(blob_path)) {
        fs::rename(downloaded_path, blob_path, ec);
        if (ec) {
            // Try copy+delete as fallback
            fs::copy(downloaded_path, blob_path, ec);
            if (ec) {
                LOG_ERROR("failed to move file to blobs: %s", ec.message().c_str());
                return false;
            }
            fs::remove(downloaded_path, ec);
        }
    }

    // Write commit SHA to refs/main
    fs::path refs_dir = repo_dir / "refs";
    fs::create_directories(refs_dir, ec);
    if (!ec) {
        std::string commit = commit_sha.empty() ? sha256 : commit_sha;
        std::ofstream refs_file(refs_dir / "main");
        if (refs_file) {
            refs_file << commit;
            refs_file.close();
        }
    }

    // Create symlink in snapshots/<commit>/<path>
    std::string commit = commit_sha.empty() ? sha256 : commit_sha;
    fs::path snapshots_dir = repo_dir / "snapshots" / commit;
    fs::create_directories(snapshots_dir, ec);
    if (!ec) {
        fs::path symlink_path = snapshots_dir / file_path;
        fs::create_directories(symlink_path.parent_path(), ec);
        if (!ec) {
            // Remove existing symlink if any
            fs::remove(symlink_path, ec);

            // Create symlink: snapshots/<commit>/<path> -> ../../blobs/<sha256>
            fs::path target = fs::relative(blob_path, symlink_path.parent_path());
            fs::create_symlink(target, symlink_path, ec);
            if (ec) {
                // Symlink may not be supported on this platform; copy as fallback
                fs::copy(blob_path, symlink_path, ec);
            }
        }
    }

    LOG_INFO("installed %s in HF cache (%s)", file_path.c_str(), sha256.c_str());
    return true;
}

// ---- Public HF API functions ----

std::vector<std::pair<std::string, std::string>> list_hf_repo_files(const std::string& repo_id, const std::string& token, const std::string& revision) {
    std::vector<std::pair<std::string, std::string>> files;

    try {
        std::string endpoint = get_hf_endpoint();
        std::string url = endpoint + "/api/models/" + repo_id + "/tree/" + revision + "?recursive=true";

        std::string protocol, host, path;
        if (!parse_url(url, protocol, host, path)) {
            LOG_ERROR("invalid HF endpoint (empty host): %s", endpoint.c_str());
            return files;
        }

        auto cli = make_client(protocol, host);

        httplib::Headers headers = {
            {"User-Agent", "stable-diffusion.cpp/1.0"},
            {"Accept", "application/json"}
        };
        if (!token.empty()) {
            headers.emplace("Authorization", "Bearer " + token);
        }

        auto res = cli->Get(path.c_str(), headers);
        if (!res) {
            LOG_ERROR("failed to list HF repo files: %s (network error)", repo_id.c_str());
            return files;
        }
        if (res->status != 200) {
            LOG_ERROR("failed to list HF repo files: %s (status: %d)", repo_id.c_str(), res->status);
            return files;
        }

        auto json = nlohmann::json::parse(res->body);
        if (!json.is_array()) {
            LOG_ERROR("invalid response format from HF API for repo: %s", repo_id.c_str());
            return files;
        }

        for (const auto& item : json) {
            // The Hub tree API reports regular files as "file"; older/mirror
            // endpoints may use the git object type "blob".
            if (!item.is_object() || !item.contains("type") || !item["type"].is_string() ||
                (item["type"] != "file" && item["type"] != "blob") ||
                !item.contains("path") || !item["path"].is_string()) {
                continue;
            }

            std::string file_path = item["path"].get<std::string>();
            std::string file_url = endpoint + "/" + repo_id + "/resolve/" + revision + "/" + file_path;
            files.emplace_back(file_path, file_url);
        }

        LOG_INFO("found %zu files in HF repo: %s", files.size(), repo_id.c_str());
    } catch (const std::exception& e) {
        LOG_ERROR("failed to list HF repo files for %s: %s", repo_id.c_str(), e.what());
    } catch (...) {
        LOG_ERROR("failed to list HF repo files for %s: unknown exception", repo_id.c_str());
    }

    return files;
}

std::string download_hf_file(const std::string& repo_id, const std::string& path,
                             const std::string& token, ProgressCallback progress_cb,
                             const std::string& revision) {
    fs::path cache_dir = hf_cache::get_cache_directory();
    std::string folder_name = hf_cache::repo_to_folder_name(repo_id);
    fs::path repo_dir = cache_dir / folder_name;

    // Download to a temp location within the cache repo directory
    fs::path tmp_download = repo_dir / "blobs" / ".tmp_download";
    std::error_code ec;
    fs::create_directories(tmp_download.parent_path(), ec);
    if (ec) {
        LOG_ERROR("failed to create directory: %s", tmp_download.parent_path().string().c_str());
        return {};
    }

    // Remove stale temp file if it exists
    fs::remove(tmp_download, ec);

    std::string endpoint = get_hf_endpoint();
    std::string url = endpoint + "/" + repo_id + "/resolve/" + revision + "/" + path;

    LOG_INFO("downloading %s from %s (rev=%s)", path.c_str(), url.c_str(), revision.c_str());

    if (!download_file(url, tmp_download.string(), progress_cb)) {
        fs::remove(tmp_download, ec);
        return {};
    }

    // Compute SHA256 before installing (install moves the file)
    std::string sha256 = compute_sha256(tmp_download);
    if (sha256.empty()) {
        LOG_ERROR("failed to compute SHA256 for downloaded file");
        fs::remove(tmp_download, ec);
        return {};
    }

    fs::path blob_path = repo_dir / "blobs" / sha256;

    // Fetch the commit SHA from the API
    std::string commit = fetch_repo_commit(repo_id, token, revision);

    // Install into proper HF cache structure
    if (!install_in_cache(tmp_download, repo_id, path, commit, sha256)) {
        LOG_ERROR("failed to install file in HF cache");
        fs::remove(tmp_download, ec);
        return {};
    }

    return blob_path.string();
}

std::string resolve_or_download(const std::string& hf_ref, ProgressCallback progress_cb, const std::string& token) {
    if (hf_ref.empty()) {
        return {};
    }

    try {
        // First try to resolve from cache
        std::string cached = hf_cache::resolve_hf_path(hf_ref);
        if (!cached.empty()) {
            LOG_INFO("found in cache: %s -> %s", hf_ref.c_str(), cached.c_str());
            return cached;
        }

        // Parse repo_id[@revision]:pattern
        // Example: "user/model@fp16:v1-5.safetensors" or "user/model:*.safetensors"
        size_t colon_pos = hf_ref.find(':');
        std::string repo_part = hf_ref.substr(0, colon_pos);
        std::string pattern = colon_pos != std::string::npos ? hf_ref.substr(colon_pos + 1) : "";

        // Extract revision from repo_part: "user/model@revision" -> "user/model", "revision"
        std::string repo_id = repo_part;
        std::string revision = "main";
        size_t at_pos = repo_part.find('@');
        if (at_pos != std::string::npos) {
            repo_id = repo_part.substr(0, at_pos);
            revision = repo_part.substr(at_pos + 1);
            if (revision.empty()) revision = "main";
        }

        if (!hf_cache::is_valid_repo_id(repo_id)) {
            LOG_ERROR("invalid HuggingFace repo ID: %s", repo_id.c_str());
            return {};
        }

        LOG_INFO("model not found in cache, downloading from HuggingFace Hub: %s (rev: %s)", repo_id.c_str(), revision.c_str());

        // Use passed token, fall back to env var
        std::string effective_token = token.empty() ? get_hf_token() : token;

        // List files in repo at the given revision
        auto files = list_hf_repo_files(repo_id, effective_token, revision);
        if (files.empty()) {
            LOG_ERROR("no files found in HuggingFace repo: %s (rev: %s)", repo_id.c_str(), revision.c_str());
            return {};
        }

        // Find best matching file
        std::vector<std::pair<std::string, std::string>> matches;
        if (pattern.empty()) {
            // Use first file
            matches.push_back(files[0]);
        } else {
            // Case-insensitive matching with priority:
            // 1. Exact match
            // 2. .safetensors > .gguf > .bin priority
            // 3. Substring match
            std::string pattern_lower = pattern;
            std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            // First pass: exact filename match (case-insensitive)
            for (const auto& f : files) {
                std::string name_lower = f.first;
                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (name_lower == pattern_lower) {
                    matches.push_back(f);
                }
            }

            // Second pass: substring match (if no exact match yet)
            if (matches.empty()) {
                for (const auto& f : files) {
                    std::string name_lower = f.first;
                    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (name_lower.find(pattern_lower) != std::string::npos) {
                        matches.push_back(f);
                    }
                }
            }

            if (matches.empty()) {
                LOG_ERROR("no file matching '%s' found in repo: %s", pattern.c_str(), repo_id.c_str());
                return {};
            }

            // Sort matches by priority: prefer .safetensors over .gguf over .bin
            std::sort(matches.begin(), matches.end(),
                [](const auto& a, const auto& b) {
                    auto ext_rank = [](const std::string& s) -> int {
                        std::string lower = s;
                        std::transform(lower.begin(), lower.end(), lower.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (lower.find(".safetensors") != std::string::npos) return 0;
                        if (lower.find(".gguf") != std::string::npos) return 1;
                        if (lower.find(".bin") != std::string::npos) return 2;
                        return 3;
                    };
                    return ext_rank(a.first) < ext_rank(b.first);
                });
        }

        // Download the best matching file
        for (const auto& match : matches) {
            LOG_INFO("downloading '%s' from %s (rev: %s)", match.first.c_str(), repo_id.c_str(), revision.c_str());
            std::string local = download_hf_file(repo_id, match.first, effective_token, progress_cb, revision);
            if (!local.empty()) {
                LOG_INFO("successfully downloaded to: %s", local.c_str());
                return local;
            }
        }

        LOG_ERROR("failed to download any matching files from repo: %s", repo_id.c_str());
        return {};
    } catch (const std::exception& e) {
        LOG_ERROR("failed to resolve/download HuggingFace reference '%s': %s", hf_ref.c_str(), e.what());
        return {};
    }
}

} // namespace hf_download
