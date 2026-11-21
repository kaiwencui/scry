#pragma once

#include "common.hpp"
#include "gitignore.hpp"
#include "index_format.hpp"

#include <filesystem>
#include <string>

namespace scry {

struct IndexBuildOptions {
    u32 threads = 0; // 0 = auto (hardware_concurrency)
    bool respect_gitignore = true;
    bool include_hidden = false;
    bool follow_symlinks = false;
    bool extract_symbols = true;
    bool build_lower_table = true;
    bool build_path_index = true;
    bool use_io_uring = true;
    u64 max_file_size = 32ULL * 1024 * 1024;
    std::filesystem::path output_path; // Empty -> default cache path for repo root
};

struct IndexBuildStats {
    u32 files_indexed = 0;
    u32 files_skipped_binary = 0;
    u64 total_corpus_bytes = 0;
    u64 index_file_bytes = 0;
    u32 num_exact_trigrams = 0;
    u32 num_lower_trigrams = 0;
    u32 num_symbols = 0;
    double walk_ms = 0.0;
    double read_and_extract_ms = 0.0;
    double merge_and_encode_ms = 0.0;
    double write_ms = 0.0;
    double total_ms = 0.0;
    double throughput_mb_s = 0.0;
    bool used_io_uring = false;
    std::filesystem::path index_path;
};

// Build a complete immutable zero-copy mmap index for `root` and atomically swap it into place.
bool build_index(const std::filesystem::path& root,
                 const IndexBuildOptions& opts,
                 IndexBuildStats& stats,
                 std::string& err);

// Helper to compute git freshness metadata (.git/index mtime + HEAD hash)
void read_git_freshness(const std::filesystem::path& root,
                        i64& git_index_mtime_ns,
                        u64& git_head_hash);

// Locate existing index.scry for `root` (or ancestor git repo root).
// Returns true and sets `out_root` and `out_index_path` if found.
bool locate_index_for_path(const std::filesystem::path& query_path,
                           std::filesystem::path& out_root,
                           std::filesystem::path& out_index_path);

} // namespace scry
