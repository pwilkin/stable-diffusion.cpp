#include "hf_cache.h"
#include "core/util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hf_cache {

fs::path get_cache_directory() {
    static const fs::path cache = []() {
        struct {
            const char* var;
            fs::path path;
        } entries[] = {
            {"LLAMA_CACHE", fs::path()},
            {"HF_HUB_CACHE", fs::path()},
            {"HUGGINGFACE_HUB_CACHE", fs::path()},
            {"HF_HOME", fs::path("hub")},
            {"XDG_CACHE_HOME", fs::path("huggingface") / "hub"},
#if defined(_WIN32)
            {"USERPROFILE", fs::path(".cache") / "huggingface" / "hub"}
#else
            {"HOME", fs::path(".cache") / "huggingface" / "hub"}
#endif
        };

        for (const auto& entry : entries) {
            if (auto* p = std::getenv(entry.var); p && *p) {
                fs::path base(p);
                return entry.path.empty() ? base : base / entry.path;
            }
        }

#if !defined(_WIN32)
        const struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir && *pw->pw_dir) {
            return fs::path(pw->pw_dir) / ".cache" / "huggingface" / "hub";
        }
#endif

        throw std::runtime_error("Failed to determine HF cache directory");
    }();
    return cache;
}

std::string folder_name_to_repo(const std::string& folder) {
    constexpr std::string_view prefix = "models--";
    if (folder.rfind(prefix, 0) != 0) {
        return {};
    }
    std::string result = folder.substr(prefix.length());
    // Replace "--" with "/" to restore user/model format
    size_t pos = 0;
    while ((pos = result.find("--", pos)) != std::string::npos) {
        result.replace(pos, 2, "/");
        pos += 1;
    }
    return result;
}

std::string repo_to_folder_name(const std::string& repo_id) {
    constexpr std::string_view prefix = "models--";
    std::string result = std::string(prefix) + repo_id;
    // huggingface_hub separates path components with "--", not "-";
    // folder_name_to_repo relies on this for the reverse mapping.
    size_t pos = 0;
    while ((pos = result.find('/', pos)) != std::string::npos) {
        result.replace(pos, 1, "--");
        pos += 2;
    }
    return result;
}

fs::path get_repo_path(const std::string& repo_id) {
    return get_cache_directory() / repo_to_folder_name(repo_id);
}

static bool is_alphanum(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

static bool is_special_char(char c) {
    return c == '/' || c == '.' || c == '-';
}

// base chars [A-Za-z0-9_] are always valid
// special chars [/.-] must be surrounded by base chars
// exactly one '/' required
bool is_valid_repo_id(const std::string& repo_id) {
    if (repo_id.empty() || repo_id.length() > 256) {
        return false;
    }
    int slash = 0;
    bool special = true;
    for (const char c : repo_id) {
        if (is_alphanum(c) || c == '_') {
            special = false;
        } else if (is_special_char(c)) {
            if (special) {
                return false;
            }
            slash += (c == '/');
            special = true;
        } else {
            return false;
        }
    }
    return !special && slash == 1;
}

static std::string get_cached_ref(const fs::path& repo_path) {
    fs::path refs_path = repo_path / "refs";
    if (!fs::is_directory(refs_path)) {
        return {};
    }
    std::string fallback;
    for (const auto& entry : fs::directory_iterator(refs_path)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream f(entry.path());
        std::string commit;
        if (!f || !std::getline(f, commit) || commit.empty()) {
            continue;
        }
        // Basic validation: should be hex string of length 40 (git SHA) or 64 (SHA256)
        if (commit.size() != 40 && commit.size() != 64) {
            continue;
        }
        bool valid = true;
        for (char c : commit) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            continue;
        }
        if (entry.path().filename() == "main") {
            return commit;
        }
        if (fallback.empty()) {
            fallback = commit;
        }
    }
    return fallback;
}

