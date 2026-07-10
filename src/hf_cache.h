#ifndef __HF_CACHE_H__
#define __HF_CACHE_H__

#include <string>
#include <vector>
#include <filesystem>

namespace hf_cache {
    struct HFFile {
        std::string path;         // relative path within repo
        std::string local_path;   // absolute path on disk
        std::string repo_id;
        std::string oid;
    };

    using HFFiles = std::vector<HFFile>;

    // Resolve the HuggingFace cache directory based on environment variables:
    // LLAMA_CACHE, HF_HUB_CACHE, HUGGINGFACE_HUB_CACHE, HF_HOME, XDG_CACHE_HOME
    // Falls back to ~/.cache/huggingface/hub
    std::filesystem::path get_cache_directory();

    // Convert repo ID to folder name: "user/model" -> "models--user--model"
    std::string repo_to_folder_name(const std::string& repo_id);

    // Convert folder name to repo ID: "models--user--model" -> "user/model"
    std::string folder_name_to_repo(const std::string& folder);

    // Get the path to a cached repo
    std::filesystem::path get_repo_path(const std::string& repo_id);

    // Validate repo ID format (user/model)
    bool is_valid_repo_id(const std::string& repo_id);

    // Get all cached files from a specific repo, or all repos if repo_id is empty
    HFFiles get_cached_files(const std::string& repo_id = {});

    // Find the best matching file in cached files
    // pattern: filename or pattern to match (case-insensitive)
    // If empty, returns the first file
    HFFile find_best_file(const HFFiles& files, const std::string& pattern = "");

    // Resolve a HuggingFace repo reference to a local file path
    // Format: "repo_id" or "repo_id:pattern"
    // Returns empty string if not found
    std::string resolve_hf_path(const std::string& hf_ref);
}

#endif // __HF_CACHE_H__
