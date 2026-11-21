#include "cli.hpp"
#include "daemon.hpp"
#include "indexer.hpp"
#include "searcher.hpp"
#include "simd.hpp"
#include "tui.hpp"

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace {

int cmd_index(const scry::ParsedCli& cli) {
    scry::IndexBuildStats stats;
    std::string err;
    if (!scry::build_index(cli.cmd_path, cli.index_opts, stats, err)) {
        std::cerr << "scry index: " << err << "\n";
        return 2;
    }

    double corpus_mb = static_cast<double>(stats.total_corpus_bytes) / (1024.0 * 1024.0);
    double index_mb = static_cast<double>(stats.index_file_bytes) / (1024.0 * 1024.0);
    double ratio = stats.total_corpus_bytes > 0
        ? static_cast<double>(stats.index_file_bytes) / static_cast<double>(stats.total_corpus_bytes)
        : 0.0;

    std::printf(
        "Indexed %u files (%.2f MiB, %u binary skipped) in %.2f ms (%.1f MiB/s)\n"
        "  index:    %s (%.2f MiB, %.2fx raw corpus)\n"
        "  tables:   %u exact trigrams, %u hybrid-lower trigrams, %u symbols\n"
        "  timings:  walk=%.1f ms, read+extract=%.1f ms (%s), merge+encode=%.1f ms, write=%.1f ms\n",
        stats.files_indexed, corpus_mb, stats.files_skipped_binary,
        stats.total_ms, stats.throughput_mb_s,
        stats.index_path.c_str(), index_mb, ratio,
        stats.num_exact_trigrams, stats.num_lower_trigrams, stats.num_symbols,
        stats.walk_ms, stats.read_and_extract_ms,
        stats.used_io_uring ? "io_uring" : "pread",
        stats.merge_and_encode_ms, stats.write_ms);
    return 0;
}

int cmd_info(const scry::ParsedCli& cli) {
    std::filesystem::path root, idx_path;
    if (!scry::locate_index_for_path(cli.cmd_path, root, idx_path)) {
        std::cerr << "scry info: no index found for " << cli.cmd_path.string() << "\n";
        return 1;
    }
    scry::MmapIndex idx;
    if (!idx.open(idx_path)) {
        std::cerr << "scry info: failed to open index " << idx_path.string() << "\n";
        return 2;
    }
    const auto& h = idx.header();
    double corpus_mb = static_cast<double>(h.total_corpus_bytes) / (1024.0 * 1024.0);
    double index_mb = static_cast<double>(idx.index_file_bytes()) / (1024.0 * 1024.0);
    double ratio = h.total_corpus_bytes > 0
        ? static_cast<double>(idx.index_file_bytes()) / static_cast<double>(h.total_corpus_bytes)
        : 0.0;

    std::printf(
        "Repository:      %s\n"
        "Index file:      %s\n"
        "Format version:  %u (SIMD=%s)\n"
        "Documents:       %u (%u virtual 32KiB chunks)\n"
        "Corpus size:     %.2f MiB\n"
        "Index size:      %.2f MiB (%.2fx raw)\n"
        "Exact trigrams:  %u\n"
        "Lower trigrams:  %u\n"
        "Path trigrams:   %u\n"
        "Symbols:         %u\n",
        root.c_str(), idx_path.c_str(), h.version, scry::simd::arch_name(),
        h.num_docs, h.num_chunks, corpus_mb, index_mb, ratio,
        h.num_exact_trigrams, h.num_lower_trigrams, h.num_path_trigrams, h.num_symbols);
    return 0;
}

int run_daemon_process(const std::vector<std::filesystem::path>& initial_repos) {
    scry::ScryDaemon daemon;
    for (const auto& r : initial_repos) {
        std::string err;
        if (!daemon.watch_repo(r, err)) {
            std::fprintf(stderr, "scryd: skipping %s (%s)\n", r.c_str(), err.c_str());
        }
    }
    return daemon.run();
}

int spawn_background_daemon(const std::vector<std::filesystem::path>& initial_repos) {
    pid_t pid = ::fork();
    if (pid < 0) {
        std::cerr << "scry daemon: fork() failed\n";
        return 2;
    }
    if (pid == 0) {
        // Child daemon process: detach session & redirect stdio to /dev/null
        ::setsid();
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        int rc = run_daemon_process(initial_repos);
        std::_Exit(rc);
    }

    // Parent: wait until Unix socket answers PING (up to 15s for initial index build)
    for (int i = 0; i < 300; ++i) {
        if (scry::daemon_ping()) {
            std::cout << "scryd started (pid=" << pid << ", socket="
                      << scry::daemon_socket_path().string() << ")\n";
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cerr << "scry daemon: timed out waiting for scryd socket\n";
    return 1;
}

int cmd_search(const scry::ParsedCli& cli) {
    scry::SearchResponse resp;
    bool handled_by_daemon = false;

    const auto& o = cli.search_opts;
    bool daemon_eligible =
        o.use_daemon && !o.force_no_index && !o.include_hidden && o.respect_gitignore &&
        !o.strict_freshness && !o.search_stdin && !o.search_binary && !o.follow_symlinks &&
        !o.files_without_match && o.target_paths.size() <= 1 &&
        o.max_file_size >= 32ULL * 1024 * 1024;

    if (daemon_eligible) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(o.target_path, ec)) {
            if (scry::daemon_query(o, resp)) {
                handled_by_daemon = true;
            }
        }
    }

    if (!handled_by_daemon) {
        resp = scry::run_search(o);
    }

    if (!resp.error.empty()) {
        std::cerr << "scry: " << resp.error << "\n";
        return 2;
    }

    bool is_tty = (::isatty(STDOUT_FILENO) == 1);
    scry::format_search_response(resp, o, is_tty, std::cout);

    if (o.show_stats) {
        std::fprintf(stderr,
                     "[scry stats] engine=%s%s plan=%s candidates=%u/%u verified=%u "
                     "matches=%llu in %u files | plan=%.3fms verify=%.3fms total=%.3fms\n",
                     resp.stats.used_daemon ? "daemon+" : "",
                     resp.stats.used_index
                         ? (resp.stats.used_linear_fallback ? "index-scan" : "index")
                         : "simd-linear",
                     resp.stats.trigram_plan_desc.empty() ? "-" : resp.stats.trigram_plan_desc.c_str(),
                     resp.stats.candidate_docs, resp.stats.total_indexed_docs,
                     resp.stats.verified_docs,
                     static_cast<unsigned long long>(resp.stats.matched_lines),
                     resp.stats.matched_files,
                     resp.stats.plan_ms, resp.stats.verify_ms, resp.stats.total_ms);
    }

    if (o.list_files) {
        return resp.listed_files.empty() ? 1 : 0;
    }
    return resp.files.empty() ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    auto cli = scry::parse_cli(argc, argv);
    if (!cli.error.empty()) {
        std::cerr << "scry: " << cli.error << "\n";
        return 2;
    }

    switch (cli.cmd) {
        case scry::CliCommand::Help:
            scry::print_help(std::cout);
            return 0;
        case scry::CliCommand::Version:
            scry::print_version(std::cout);
            return 0;
        case scry::CliCommand::TypeList:
            scry::print_type_list(std::cout);
            return 0;
        case scry::CliCommand::Index:
            return cmd_index(cli);
        case scry::CliCommand::Info:
            return cmd_info(cli);
        case scry::CliCommand::Interactive:
            return scry::run_interactive_tui(cli.tui_opts);
        case scry::CliCommand::Search:
            return cmd_search(cli);
        case scry::CliCommand::DaemonStop:
            if (scry::daemon_stop()) {
                std::cout << "scryd stopped\n";
                return 0;
            }
            std::cerr << "scry daemon stop: daemon not running\n";
            return 1;
        case scry::CliCommand::DaemonStatus: {
            std::string out;
            if (scry::daemon_status(out)) {
                std::cout << out;
                return 0;
            }
            std::cout << "scryd not running\n";
            return 1;
        }
        case scry::CliCommand::DaemonStart: {
            if (scry::daemon_ping()) {
                std::cout << "scryd already running at " << scry::daemon_socket_path().string() << "\n";
                return 0;
            }
            auto repos = scry::load_persisted_repos();
            if (cli.has_explicit_cmd_path || repos.empty()) {
                std::error_code ec;
                auto canon = std::filesystem::weakly_canonical(cli.cmd_path, ec);
                auto target = ec ? cli.cmd_path : canon;
                if (std::find(repos.begin(), repos.end(), target) == repos.end()) {
                    repos.push_back(target);
                }
            }
            if (cli.daemon_foreground) {
                std::cerr << "scryd listening on " << scry::daemon_socket_path().string()
                          << " (" << repos.size() << " repo(s))\n";
                return run_daemon_process(repos);
            }
            return spawn_background_daemon(repos);
        }
        case scry::CliCommand::Watch: {
            scry::persist_watched_repo(cli.cmd_path);
            std::string reply;
            if (scry::daemon_watch(cli.cmd_path, reply)) {
                std::cout << reply << "\n";
                return 0;
            }
            auto repos = scry::load_persisted_repos();
            if (cli.daemon_foreground) {
                std::cerr << "scry watch: listening on " << scry::daemon_socket_path().string()
                          << " (" << repos.size() << " repo(s))\n";
                return run_daemon_process(repos);
            }
            return spawn_background_daemon(repos);
        }
        case scry::CliCommand::Unwatch: {
            bool removed_cfg = scry::unpersist_watched_repo(cli.cmd_path);
            std::string reply;
            bool removed_live = scry::daemon_unwatch(cli.cmd_path, reply);
            if (removed_live) {
                std::cout << reply << "\n";
                return 0;
            }
            if (removed_cfg) {
                std::cout << "OK removed from persisted watch list: " << cli.cmd_path.string() << "\n";
                return 0;
            }
            std::cerr << "scry unwatch: repository not watched: " << cli.cmd_path.string() << "\n";
            return 1;
        }
    }
    return 0;
}