HFFiles get_cached_files(const std::string& repo_id) {
    fs::path cache_dir = get_cache_directory();
    if (!fs::exists(cache_dir)) {
        return {};
    }

    HFFiles files;

    for (const auto& repo : fs::directory_iterator(cache_dir)) {
        if (!repo.is_directory()) {
            continue;
        }

        std::string _repo_id = folder_name_to_repo(repo.path().filename().string());
        if (!is_valid_repo_id(_repo_id)) {
            continue;
        }

        if (!repo_id.empty() && _repo_id != repo_id) {
            continue;
        }

        fs::path snapshots_path = repo.path() / "snapshots";
        bool found_in_snapshots = false;

        if (fs::exists(snapshots_path)) {
            std::string commit = get_cached_ref(repo.path());
            if (!commit.empty()) {
                fs::path commit_path = snapshots_path / commit;
                if (fs::is_directory(commit_path)) {
                    for (const auto& entry : fs::recursive_directory_iterator(commit_path)) {
                        if (!entry.is_regular_file() && !entry.is_symlink()) {
                            continue;
                        }

                        fs::path rel_path = entry.path().lexically_relative(commit_path);
                        if (rel_path.empty()) {
                            continue;
                        }

                        HFFile file;
                        file.repo_id = _repo_id;
                        file.path = rel_path.generic_string();
                        file.local_path = entry.path().string();
                        file.oid = "";
                        files.push_back(std::move(file));
                        found_in_snapshots = true;
                    }
                }
            }
        }

        // Fallback: also scan blobs/ directory for any files not yet linked in snapshots
        fs::path blobs_path = repo.path() / "blobs";
        if (fs::exists(blobs_path)) {
            for (const auto& entry : fs::directory_iterator(blobs_path)) {
                if (!entry.is_regular_file() && !entry.is_symlink()) {
                    continue;
                }
                // Skip temp download files
                std::string fname = entry.path().filename().string();
                if (fname.find(".tmp") == 0) continue;

                HFFile file;
                file.repo_id = _repo_id;
                file.path = entry.path().filename().string();  // Use blob hash as path
                file.local_path = entry.path().string();
                file.oid = "";
                files.push_back(std::move(file));
            }
        }
    }

    return files;
}

HFFile find_best_file(const HFFiles& files, const std::string& pattern) {
    if (files.empty()) {
        return {};
    }

    if (pattern.empty()) {
        return files[0];
    }

    // Case-insensitive pattern matching
    std::string pattern_lower = pattern;
    std::transform(pattern_lower.begin(), pattern_lower.end(), pattern_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // First try exact match
    for (const auto& f : files) {
        std::string filename = f.path;
        std::transform(filename.begin(), filename.end(), filename.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (filename == pattern_lower) {
            return f;
        }
    }

    // Collect all substring matches, then sort by priority
    std::vector<HFFile> candidates;
    for (const auto& f : files) {
        std::string filename = f.path;
        std::transform(filename.begin(), filename.end(), filename.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (filename.find(pattern_lower) != std::string::npos) {
            candidates.push_back(f);
        }
    }

    if (!candidates.empty()) {
        // Sort by priority: prefer .safetensors over .gguf over .bin
        std::sort(candidates.begin(), candidates.end(),
            [](const HFFile& a, const HFFile& b) {
                auto ext_rank = [](const std::string& s) -> int {
                    std::string lower = s;
                    std::transform(lower.begin(), lower.end(), lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (lower.find(".safetensors") != std::string::npos) return 0;
                    if (lower.find(".gguf") != std::string::npos) return 1;
                    if (lower.find(".bin") != std::string::npos) return 2;
                    return 3;
                };
                return ext_rank(a.path) < ext_rank(b.path);
            });
        return candidates[0];
    }

    // Finally try regex match
    try {
        std::regex pattern_regex(pattern_lower, std::regex_constants::icase);
        for (const auto& f : files) {
            if (std::regex_search(f.path, pattern_regex)) {
                return f;
            }
        }
    } catch (const std::regex_error&) {
        // Invalid regex, ignore
    }

    return {};
}

std::string resolve_hf_path(const std::string& hf_ref) {
    if (hf_ref.empty()) {
        return {};
    }

    // Split repo_id[@revision] and pattern by ':'
    size_t colon_pos = hf_ref.find(':');
    std::string repo_part = hf_ref.substr(0, colon_pos);
    std::string pattern = colon_pos != std::string::npos ? hf_ref.substr(colon_pos + 1) : "";

    // Extract revision from repo_part
    std::string repo_id = repo_part;
    size_t at_pos = repo_part.find('@');
    if (at_pos != std::string::npos) {
        repo_id = repo_part.substr(0, at_pos);
        // revision = repo_part.substr(at_pos + 1);  // unused for now
    }

    if (!is_valid_repo_id(repo_id)) {
        return {};
    }

    HFFiles files = get_cached_files(repo_id);
    if (files.empty()) {
        return {};
    }

    HFFile best = find_best_file(files, pattern);
    if (best.path.empty()) {
        return {};
    }

    return best.local_path;
}

} // namespace hf_cache
