#pragma once

#include "common.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scry {

// Language IDs stored compactly in FileEntry::lang_id
enum class LangId : u16 {
    Unknown = 0,
    C,
    Cpp,
    Rust,
    Python,
    Go,
    JavaScript,
    TypeScript,
    Java,
    Shell,
    Markdown,
    Json,
    Yaml,
    Toml,
    Make,
    CMake,
    Sql,
    Html,
    Css,
    Asm,
    Zig,
    Lua,
    Ruby,
    Php,
    Swift,
    Kotlin,
    Proto,
    Xml,
    Txt,
    Count
};

// Map extension / filename to LangId
LangId detect_lang(std::string_view rel_path) noexcept;
// Map CLI `-t <name>` to matching LangIds (e.g. "cpp" -> {Cpp}, "c" -> {C})
std::vector<LangId> resolve_type_name(std::string_view type_name);
const char* lang_name(LangId id) noexcept;

// Fast known-binary extension check (before even opening the file)
bool is_known_binary_ext(std::string_view path) noexcept;

// Glob matcher supporting `*`, `?`, `**`, `[abc]`, `[a-z]`, `[!a-z]`
bool glob_match(std::string_view pattern, std::string_view text, bool case_insensitive = false) noexcept;

// Filter for `-g` / `--glob` CLI arguments (supports leading `!` negation)
struct GlobFilter {
    struct Rule {
        std::string pattern;
        bool negated = false;
        bool match_basename = false;
        bool case_insensitive = false;
    };
    std::vector<Rule> rules;
    bool has_positive = false;

    void add(std::string_view pat, bool case_insensitive = false);
    bool empty() const noexcept { return rules.empty(); }
    bool matches(std::string_view rel_path) const noexcept;
};

// Full .gitignore rule & hierarchical matcher
struct GitignoreRule {
    std::string base_dir; // Relative to repo root, "" for root, or "foo/bar"
    std::string pattern;
    bool negated = false;
    bool dir_only = false;
    bool anchored = false; // Pattern contains '/' (not counting trailing '/')
};

class GitignoreMatcher {
public:
    void load_file(const std::filesystem::path& file_path, std::string_view base_dir);
    void load_root_defaults(const std::filesystem::path& repo_root);
    void add_pattern(std::string_view line, std::string_view base_dir);

    // Check if `rel_path` (normalized forward-slash path relative to root) is ignored
    bool is_ignored(std::string_view rel_path, bool is_dir) const noexcept;

    size_t rule_count() const noexcept { return rules_.size(); }

    // Snapshot size for hierarchical push/pop during directory traversal
    size_t checkpoint() const noexcept { return rules_.size(); }
    void restore(size_t cp) noexcept {
        if (cp < rules_.size()) rules_.resize(cp);
    }

private:
    std::vector<GitignoreRule> rules_;
};

// Discover files respecting .gitignore, hidden-file policy, binary ext filtering,
// optional globs & types. Returns relative paths (sorted lexicographically).
struct WalkOptions {
    bool respect_gitignore = true;
    bool include_hidden = false;
    bool follow_symlinks = false;
    u64 max_file_size = 64ULL * 1024 * 1024; // 64 MiB default safety cap
    GlobFilter globs;
    std::vector<LangId> include_types;
    std::vector<LangId> exclude_types;
};

struct DiscoveredFile {
    std::string rel_path;
    u64 file_size = 0;
    i64 mtime_ns = 0;
    u64 inode = 0;
    LangId lang = LangId::Unknown;
};

std::vector<DiscoveredFile> walk_directory(const std::filesystem::path& root,
                                           const WalkOptions& opts = {});

} // namespace scry
