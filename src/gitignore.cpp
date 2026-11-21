#include "gitignore.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <dirent.h>
#include <fstream>
#include <unordered_map>

namespace scry {

namespace {

std::string to_lower_ascii(std::string_view sv) {
    std::string out(sv.size(), '\0');
    for (size_t i = 0; i < sv.size(); ++i) {
        out[i] = static_cast<char>(ascii_lower(static_cast<u8>(sv[i])));
    }
    return out;
}

std::string_view path_basename(std::string_view p) noexcept {
    auto pos = p.rfind('/');
    return (pos == std::string_view::npos) ? p : p.substr(pos + 1);
}

std::string_view path_extension(std::string_view p) noexcept {
    auto base = path_basename(p);
    auto dot = base.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return {};
    return base.substr(dot + 1);
}

bool char_eq(char a, char b, bool icase) noexcept {
    if (!icase) return a == b;
    return ascii_lower(static_cast<u8>(a)) == ascii_lower(static_cast<u8>(b));
}

// Match a single path segment or full string with *, ?, **, [...]
bool glob_match_impl(std::string_view pat, size_t pi,
                     std::string_view txt, size_t ti,
                     bool icase) noexcept {
    while (pi < pat.size()) {
        // Handle '**'
        if (pat[pi] == '*' && pi + 1 < pat.size() && pat[pi + 1] == '*') {
            size_t next_pi = pi + 2;
            while (next_pi < pat.size() && pat[next_pi] == '*') ++next_pi;
            // If '**/' -> can match zero directories as well!
            if (next_pi < pat.size() && pat[next_pi] == '/') {
                if (glob_match_impl(pat, next_pi + 1, txt, ti, icase)) return true;
            }
            if (next_pi == pat.size()) return true;
            for (size_t k = ti; k <= txt.size(); ++k) {
                if (glob_match_impl(pat, next_pi, txt, k, icase)) return true;
            }
            return false;
        }
        // Single '*' matches anything except '/'
        if (pat[pi] == '*') {
            size_t next_pi = pi + 1;
            for (size_t k = ti; k <= txt.size(); ++k) {
                if (k > ti && txt[k - 1] == '/') break;
                if (glob_match_impl(pat, next_pi, txt, k, icase)) return true;
            }
            return false;
        }
        if (ti >= txt.size()) return false;
        if (pat[pi] == '?') {
            if (txt[ti] == '/') return false;
            ++pi;
            ++ti;
            continue;
        }
        if (pat[pi] == '[') {
            if (txt[ti] == '/') return false;
            size_t close = pi + 1;
            bool neg = false;
            if (close < pat.size() && (pat[close] == '!' || pat[close] == '^')) {
                neg = true;
                ++close;
            }
            bool matched = false;
            char tc = icase ? static_cast<char>(ascii_lower(static_cast<u8>(txt[ti]))) : txt[ti];
            while (close < pat.size() && pat[close] != ']') {
                char lo = pat[close];
                if (icase) lo = static_cast<char>(ascii_lower(static_cast<u8>(lo)));
                if (close + 2 < pat.size() && pat[close + 1] == '-' && pat[close + 2] != ']') {
                    char hi = pat[close + 2];
                    if (icase) hi = static_cast<char>(ascii_lower(static_cast<u8>(hi)));
                    if (tc >= lo && tc <= hi) matched = true;
                    close += 3;
                } else {
                    if (tc == lo) matched = true;
                    ++close;
                }
            }
            if (close < pat.size() && pat[close] == ']') {
                if (neg) matched = !matched;
                if (!matched) return false;
                pi = close + 1;
                ++ti;
                continue;
            }
        }
        if (pat[pi] == '\\' && pi + 1 < pat.size()) {
            ++pi;
        }
        if (!char_eq(pat[pi], txt[ti], icase)) return false;
        ++pi;
        ++ti;
    }
    return ti == txt.size();
}

} // namespace

bool glob_match(std::string_view pattern, std::string_view text, bool case_insensitive) noexcept {
    return glob_match_impl(pattern, 0, text, 0, case_insensitive);
}

LangId detect_lang(std::string_view rel_path) noexcept {
    auto base = path_basename(rel_path);
    if (base == "Makefile" || base == "GNUmakefile" || base == "makefile") return LangId::Make;
    if (base == "CMakeLists.txt") return LangId::CMake;

    auto ext_sv = path_extension(rel_path);
    if (ext_sv.empty()) return LangId::Unknown;
    std::string ext = to_lower_ascii(ext_sv);

    static const std::unordered_map<std::string, LangId> kExtMap = {
        {"c", LangId::C},
        {"h", LangId::C},
        {"cc", LangId::Cpp},
        {"cpp", LangId::Cpp},
        {"cxx", LangId::Cpp},
        {"c++", LangId::Cpp},
        {"hh", LangId::Cpp},
        {"hpp", LangId::Cpp},
        {"hxx", LangId::Cpp},
        {"h++", LangId::Cpp},
        {"ipp", LangId::Cpp},
        {"tpp", LangId::Cpp},
        {"inl", LangId::Cpp},
        {"cu", LangId::Cpp},
        {"cuh", LangId::Cpp},
        {"rs", LangId::Rust},
        {"py", LangId::Python},
        {"pyi", LangId::Python},
        {"pyx", LangId::Python},
        {"go", LangId::Go},
        {"js", LangId::JavaScript},
        {"mjs", LangId::JavaScript},
        {"cjs", LangId::JavaScript},
        {"jsx", LangId::JavaScript},
        {"ts", LangId::TypeScript},
        {"mts", LangId::TypeScript},
        {"cts", LangId::TypeScript},
        {"tsx", LangId::TypeScript},
        {"java", LangId::Java},
        {"sh", LangId::Shell},
        {"bash", LangId::Shell},
        {"zsh", LangId::Shell},
        {"fish", LangId::Shell},
        {"md", LangId::Markdown},
        {"markdown", LangId::Markdown},
        {"rst", LangId::Markdown},
        {"json", LangId::Json},
        {"jsonc", LangId::Json},
        {"yaml", LangId::Yaml},
        {"yml", LangId::Yaml},
        {"toml", LangId::Toml},
        {"mk", LangId::Make},
        {"cmake", LangId::CMake},
        {"sql", LangId::Sql},
        {"html", LangId::Html},
        {"htm", LangId::Html},
        {"css", LangId::Css},
        {"scss", LangId::Css},
        {"s", LangId::Asm},
        {"asm", LangId::Asm},
        {"zig", LangId::Zig},
        {"lua", LangId::Lua},
        {"rb", LangId::Ruby},
        {"php", LangId::Php},
        {"swift", LangId::Swift},
        {"kt", LangId::Kotlin},
        {"kts", LangId::Kotlin},
        {"proto", LangId::Proto},
        {"xml", LangId::Xml},
        {"txt", LangId::Txt},
    };
    auto it = kExtMap.find(ext);
    return it != kExtMap.end() ? it->second : LangId::Unknown;
}

std::vector<LangId> resolve_type_name(std::string_view type_name) {
    std::string t = to_lower_ascii(type_name);
    if (t == "c") return {LangId::C};
    if (t == "cpp" || t == "c++" || t == "cxx") return {LangId::Cpp, LangId::C};
    if (t == "rust" || t == "rs") return {LangId::Rust};
    if (t == "py" || t == "python") return {LangId::Python};
    if (t == "go") return {LangId::Go};
    if (t == "js" || t == "javascript") return {LangId::JavaScript};
    if (t == "ts" || t == "typescript") return {LangId::TypeScript};
    if (t == "java") return {LangId::Java};
    if (t == "sh" || t == "shell" || t == "bash") return {LangId::Shell};
    if (t == "md" || t == "markdown") return {LangId::Markdown};
    if (t == "json") return {LangId::Json};
    if (t == "yaml" || t == "yml") return {LangId::Yaml};
    if (t == "toml") return {LangId::Toml};
    if (t == "make" || t == "makefile") return {LangId::Make};
    if (t == "cmake") return {LangId::CMake};
    if (t == "sql") return {LangId::Sql};
    if (t == "html") return {LangId::Html};
    if (t == "css") return {LangId::Css};
    if (t == "asm") return {LangId::Asm};
    if (t == "zig") return {LangId::Zig};
    if (t == "lua") return {LangId::Lua};
    if (t == "rb" || t == "ruby") return {LangId::Ruby};
    if (t == "php") return {LangId::Php};
    if (t == "swift") return {LangId::Swift};
    if (t == "kt" || t == "kotlin") return {LangId::Kotlin};
    if (t == "proto" || t == "protobuf") return {LangId::Proto};
    if (t == "xml") return {LangId::Xml};
    if (t == "txt") return {LangId::Txt};
    return {};
}

const char* lang_name(LangId id) noexcept {
    switch (id) {
        case LangId::C: return "c";
        case LangId::Cpp: return "cpp";
        case LangId::Rust: return "rust";
        case LangId::Python: return "py";
        case LangId::Go: return "go";
        case LangId::JavaScript: return "js";
        case LangId::TypeScript: return "ts";
        case LangId::Java: return "java";
        case LangId::Shell: return "sh";
        case LangId::Markdown: return "md";
        case LangId::Json: return "json";
        case LangId::Yaml: return "yaml";
        case LangId::Toml: return "toml";
        case LangId::Make: return "make";
        case LangId::CMake: return "cmake";
        case LangId::Sql: return "sql";
        case LangId::Html: return "html";
        case LangId::Css: return "css";
        case LangId::Asm: return "asm";
        case LangId::Zig: return "zig";
        case LangId::Lua: return "lua";
        case LangId::Ruby: return "ruby";
        case LangId::Php: return "php";
        case LangId::Swift: return "swift";
        case LangId::Kotlin: return "kotlin";
        case LangId::Proto: return "proto";
        case LangId::Xml: return "xml";
        case LangId::Txt: return "txt";
        default: return "unknown";
    }
}

bool is_known_binary_ext(std::string_view path) noexcept {
    auto ext_sv = path_extension(path);
    if (ext_sv.empty()) return false;
    std::string ext = to_lower_ascii(ext_sv);
    static const std::unordered_map<std::string, bool> kBin = {
        {"o", true}, {"obj", true}, {"a", true}, {"so", true}, {"dylib", true}, {"dll", true},
        {"exe", true}, {"bin", true}, {"elf", true}, {"class", true}, {"pyc", true}, {"pyo", true},
        {"wasm", true}, {"bc", true}, {"pdb", true}, {"lib", true},
        {"png", true}, {"jpg", true}, {"jpeg", true}, {"gif", true}, {"webp", true}, {"ico", true},
        {"bmp", true}, {"tiff", true}, {"psd", true},
        {"mp3", true}, {"mp4", true}, {"wav", true}, {"ogg", true}, {"flac", true}, {"avi", true},
        {"mkv", true}, {"mov", true}, {"webm", true},
        {"zip", true}, {"gz", true}, {"tar", true}, {"bz2", true}, {"xz", true}, {"zst", true},
        {"7z", true}, {"rar", true}, {"lz4", true}, {"jar", true}, {"war", true}, {"whl", true},
        {"pdf", true}, {"doc", true}, {"docx", true}, {"xls", true}, {"xlsx", true}, {"ppt", true},
        {"ttf", true}, {"otf", true}, {"woff", true}, {"woff2", true}, {"eot", true},
        {"sqlite", true}, {"db", true}, {"scry", true},
    };
    return kBin.find(ext) != kBin.end();
}

void GlobFilter::add(std::string_view pat, bool case_insensitive) {
    Rule r;
    r.case_insensitive = case_insensitive;
    if (!pat.empty() && pat[0] == '!') {
        r.negated = true;
        pat.remove_prefix(1);
    } else {
        has_positive = true;
    }
    if (!pat.empty() && pat[0] == '/') {
        pat.remove_prefix(1);
    }
    r.match_basename = (pat.find('/') == std::string_view::npos);
    r.pattern = std::string(pat);
    rules.push_back(std::move(r));
}

bool GlobFilter::matches(std::string_view rel_path) const noexcept {
    if (rules.empty()) return true;
    bool included = !has_positive;
    auto base = path_basename(rel_path);
    for (const auto& r : rules) {
        bool hit = false;
        if (r.match_basename) {
            hit = glob_match(r.pattern, base, r.case_insensitive);
        } else {
            hit = glob_match(r.pattern, rel_path, r.case_insensitive);
            if (!hit && r.pattern.rfind("**/", 0) != 0) {
                // Also try matching suffix path segments
                hit = glob_match(r.pattern, rel_path, r.case_insensitive);
            }
        }
        if (hit) {
            included = !r.negated;
        }
    }
    return included;
}

void GitignoreMatcher::add_pattern(std::string_view raw_line, std::string_view base_dir) {
    // Strip trailing '\r'
    if (!raw_line.empty() && raw_line.back() == '\r') {
        raw_line.remove_suffix(1);
    }
    // Strip unescaped trailing spaces
    while (!raw_line.empty() && raw_line.back() == ' ') {
        if (raw_line.size() >= 2 && raw_line[raw_line.size() - 2] == '\\') {
            break;
        }
        raw_line.remove_suffix(1);
    }
    if (raw_line.empty() || raw_line[0] == '#') return;

    GitignoreRule r;
    r.base_dir = std::string(base_dir);
    if (raw_line[0] == '!') {
        r.negated = true;
        raw_line.remove_prefix(1);
    } else if (raw_line.size() >= 2 && raw_line[0] == '\\' && (raw_line[1] == '#' || raw_line[1] == '!')) {
        raw_line.remove_prefix(1);
    }
    if (raw_line.empty()) return;

    if (raw_line.back() == '/') {
        r.dir_only = true;
        raw_line.remove_suffix(1);
    }
    if (raw_line.empty()) return;

    if (raw_line.front() == '/') {
        r.anchored = true;
        raw_line.remove_prefix(1);
    } else if (raw_line.find('/') != std::string_view::npos) {
        r.anchored = true;
    }

    r.pattern = std::string(raw_line);
    rules_.push_back(std::move(r));
}

void GitignoreMatcher::load_file(const std::filesystem::path& file_path, std::string_view base_dir) {
    std::ifstream in(file_path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        add_pattern(line, base_dir);
    }
}

void GitignoreMatcher::load_root_defaults(const std::filesystem::path& repo_root) {
    // .git/info/exclude
    auto exclude = repo_root / ".git" / "info" / "exclude";
    if (std::filesystem::exists(exclude)) {
        load_file(exclude, "");
    }
    // Root .gitignore
    auto gi = repo_root / ".gitignore";
    if (std::filesystem::exists(gi)) {
        load_file(gi, "");
    }
    // .ignore (rg also respects .ignore)
    auto ign = repo_root / ".ignore";
    if (std::filesystem::exists(ign)) {
        load_file(ign, "");
    }
}

bool GitignoreMatcher::is_ignored(std::string_view rel_path, bool is_dir) const noexcept {
    bool ignored = false;
    for (const auto& r : rules_) {
        if (r.dir_only && !is_dir) continue;

        std::string_view sub = rel_path;
        if (!r.base_dir.empty()) {
            if (rel_path.size() <= r.base_dir.size() ||
                rel_path.compare(0, r.base_dir.size(), r.base_dir) != 0 ||
                rel_path[r.base_dir.size()] != '/') {
                continue;
            }
            sub = rel_path.substr(r.base_dir.size() + 1);
        }

        bool matched = false;
        if (r.anchored) {
            matched = glob_match(r.pattern, sub, false);
        } else {
            auto base = path_basename(sub);
            matched = glob_match(r.pattern, base, false);
            if (!matched && r.pattern.find("**") != std::string::npos) {
                matched = glob_match(r.pattern, sub, false);
            }
        }
        if (matched) {
            ignored = !r.negated;
        }
    }
    return ignored;
}

namespace {

bool matches_type_filter(LangId lang, const WalkOptions& opts) noexcept {
    if (!opts.include_types.empty()) {
        bool ok = false;
        for (auto t : opts.include_types) {
            if (t == lang) { ok = true; break; }
        }
        if (!ok) return false;
    }
    for (auto t : opts.exclude_types) {
        if (t == lang) return false;
    }
    return true;
}

void walk_rec(const std::filesystem::path& root,
              const std::string& rel_dir,
              const WalkOptions& opts,
              GitignoreMatcher& gi,
              std::vector<DiscoveredFile>& out) {
    std::filesystem::path abs_dir = rel_dir.empty() ? root : (root / rel_dir);

    size_t cp = gi.checkpoint();
    if (opts.respect_gitignore && !rel_dir.empty()) {
        auto nested_gi = abs_dir / ".gitignore";
        struct stat st {};
        if (::stat(nested_gi.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            gi.load_file(nested_gi, rel_dir);
        }
        auto nested_ign = abs_dir / ".ignore";
        if (::stat(nested_ign.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            gi.load_file(nested_ign, rel_dir);
        }
    }

    DIR* dir = ::opendir(abs_dir.c_str());
    if (!dir) {
        gi.restore(cp);
        return;
    }

    std::vector<std::string> subdirs;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(dir)) != nullptr) {
        const char* name = ent->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        // Always skip .git and .scry internal directories
        if (std::strcmp(name, ".git") == 0 || std::strcmp(name, ".scry") == 0) {
            continue;
        }
        if (!opts.include_hidden && name[0] == '.') {
            continue;
        }

        std::string child_rel = rel_dir.empty() ? std::string(name) : (rel_dir + "/" + name);
        std::string child_abs = abs_dir.string() + "/" + name;

        bool is_dir = false;
        bool is_reg = false;
        struct stat st {};
        int stat_rc = opts.follow_symlinks ? ::stat(child_abs.c_str(), &st)
                                           : ::lstat(child_abs.c_str(), &st);
        if (stat_rc != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            is_dir = true;
        } else if (S_ISREG(st.st_mode)) {
            is_reg = true;
        } else {
            continue;
        }

        if (opts.respect_gitignore && gi.is_ignored(child_rel, is_dir)) {
            continue;
        }

        if (is_dir) {
            subdirs.push_back(std::move(child_rel));
        } else if (is_reg) {
            if (static_cast<u64>(st.st_size) > opts.max_file_size) continue;
            if (is_known_binary_ext(child_rel)) continue;
            if (!opts.globs.empty() && !opts.globs.matches(child_rel)) continue;
            LangId lang = detect_lang(child_rel);
            if (!matches_type_filter(lang, opts)) continue;

            DiscoveredFile df;
            df.rel_path = std::move(child_rel);
            df.file_size = static_cast<u64>(st.st_size);
            df.mtime_ns = stat_mtime_ns(st);
            df.inode = static_cast<u64>(st.st_ino);
            df.lang = lang;
            out.push_back(std::move(df));
        }
    }
    ::closedir(dir);

    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& sub : subdirs) {
        walk_rec(root, sub, opts, gi, out);
    }

