#include "tui.hpp"
#include "indexer.hpp"
#include "io_uring_reader.hpp"
#include "simd.hpp"

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/ioctl.h>
#include <termios.h>
#include <thread>

namespace scry {

namespace {

std::atomic<bool> g_resized{false};

void sigwinch_handler(int) {
    g_resized.store(true, std::memory_order_relaxed);
}

struct FlatMatch {
    std::string rel_path;
    u32 line_no = 0;
    u32 col_no = 1;
    std::string line;
    std::vector<MatchSpan> spans;
    bool is_def = false;
    SymbolKind def_kind = SymbolKind::Unknown;
};

class RawTerminal {
public:
    bool enter() {
        tty_fd_ = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
        if (tty_fd_ < 0) return false;
        if (::tcgetattr(tty_fd_, &orig_) != 0) {
            ::close(tty_fd_);
            tty_fd_ = -1;
            return false;
        }
        struct termios raw = orig_;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cflag |= (CS8);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(tty_fd_, TCSAFLUSH, &raw) != 0) {
            ::close(tty_fd_);
            tty_fd_ = -1;
            return false;
        }
        active_ = true;
        // Enter alternate screen buffer + hide cursor
        write_str("\033[?1049h\033[?25l\033[2J");
        return true;
    }

    ~RawTerminal() { leave(); }

    void leave() {
        if (active_) {
            write_str("\033[?25h\033[?1049l");
            ::tcsetattr(tty_fd_, TCSAFLUSH, &orig_);
            active_ = false;
        }
        if (tty_fd_ >= 0) {
            ::close(tty_fd_);
            tty_fd_ = -1;
        }
    }

    int fd() const noexcept { return tty_fd_; }

