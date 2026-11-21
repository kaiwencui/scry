#include "delta.hpp"
#include "indexer.hpp"
#include "io_uring_reader.hpp"
#include "simd.hpp"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <unordered_set>
#if defined(__GLIBC__)
  #include <malloc.h>
#endif

namespace scry {

namespace {

// Ultra-fast L1-cache-friendly per-file trigram extraction (zero 4 MiB page faults!)
void extract_doc_trigrams(const u8* data, size_t len,
                          std::vector<Trigram>& exact_out,
                          std::vector<Trigram>& lower_out) {
    exact_out.clear();
    lower_out.clear();
    if (len < 3) return;

    // Small open-addressed hash table (u32 key = trigram + 1, 0 = empty)
    // Fits in L1/L2 cache (~32 KiB) so single-file edits take <0.2 ms with minimal RSS!
    size_t cap = 4096;
    while (cap < std::min<size_t>(len * 2, 1u << 18)) cap <<= 1;

    std::vector<u32> exact_tbl(cap, 0);
    std::vector<u32> lower_tbl(cap, 0);
    size_t mask = cap - 1;
    size_t exact_cnt = 0;
    size_t lower_cnt = 0;

    auto insert_tbl = [](std::vector<u32>& tbl, size_t& msk, size_t& cnt, Trigram t) -> bool {
        if ((cnt + 1) * 10 >= tbl.size() * 7) {
            size_t ncap = tbl.size() * 2;
            std::vector<u32> ntbl(ncap, 0);
            size_t nmsk = ncap - 1;
            for (u32 v : tbl) {
                if (v == 0) continue;
                u32 idx = ((v - 1) * 2654435761u) & static_cast<u32>(nmsk);
                while (ntbl[idx] != 0) idx = (idx + 1) & static_cast<u32>(nmsk);
                ntbl[idx] = v;
            }
            tbl.swap(ntbl);
            msk = nmsk;
        }
        u32 key = t + 1;
        u32 idx = (t * 2654435761u) & static_cast<u32>(msk);
        while (true) {
            u32 cur = tbl[idx];
            if (cur == 0) {
                tbl[idx] = key;
                ++cnt;
                return true;
            }
            if (cur == key) return false;
            idx = (idx + 1) & static_cast<u32>(msk);
        }
    };

    size_t lower_mask = mask;
    exact_out.reserve(std::min<size_t>(len / 2, 4096));
    lower_out.reserve(std::min<size_t>(len / 2, 4096));

    u32 tri = (static_cast<u32>(data[0]) << 8) | static_cast<u32>(data[1]);
    for (size_t i = 2; i < len; ++i) {
        tri = ((tri << 8) | static_cast<u32>(data[i])) & 0x00FFFFFFu;
        if (insert_tbl(exact_tbl, mask, exact_cnt, tri)) {
            exact_out.push_back(tri);
            Trigram lt = lower_trigram(tri);
            if (insert_tbl(lower_tbl, lower_mask, lower_cnt, lt)) {
                lower_out.push_back(lt);
            }
        }
    }
}

} // namespace

void LiveIndex::rebuild_base_path_map(const MmapIndex& base) {
    base_path_to_id_.clear();
    base_path_to_id_.reserve(base.num_docs());
    for (DocId d = 0; d < base.num_docs(); ++d) {
        base_path_to_id_.emplace(std::string(base.file_path(d)), d);
    }
    size_t n_words = (static_cast<size_t>(base.num_docs()) + 63) / 64;
    live_tombstones_.assign(n_words, 0);
    live_tombstone_count_ = 0;
}

bool LiveIndex::load(const std::filesystem::path& root, const std::filesystem::path& index_path) {
    std::lock_guard<std::mutex> lk(write_mu_);
    root_ = root;
    index_path_ = index_path;
    gi_ = GitignoreMatcher{};
    gi_.load_root_defaults(root_);

    auto base = std::make_shared<MmapIndex>();
    if (!base->open(index_path)) return false;

    rebuild_base_path_map(*base);
    live_delta_entries_.clear();

    auto snap = std::make_shared<IndexSnapshot>();
    snap->root = root_;
    snap->index_path = index_path_;
    snap->base = std::move(base);
    snap->delta = std::make_shared<DeltaSegment>();
    snap->generation = ++gen_;
    store_snapshot(std::move(snap));
    return true;
}

bool LiveIndex::reload_base(const std::filesystem::path& index_path) {
    return load(root_, index_path);
}

LiveIndex::UpdateStats LiveIndex::apply_batch(const std::vector<std::string>& rel_paths) {
    Stopwatch sw;
    UpdateStats st;
    if (rel_paths.empty()) return st;

    std::lock_guard<std::mutex> lk(write_mu_);
    auto prev_snap = snapshot();
    if (!prev_snap || !prev_snap->base) return st;

    // Deduplicate incoming paths (git checkout storms often emit duplicate events per file)
    std::vector<std::string> unique_paths = rel_paths;
    std::sort(unique_paths.begin(), unique_paths.end());
    unique_paths.erase(std::unique(unique_paths.begin(), unique_paths.end()), unique_paths.end());

    // Process files in parallel when batch is large (e.g. git checkout storm)
    struct FileJobResult {
        enum class Action : u8 { Skip, Delete, Upsert } action = Action::Skip;
        LiveDeltaEntry entry;
    };
    const size_t npaths = unique_paths.size();
    std::vector<FileJobResult> job_results(npaths);

    auto process_one = [&](size_t idx, std::vector<u8>& buf) {
        const auto& rel = unique_paths[idx];
        auto& out = job_results[idx];
        if (rel.empty() || rel.rfind(".git/", 0) == 0 || rel == ".git" ||
            rel.rfind(".scry/", 0) == 0 || rel == ".scry" ||
            rel[0] == '.' || rel.find("/.") != std::string::npos ||
            is_known_binary_ext(rel) || gi_.is_ignored(rel, false)) {
            out.action = FileJobResult::Action::Skip;
            return;
        }
        std::string abs_path = (root_ / rel).string();
        struct stat fst {};
        if (::lstat(abs_path.c_str(), &fst) != 0 || !S_ISREG(fst.st_mode)) {
            out.action = FileJobResult::Action::Delete;
            return;
        }
        if (static_cast<u64>(fst.st_size) > 32ULL * 1024 * 1024) {
            out.action = FileJobResult::Action::Skip;
            return;
        }
        if (!BatchedFileReader::read_file_sync(abs_path, buf, static_cast<u64>(fst.st_size))) {
            out.action = FileJobResult::Action::Delete;
            return;
        }
        size_t probe = std::min<size_t>(buf.size(), 8192);
        if (probe > 0 && simd::contains_nul(buf.data(), probe)) {
            out.action = FileJobResult::Action::Skip;
            return;
        }
        out.action = FileJobResult::Action::Upsert;
        out.entry.doc.rel_path = rel;
        out.entry.doc.lang = detect_lang(rel);
        out.entry.doc.file_size = buf.size();
        out.entry.doc.mtime_ns = stat_mtime_ns(fst);
        out.entry.doc.inode = static_cast<u64>(fst.st_ino);
        out.entry.doc.deleted = false;
        extract_doc_trigrams(buf.data(), buf.size(), out.entry.exact_trigrams, out.entry.lower_trigrams);
        extract_symbols(buf.data(), buf.size(), out.entry.doc.lang, out.entry.doc.symbols);
    };

    if (npaths < 16) {
        std::vector<u8> buf;
        for (size_t i = 0; i < npaths; ++i) process_one(i, buf);
    } else {
        u32 nt = std::min<u32>(16, std::max<u32>(1, std::thread::hardware_concurrency()));
        nt = std::min<u32>(nt, static_cast<u32>((npaths + 3) / 4));
        std::atomic<size_t> next_i{0};
        std::vector<std::thread> workers;
        workers.reserve(nt);
        for (u32 t = 0; t < nt; ++t) {
            workers.emplace_back([&]() {
                std::vector<u8> buf;
                while (true) {
                    size_t i = next_i.fetch_add(4, std::memory_order_relaxed);
                    if (i >= npaths) break;
                    size_t end = std::min(i + 4, npaths);
                    for (size_t k = i; k < end; ++k) process_one(k, buf);
                }
            });
        }
        for (auto& th : workers) th.join();
    }

    for (size_t i = 0; i < npaths; ++i) {
        const auto& rel = unique_paths[i];
        auto& res = job_results[i];
        if (res.action == FileJobResult::Action::Skip) {
            st.skipped++;
            continue;
        }
        // Tombstone base entry if it exists
        auto bit = base_path_to_id_.find(rel);
        if (bit != base_path_to_id_.end()) {
            DocId bid = bit->second;
            u64 mask = 1ULL << (bid & 63);
            if (!(live_tombstones_[bid >> 6] & mask)) {
                live_tombstones_[bid >> 6] |= mask;
                live_tombstone_count_++;
            }
        }
        if (res.action == FileJobResult::Action::Delete) {
            live_delta_entries_.erase(rel);
            st.deleted++;
        } else {
            live_delta_entries_[rel] = std::move(res.entry);
            st.updated++;
        }
    }

    // Build a fresh immutable DeltaSegment from live_delta_entries_ and atomically publish!
    auto new_delta = std::make_shared<DeltaSegment>();
    new_delta->base_tombstones = live_tombstones_;
    new_delta->tombstoned_base_docs = live_tombstone_count_;
    new_delta->docs.reserve(live_delta_entries_.size());
    new_delta->path_to_delta_id.reserve(live_delta_entries_.size());
    new_delta->exact_postings.reserve(std::min<size_t>(live_delta_entries_.size() * 64 + 1024, 262144));
    new_delta->lower_postings.reserve(std::min<size_t>(live_delta_entries_.size() * 64 + 1024, 262144));

    // Deterministic sorted order by rel_path
    std::vector<const LiveDeltaEntry*> ordered;
    ordered.reserve(live_delta_entries_.size());
    for (const auto& [k, v] : live_delta_entries_) ordered.push_back(&v);
    std::sort(ordered.begin(), ordered.end(), [](const LiveDeltaEntry* a, const LiveDeltaEntry* b) {
        return a->doc.rel_path < b->doc.rel_path;
    });

    for (const LiveDeltaEntry* ep : ordered) {
        DocId did = static_cast<DocId>(new_delta->docs.size());
        new_delta->path_to_delta_id.emplace(ep->doc.rel_path, did);
        new_delta->docs.push_back(ep->doc);
        new_delta->live_delta_docs++;
        for (Trigram t : ep->exact_trigrams) {
            new_delta->exact_postings[t].push_back(did);
        }
        for (Trigram t : ep->lower_trigrams) {
            new_delta->lower_postings[t].push_back(did);
        }
    }

    auto new_snap = std::make_shared<IndexSnapshot>();
    new_snap->root = prev_snap->root;
    new_snap->index_path = prev_snap->index_path;
    new_snap->base = prev_snap->base;
    new_snap->delta = std::move(new_delta);
    new_snap->generation = ++gen_;

    // Atomic snapshot swap; existing readers finish safely on prev_snap.
    store_snapshot(std::move(new_snap));

    st.elapsed_ms = sw.elapsed_ms();
    return st;
}

bool LiveIndex::needs_compaction() const noexcept {
    auto snap = snapshot();
    if (!snap || !snap->base || !snap->delta) return false;
    u32 base_docs = std::max<u32>(1, snap->base->num_docs());
    u32 delta_docs = snap->delta->live_delta_docs + snap->delta->tombstoned_base_docs;
    return delta_docs >= 2000 || (delta_docs >= 256 && delta_docs * 10 >= base_docs);
}

bool LiveIndex::compact(std::string& err) {
    IndexBuildOptions opts;
    opts.output_path = index_path_;
    IndexBuildStats stats;
    if (!build_index(root_, opts, stats, err)) return false;
    return reload_base(index_path_);
}

std::shared_ptr<const DeltaSegment> build_freshness_delta(
    const std::filesystem::path& root,
    const MmapIndex& base,
    bool strict_freshness) {

    const auto& hdr = base.header();
    i64 cur_git_mtime = 0;
    u64 cur_git_head = 0;
    read_git_freshness(root, cur_git_mtime, cur_git_head);

    bool git_changed = (hdr.git_index_mtime_ns != 0 || cur_git_mtime != 0) &&
                       (cur_git_mtime != hdr.git_index_mtime_ns || cur_git_head != hdr.git_head_hash);

    // For small/medium repos (<= 5,000 files), do an ultra-fast stat check (~0.2-1 ms)
    // even when scryd is NOT running so unstaged edits are ALWAYS immediately searchable!
    // For huge monorepos (> 5,000 files), keep the 5us .git/index+HEAD check (use scryd or --fresh).
    std::vector<std::string> fast_stat_dirty;
    if (!strict_freshness && !git_changed) {
        if (base.num_docs() > 5000) {
            return nullptr;
        }
        const std::string root_prefix = root.string() + "/";
        for (DocId d = 0; d < base.num_docs(); ++d) {
            std::string_view p = base.file_path(d);
            const auto& fe = base.file(d);
            std::string abs;
            abs.reserve(root_prefix.size() + p.size());
            abs.append(root_prefix);
            abs.append(p);
            struct stat st {};
            if (::lstat(abs.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
                stat_mtime_ns(st) != fe.mtime_ns ||
                static_cast<u64>(st.st_size) != fe.file_size ||
                static_cast<u64>(st.st_ino) != fe.inode) {
                fast_stat_dirty.emplace_back(p);
            }
        }
        if (fast_stat_dirty.empty()) {
            return nullptr;
        }
    }

    // Collect dirty/modified/untracked/deleted paths
    std::vector<std::string> dirty_paths = std::move(fast_stat_dirty);
    std::unordered_map<std::string, DocId> base_map;
    base_map.reserve(base.num_docs());
    for (DocId d = 0; d < base.num_docs(); ++d) {
        base_map.emplace(std::string(base.file_path(d)), d);
    }

    auto git_dir = root / ".git";
    struct stat gst {};
    bool is_git = (::stat(git_dir.c_str(), &gst) == 0 && S_ISDIR(gst.st_mode));

    if (!dirty_paths.empty()) {
        // Already detected dirty tracked files via fast stat check!
    } else if (is_git && !strict_freshness) {
        // Fast git status porcelain -z to detect modified, untracked, and deleted files
        std::string cmd = "git -C \"" + root.string() + "\" status --porcelain -z -u 2>/dev/null";
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (pipe) {
            std::string raw;
            char tmp[4096];
            size_t n = 0;
            while ((n = std::fread(tmp, 1, sizeof(tmp), pipe)) > 0) {
                raw.append(tmp, n);
            }
            ::pclose(pipe);

            size_t pos = 0;
            while (pos + 3 < raw.size()) {
                size_t nul = raw.find('\0', pos);
                if (nul == std::string::npos) break;
                if (nul - pos >= 4) {
                    char x = raw[pos];
                    char y = raw[pos + 1];
                    std::string p = raw.substr(pos + 3, nul - (pos + 3));
                    dirty_paths.push_back(p);
                    if (x == 'R' || y == 'R' || x == 'C' || y == 'C') {
                        // Next NUL-terminated field is original path
                        pos = nul + 1;
                        size_t nul2 = raw.find('\0', pos);
                        if (nul2 != std::string::npos) {
                            dirty_paths.push_back(raw.substr(pos, nul2 - pos));
                            nul = nul2;
                        }
                    }
                }
                pos = nul + 1;
            }
        }
    } else {
        // Full stat freshness scan (works for both git and non-git repos!)
        WalkOptions wopts;
        auto current_files = walk_directory(root, wopts);
        std::unordered_set<std::string> seen_on_disk;
        seen_on_disk.reserve(current_files.size());

        for (const auto& df : current_files) {
            seen_on_disk.insert(df.rel_path);
            auto it = base_map.find(df.rel_path);
            if (it == base_map.end()) {
                dirty_paths.push_back(df.rel_path);
            } else {
                const auto& fe = base.file(it->second);
                if (fe.mtime_ns != df.mtime_ns || fe.file_size != df.file_size || fe.inode != df.inode) {
                    dirty_paths.push_back(df.rel_path);
                }
            }
        }
        // Any file in base that disappeared on disk -> deleted!
        for (DocId d = 0; d < base.num_docs(); ++d) {
            std::string p(base.file_path(d));
            if (seen_on_disk.find(p) == seen_on_disk.end()) {
                dirty_paths.push_back(std::move(p));
            }
        }
    }

    if (dirty_paths.empty()) return nullptr;

    std::sort(dirty_paths.begin(), dirty_paths.end());
    dirty_paths.erase(std::unique(dirty_paths.begin(), dirty_paths.end()), dirty_paths.end());

    auto delta = std::make_shared<DeltaSegment>();
    delta->base_tombstones.assign((static_cast<size_t>(base.num_docs()) + 63) / 64, 0);

    std::vector<u8> buf;
    std::vector<Trigram> exact_t, lower_t;

    for (const auto& rel : dirty_paths) {
        auto bit = base_map.find(rel);
        if (bit != base_map.end()) {
            DocId bid = bit->second;
            u64 mask = 1ULL << (bid & 63);
            if (!(delta->base_tombstones[bid >> 6] & mask)) {
                delta->base_tombstones[bid >> 6] |= mask;
                delta->tombstoned_base_docs++;
            }
        }

        if (is_known_binary_ext(rel)) continue;
        std::string abs_path = (root / rel).string();
        struct stat st {};
        if (::lstat(abs_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!BatchedFileReader::read_file_sync(abs_path, buf, static_cast<u64>(st.st_size))) continue;

        size_t probe = std::min<size_t>(buf.size(), 8192);
        if (probe > 0 && simd::contains_nul(buf.data(), probe)) continue;

        DocId did = static_cast<DocId>(delta->docs.size());
        DeltaDoc dd;
        dd.rel_path = rel;
        dd.lang = detect_lang(rel);
        dd.file_size = buf.size();
        dd.mtime_ns = stat_mtime_ns(st);
        dd.inode = static_cast<u64>(st.st_ino);
        extract_symbols(buf.data(), buf.size(), dd.lang, dd.symbols);

        delta->path_to_delta_id.emplace(rel, did);
        delta->docs.push_back(std::move(dd));
        delta->live_delta_docs++;

        extract_doc_trigrams(buf.data(), buf.size(), exact_t, lower_t);
        for (Trigram t : exact_t) delta->exact_postings[t].push_back(did);
        for (Trigram t : lower_t) delta->lower_postings[t].push_back(did);
    }

    return delta->empty() ? nullptr : delta;
}

} // namespace scry