    gi.restore(cp);
}

} // namespace

std::vector<DiscoveredFile> walk_directory(const std::filesystem::path& root,
                                           const WalkOptions& opts) {
    std::vector<DiscoveredFile> out;
    struct stat st {};
    if (::stat(root.c_str(), &st) != 0) return out;

    // Single file target
    if (S_ISREG(st.st_mode)) {
        DiscoveredFile df;
        df.rel_path = root.filename().string();
        df.file_size = static_cast<u64>(st.st_size);
        df.mtime_ns = stat_mtime_ns(st);
        df.inode = static_cast<u64>(st.st_ino);
        df.lang = detect_lang(df.rel_path);
        out.push_back(std::move(df));
        return out;
    }

    GitignoreMatcher root_gi;
    if (opts.respect_gitignore) {
        root_gi.load_root_defaults(root);
    }

    // Seed parallel work queue by expanding root (and depth-1 subdirs) so giant subtrees
    // like linux/drivers/ are balanced evenly across all cores!
    struct DirTask {
        std::string rel_dir;
        GitignoreMatcher gi;
    };
    std::vector<DirTask> tasks;

    auto scan_one_level = [&](const std::string& rel_dir,
                              GitignoreMatcher gi,
                              std::vector<DirTask>& child_tasks,
                              std::vector<DiscoveredFile>& files_out) {
        std::filesystem::path abs_dir = rel_dir.empty() ? root : (root / rel_dir);
        if (opts.respect_gitignore && !rel_dir.empty()) {
            struct stat st2 {};
            auto ngi = abs_dir / ".gitignore";
            if (::stat(ngi.c_str(), &st2) == 0 && S_ISREG(st2.st_mode)) gi.load_file(ngi, rel_dir);
            auto nign = abs_dir / ".ignore";
            if (::stat(nign.c_str(), &st2) == 0 && S_ISREG(st2.st_mode)) gi.load_file(nign, rel_dir);
        }
        DIR* dir = ::opendir(abs_dir.c_str());
        if (!dir) return;
        struct dirent* ent = nullptr;
        while ((ent = ::readdir(dir)) != nullptr) {
            const char* name = ent->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
            if (std::strcmp(name, ".git") == 0 || std::strcmp(name, ".scry") == 0) continue;
            if (!opts.include_hidden && name[0] == '.') continue;

            std::string child_rel = rel_dir.empty() ? std::string(name) : (rel_dir + "/" + name);
            std::string child_abs = abs_dir.string() + "/" + name;

            struct stat cst {};
            int rc = opts.follow_symlinks ? ::stat(child_abs.c_str(), &cst)
                                          : ::lstat(child_abs.c_str(), &cst);
            if (rc != 0) continue;

            bool is_dir = S_ISDIR(cst.st_mode);
            bool is_reg = S_ISREG(cst.st_mode);
            if (!is_dir && !is_reg) continue;
            if (opts.respect_gitignore && gi.is_ignored(child_rel, is_dir)) continue;

            if (is_dir) {
                child_tasks.push_back({std::move(child_rel), gi});
            } else if (is_reg) {
                if (static_cast<u64>(cst.st_size) > opts.max_file_size) continue;
                if (is_known_binary_ext(child_rel)) continue;
                if (!opts.globs.empty() && !opts.globs.matches(child_rel)) continue;
                LangId lang = detect_lang(child_rel);
                if (!matches_type_filter(lang, opts)) continue;
                files_out.push_back({std::move(child_rel), static_cast<u64>(cst.st_size),
                                     stat_mtime_ns(cst), static_cast<u64>(cst.st_ino), lang});
            }
        }
        ::closedir(dir);
    };

    std::vector<DirTask> depth1;
    scan_one_level("", root_gi, depth1, out);
    if (depth1.size() <= 2) {
        for (auto& d : depth1) {
            walk_rec(root, d.rel_dir, opts, d.gi, out);
        }
    } else {
        // Expand depth-1 directories into depth-2 tasks for fine-grained parallel load balancing
        for (auto& d : depth1) {
            scan_one_level(d.rel_dir, d.gi, tasks, out);
        }
        if (!tasks.empty()) {
            u32 nw = std::min<u32>(WorkerPool::global().max_threads(),
                                   static_cast<u32>(tasks.size()));
            std::vector<std::vector<DiscoveredFile>> thread_outs(nw);
            std::atomic<size_t> next_task{0};
            WorkerPool::global().run(nw, [&](u32 tid) {
                auto& local = thread_outs[tid];
                local.reserve(512);
                while (true) {
                    size_t idx = next_task.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= tasks.size()) break;
                    walk_rec(root, tasks[idx].rel_dir, opts, tasks[idx].gi, local);
                }
            });
            size_t total = out.size();
            for (const auto& v : thread_outs) total += v.size();
            out.reserve(total);
            for (auto& v : thread_outs) {
                out.insert(out.end(), std::make_move_iterator(v.begin()),
                           std::make_move_iterator(v.end()));
            }
        }
    }

    std::sort(out.begin(), out.end(), [](const DiscoveredFile& a, const DiscoveredFile& b) {
        return a.rel_path < b.rel_path;
    });
    return out;
}

} // namespace scry
