#include "cli.hpp"
#include "simd.hpp"

#include <cctype>
#include <cstring>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace scry {

namespace {

std::string regex_escape_literal(std::string_view lit) {
    std::string out;
    out.reserve(lit.size() + 8);
    for (char c : lit) {
        switch (c) {
            case '\\': case '.': case '^': case '$': case '*': case '+':
            case '?':  case '(': case ')': case '[': case ']': case '{':
            case '}':  case '|':
                out.push_back('\\');
                out.push_back(c);
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

bool parse_size_bytes(std::string_view s, u64& out) {
    if (s.empty()) return false;
    u64 mult = 1;
    char last = static_cast<char>(std::toupper(static_cast<unsigned char>(s.back())));
    if (last == 'K') { mult = 1024ULL; s.remove_suffix(1); }
    else if (last == 'M') { mult = 1024ULL * 1024ULL; s.remove_suffix(1); }
    else if (last == 'G') { mult = 1024ULL * 1024ULL * 1024ULL; s.remove_suffix(1); }
    if (s.empty()) return false;
    u64 val = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        val = val * 10 + static_cast<u64>(c - '0');
    }
    out = val * mult;
    return true;
}

bool stdin_is_piped_or_file() noexcept {
    if (::isatty(STDIN_FILENO) == 1) return false;
    struct stat st {};
    if (::fstat(STDIN_FILENO, &st) != 0) return false;
    return S_ISFIFO(st.st_mode) || S_ISREG(st.st_mode);
}

} // namespace

void print_version(std::ostream& out) {
    out << "scry 0.2.0 (C++23, SIMD=" << simd::arch_name() << ")\n";
}

void print_type_list(std::ostream& out) {
    out <<
R"(c: *.c, *.h
cpp: *.cc, *.cpp, *.cxx, *.c++, *.hh, *.hpp, *.hxx, *.h++, *.ipp, *.tpp, *.inl, *.cu, *.cuh, *.c, *.h
rust: *.rs
py: *.py, *.pyi, *.pyx
go: *.go
js: *.js, *.mjs, *.cjs, *.jsx
ts: *.ts, *.mts, *.cts, *.tsx
java: *.java
sh: *.sh, *.bash, *.zsh, *.fish
md: *.md, *.markdown, *.rst
json: *.json, *.jsonc
yaml: *.yaml, *.yml
toml: *.toml
make: Makefile, GNUmakefile, makefile, *.mk
cmake: CMakeLists.txt, *.cmake
sql: *.sql
html: *.html, *.htm
css: *.css, *.scss
asm: *.s, *.asm
zig: *.zig
lua: *.lua
rb: *.rb
php: *.php
swift: *.swift
kt: *.kt, *.kts
proto: *.proto
xml: *.xml
txt: *.txt
)";
}

void print_help(std::ostream& out) {
    out <<
R"(scry — Sub-2ms local code search engine (zero-copy mmap trigram index + SIMD fallback)

USAGE:
    scry [OPTIONS] <PATTERN> [PATH ...]
    scry [OPTIONS] -e <PATTERN> [-e <PATTERN> ...] [PATH ...]
    cat file | scry [OPTIONS] <PATTERN>
    scry -I [INITIAL_QUERY] [PATH]           Zero-lag interactive TUI picker
    scry index [OPTIONS] [PATH]              Build/rebuild zero-copy mmap trigram index
    scry watch [PATH]                        Auto-start daemon & keep repo live-indexed
    scry unwatch [PATH]                      Remove repo from watched daemon list
    scry daemon [start|stop|status]          Manage quiet resident indexer daemon
    scry info [PATH]                         Show index statistics for repository

SEARCH OPTIONS (ripgrep-compatible):
    -e, --regexp <PATTERN>     Pattern to search for (may be passed multiple times)
    -i, --ignore-case          Case-insensitive search (uses hybrid lowercased trigram table)
    -s, --case-sensitive       Case-sensitive search (overrides earlier -i / -S)
    -S, --smart-case           Case-insensitive if pattern is all lowercase
    -F, --fixed-strings        Treat pattern as literal string
    -w, --word-regexp          Match whole words only
    -x, --line-regexp          Match whole lines only
    -v, --invert-match         Select non-matching lines
    -o, --only-matching        Print only matched parts of matching lines
    -q, --quiet                Do not print anything; exit 0 on first match, 1 otherwise
    -g, --glob <GLOB>          Include/exclude files matching glob (prefix '!' to negate)
        --iglob <GLOB>         Case-insensitive glob filter
    -t, --type <TYPE>          Search only files of TYPE (see --type-list)
    -T, --type-not <TYPE>      Exclude files of TYPE
        --type-list            Print all supported language types
    -C, --context <NUM>        Show NUM lines before and after each match
    -B, --before-context <NUM> Show NUM lines before each match
    -A, --after-context <NUM>  Show NUM lines after each match
    -c, --count                Show match count per file
    -l, --files-with-matches   Print only paths with matches
        --files-without-match  Print only paths with NO matches
        --files                Print searchable files respecting .gitignore & filters
        --json                 Output matches in ripgrep-compatible JSON Lines format
        --vimgrep              Print file:line:col:text once per match
        --heading              Group matches under filename headers
        --no-heading           Print filename prefix on every match line
    -0, --null                 Print NUL byte after file paths
    -n, --line-number          Show 1-based line numbers (default)
    -N, --no-line-number       Suppress line numbers
        --column               Show 1-based column numbers
    -H, --with-filename        Print file path prefix (default)
    -h, --no-filename          Suppress file path prefix
    -m, --max-count <NUM>      Limit matches per file
        --max-results <NUM>    Early-exit Top-K total matches
        --max-filesize <SIZE>  Skip files larger than SIZE (e.g. 5M, 512K)
    -L, --follow               Follow symbolic links
    -a, --text                 Search binary files as text
    -., --hidden               Include hidden files and directories
        --no-ignore            Do not respect .gitignore / .ignore files
    -u, --unrestricted         Reduce filtering (-u: no-ignore, -uu: +hidden, -uuu: +binary)
        --color <WHEN>         Colorize output: auto, always, never
    -j, --threads <NUM>        Number of worker threads

SCRY-SPECIFIC OPTIONS:
    -I, --interactive          Launch zero-lag interactive terminal picker
        --open                 Open selected TUI match in $EDITOR / zed
        --def                  Search lexical symbol definitions (fn/struct/class/enum/macro)
        --boost-defs           Rank symbol definitions above call sites
        --fresh                Force full stat freshness overlay even without daemon
        --no-index             Bypass trigram index and use AVX-512/NEON linear scanner
        --no-daemon            Query mmap index directly without contacting daemon socket
        --stats                Print query plan, candidate count, and latency to stderr
        --local                Store index in <repo>/.scry/index.scry instead of cache dir
    -f, --foreground           Keep `scry daemon start` in foreground (default: daemonize)
    -V, --version              Print version and SIMD architecture
        --help                 Print this help message
)";
}