    void size(int& rows, int& cols) const {
        struct winsize ws {};
        if (tty_fd_ >= 0 && ::ioctl(tty_fd_, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
            rows = ws.ws_row;
            cols = ws.ws_col;
        } else {
            rows = 24;
            cols = 80;
        }
    }

    void write_str(std::string_view s) const {
        if (tty_fd_ < 0) return;
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = ::write(tty_fd_, s.data() + off, s.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
    }

private:
    int tty_fd_ = -1;
    struct termios orig_ {};
    bool active_ = false;
};

std::string sanitize_truncate(std::string_view s, size_t max_cols) {
    std::string out;
    out.reserve(std::min(s.size(), max_cols));
    for (char c : s) {
        if (out.size() >= max_cols) break;
        if (c == '\t') {
            out += "  ";
            if (out.size() > max_cols) out.resize(max_cols);
        } else if (static_cast<unsigned char>(c) >= 0x20) {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<std::string> load_preview_lines(const std::filesystem::path& abs_path,
                                            u32 center_line,
                                            u32 max_lines,
                                            u32& first_line_no_out) {
    std::vector<std::string> out;
    std::vector<u8> buf;
    if (!BatchedFileReader::read_file_sync(abs_path.string(), buf)) return out;

    u32 half = max_lines / 2;
    u32 start_ln = (center_line > half) ? (center_line - half) : 1;
    u32 end_ln = start_ln + max_lines - 1;
    first_line_no_out = start_ln;

    size_t pos = 0;
    u32 ln = 1;
    while (pos < buf.size() && ln <= end_ln) {
        size_t nl = simd::find_byte(buf.data() + pos, buf.size() - pos, '\n');
        size_t end = pos + nl;
        if (ln >= start_ln) {
            size_t tend = (end > pos && buf[end - 1] == '\r') ? (end - 1) : end;
            out.emplace_back(reinterpret_cast<const char*>(buf.data() + pos), tend - pos);
        }
        pos = (end < buf.size()) ? (end + 1) : buf.size();
        ++ln;
    }
    return out;
}

} // namespace

int run_interactive_tui(const TuiOptions& opts) {
    RawTerminal term;
    if (!term.enter()) {
        std::fprintf(stderr, "scry: interactive TUI requires a terminal (/dev/tty)\n");
        return 1;
    }

    std::signal(SIGWINCH, sigwinch_handler);

    // Pre-load mmap index snapshot once so every keystroke is <1 ms!
    std::filesystem::path idx_root, idx_path;
    std::shared_ptr<IndexSnapshot> preloaded_snap;
    std::string subdir_prefix;
    std::filesystem::path display_root = opts.base_opts.target_path;
    std::error_code ec;

    if (!opts.base_opts.force_no_index &&
        locate_index_for_path(opts.base_opts.target_path, idx_root, idx_path)) {
        auto base = std::make_shared<MmapIndex>();
        if (base->open(idx_path)) {
            preloaded_snap = std::make_shared<IndexSnapshot>();
            preloaded_snap->root = idx_root;
            preloaded_snap->index_path = idx_path;
            preloaded_snap->base = std::move(base);
            preloaded_snap->delta = build_freshness_delta(idx_root, *preloaded_snap->base,
                                                          opts.base_opts.strict_freshness);
            display_root = idx_root;

            auto canon_t = std::filesystem::weakly_canonical(opts.base_opts.target_path, ec);
            if (!ec && canon_t != idx_root) {
                auto rel = std::filesystem::relative(canon_t, idx_root, ec);
                if (!ec && !rel.empty() && rel.string() != ".") {
                    subdir_prefix = rel.string();
                    if (subdir_prefix.back() != '/') subdir_prefix.push_back('/');
                }
            }
        }
    }

    // Shared state between UI thread & background search worker
    struct SharedState {
        std::mutex mu;
        std::condition_variable cv;
        bool quit = false;
        u64 requested_gen = 0;
        u64 completed_gen = 0;

        std::string query;
        bool fixed_strings = false;
        bool case_insensitive = false;
        bool smart_case = true;
        bool def_only = false;

        std::vector<FlatMatch> results;
        SearchStats stats;
        std::string err;
        bool ui_dirty = true;
    } st;

    st.query = opts.base_opts.pattern;
    st.fixed_strings = opts.base_opts.regex_opts.fixed_strings;
    st.case_insensitive = opts.base_opts.regex_opts.case_insensitive;
    st.smart_case = opts.base_opts.regex_opts.smart_case || !st.case_insensitive;
    st.def_only = opts.base_opts.def_only;

    std::atomic<bool> cancel_flag{false};

    auto trigger_search = [&]() {
        cancel_flag.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(st.mu);
            st.requested_gen++;
            st.ui_dirty = true;
        }
        st.cv.notify_one();
    };

    // Dedicated search worker thread with immediate cancellation on new keystroke
    std::thread worker([&]() {
        u64 last_seen = 0;
        while (true) {
            std::string q;
            bool fixed, ic, sc, def_only;
            u64 my_gen = 0;
            {
                std::unique_lock<std::mutex> lk(st.mu);
                st.cv.wait(lk, [&]() { return st.quit || st.requested_gen != last_seen; });
                if (st.quit) break;
                my_gen = st.requested_gen;
                last_seen = my_gen;
                q = st.query;
                fixed = st.fixed_strings;
                ic = st.case_insensitive;
                sc = st.smart_case;
                def_only = st.def_only;
            }

            cancel_flag.store(false, std::memory_order_release);

            std::vector<FlatMatch> flat;
            SearchStats stats;
            std::string err;

            if (!q.empty()) {
                SearchOptions sopts = opts.base_opts;
                sopts.pattern = q;
                sopts.regex_opts.fixed_strings = fixed;
                sopts.regex_opts.case_insensitive = ic;
                sopts.regex_opts.smart_case = sc && !ic;
                sopts.def_only = def_only;
                sopts.boost_defs = true;
                sopts.max_total_matches = 150; // Streaming Top-K early exit for <1-2 ms latency!
                sopts.max_count_per_file = 25;

                SearchResponse resp = preloaded_snap
                    ? search_snapshot(*preloaded_snap, sopts, subdir_prefix, &cancel_flag)
                    : run_search(sopts, &cancel_flag);

                stats = std::move(resp.stats);
                err = std::move(resp.error);

                if (!stats.cancelled) {
                    for (const auto& fr : resp.files) {
                        for (const auto& m : fr.matches) {
                            FlatMatch fm;
                            fm.rel_path = fr.rel_path;
                            fm.line_no = m.line_no;
                            fm.col_no = m.spans.empty() ? 1 : (m.spans[0].start + 1);
                            fm.line = m.line;
                            fm.spans = m.spans;
                            fm.is_def = m.is_definition;
                            fm.def_kind = m.def_kind;
                            flat.push_back(std::move(fm));
                            if (flat.size() >= 150) break;
                        }
                        if (flat.size() >= 150) break;
                    }
                }
            }

            std::lock_guard<std::mutex> lk(st.mu);
            if (my_gen == st.requested_gen) {
                st.results = std::move(flat);
                st.stats = std::move(stats);
                st.err = std::move(err);
                st.completed_gen = my_gen;
                st.ui_dirty = true;
            }
        }
    });

    if (!st.query.empty()) {
        trigger_search();
    }

    size_t selected = 0;
    size_t scroll_off = 0;
    std::optional<FlatMatch> chosen;
    bool running = true;

    std::string cached_preview_path;
    u32 cached_preview_line = 0;
    u32 preview_first_ln = 1;
    std::vector<std::string> preview_lines;

    auto render = [&]() {
        int rows = 24, cols = 80;
        term.size(rows, cols);
        if (rows < 8 || cols < 30) return;

        std::string query;
        bool fixed, ic, sc, def_only, searching;
        std::vector<FlatMatch> results;
        SearchStats stats;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(st.mu);
            query = st.query;
            fixed = st.fixed_strings;
            ic = st.case_insensitive;
            sc = st.smart_case;
            def_only = st.def_only;
            searching = (st.completed_gen != st.requested_gen);
            results = st.results;
            stats = st.stats;
            err = st.err;
            st.ui_dirty = false;
        }

        if (results.empty()) {
            selected = 0;
            scroll_off = 0;
        } else if (selected >= results.size()) {
            selected = results.size() - 1;
        }

        const int body_rows = rows - 3; // Row 1: input, Row 2: status, Row rows: help
        if (selected < scroll_off) scroll_off = selected;
        if (selected >= scroll_off + static_cast<size_t>(body_rows)) {
            scroll_off = selected - static_cast<size_t>(body_rows) + 1;
        }

        const int left_w = (cols >= 90) ? (cols / 2 - 1) : cols;
        const int right_w = (cols >= 90) ? (cols - left_w - 3) : 0;

        // Refresh preview cache if selection changed
        if (right_w > 10 && !results.empty()) {
            const auto& sel = results[selected];
            if (sel.rel_path != cached_preview_path || sel.line_no != cached_preview_line) {
                cached_preview_path = sel.rel_path;
                cached_preview_line = sel.line_no;
                preview_lines = load_preview_lines(display_root / sel.rel_path, sel.line_no,
                                                   static_cast<u32>(body_rows), preview_first_ln);
            }
        } else {
            preview_lines.clear();
            cached_preview_path.clear();
        }

        std::ostringstream frame;
        frame << "\033[H"; // Cursor home

        // Row 1: Prompt & query
        frame << "\033[2K\033[1;36mscry>\033[0m " << sanitize_truncate(query, static_cast<size_t>(cols - 8))
              << "\033[7m \033[0m\r\n";

        // Row 2: Status bar
        char sbuf[256];
        const char* mode_s = def_only ? "DEF" : (fixed ? "FIXED" : "REGEX");
        const char* case_s = ic ? "ICASE" : (sc ? "SMART" : "CASE");
        const char* eng_s = stats.used_index ? (stats.used_linear_fallback ? "IDX+SCAN" : "INDEX") : "SIMD";
        if (!err.empty()) {
            std::snprintf(sbuf, sizeof(sbuf), " ERROR: %s", err.c_str());
            frame << "\033[2K\033[1;31m" << sanitize_truncate(sbuf, static_cast<size_t>(cols)) << "\033[0m\r\n";
        } else {
            std::snprintf(sbuf, sizeof(sbuf),
                          " [%s|%s|%s] %zu results | %u/%u files | %.2f ms%s",
                          mode_s, case_s, eng_s, results.size(),
                          stats.verified_docs, stats.total_indexed_docs,
                          stats.total_ms, searching ? " ..." : "");
            frame << "\033[2K\033[44;37m" << sanitize_truncate(sbuf, static_cast<size_t>(cols));
            size_t slen = std::min<size_t>(std::strlen(sbuf), static_cast<size_t>(cols));
            for (size_t k = slen; k < static_cast<size_t>(cols); ++k) frame.put(' ');
            frame << "\033[0m\r\n";
        }

        // Body rows: left = matches, right = live file preview
        for (int r = 0; r < body_rows; ++r) {
            frame << "\033[2K";
            size_t idx = scroll_off + static_cast<size_t>(r);
            if (idx < results.size()) {
                const auto& m = results[idx];
                bool is_sel = (idx == selected);
                std::string entry = m.rel_path + ":" + std::to_string(m.line_no) + ": " + m.line;
                std::string trunc = sanitize_truncate(entry, static_cast<size_t>(left_w - 2));
                if (is_sel) {
                    frame << "\033[1;48;5;237;33m> " << trunc;
                    for (size_t k = trunc.size() + 2; k < static_cast<size_t>(left_w); ++k) frame.put(' ');
                    frame << "\033[0m";
                } else {
                    frame << "  " << trunc;
                    if (right_w > 0) {
                        for (size_t k = trunc.size() + 2; k < static_cast<size_t>(left_w); ++k) frame.put(' ');
                    }
                }
            } else if (right_w > 0) {
                for (int k = 0; k < left_w; ++k) frame.put(' ');
            }

            if (right_w > 0) {
                frame << " \033[90m|\033[0m ";
                if (static_cast<size_t>(r) < preview_lines.size()) {
                    u32 ln = preview_first_ln + static_cast<u32>(r);
                    bool is_hit = (!results.empty() && ln == results[selected].line_no);
                    char lnbuf[16];
                    std::snprintf(lnbuf, sizeof(lnbuf), "%5u ", ln);
                    int text_w = std::max(1, right_w - 6);
                    std::string ptxt = sanitize_truncate(preview_lines[r], static_cast<size_t>(text_w));
                    if (is_hit) {
                        frame << "\033[1;48;5;237;32m" << lnbuf << "\033[37m" << ptxt << "\033[0m";
                    } else {
                        frame << "\033[90m" << lnbuf << "\033[0m" << ptxt;
                    }
                }
            }
            frame << "\r\n";
        }

        // Bottom footer
        const char* help = " Enter:open  Up/Down:move  Tab:regex/fixed  Ctrl-R:icase  Ctrl-D:def  Esc/Ctrl-C:quit";
        frame << "\033[2K\033[90m" << sanitize_truncate(help, static_cast<size_t>(cols)) << "\033[0m";

        term.write_str(frame.str());
    };

    render();

    while (running) {
        struct pollfd pfd {};
        pfd.fd = term.fd();
        pfd.events = POLLIN;
        int rc = ::poll(&pfd, 1, 16);

        bool need_render = g_resized.exchange(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(st.mu);
            if (st.ui_dirty) need_render = true;
        }

        if (rc > 0 && (pfd.revents & POLLIN)) {
            char buf[32];
            ssize_t n = ::read(term.fd(), buf, sizeof(buf));
            if (n > 0) {
                need_render = true;
                for (ssize_t i = 0; i < n; ++i) {
                    unsigned char c = static_cast<unsigned char>(buf[i]);
                    if (c == 3) { // Ctrl-C
                        running = false;
                        break;
                    }
                    if (c == 27) { // Esc or arrow sequence
                        if (i + 2 < n && buf[i + 1] == '[') {
                            char code = buf[i + 2];
                            i += 2;
                            if (code == 'A' && selected > 0) --selected; // Up
                            else if (code == 'B') ++selected;            // Down
                        } else {
                            running = false;
                            break;
                        }
                    } else if (c == '\r' || c == '\n') { // Enter
                        std::lock_guard<std::mutex> lk(st.mu);
                        if (!st.results.empty() && selected < st.results.size()) {
                            chosen = st.results[selected];
                        }
                        running = false;
                        break;
                    } else if (c == 127 || c == 8) { // Backspace
                        std::lock_guard<std::mutex> lk(st.mu);
                        if (!st.query.empty()) {
                            st.query.pop_back();
                            selected = 0;
                            scroll_off = 0;
                        }
                        trigger_search();
                    } else if (c == 21) { // Ctrl-U clear line
                        {
                            std::lock_guard<std::mutex> lk(st.mu);
                            st.query.clear();
                            selected = 0;
                            scroll_off = 0;
                        }
                        trigger_search();
                    } else if (c == 14) { // Ctrl-N down
                        ++selected;
                    } else if (c == 16) { // Ctrl-P up
                        if (selected > 0) --selected;
                    } else if (c == '\t') { // Tab: toggle fixed/regex
                        {
                            std::lock_guard<std::mutex> lk(st.mu);
                            st.fixed_strings = !st.fixed_strings;
                        }
                        trigger_search();
                    } else if (c == 18) { // Ctrl-R: toggle icase
                        {
                            std::lock_guard<std::mutex> lk(st.mu);
                            st.case_insensitive = !st.case_insensitive;
                        }
                        trigger_search();
                    } else if (c == 4) { // Ctrl-D: toggle --def
                        {
                            std::lock_guard<std::mutex> lk(st.mu);
                            st.def_only = !st.def_only;
                        }
                        trigger_search();
                    } else if (c >= 0x20 && c < 0x7f) {
                        {
                            std::lock_guard<std::mutex> lk(st.mu);
                            st.query.push_back(static_cast<char>(c));
                            selected = 0;
                            scroll_off = 0;
                        }
                        trigger_search();
                    }
                }
            }
        }

        if (need_render && running) {
            render();
        }
    }

    // Shutdown worker cleanly
    cancel_flag.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.quit = true;
    }
    st.cv.notify_one();
    worker.join();
    term.leave();

    if (chosen) {
        std::string target = chosen->rel_path + ":" + std::to_string(chosen->line_no) + ":" +
                             std::to_string(chosen->col_no);
        if (opts.open_in_editor) {
            const char* ed = std::getenv("EDITOR");
            std::string editor = (ed && *ed) ? std::string(ed) : "vi";
            std::string cmd;
            if (editor.find("zed") != std::string::npos || editor.find("code") != std::string::npos) {
                cmd = editor + " \"" + (display_root / chosen->rel_path).string() + ":" +
                      std::to_string(chosen->line_no) + ":" + std::to_string(chosen->col_no) + "\"";
            } else {
                cmd = editor + " +" + std::to_string(chosen->line_no) + " \"" +
                      (display_root / chosen->rel_path).string() + "\"";
            }
            int rc = std::system(cmd.c_str());
            return rc == 0 ? 0 : 1;
        } else {
            std::cout << target << "\n";
        }
    }

    return 0;
}

} // namespace scry
