#include "daemon.hpp"
#include "indexer.hpp"

#include <cerrno>
#include <csignal>
#include <dirent.h>
#include <fstream>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unordered_map>

#if defined(__linux__)
  #include <sys/inotify.h>
#elif defined(__APPLE__)
  #include <CoreServices/CoreServices.h>
#endif

namespace scry {

namespace {

std::atomic<ScryDaemon*> g_active_daemon{nullptr};

void daemon_sig_handler(int) {
    if (auto* d = g_active_daemon.load(std::memory_order_relaxed)) {
        d->stop();
    }
}

bool send_all(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const u8*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* buf, size_t len) {
    auto* p = static_cast<u8*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool send_frame(int fd, const std::string& payload) {
    u32 sz = static_cast<u32>(payload.size());
    return send_all(fd, &sz, sizeof(sz)) && (sz == 0 || send_all(fd, payload.data(), sz));
}

bool recv_frame(int fd, std::string& out) {
    u32 sz = 0;
    if (!recv_all(fd, &sz, sizeof(sz))) return false;
    if (sz > 64u * 1024u * 1024u) return false; // 64 MiB frame cap
    out.resize(sz);
    return sz == 0 || recv_all(fd, out.data(), sz);
}

int open_unix_socket() {
#if defined(SOCK_CLOEXEC)
    return ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

int accept_unix_client(int listen_fd) {
#if defined(__linux__) && defined(SOCK_CLOEXEC)
    return ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
#else
    int cfd = ::accept(listen_fd, nullptr, nullptr);
    if (cfd >= 0) ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
    return cfd;
#endif
}

int connect_unix(const std::filesystem::path& sock_path) {
    int fd = open_unix_socket();
    if (fd < 0) return -1;

    struct timeval tv {};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::string sp = sock_path.string();
    if (sp.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::memcpy(addr.sun_path, sp.c_str(), sp.size() + 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Compact binary serialization helpers for SearchOptions / SearchResponse over AF_UNIX
struct BinWriter {
    std::string buf;
    void put_u8(u8 v) { buf.push_back(static_cast<char>(v)); }
    void put_u16(u16 v) { buf.append(reinterpret_cast<const char*>(&v), 2); }
    void put_u32(u32 v) { buf.append(reinterpret_cast<const char*>(&v), 4); }
    void put_u64(u64 v) { buf.append(reinterpret_cast<const char*>(&v), 8); }
    void put_f64(double v) { buf.append(reinterpret_cast<const char*>(&v), 8); }
    void put_str(std::string_view s) {
        put_u32(static_cast<u32>(s.size()));
        buf.append(s.data(), s.size());
    }
};

struct BinReader {
    const u8* p = nullptr;
    size_t rem = 0;
    BinReader(const std::string& s)
        : p(reinterpret_cast<const u8*>(s.data())), rem(s.size()) {}

    bool get_u8(u8& v) { if (rem < 1) return false; v = *p++; --rem; return true; }
    bool get_u16(u16& v) { if (rem < 2) return false; std::memcpy(&v, p, 2); p += 2; rem -= 2; return true; }
    bool get_u32(u32& v) { if (rem < 4) return false; std::memcpy(&v, p, 4); p += 4; rem -= 4; return true; }
    bool get_u64(u64& v) { if (rem < 8) return false; std::memcpy(&v, p, 8); p += 8; rem -= 8; return true; }
    bool get_f64(double& v) { if (rem < 8) return false; std::memcpy(&v, p, 8); p += 8; rem -= 8; return true; }
    bool get_str(std::string& s) {
        u32 n = 0;
        if (!get_u32(n) || rem < n) return false;
        s.assign(reinterpret_cast<const char*>(p), n);
        p += n;
        rem -= n;
        return true;
    }
};

std::string serialize_query_req(const SearchOptions& opts) {
    BinWriter w;
    w.put_u8('Q');
    w.put_str(opts.pattern);
    std::error_code ec;
    auto abs_target = std::filesystem::weakly_canonical(opts.target_path, ec);
    w.put_str(ec ? opts.target_path.string() : abs_target.string());

    u32 flags = 0;
    if (opts.regex_opts.case_insensitive) flags |= (1u << 0);
    if (opts.regex_opts.smart_case)       flags |= (1u << 1);
    if (opts.regex_opts.fixed_strings)    flags |= (1u << 2);
    if (opts.regex_opts.word_regexp)      flags |= (1u << 3);
    if (opts.regex_opts.line_regexp)      flags |= (1u << 4);
    if (opts.count_only)                  flags |= (1u << 5);
    if (opts.files_with_matches)          flags |= (1u << 6);
    if (opts.list_files)                  flags |= (1u << 7);
    if (opts.invert_match)                flags |= (1u << 8);
    if (opts.def_only)                    flags |= (1u << 9);
    if (opts.boost_defs)                  flags |= (1u << 10);
    w.put_u32(flags);

    w.put_u32(opts.before_context);
    w.put_u32(opts.after_context);
    w.put_u32(opts.max_count_per_file);
    w.put_u32(opts.max_total_matches);

    w.put_u32(static_cast<u32>(opts.globs.rules.size()));
    for (const auto& r : opts.globs.rules) {
        std::string raw = (r.negated ? "!" : "") + r.pattern;
        w.put_str(raw);
        w.put_u8(r.case_insensitive ? 1 : 0);
    }
    w.put_u32(static_cast<u32>(opts.include_types.size()));
    for (auto t : opts.include_types) w.put_u16(static_cast<u16>(t));
    w.put_u32(static_cast<u32>(opts.exclude_types.size()));
    for (auto t : opts.exclude_types) w.put_u16(static_cast<u16>(t));

    return w.buf;
}

bool deserialize_query_req(const std::string& payload, SearchOptions& opts) {
    BinReader r(payload);
    u8 tag = 0;
    if (!r.get_u8(tag) || tag != 'Q') return false;
    std::string target;
    if (!r.get_str(opts.pattern) || !r.get_str(target)) return false;
    opts.target_path = target;

    u32 flags = 0;
    if (!r.get_u32(flags)) return false;
    opts.regex_opts.case_insensitive = (flags & (1u << 0)) != 0;
    opts.regex_opts.smart_case       = (flags & (1u << 1)) != 0;
    opts.regex_opts.fixed_strings    = (flags & (1u << 2)) != 0;
    opts.regex_opts.word_regexp      = (flags & (1u << 3)) != 0;
    opts.regex_opts.line_regexp      = (flags & (1u << 4)) != 0;
    opts.count_only                  = (flags & (1u << 5)) != 0;
    opts.files_with_matches          = (flags & (1u << 6)) != 0;
    opts.list_files                  = (flags & (1u << 7)) != 0;
    opts.invert_match                = (flags & (1u << 8)) != 0;
    opts.def_only                    = (flags & (1u << 9)) != 0;
    opts.boost_defs                  = (flags & (1u << 10)) != 0;

    if (!r.get_u32(opts.before_context) || !r.get_u32(opts.after_context) ||
        !r.get_u32(opts.max_count_per_file) || !r.get_u32(opts.max_total_matches)) {
        return false;
    }

    u32 ng = 0;
    if (!r.get_u32(ng)) return false;
    for (u32 i = 0; i < ng; ++i) {
        std::string g;
        u8 ic = 0;
        if (!r.get_str(g) || !r.get_u8(ic)) return false;
        opts.globs.add(g, ic != 0);
    }
    u32 ni = 0, ne = 0;
    if (!r.get_u32(ni)) return false;
    for (u32 i = 0; i < ni; ++i) {
        u16 v = 0;
        if (!r.get_u16(v)) return false;
        opts.include_types.push_back(static_cast<LangId>(v));
    }
    if (!r.get_u32(ne)) return false;
    for (u32 i = 0; i < ne; ++i) {
        u16 v = 0;
        if (!r.get_u16(v)) return false;
        opts.exclude_types.push_back(static_cast<LangId>(v));
    }
    return true;
}

std::string serialize_search_resp(const SearchResponse& resp, bool found_repo) {
    BinWriter w;
    w.put_u8(found_repo ? 'R' : 'M'); // R = result, M = repo not watched (miss)
    if (!found_repo) return w.buf;

    w.put_str(resp.error);
    w.put_u8(resp.stats.used_index ? 1 : 0);
    w.put_u8(resp.stats.used_linear_fallback ? 1 : 0);
    w.put_u32(resp.stats.total_indexed_docs);
    w.put_u32(resp.stats.candidate_docs);
    w.put_u32(resp.stats.verified_docs);
    w.put_u32(resp.stats.matched_files);
    w.put_u64(resp.stats.matched_lines);
    w.put_f64(resp.stats.plan_ms);
    w.put_f64(resp.stats.verify_ms);
    w.put_f64(resp.stats.total_ms);
    w.put_str(resp.stats.trigram_plan_desc);

    w.put_u32(static_cast<u32>(resp.listed_files.size()));
    for (const auto& lf : resp.listed_files) w.put_str(lf);

    w.put_u32(static_cast<u32>(resp.files.size()));
    for (const auto& fr : resp.files) {
        w.put_str(fr.rel_path);
        w.put_u32(fr.match_count);
        w.put_u8(fr.has_def ? 1 : 0);
        w.put_u32(static_cast<u32>(fr.matches.size()));
        for (const auto& m : fr.matches) {
            w.put_u32(m.line_no);
            w.put_str(m.line);
            w.put_u8(m.is_definition ? 1 : 0);
            w.put_u8(static_cast<u8>(m.def_kind));
            w.put_u32(static_cast<u32>(m.spans.size()));
            for (const auto& sp : m.spans) {
                w.put_u32(sp.start);
                w.put_u32(sp.end);
            }
            w.put_u32(static_cast<u32>(m.before.size()));
            for (const auto& b : m.before) {
                w.put_u32(b.line_no);
                w.put_str(b.text);
            }
            w.put_u32(static_cast<u32>(m.after.size()));
            for (const auto& a : m.after) {
                w.put_u32(a.line_no);
                w.put_str(a.text);
            }
        }
    }
    return w.buf;
}

bool deserialize_search_resp(const std::string& payload, SearchResponse& resp, bool& found_repo) {
    BinReader r(payload);
    u8 tag = 0;
    if (!r.get_u8(tag)) return false;
    if (tag == 'M') {
        found_repo = false;
        return true;
    }
    if (tag != 'R') return false;
    found_repo = true;

    u8 ui = 0, ul = 0;
    if (!r.get_str(resp.error) || !r.get_u8(ui) || !r.get_u8(ul) ||
        !r.get_u32(resp.stats.total_indexed_docs) ||
        !r.get_u32(resp.stats.candidate_docs) ||
        !r.get_u32(resp.stats.verified_docs) ||
        !r.get_u32(resp.stats.matched_files) ||
        !r.get_u64(resp.stats.matched_lines) ||
        !r.get_f64(resp.stats.plan_ms) ||
        !r.get_f64(resp.stats.verify_ms) ||
        !r.get_f64(resp.stats.total_ms) ||
        !r.get_str(resp.stats.trigram_plan_desc)) {
        return false;
    }
    resp.stats.used_index = (ui != 0);
    resp.stats.used_linear_fallback = (ul != 0);
    resp.stats.used_daemon = true;

    u32 nlf = 0;
    if (!r.get_u32(nlf)) return false;
    resp.listed_files.resize(nlf);
    for (u32 i = 0; i < nlf; ++i) {
        if (!r.get_str(resp.listed_files[i])) return false;
    }

    u32 nf = 0;
    if (!r.get_u32(nf)) return false;
    resp.files.resize(nf);
    for (u32 i = 0; i < nf; ++i) {
        auto& fr = resp.files[i];
        u8 hd = 0;
        u32 nm = 0;
        if (!r.get_str(fr.rel_path) || !r.get_u32(fr.match_count) ||
            !r.get_u8(hd) || !r.get_u32(nm)) {
            return false;
        }
        fr.has_def = (hd != 0);
        fr.matches.resize(nm);
        for (u32 j = 0; j < nm; ++j) {
            auto& m = fr.matches[j];
            u8 isd = 0, dk = 0;
            u32 ns = 0, nb = 0, na = 0;
            if (!r.get_u32(m.line_no) || !r.get_str(m.line) ||
                !r.get_u8(isd) || !r.get_u8(dk) || !r.get_u32(ns)) {
                return false;
            }
            m.is_definition = (isd != 0);
            m.def_kind = static_cast<SymbolKind>(dk);
            m.spans.resize(ns);
            for (u32 k = 0; k < ns; ++k) {
                if (!r.get_u32(m.spans[k].start) || !r.get_u32(m.spans[k].end)) return false;
            }
            if (!r.get_u32(nb)) return false;
            m.before.resize(nb);
            for (u32 k = 0; k < nb; ++k) {
                if (!r.get_u32(m.before[k].line_no) || !r.get_str(m.before[k].text)) return false;
            }
            if (!r.get_u32(na)) return false;
            m.after.resize(na);
            for (u32 k = 0; k < na; ++k) {
                if (!r.get_u32(m.after[k].line_no) || !r.get_str(m.after[k].text)) return false;
            }
        }
    }
    return true;
}

} // namespace

struct ScryDaemon::WatchedRepo {
    std::filesystem::path root;
    std::filesystem::path index_path;
    LiveIndex live;
    GitignoreMatcher gi;
    std::unordered_map<int, std::string> wd_to_rel_dir;
    std::vector<std::string> pending_paths;
    std::chrono::steady_clock::time_point last_event_tp;
};

ScryDaemon::ScryDaemon(std::filesystem::path sock_path)
    : sock_path_(std::move(sock_path)) {
#if defined(__linux__)
    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
}

ScryDaemon::~ScryDaemon() {
    stop();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (inotify_fd_ >= 0) {
        ::close(inotify_fd_);
        inotify_fd_ = -1;
    }
    std::error_code ec;
    std::filesystem::remove(sock_path_, ec);
}

void ScryDaemon::stop() noexcept {
    stop_.store(true, std::memory_order_release);
}

namespace {

#if defined(__linux__)
void add_inotify_watches_rec(int ifd,
                             const std::filesystem::path& root,
                             const std::string& rel_dir,
                             const GitignoreMatcher& gi,
                             std::unordered_map<int, std::string>& wd_map,
                             bool& limit_warned) {
    std::filesystem::path abs = rel_dir.empty() ? root : (root / rel_dir);
    constexpr u32 kMask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM |
                          IN_CREATE | IN_DELETE | IN_DELETE_SELF;
    int wd = ::inotify_add_watch(ifd, abs.c_str(), kMask);
    if (wd >= 0) {
        wd_map[wd] = rel_dir;
    } else if ((errno == ENOSPC || errno == EMFILE) && !limit_warned) {
        limit_warned = true;
        std::fprintf(stderr,
                     "scryd: warning: inotify watch limit reached (%s) while watching %s; "
                     "increase fs.inotify.max_user_watches for full live coverage\n",
                     std::strerror(errno), root.c_str());
        return;
    }

    DIR* dir = ::opendir(abs.c_str());
    if (!dir) return;
    struct dirent* ent = nullptr;
    while ((ent = ::readdir(dir)) != nullptr) {
        const char* name = ent->d_name;
        if (name[0] == '.') continue;
        std::string child_rel = rel_dir.empty() ? std::string(name) : (rel_dir + "/" + name);
        std::string child_abs = abs.string() + "/" + name;
        struct stat st {};
        if (::lstat(child_abs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (gi.is_ignored(child_rel, true)) continue;
        add_inotify_watches_rec(ifd, root, child_rel, gi, wd_map, limit_warned);
        if (limit_warned) break;
    }
    ::closedir(dir);
}
#endif

} // namespace

std::vector<std::filesystem::path> load_persisted_repos() {
    std::vector<std::filesystem::path> out;
    std::ifstream in(daemon_repos_config_path());
    if (!in) return out;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') continue;
        out.emplace_back(line);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool persist_watched_repo(const std::filesystem::path& repo_root) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(repo_root, ec);
    auto target = ec ? repo_root : canon;
    auto list = load_persisted_repos();
    if (std::find(list.begin(), list.end(), target) != list.end()) return true;
    list.push_back(target);
    auto cfg = daemon_repos_config_path();
    std::filesystem::create_directories(cfg.parent_path(), ec);
    std::ofstream out(cfg, std::ios::trunc);
    if (!out) return false;
    for (const auto& p : list) out << p.string() << "\n";
    return true;
}

bool unpersist_watched_repo(const std::filesystem::path& repo_root) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(repo_root, ec);
    auto target = ec ? repo_root : canon;
    auto list = load_persisted_repos();
    auto prev_sz = list.size();
    list.erase(std::remove(list.begin(), list.end(), target), list.end());
    if (list.size() == prev_sz) return false;
    auto cfg = daemon_repos_config_path();
    std::filesystem::create_directories(cfg.parent_path(), ec);
    std::ofstream out(cfg, std::ios::trunc);
    if (!out) return false;
    for (const auto& p : list) out << p.string() << "\n";
    return true;
}

bool ScryDaemon::unwatch_repo(const std::filesystem::path& repo_root) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(repo_root, ec);
    auto target = ec ? repo_root : canon;
    std::lock_guard<std::mutex> lk(repos_mu_);
    for (auto it = repos_.begin(); it != repos_.end(); ++it) {
        if ((*it)->root == target) {
#if defined(__linux__)
            if (inotify_fd_ >= 0) {
                for (const auto& [wd, _] : (*it)->wd_to_rel_dir) {
                    ::inotify_rm_watch(inotify_fd_, wd);
                }
            }
#endif
            repos_.erase(it);
            return true;
        }
    }
    return false;
}

bool ScryDaemon::watch_repo(const std::filesystem::path& repo_root, std::string& err) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(repo_root, ec);
    if (ec || !std::filesystem::is_directory(canon, ec)) {
        err = "Not a directory: " + repo_root.string();
        return false;
    }

    // Check if already watched
    {
        std::lock_guard<std::mutex> lk(repos_mu_);
        for (const auto& r : repos_) {
            if (r->root == canon) return true;
        }
    }

    // Build index if not present
    std::filesystem::path idx_root, idx_path;
    if (!locate_index_for_path(canon, idx_root, idx_path) || idx_root != canon) {
        IndexBuildOptions bopts;
        IndexBuildStats bstats;
        if (!build_index(canon, bopts, bstats, err)) {
            return false;
        }
        idx_path = bstats.index_path;
    }

    auto wr = std::make_unique<WatchedRepo>();
    wr->root = canon;
    wr->index_path = idx_path;
    wr->gi.load_root_defaults(canon);
    if (!wr->live.load(canon, idx_path)) {
        err = "Failed to load index: " + idx_path.string();
        return false;
    }

#if defined(__linux__)
    if (inotify_fd_ >= 0) {
        bool limit_warned = false;
        add_inotify_watches_rec(inotify_fd_, canon, "", wr->gi, wr->wd_to_rel_dir, limit_warned);
    }
#endif

    std::lock_guard<std::mutex> lk(repos_mu_);
    repos_.push_back(std::move(wr));
    return true;
}

void ScryDaemon::watcher_loop() {
#if defined(__linux__)
    alignas(struct inotify_event) char buf[16384];

    while (!stop_.load(std::memory_order_acquire)) {
        bool any_pending = false;
        {
            std::lock_guard<std::mutex> lk(repos_mu_);
            for (const auto& r : repos_) {
                if (!r->pending_paths.empty()) { any_pending = true; break; }
            }
        }

        // Block cleanly in kernel poll() when idle (0 CPU!), or 5ms debounce timeout when events pending!
        struct pollfd pfd {};
        pfd.fd = inotify_fd_;
        pfd.events = POLLIN;
        int timeout_ms = any_pending ? 5 : 200;
        int prc = (inotify_fd_ >= 0) ? ::poll(&pfd, 1, timeout_ms) : 0;

        if (prc > 0 && (pfd.revents & POLLIN)) {
            while (true) {
                ssize_t len = ::read(inotify_fd_, buf, sizeof(buf));
                if (len <= 0) break;

                auto now = std::chrono::steady_clock::now();
                size_t off = 0;
                std::lock_guard<std::mutex> lk(repos_mu_);
                while (off < static_cast<size_t>(len)) {
                    const auto* ev = reinterpret_cast<const struct inotify_event*>(buf + off);
                    off += sizeof(struct inotify_event) + ev->len;

                    if (ev->len == 0 || ev->name[0] == '.') continue;

                    for (auto& r : repos_) {
                        auto it = r->wd_to_rel_dir.find(ev->wd);
                        if (it == r->wd_to_rel_dir.end()) continue;

                        std::string rel = it->second.empty()
                            ? std::string(ev->name)
                            : (it->second + "/" + ev->name);

                        if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                            if (!r->gi.is_ignored(rel, true)) {
                                bool limit_warned = false;
                                add_inotify_watches_rec(inotify_fd_, r->root, rel, r->gi, r->wd_to_rel_dir, limit_warned);
                                // Also queue any files created inside the new directory
                                WalkOptions wopts;
                                auto files = walk_directory(r->root / rel, wopts);
                                for (const auto& df : files) {
                                    r->pending_paths.push_back(rel + "/" + df.rel_path);
                                }
                            }
                        } else {
                            r->pending_paths.push_back(std::move(rel));
                        }
                        r->last_event_tp = now;
                        break;
                    }
                }
            }
        }

        // Flush coalesced batches (5ms debounce for single save; 25ms coalesce for checkout storms)
        auto now = std::chrono::steady_clock::now();
        std::vector<std::pair<WatchedRepo*, std::vector<std::string>>> to_flush;
        {
            std::lock_guard<std::mutex> lk(repos_mu_);
            for (auto& r : repos_) {
                if (r->pending_paths.empty()) continue;
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - r->last_event_tp).count();
                if (elapsed_ms >= 5 || r->pending_paths.size() >= 1024) {
                    to_flush.emplace_back(r.get(), std::move(r->pending_paths));
                    r->pending_paths.clear();
                }
            }
        }

        for (auto& [repo, paths] : to_flush) {
            repo->live.apply_batch(paths);
            if (repo->live.needs_compaction()) {
                std::string cerr;
                repo->live.compact(cerr);
            }
        }
    }
#elif defined(__APPLE__)
    // macOS FSEvents native watcher with low-latency coalescing!
    struct FsEventCtx {
        ScryDaemon* self;
    } ctx{this};

    auto fsevents_cb = [](ConstFSEventStreamRef,
                          void* info,
                          size_t numEvents,
                          void* eventPaths,
                          const FSEventStreamEventFlags*,
                          const FSEventStreamEventId*) {
        auto* c = static_cast<FsEventCtx*>(info);
        auto** paths = static_cast<char**>(eventPaths);
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(c->self->repos_mu_);
        for (size_t i = 0; i < numEvents; ++i) {
            std::string full(paths[i]);
            for (auto& r : c->self->repos_) {
                std::string rs = r->root.string();
                if (full.size() > rs.size() + 1 &&
                    full.compare(0, rs.size(), rs) == 0 &&
                    full[rs.size()] == '/') {
                    r->pending_paths.push_back(full.substr(rs.size() + 1));
                    r->last_event_tp = now;
                    break;
                }
            }
        }
    };

    FSEventStreamRef stream = nullptr;
    size_t registered_count = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        std::vector<std::string> roots;
        {
            std::lock_guard<std::mutex> lk(repos_mu_);
            for (const auto& r : repos_) roots.push_back(r->root.string());
        }
        if (roots.size() != registered_count) {
            if (stream) {
                FSEventStreamStop(stream);
                FSEventStreamInvalidate(stream);
                FSEventStreamRelease(stream);
                stream = nullptr;
            }
            registered_count = roots.size();
            if (!roots.empty()) {
                CFMutableArrayRef arr = CFArrayCreateMutable(nullptr, static_cast<CFIndex>(roots.size()),
                                                             &kCFTypeArrayCallBacks);
                for (const auto& s : roots) {
                    CFStringRef cs = CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8);
                    CFArrayAppendValue(arr, cs);
                    CFRelease(cs);
                }
                FSEventStreamContext sctx{0, &ctx, nullptr, nullptr, nullptr};
                stream = FSEventStreamCreate(
                    nullptr, fsevents_cb, &sctx, arr, kFSEventStreamEventIdSinceNow,
                    0.01, kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
                CFRelease(arr);
                if (stream) {
                    FSEventStreamScheduleWithRunLoop(stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
                    FSEventStreamStart(stream);
                }
            }
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);

        std::vector<std::pair<WatchedRepo*, std::vector<std::string>>> to_flush;
        {
            std::lock_guard<std::mutex> lk(repos_mu_);
            for (auto& r : repos_) {
                if (!r->pending_paths.empty()) {
                    to_flush.emplace_back(r.get(), std::move(r->pending_paths));
                    r->pending_paths.clear();
                }
            }
        }
        for (auto& [repo, paths] : to_flush) {
            repo->live.apply_batch(paths);
            if (repo->live.needs_compaction()) {
                std::string cerr;
                repo->live.compact(cerr);
            }
        }
    }
    if (stream) {
        FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
    }
#else
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

std::string ScryDaemon::handle_request_line(const std::string& req) {
    if (req.empty()) return "ERR empty";
    if (req == "PING") return "PONG";
    if (req == "STOP") {
        stop();
        return "OK stopping";
    }
    if (req == "STATUS") {
        std::ostringstream oss;
        oss << "scryd running | RSS=" << current_rss_kib()
            << " KiB | Private_Dirty=" << current_private_dirty_kib()
            << " KiB | queries=" << queries_served_.load() << "\n";
        std::lock_guard<std::mutex> lk(repos_mu_);
        for (const auto& r : repos_) {
            auto snap = r->live.snapshot();
            u32 base_d = snap && snap->base ? snap->base->num_docs() : 0;
            u32 delta_d = snap && snap->delta ? snap->delta->live_delta_docs : 0;
            u32 tombs = snap && snap->delta ? snap->delta->tombstoned_base_docs : 0;
            u64 gen = snap ? snap->generation : 0;
            oss << "  repo=" << r->root.string()
                << " base_docs=" << base_d
                << " delta_docs=" << delta_d
                << " tombstones=" << tombs
                << " gen=" << gen << "\n";
        }
        return oss.str();
    }
    if (req.rfind("WATCH ", 0) == 0) {
        std::string path = req.substr(6);
        std::string err;
        if (watch_repo(path, err)) return "OK watching " + path;
        return "ERR " + err;
    }
    if (req.rfind("UNWATCH ", 0) == 0) {
        std::string path = req.substr(8);
        if (unwatch_repo(path)) return "OK unwatched " + path;
        return "ERR not watched: " + path;
    }
    if (req[0] == 'Q') {
        SearchOptions opts;
        if (!deserialize_query_req(req, opts)) {
            return serialize_search_resp({}, false);
        }
        // Find watched repo whose root is an ancestor of opts.target_path
        std::shared_ptr<const IndexSnapshot> snap;
        std::string subdir_prefix;
        std::string target_str = opts.target_path.string();
        {
            std::lock_guard<std::mutex> lk(repos_mu_);
            for (const auto& r : repos_) {
                std::string rs = r->root.string();
                if (target_str == rs ||
                    (target_str.size() > rs.size() &&
                     target_str.compare(0, rs.size(), rs) == 0 &&
                     target_str[rs.size()] == '/')) {
                    snap = r->live.snapshot();
                    if (target_str.size() > rs.size()) {
                        subdir_prefix = target_str.substr(rs.size() + 1);
                        if (!subdir_prefix.empty() && subdir_prefix.back() != '/') {
                            subdir_prefix.push_back('/');
                        }
                    }
                    break;
                }
            }
        }
        if (!snap) {
            return serialize_search_resp({}, false);
        }
        queries_served_.fetch_add(1, std::memory_order_relaxed);
        auto resp = search_snapshot(*snap, opts, subdir_prefix);
        if (!subdir_prefix.empty()) {
            for (auto& fr : resp.files) {
                if (fr.rel_path.rfind(subdir_prefix, 0) == 0) fr.rel_path.erase(0, subdir_prefix.size());
            }
            for (auto& p : resp.listed_files) {
                if (p.rfind(subdir_prefix, 0) == 0) p.erase(0, subdir_prefix.size());
            }
        }
        return serialize_search_resp(resp, true);
    }
    return "ERR unknown command";
}

void ScryDaemon::handle_client(int client_fd) {
    std::string req;
    if (recv_frame(client_fd, req)) {
        std::string resp = handle_request_line(req);
        send_frame(client_fd, resp);
    }
    ::close(client_fd);
}

int ScryDaemon::run() {
    std::error_code ec;
    std::filesystem::create_directories(sock_path_.parent_path(), ec);
    std::filesystem::remove(sock_path_, ec);

    listen_fd_ = open_unix_socket();
    if (listen_fd_ < 0) return 1;

    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::string sp = sock_path_.string();
    if (sp.size() >= sizeof(addr.sun_path)) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return 1;
    }
    std::memcpy(addr.sun_path, sp.c_str(), sp.size() + 1);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return 1;
    }
    if (::listen(listen_fd_, 64) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return 1;
    }

    g_active_daemon.store(this, std::memory_order_release);
    std::signal(SIGINT, daemon_sig_handler);
    std::signal(SIGTERM, daemon_sig_handler);

    std::thread watcher(&ScryDaemon::watcher_loop, this);

    while (!stop_.load(std::memory_order_acquire)) {
        struct pollfd pfd {};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 100);
        if (rc <= 0) continue;
        if (pfd.revents & POLLIN) {
            int cfd = accept_unix_client(listen_fd_);
            if (cfd >= 0) {
                handle_client(cfd);
            }
        }
    }