ParsedCli parse_cli(int argc, char** argv) {
    ParsedCli res;
    if (argc <= 1) {
        res.cmd = CliCommand::Help;
        return res;
    }

    std::string_view first = argv[1];
    int i = 1;
    if (first == "index") {
        res.cmd = CliCommand::Index;
        i = 2;
    } else if (first == "watch") {
        res.cmd = CliCommand::Watch;
        i = 2;
    } else if (first == "unwatch") {
        res.cmd = CliCommand::Unwatch;
        i = 2;
    } else if (first == "info") {
        res.cmd = CliCommand::Info;
        i = 2;
    } else if (first == "daemon") {
        i = 2;
        if (i < argc) {
            std::string_view sub = argv[i];
            if (sub == "start") { res.cmd = CliCommand::DaemonStart; ++i; }
            else if (sub == "stop") { res.cmd = CliCommand::DaemonStop; ++i; }
            else if (sub == "status") { res.cmd = CliCommand::DaemonStatus; ++i; }
            else { res.cmd = CliCommand::DaemonStart; }
        } else {
            res.cmd = CliCommand::DaemonStart;
        }
    }

    std::vector<std::string> positional;
    std::vector<std::string> explicit_patterns;
    bool end_of_flags = false;
    bool local_index = false;
    bool open_in_editor = false;
    int unrestricted_level = 0;

    auto need_arg = [&](const char* opt) -> const char* {
        if (i + 1 >= argc) {
            res.error = std::string("missing value for option: ") + opt;
            return nullptr;
        }
        return argv[++i];
    };

    for (; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (!end_of_flags && arg == "--") {
            end_of_flags = true;
            continue;
        }
        if (end_of_flags || arg.empty() || arg[0] != '-' || arg == "-") {
            positional.emplace_back(arg);
            continue;
        }

        if (arg == "--help") { res.cmd = CliCommand::Help; return res; }
        if (arg == "-V" || arg == "--version") { res.cmd = CliCommand::Version; return res; }
        if (arg == "--type-list") { res.cmd = CliCommand::TypeList; return res; }
        if (arg == "-I" || arg == "--interactive") { res.cmd = CliCommand::Interactive; continue; }
        if (arg == "--open") { open_in_editor = true; continue; }
        if (arg == "--foreground" || arg == "-f") { res.daemon_foreground = true; continue; }
        if (arg == "--local") { local_index = true; continue; }
        if (arg == "--no-uring") { res.index_opts.use_io_uring = false; continue; }

        if (arg == "-e" || arg == "--regexp") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            explicit_patterns.emplace_back(v);
            continue;
        }
        if (arg == "-i" || arg == "--ignore-case") {
            res.search_opts.regex_opts.case_insensitive = true;
            res.search_opts.regex_opts.smart_case = false;
            continue;
        }
        if (arg == "-s" || arg == "--case-sensitive") {
            res.search_opts.regex_opts.case_insensitive = false;
            res.search_opts.regex_opts.smart_case = false;
            continue;
        }
        if (arg == "-S" || arg == "--smart-case") {
            res.search_opts.regex_opts.smart_case = true;
            res.search_opts.regex_opts.case_insensitive = false;
            continue;
        }
        if (arg == "-F" || arg == "--fixed-strings") { res.search_opts.regex_opts.fixed_strings = true; continue; }
        if (arg == "-w" || arg == "--word-regexp") { res.search_opts.regex_opts.word_regexp = true; continue; }
        if (arg == "-x" || arg == "--line-regexp") { res.search_opts.regex_opts.line_regexp = true; continue; }
        if (arg == "-v" || arg == "--invert-match") { res.search_opts.invert_match = true; continue; }
        if (arg == "-o" || arg == "--only-matching") { res.search_opts.only_matching = true; continue; }
        if (arg == "-q" || arg == "--quiet") { res.search_opts.quiet = true; continue; }
        if (arg == "-c" || arg == "--count") { res.search_opts.count_only = true; continue; }
        if (arg == "-l" || arg == "--files-with-matches") { res.search_opts.files_with_matches = true; continue; }
        if (arg == "--files-without-match") { res.search_opts.files_without_match = true; continue; }
        if (arg == "--files") { res.search_opts.list_files = true; continue; }
        if (arg == "--json") { res.search_opts.json_output = true; continue; }
        if (arg == "--vimgrep") {
            res.search_opts.vimgrep = true;
            res.search_opts.show_line_numbers = true;
            res.search_opts.show_column = true;
            res.search_opts.with_filename = true;
            res.search_opts.filename_explicit = true;
            res.search_opts.heading = false;
            res.search_opts.heading_explicit = true;
            continue;
        }
        if (arg == "--heading") {
            res.search_opts.heading = true;
            res.search_opts.heading_explicit = true;
            continue;
        }
        if (arg == "--no-heading") {
            res.search_opts.heading = false;
            res.search_opts.heading_explicit = true;
            continue;
        }
        if (arg == "-0" || arg == "--null") { res.search_opts.null_sep = true; continue; }
        if (arg == "--def") { res.search_opts.def_only = true; continue; }
        if (arg == "--boost-defs") { res.search_opts.boost_defs = true; continue; }
        if (arg == "-n" || arg == "--line-number") { res.search_opts.show_line_numbers = true; continue; }
        if (arg == "-N" || arg == "--no-line-number") { res.search_opts.show_line_numbers = false; continue; }
        if (arg == "--column") { res.search_opts.show_column = true; continue; }
        if (arg == "-H" || arg == "--with-filename") {
            res.search_opts.with_filename = true;
            res.search_opts.filename_explicit = true;
            continue;
        }
        if (arg == "-h" || arg == "--no-filename") {
            res.search_opts.with_filename = false;
            res.search_opts.filename_explicit = true;
            continue;
        }
        if (arg == "-L" || arg == "--follow") {
            res.search_opts.follow_symlinks = true;
            res.index_opts.follow_symlinks = true;
            continue;
        }
        if (arg == "-a" || arg == "--text") { res.search_opts.search_binary = true; continue; }
        if (arg == "-." || arg == "--hidden") {
            res.search_opts.include_hidden = true;
            res.index_opts.include_hidden = true;
            continue;
        }
        if (arg == "--no-ignore" || arg == "--no-ignore-vcs") {
            res.search_opts.respect_gitignore = false;
            res.index_opts.respect_gitignore = false;
            continue;
        }
        if (arg == "-u" || arg == "--unrestricted") {
            unrestricted_level++;
            continue;
        }
        if (arg == "--no-index") { res.search_opts.force_no_index = true; continue; }
        if (arg == "--no-daemon") { res.search_opts.use_daemon = false; continue; }
        if (arg == "--fresh") { res.search_opts.strict_freshness = true; continue; }
        if (arg == "--stats") { res.search_opts.show_stats = true; continue; }

        if (arg == "-g" || arg == "--glob") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            res.search_opts.globs.add(v, false);
            continue;
        }
        if (arg == "--iglob") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            res.search_opts.globs.add(v, true);
            continue;
        }
        if (arg == "-t" || arg == "--type") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            auto langs = resolve_type_name(v);
            if (langs.empty()) {
                res.error = std::string("unknown file type: ") + v;
                return res;
            }
            res.search_opts.include_types.insert(res.search_opts.include_types.end(),
                                                 langs.begin(), langs.end());
            continue;
        }
        if (arg == "-T" || arg == "--type-not") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            auto langs = resolve_type_name(v);
            if (langs.empty()) {
                res.error = std::string("unknown file type: ") + v;
                return res;
            }
            res.search_opts.exclude_types.insert(res.search_opts.exclude_types.end(),
                                                 langs.begin(), langs.end());
            continue;
        }
        if (arg == "-C" || arg == "--context") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            u32 n = static_cast<u32>(std::strtoul(v, nullptr, 10));
            res.search_opts.before_context = n;
            res.search_opts.after_context = n;
            continue;
        }
        if (arg == "-B" || arg == "--before-context") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            res.search_opts.before_context = static_cast<u32>(std::strtoul(v, nullptr, 10));
            continue;
        }
        if (arg == "-A" || arg == "--after-context") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            res.search_opts.after_context = static_cast<u32>(std::strtoul(v, nullptr, 10));
            continue;
        }
        if (arg == "-m" || arg == "--max-count") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            res.search_opts.max_count_per_file = static_cast<u32>(std::strtoul(v, nullptr, 10));
            continue;
        }
        if (arg == "--max-results") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            res.search_opts.max_total_matches = static_cast<u32>(std::strtoul(v, nullptr, 10));
            continue;
        }
        if (arg == "--max-filesize" || arg.rfind("--max-filesize=", 0) == 0) {
            std::string_view val;
            if (arg.rfind("--max-filesize=", 0) == 0) val = arg.substr(15);
            else {
                const char* v = need_arg(argv[i]);
                if (!v) return res;
                val = v;
            }
            u64 bytes = 0;
            if (!parse_size_bytes(val, bytes)) {
                res.error = "invalid --max-filesize value: " + std::string(val);
                return res;
            }
            res.search_opts.max_file_size = bytes;
            res.index_opts.max_file_size = bytes;
            continue;
        }
        if (arg == "-j" || arg == "--threads") {
            const char* v = need_arg(argv[i]);
            if (!v) return res;
            u32 t = static_cast<u32>(std::strtoul(v, nullptr, 10));
            res.search_opts.threads = t;
            res.index_opts.threads = t;
            continue;
        }
        if (arg == "--color" || arg.rfind("--color=", 0) == 0) {
            std::string_view val;
            if (arg.rfind("--color=", 0) == 0) {
                val = arg.substr(8);
            } else {
                const char* v = need_arg(argv[i]);
                if (!v) return res;
                val = v;
            }
            if (val == "always") res.search_opts.color = ColorMode::Always;
            else if (val == "never") res.search_opts.color = ColorMode::Never;
            else res.search_opts.color = ColorMode::Auto;
            continue;
        }

        // Combined short flags like -iF, -nH, -uu, -uuu, -qi, -il
        if (arg.size() > 2 && arg[0] == '-' && arg[1] != '-') {
            bool ok = true;
            for (size_t k = 1; k < arg.size(); ++k) {
                switch (arg[k]) {
                    case 'i':
                        res.search_opts.regex_opts.case_insensitive = true;
                        res.search_opts.regex_opts.smart_case = false;
                        break;
                    case 's':
                        res.search_opts.regex_opts.case_insensitive = false;
                        res.search_opts.regex_opts.smart_case = false;
                        break;
                    case 'S':
                        res.search_opts.regex_opts.smart_case = true;
                        res.search_opts.regex_opts.case_insensitive = false;
                        break;
                    case 'F': res.search_opts.regex_opts.fixed_strings = true; break;
                    case 'w': res.search_opts.regex_opts.word_regexp = true; break;
                    case 'x': res.search_opts.regex_opts.line_regexp = true; break;
                    case 'v': res.search_opts.invert_match = true; break;
                    case 'o': res.search_opts.only_matching = true; break;
                    case 'q': res.search_opts.quiet = true; break;
                    case 'c': res.search_opts.count_only = true; break;
                    case 'l': res.search_opts.files_with_matches = true; break;
                    case 'n': res.search_opts.show_line_numbers = true; break;
                    case 'N': res.search_opts.show_line_numbers = false; break;
                    case 'H':
                        res.search_opts.with_filename = true;
                        res.search_opts.filename_explicit = true;
                        break;
                    case 'h':
                        res.search_opts.with_filename = false;
                        res.search_opts.filename_explicit = true;
                        break;
                    case '0': res.search_opts.null_sep = true; break;
                    case 'L':
                        res.search_opts.follow_symlinks = true;
                        res.index_opts.follow_symlinks = true;
                        break;
                    case 'a': res.search_opts.search_binary = true; break;
                    case 'u': unrestricted_level++; break;
                    case 'I': res.cmd = CliCommand::Interactive; break;
                    case '.':
                        res.search_opts.include_hidden = true;
                        res.index_opts.include_hidden = true;
                        break;
                    default: ok = false; break;
                }
            }
            if (ok) continue;
        }

        res.error = "unknown option: " + std::string(arg);
        return res;
    }

    // Apply -u / -uu / -uuu
    if (unrestricted_level >= 1) {
        res.search_opts.respect_gitignore = false;
        res.index_opts.respect_gitignore = false;
    }
    if (unrestricted_level >= 2) {
        res.search_opts.include_hidden = true;
        res.index_opts.include_hidden = true;
    }
    if (unrestricted_level >= 3) {
        res.search_opts.search_binary = true;
    }

    // -q early exits on first match!
    if (res.search_opts.quiet && res.search_opts.max_total_matches == 0) {
        res.search_opts.max_total_matches = 1;
    }

    if (res.cmd == CliCommand::Index || res.cmd == CliCommand::Watch ||
        res.cmd == CliCommand::Unwatch || res.cmd == CliCommand::Info ||
        res.cmd == CliCommand::DaemonStart || res.cmd == CliCommand::DaemonStop ||
        res.cmd == CliCommand::DaemonStatus) {
        if (!positional.empty()) {
            res.cmd_path = positional[0];
            res.has_explicit_cmd_path = true;
        }
        if (local_index && res.cmd == CliCommand::Index) {
            std::error_code ec;
            auto canon = std::filesystem::weakly_canonical(res.cmd_path, ec);
            res.index_opts.output_path = (ec ? res.cmd_path : canon) / ".scry" / "index.scry";
        }
        return res;
    }

    // Combine explicit -e patterns if provided
    auto combine_patterns = [&](const std::vector<std::string>& pats) {
        if (pats.size() == 1) {
            res.search_opts.pattern = pats[0];
            return;
        }
        std::string combined;
        for (size_t pi = 0; pi < pats.size(); ++pi) {
            if (pi > 0) combined.push_back('|');
            if (res.search_opts.regex_opts.fixed_strings) {
                combined += "(?:" + regex_escape_literal(pats[pi]) + ")";
            } else {
                combined += "(?:" + pats[pi] + ")";
            }
        }
        res.search_opts.pattern = std::move(combined);
        res.search_opts.regex_opts.fixed_strings = false;
    };

    if (res.cmd == CliCommand::Interactive) {
        if (!explicit_patterns.empty()) {
            combine_patterns(explicit_patterns);
            if (!positional.empty()) res.search_opts.target_path = positional[0];
        } else if (positional.size() >= 2) {
            res.search_opts.pattern = positional[0];
            res.search_opts.target_path = positional[1];
        } else if (positional.size() == 1) {
            std::error_code ec;
            if (std::filesystem::exists(positional[0], ec)) {
                res.search_opts.target_path = positional[0];
            } else {
                res.search_opts.pattern = positional[0];
            }
        }
        res.tui_opts.base_opts = res.search_opts;
        res.tui_opts.open_in_editor = open_in_editor;
        return res;
    }

    // --files mode
    if (res.search_opts.list_files) {
        if (!positional.empty()) {
            res.search_opts.target_path = positional[0];
            for (const auto& p : positional) res.search_opts.target_paths.emplace_back(p);
        }
        return res;
    }

    // Standard Search command
    size_t path_start = 0;
    if (!explicit_patterns.empty()) {
        combine_patterns(explicit_patterns);
        path_start = 0;
    } else {
        if (positional.empty()) {
            res.error = "missing search pattern (try `scry --help`)";
            return res;
        }
        res.search_opts.pattern = positional[0];
        path_start = 1;
    }

    if (path_start >= positional.size()) {
        // No path argument supplied: auto-detect piped/redirected stdin (matches rg!)
        if (stdin_is_piped_or_file()) {
            res.search_opts.search_stdin = true;
            if (!res.search_opts.filename_explicit) {
                res.search_opts.with_filename = false;
            }
        }
    } else {
        for (size_t k = path_start; k < positional.size(); ++k) {
            if (positional[k] == "-") {
                res.search_opts.search_stdin = true;
                if (!res.search_opts.filename_explicit && positional.size() - path_start == 1) {
                    res.search_opts.with_filename = false;
                }
            } else {
                res.search_opts.target_paths.emplace_back(positional[k]);
            }
        }
        if (!res.search_opts.target_paths.empty()) {
            res.search_opts.target_path = res.search_opts.target_paths[0];
            // Match rg: single file target omits filename prefix unless -H was explicitly passed
            std::error_code ec;
            if (!res.search_opts.filename_explicit &&
                res.search_opts.target_paths.size() == 1 &&
                !res.search_opts.search_stdin &&
                std::filesystem::is_regular_file(res.search_opts.target_path, ec)) {
                res.search_opts.with_filename = false;
            }
        }
    }

    return res;
}

} // namespace scry
