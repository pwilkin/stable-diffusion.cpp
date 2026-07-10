#ifndef __HF_DOWNLOAD_H__
#define __HF_DOWNLOAD_H__

#include <string>
#include <functional>
#include <vector>
#include <filesystem>

namespace hf_download {

struct DownloadProgress {
    size_t downloaded = 0;
    size_t total = 0;
    bool cancelled = false;
};

using ProgressCallback = std::function<bool(const DownloadProgress&)>;

// Download a file from URL to local path
// Returns true on success, false on failure
bool download_file(const std::string& url, const std::string& local_path, ProgressCallback progress_cb = nullptr);

// Download multiple files concurrently (respects hardware concurrency)
// Returns true if all downloads succeed
bool download_files(const std::vector<std::pair<std::string, std::string>>& url_path_pairs, ProgressCallback progress_cb = nullptr);

// Get the HuggingFace Hub API endpoint (respects HF_ENDPOINT env var)
std::string get_hf_endpoint();

// Get the HuggingFace auth token from environment (HF_TOKEN, HUGGINGFACE_TOKEN, HF_HUB_TOKEN)
std::string get_hf_token();

// List files in a HuggingFace repository
// revision: optional branch/tag/commit (default "main")
// Returns vector of {path, url} pairs
std::vector<std::pair<std::string, std::string>> list_hf_repo_files(const std::string& repo_id, const std::string& token = "", const std::string& revision = "main");

// Download a file from HuggingFace Hub to the cache
// revision: optional branch/tag/commit (default "main")
// Returns the local path on success, empty string on failure
std::string download_hf_file(const std::string& repo_id, const std::string& path, const std::string& token = "", ProgressCallback progress_cb = nullptr, const std::string& revision = "main");

// Resolve and optionally download a HuggingFace cache reference
// If the file exists in cache, returns its path
// If not, downloads it from the Hub
// If token is empty, uses HF_TOKEN / HUGGINGFACE_TOKEN env var for auth
// Returns empty string on failure
std::string resolve_or_download(const std::string& hf_ref, ProgressCallback progress_cb = nullptr, const std::string& token = "");

} // namespace hf_download

#endif // __HF_DOWNLOAD_H__
