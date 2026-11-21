#pragma once

#include "common.hpp"
#include "delta.hpp"
#include "gitignore.hpp"
#include "index_format.hpp"
#include "regex_engine.hpp"

#include <atomic>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace scry {

enum class ColorMode : u8 { Auto, Always, Never };

struct SearchOptions {
    std::string pattern;
    std::filesystem::path target_path = ".";
    std::vector<std::filesystem::path> target_paths; // Multiple targets; empty -> uses target_path

    RegexOptions regex_opts;
    GlobFilter globs;
    std::vector<LangId> include_types;
    std::vector<LangId> exclude_types;

    bool respect_gitignore = true;
    bool include_hidden = false;
    bool follow_symlinks = false;
    bool search_binary = false;      // -uuu / --text (-a)
    bool search_stdin = false;       // Read from stdin (piped or '-')

    bool count_only = false;         // -c / --count
    bool files_with_matches = false; // -l / --files-with-matches
    bool files_without_match = false;// --files-without-match
    bool list_files = false;         // --files
    bool json_output = false;        // --json
    bool vimgrep = false;            // --vimgrep (file:line:col:text per match)
    bool heading = false;            // --heading (grouped filename headers)
    bool heading_explicit = false;   // Track if --heading or --no-heading was set
    bool quiet = false;              // -q / --quiet
    bool null_sep = false;           // -0 / --null (NUL after filenames)
    bool invert_match = false;       // -v / --invert-match
    bool only_matching = false;      // -o / --only-matching
    bool def_only = false;           // --def (symbol definition search)
    bool boost_defs = false;         // --boost-defs
    bool show_line_numbers = true;   // -n (default on) / -N (off)
    bool show_column = false;        // --column
    bool with_filename = true;       // -H / -h
    bool filename_explicit = false;  // Track if -H or -h was explicitly passed
    bool force_no_index = false;     // --no-index
    bool strict_freshness = false;   // --fresh
    bool use_daemon = true;          // Try daemon socket if running
    bool show_stats = false;         // --stats

    u64 max_file_size = 64ULL * 1024 * 1024; // --max-filesize
    u32 before_context = 0;          // -B
    u32 after_context = 0;           // -A
    u32 max_count_per_file = 0;      // -m (0 = unlimited)
    u32 max_total_matches = 0;       // Top-K limit (0 = unlimited, used by TUI / --max-results)
    u32 threads = 0;                 // 0 = auto

    ColorMode color = ColorMode::Auto;
};

struct ContextLine {
    u32 line_no = 0;
    std::string text;
};

struct LineMatch {
    u32 line_no = 0;
    std::string line;
    std::vector<MatchSpan> spans;
    std::vector<ContextLine> before;
    std::vector<ContextLine> after;
    bool is_definition = false;
    SymbolKind def_kind = SymbolKind::Unknown;
};

struct FileSearchResult {
    std::string rel_path;
    std::vector<LineMatch> matches;
    u32 match_count = 0;
    bool has_def = false;
};

struct SearchStats {
    bool used_index = false;
    bool used_daemon = false;
    bool used_linear_fallback = false;
    bool cancelled = false;
    u32 total_indexed_docs = 0;
    u32 candidate_docs = 0;
    u32 verified_docs = 0;
    u32 matched_files = 0;
    u64 matched_lines = 0;
    double plan_ms = 0.0;
    double verify_ms = 0.0;
    double total_ms = 0.0;
    std::string trigram_plan_desc;
};

struct SearchResponse {
    std::vector<FileSearchResult> files;
    std::vector<std::string> listed_files; // Populated when opts.list_files == true
    SearchStats stats;
    std::string error;
};

// Search using a pre-loaded IndexSnapshot (used by daemon, TUI, benchmarks, and CLI)
SearchResponse search_snapshot(const IndexSnapshot& snap,
                               const SearchOptions& opts,
                               const std::string& subdir_prefix = "",
                               const std::atomic<bool>* cancel = nullptr);

// End-to-end search entry point (auto-discovers index, freshness delta, stdin, multi-path, or SIMD fallback)
SearchResponse run_search(const SearchOptions& opts,
                          const std::atomic<bool>* cancel = nullptr);

// Search an in-memory buffer (used for stdin piping)
SearchResponse search_stdin_buffer(const u8* data, size_t len, const SearchOptions& opts);

// Format SearchResponse to an output stream (rg-compatible text, --count, -l, --files, --json)
void format_search_response(const SearchResponse& resp,
                            const SearchOptions& opts,
                            bool is_tty,
                            std::ostream& out);

} // namespace scry