    watcher.join();
    g_active_daemon.store(nullptr, std::memory_order_release);
    return 0;
}

// ============================================================
// Client Helpers
// ============================================================

namespace {

bool rpc_simple(const std::filesystem::path& sock_path, const std::string& req, std::string& resp) {
    int fd = connect_unix(sock_path);
    if (fd < 0) return false;
    bool ok = send_frame(fd, req) && recv_frame(fd, resp);
    ::close(fd);
    return ok;
}

} // namespace

bool daemon_ping(const std::filesystem::path& sock_path) {
    std::string resp;
    return rpc_simple(sock_path, "PING", resp) && resp == "PONG";
}

bool daemon_watch(const std::filesystem::path& repo_root, std::string& reply,
                  const std::filesystem::path& sock_path) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(repo_root, ec);
    std::string target = ec ? repo_root.string() : canon.string();
    return rpc_simple(sock_path, "WATCH " + target, reply) && reply.rfind("OK", 0) == 0;
}

bool daemon_unwatch(const std::filesystem::path& repo_root, std::string& reply,
                    const std::filesystem::path& sock_path) {
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(repo_root, ec);
    std::string target = ec ? repo_root.string() : canon.string();
    return rpc_simple(sock_path, "UNWATCH " + target, reply) && reply.rfind("OK", 0) == 0;
}

bool daemon_stop(const std::filesystem::path& sock_path) {
    std::string resp;
    return rpc_simple(sock_path, "STOP", resp);
}

bool daemon_status(std::string& out_text, const std::filesystem::path& sock_path) {
    return rpc_simple(sock_path, "STATUS", out_text);
}

bool daemon_query(const SearchOptions& opts, SearchResponse& resp,
                  const std::filesystem::path& sock_path) {
    int fd = connect_unix(sock_path);
    if (fd < 0) return false;
    std::string req = serialize_query_req(opts);
    std::string raw_resp;
    bool ok = send_frame(fd, req) && recv_frame(fd, raw_resp);
    ::close(fd);
    if (!ok) return false;
    bool found_repo = false;
    if (!deserialize_search_resp(raw_resp, resp, found_repo)) return false;
    return found_repo;
}

} // namespace scry
