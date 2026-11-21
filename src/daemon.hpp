#pragma once

#include "common.hpp"
#include "delta.hpp"
#include "searcher.hpp"

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace scry {

struct DaemonRepoStatus {
    std::string root;
    u32 base_docs = 0;
    u32 delta_docs = 0;
    u32 tombstones = 0;
    u64 generation = 0;
};

struct DaemonStatus {
    bool running = false;
    u64 private_dirty_kib = 0;
    u64 rss_kib = 0;
    u64 queries_served = 0;
    std::vector<DaemonRepoStatus> repos;
};

// Quiet resident indexer daemon (Linux inotify / macOS FSEvents + Unix domain socket IPC)
class ScryDaemon {
public:
    explicit ScryDaemon(std::filesystem::path sock_path = daemon_socket_path());
    ~ScryDaemon();

    // Register/unregister a repository to keep indexed and live-watched
    bool watch_repo(const std::filesystem::path& repo_root, std::string& err);
    bool unwatch_repo(const std::filesystem::path& repo_root);

    // Run event loop (blocks until stop() or SIGINT/SIGTERM)
    int run();
    void stop() noexcept;

private:
    struct WatchedRepo;
    void watcher_loop();
    void ipc_loop();
    void handle_client(int client_fd);
    std::string handle_request_line(const std::string& req);

    std::filesystem::path sock_path_;
    int listen_fd_ = -1;
    int inotify_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::atomic<u64> queries_served_{0};

    std::mutex repos_mu_;
    std::vector<std::unique_ptr<WatchedRepo>> repos_;
};

// Persisted watched repository list (~/.config/scry/repos.txt)
std::vector<std::filesystem::path> load_persisted_repos();
bool persist_watched_repo(const std::filesystem::path& repo_root);
bool unpersist_watched_repo(const std::filesystem::path& repo_root);

// Lightweight Unix domain socket client helpers
bool daemon_ping(const std::filesystem::path& sock_path = daemon_socket_path());
bool daemon_watch(const std::filesystem::path& repo_root, std::string& reply,
                  const std::filesystem::path& sock_path = daemon_socket_path());
bool daemon_unwatch(const std::filesystem::path& repo_root, std::string& reply,
                    const std::filesystem::path& sock_path = daemon_socket_path());
bool daemon_stop(const std::filesystem::path& sock_path = daemon_socket_path());
bool daemon_status(std::string& out_text,
                   const std::filesystem::path& sock_path = daemon_socket_path());
bool daemon_query(const SearchOptions& opts, SearchResponse& resp,
                  const std::filesystem::path& sock_path = daemon_socket_path());

} // namespace scry
