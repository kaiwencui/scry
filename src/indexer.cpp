#include "indexer.hpp"
#include "io_uring_reader.hpp"
#include "posting.hpp"
#include "simd.hpp"
#include "symbols.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sys/resource.h>
#include <thread>
#include <unordered_map>
#if defined(__GLIBC__)
  #include <malloc.h>
#endif

namespace scry {

void read_git_freshness(const std::filesystem::path& root,
                        i64& git_index_mtime_ns,
                        u64& git_head_hash) {
    git_index_mtime_ns = 0;
    git_head_hash = 0;

    auto git_dir = root / ".git";
    struct stat st {};
    if (::stat(git_dir.c_str(), &st) != 0) return;

    auto idx_file = git_dir / "index";
    if (::stat(idx_file.c_str(), &st) == 0) {
        git_index_mtime_ns = stat_mtime_ns(st);
    }

    auto head_file = git_dir / "HEAD";
    std::ifstream in(head_file);
    if (in) {
        std::string head_content;
        std::getline(in, head_content);
        if (head_content.rfind("ref: ", 0) == 0) {
            std::string ref = head_content.substr(5);
            while (!ref.empty() && (ref.back() == '\r' || ref.back() == '\n')) ref.pop_back();
            std::ifstream ref_in(git_dir / ref);
            if (ref_in) {
                std::string sha;
                std::getline(ref_in, sha);
                head_content += "|" + sha;
            }
        }
        git_head_hash = hash_sv(head_content);
    }
}

bool locate_index_for_path(const std::filesystem::path& query_path,
                           std::filesystem::path& out_root,
                           std::filesystem::path& out_index_path) {
    std::error_code ec;
    auto cur = std::filesystem::weakly_canonical(query_path, ec);
    if (ec) cur = std::filesystem::absolute(query_path, ec);
    if (std::filesystem::is_regular_file(cur, ec)) {
        cur = cur.parent_path();
    }

    while (!cur.empty()) {
        auto local_idx = cur / ".scry" / "index.scry";
        if (std::filesystem::exists(local_idx, ec)) {
            out_root = cur;
            out_index_path = local_idx;
            return true;
        }
        auto cache_idx = index_dir_for_root(cur) / "index.scry";
        if (std::filesystem::exists(cache_idx, ec)) {
            out_root = cur;
            out_index_path = cache_idx;
            return true;
        }
        auto parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return false;
}

namespace {

constexpr u32 kNumPartitions = 64;

inline u32 partition_for_trigram(Trigram t) noexcept {
    // Multiplicative Fibonacci hash of lowercased trigram:
    // - Uniformly balances ASCII source trigrams across all 64 partitions
    // - Keeps all case variants of the same trigram in the exact same partition!
    return ((lower_trigram(t) * 2654435761u) >> 26) & (kNumPartitions - 1);
}

std::vector<u32> build_prefix_table(const std::vector<TrigramEntry>& entries) {
    std::vector<u32> prefix(65537, 0);
    for (const auto& e : entries) {
        u32 p2 = (e.trigram() >> 8) & 0xFFFFu;
        prefix[p2 + 1]++;
    }
    for (u32 i = 1; i <= 65536; ++i) {
        prefix[i] += prefix[i - 1];
    }
    return prefix;
}

bool write_all(int fd, const void* buf, size_t len) {
    const auto* p = static_cast<const u8*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool write_padding(int fd, size_t cur_off, size_t align, size_t& new_off) {
    new_off = static_cast<size_t>(align_up(cur_off, align));
    if (new_off > cur_off) {
        static const u8 zeros[64] = {};
        size_t rem = new_off - cur_off;
        while (rem > 0) {
            size_t chunk = std::min<size_t>(rem, sizeof(zeros));
            if (!write_all(fd, zeros, chunk)) return false;
            rem -= chunk;
        }
    }
    return true;
}

} // namespace

bool build_index(const std::filesystem::path& root_in,
                 const IndexBuildOptions& opts,
                 IndexBuildStats& stats,
                 std::string& err) {
    Stopwatch total_sw;
    err.clear();
    stats = {};

    std::error_code ec;
    auto root = std::filesystem::weakly_canonical(root_in, ec);
    if (ec || !std::filesystem::is_directory(root, ec)) {
        err = "Index root does not exist or is not a directory: " + root_in.string();
        return false;
    }

    // 1. Parallel directory walk respecting .gitignore
    Stopwatch walk_sw;
    WalkOptions wopts;
    wopts.respect_gitignore = opts.respect_gitignore;
    wopts.include_hidden = opts.include_hidden;
    wopts.follow_symlinks = opts.follow_symlinks;
    wopts.max_file_size = opts.max_file_size;
    auto discovered = walk_directory(root, wopts);
    stats.walk_ms = walk_sw.elapsed_ms();

    const u32 num_candidates = static_cast<u32>(discovered.size());
    const u32 pool_max = WorkerPool::global().max_threads();
    const u32 requested_threads = opts.threads > 0 ? opts.threads : pool_max;
    const u32 nthreads = std::max<u32>(
        1,
        std::min<u32>(
            std::min<u32>(requested_threads, pool_max),
            std::max<u32>(1, (num_candidates + 15) / 16)));

    // Respect process RLIMIT_NOFILE so concurrent io_uring rings never hit -EMFILE
    u32 per_ring_depth = 64;
    {
        struct rlimit rl {};
        if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur > 128) {
            u64 budget = (static_cast<u64>(rl.rlim_cur) - 64) / std::max<u32>(1, nthreads);
            u32 safe_depth = static_cast<u32>(std::clamp<u64>(budget * 2, 16, 128));
            u32 pow2 = 16;
            while (pow2 * 2 <= safe_depth) pow2 *= 2;
            per_ring_depth = pow2;
        }
    }
    const bool enable_uring = opts.use_io_uring && (num_candidates >= 512);

    // 2. Parallel file ingestion (io_uring batched reads) + 32 KiB line-aligned chunking
    //    + per-thread strictly-ascending local_chunk_id trigram extraction!
    Stopwatch extract_sw;

    struct LocalChunk {
        u32 orig_file_idx; // Index into `discovered`
        u32 byte_off;
        u32 byte_len;
        u32 start_line_no;
    };

    struct ThreadAccum {
        // 64 partitions by Fibonacci(lower_trigram(t)); each element packs (u64(trigram) << 32) | local_chunk_id.
        // Because local_chunk_id is strictly ascending within each worker, concatenating worker 0..n-1
        // produces a GLOBALLY ChunkId-sorted stream so stable LSD radix sort needs ZERO per-run std::sort!
        std::array<std::vector<u64>, kNumPartitions> trigram_pairs;
        std::vector<LocalChunk> chunks;
        std::vector<std::pair<u32, ExtractedSymbol>> symbols; // (orig_file_idx, sym)
        u64 bytes_read = 0;
        u32 binary_skipped = 0;
        bool used_uring = false;
    };

    std::vector<ThreadAccum> accums(nthreads);
    std::vector<u8> is_binary(num_candidates, 0);
    std::vector<u64> actual_sizes(num_candidates, 0);
    // Per original file: (owner_tid, local_first_chunk, chunk_count)
    struct FileChunkSpan {
        u32 owner_tid = 0;
        u32 local_first = 0;
        u32 count = 0;
    };
    std::vector<FileChunkSpan> file_chunk_spans(num_candidates);

    std::atomic<u32> next_chunk{0};
    constexpr u32 kBatchFiles = 128;

    auto worker_fn = [&](u32 tid) {
        auto& acc = accums[tid];
        acc.chunks.reserve(num_candidates / nthreads + 256);

        // 2 MiB reusable bitset for O(1) per-chunk trigram dedup
        std::vector<u64> seen_bits(1u << 18, 0);
        std::vector<Trigram> chunk_trigrams;
        chunk_trigrams.reserve(4096);
        std::vector<ExtractedSymbol> file_syms;
        file_syms.reserve(64);

        std::unique_ptr<BatchedFileReader> reader;
        if (enable_uring) {
            reader = std::make_unique<BatchedFileReader>(per_ring_depth);
            acc.used_uring = reader->using_io_uring();
        }

        std::vector<ReadRequest> reqs;
        reqs.reserve(kBatchFiles);
        std::vector<u8> sync_buf;
        const std::string root_prefix = root.string() + "/";

        auto index_chunk = [&](u32 orig_file_idx, const u8* cdata, size_t clen,
                               u32 byte_off, u32 start_line_no) {
            u32 lcid = static_cast<u32>(acc.chunks.size());
            acc.chunks.push_back({orig_file_idx, byte_off, static_cast<u32>(clen), start_line_no});

            if (clen >= 3) {
                chunk_trigrams.clear();
                u32 tri = (static_cast<u32>(cdata[0]) << 8) | static_cast<u32>(cdata[1]);
                for (size_t i = 2; i < clen; ++i) {
                    tri = ((tri << 8) | static_cast<u32>(cdata[i])) & 0x00FFFFFFu;
                    u32 w = tri >> 6;
                    u64 b = 1ULL << (tri & 63);
                    if (!(seen_bits[w] & b)) {
                        seen_bits[w] |= b;
                        chunk_trigrams.push_back(tri);
                    }
                }
                for (Trigram t : chunk_trigrams) {
                    seen_bits[t >> 6] = 0;
                    u32 part = partition_for_trigram(t);
                    acc.trigram_pairs[part].push_back((static_cast<u64>(t) << 32) | lcid);
                }
            }
        };

        auto process_file = [&](DocId orig_idx, const u8* data, size_t len, bool ok) {
            if (!ok) {
                is_binary[orig_idx] = 1;
                return;
            }
            actual_sizes[orig_idx] = len;
            size_t probe = std::min<size_t>(len, 8192);
            if (probe > 0 && simd::contains_nul(data, probe)) {
                is_binary[orig_idx] = 1;
                acc.binary_skipped++;
                return;
            }
            acc.bytes_read += len;

            u32 first_lcid = static_cast<u32>(acc.chunks.size());

            // Small/medium files (<= 32 KiB) stay as a single chunk;
            // large files (> 32 KiB) split on '\n' boundaries into ~32 KiB virtual chunks!
            if (len <= kChunkTargetBytes) {
                index_chunk(orig_idx, data, len, 0, 1);
            } else {
                size_t off = 0;
                u32 line_no = 1;
                while (off < len) {
                    size_t target_end = std::min<size_t>(off + kChunkTargetBytes, len);
                    size_t chunk_end = target_end;
                    if (chunk_end < len) {
                        size_t nl = simd::find_byte(data + chunk_end, len - chunk_end, '\n');
                        chunk_end = (chunk_end + nl < len) ? (chunk_end + nl + 1) : len;
                    }
                    size_t clen = chunk_end - off;
                    index_chunk(orig_idx, data + off, clen, static_cast<u32>(off), line_no);
                    if (chunk_end < len) {
                        line_no += static_cast<u32>(simd::count_byte(data + off, clen, '\n'));
                    }
                    off = chunk_end;
                }
            }

            u32 cnt = static_cast<u32>(acc.chunks.size()) - first_lcid;
            file_chunk_spans[orig_idx] = {tid, first_lcid, cnt};

            // Lightweight lexical symbol extraction
            if (opts.extract_symbols && len > 0) {
                extract_symbols(data, len, discovered[orig_idx].lang, file_syms);
                for (auto& s : file_syms) {
                    acc.symbols.emplace_back(orig_idx, std::move(s));
                }
            }
        };

        while (true) {
            u32 start = next_chunk.fetch_add(kBatchFiles, std::memory_order_relaxed);
            if (start >= num_candidates) break;
            u32 end = std::min<u32>(start + kBatchFiles, num_candidates);

            if (reader && reader->using_io_uring()) {
                reqs.clear();
                for (u32 i = start; i < end; ++i) {
                    reqs.push_back({root_prefix + discovered[i].rel_path, discovered[i].file_size, i});
                }
                reader->read_batch(reqs, process_file);
            } else {
                for (u32 i = start; i < end; ++i) {
                    bool ok = BatchedFileReader::read_file_sync(
                        root_prefix + discovered[i].rel_path, sync_buf, discovered[i].file_size);
                    process_file(i, ok ? sync_buf.data() : nullptr, ok ? sync_buf.size() : 0, ok);
                }
            }
        }
    };

    WorkerPool::global().run(nthreads, worker_fn);
    stats.read_and_extract_ms = extract_sw.elapsed_ms();

    // 3. Assign global ChunkId offsets per worker & build FileTable + ChunkTable!
    Stopwatch merge_sw;

    std::vector<u32> chunk_base(nthreads + 1, 0);
    for (u32 t = 0; t < nthreads; ++t) {
        chunk_base[t + 1] = chunk_base[t] + static_cast<u32>(accums[t].chunks.size());
    }
    const u32 num_chunks = chunk_base[nthreads];

    std::vector<DocId> old_to_new(num_candidates, INVALID_DOC_ID);
    std::vector<FileEntry> file_entries;
    std::string path_arena;
    file_entries.reserve(num_candidates);

    u32 num_docs = 0;
    for (u32 i = 0; i < num_candidates; ++i) {
        if (is_binary[i]) continue;
        old_to_new[i] = num_docs++;
        const auto& df = discovered[i];
        const auto& sp = file_chunk_spans[i];
        FileEntry fe{};
        fe.path_off = static_cast<u32>(path_arena.size());
        fe.path_len = static_cast<u16>(std::min<size_t>(df.rel_path.size(), 65535));
        fe.lang_id = static_cast<u16>(df.lang);
        fe.file_size = actual_sizes[i];
        fe.mtime_ns = df.mtime_ns;
        fe.inode = df.inode;
        fe.first_chunk = chunk_base[sp.owner_tid] + sp.local_first;
        fe.chunk_count = sp.count;
        path_arena.append(df.rel_path.data(), fe.path_len);
        path_arena.push_back('\0');
        file_entries.push_back(fe);
    }

    // Materialize global ChunkTable in global ChunkId order
    std::vector<ChunkEntry> chunk_entries;
    chunk_entries.reserve(num_chunks);
    for (u32 t = 0; t < nthreads; ++t) {
        for (const auto& lc : accums[t].chunks) {
            ChunkEntry ce{};
            ce.file_id = old_to_new[lc.orig_file_idx];
            ce.byte_off = lc.byte_off;
            ce.byte_len = lc.byte_len;
            ce.start_line_no = lc.start_line_no;
            chunk_entries.push_back(ce);
        }
        std::vector<LocalChunk>().swap(accums[t].chunks);
    }

    for (const auto& acc : accums) {
        stats.total_corpus_bytes += acc.bytes_read;
        stats.files_skipped_binary += acc.binary_skipped;
        if (acc.used_uring) stats.used_io_uring = true;
    }
    stats.files_indexed = num_docs;

    // 4. Parallel partitioned STABLE ZERO-SORT LSD radix merge + adaptive 3-tier encoding!
    struct PartOutput {
        std::vector<TrigramEntry> exact_entries;
        std::vector<u8> exact_arena;
        std::vector<TrigramEntry> lower_entries;
        std::vector<u8> lower_arena;
    };
    std::vector<PartOutput> part_outs(kNumPartitions);

    std::atomic<u32> next_part{0};
    auto merge_worker = [&](u32) {
        std::vector<u64> merged;
        std::vector<u64> radix_tmp;
        std::vector<DocId> docs_buf;
        std::vector<DocId> union_buf;
        std::vector<DocId> tmp_buf;

        while (true) {
            u32 p = next_part.fetch_add(1, std::memory_order_relaxed);
            if (p >= kNumPartitions) break;

            size_t total_pairs = 0;
            for (u32 t = 0; t < nthreads; ++t) total_pairs += accums[t].trigram_pairs[p].size();
            if (total_pairs == 0) continue;

            merged.clear();
            merged.reserve(total_pairs);
            // Append workers 0..nthreads-1 in order -> globally strictly ascending by ChunkId!
            for (u32 t = 0; t < nthreads; ++t) {
                auto& v = accums[t].trigram_pairs[p];
                u32 cbase = chunk_base[t];
                for (u64 pair : v) {
                    merged.push_back(pair + cbase); // Adds cbase to low 32 bits (never overflows into high 32!)
                }
                std::vector<u64>().swap(v);
            }
            if (merged.empty()) continue;

            // STABLE 3-pass 8-bit LSD radix sort on 24-bit trigram key!
            // Preserves global ascending ChunkId order within every trigram run -> ZERO per-run std::sort!
            if (merged.size() > 64) {
                radix_tmp.resize(merged.size());
                for (u32 pass = 0; pass < 3; ++pass) {
                    u32 shift = 32 + pass * 8;
                    u32 counts[256] = {};
                    for (u64 v : merged) counts[(v >> shift) & 0xFFu]++;
                    u32 sum = 0;
                    for (u32 b = 0; b < 256; ++b) {
                        u32 c = counts[b];
                        counts[b] = sum;
                        sum += c;
                    }
                    for (u64 v : merged) {
                        radix_tmp[counts[(v >> shift) & 0xFFu]++] = v;
                    }
                    merged.swap(radix_tmp);
                }
            } else {
                std::sort(merged.begin(), merged.end());
            }

            auto& out = part_outs[p];

            struct ExactRun {
                Trigram tri;
                size_t begin;
                size_t end;
            };
            std::vector<ExactRun> runs;
            runs.reserve(std::min<size_t>(merged.size(), 65536));

            size_t i = 0;
            while (i < merged.size()) {
                Trigram tri = static_cast<Trigram>(merged[i] >> 32);
                size_t j = i + 1;
                while (j < merged.size() && static_cast<Trigram>(merged[j] >> 32) == tri) ++j;
                runs.push_back({tri, i, j});
                i = j;
            }

            // Group runs by lower_trigram(tri) for hybrid lower table
            std::unordered_map<Trigram, std::vector<size_t>> lower_groups;
            if (opts.build_lower_table) {
                lower_groups.reserve(runs.size());
                for (size_t ri = 0; ri < runs.size(); ++ri) {
                    lower_groups[lower_trigram(runs[ri].tri)].push_back(ri);
                }
            }

            out.exact_entries.reserve(runs.size());
            for (size_t ri = 0; ri < runs.size(); ++ri) {
                const auto& run = runs[ri];
                docs_buf.resize(run.end - run.begin);
                for (size_t k = 0; k < docs_buf.size(); ++k) {
                    docs_buf[k] = static_cast<DocId>(merged[run.begin + k] & 0xFFFFFFFFu);
                }
                bool has_upper_var = false;
                if (opts.build_lower_table) {
                    Trigram tl = lower_trigram(run.tri);
                    auto it = lower_groups.find(tl);
                    if (it != lower_groups.end()) {
                        for (size_t idx : it->second) {
                            if (trigram_has_upper(runs[idx].tri)) {
                                has_upper_var = true;
                                break;
                            }
                        }
                    }
                }
                out.exact_entries.push_back(
                    encode_posting_list(run.tri, docs_buf, num_chunks, out.exact_arena, has_upper_var));
            }

            if (opts.build_lower_table) {
                std::vector<Trigram> lower_keys;
                lower_keys.reserve(lower_groups.size());
                for (const auto& [tl, idxs] : lower_groups) {
                    bool any_upper = false;
                    for (size_t idx : idxs) {
                        if (trigram_has_upper(runs[idx].tri)) {
                            any_upper = true;
                            break;
                        }
                    }
                    if (any_upper) lower_keys.push_back(tl);
                }
                std::sort(lower_keys.begin(), lower_keys.end());
                out.lower_entries.reserve(lower_keys.size());

                for (Trigram tl : lower_keys) {
                    const auto& idxs = lower_groups[tl];
                    union_buf.clear();
                    for (size_t idx : idxs) {
                        const auto& run = runs[idx];
                        docs_buf.resize(run.end - run.begin);
                        for (size_t k = 0; k < docs_buf.size(); ++k) {
                            docs_buf[k] = static_cast<DocId>(merged[run.begin + k] & 0xFFFFFFFFu);
                        }
                        if (union_buf.empty()) {
                            union_buf = docs_buf;
                        } else {
                            union_sorted_u32(union_buf, docs_buf, tmp_buf);
                            union_buf.swap(tmp_buf);
                        }
                    }
                    out.lower_entries.push_back(
                        encode_posting_list(tl, union_buf, num_chunks, out.lower_arena, false));
                }
            }
        }
    };

    WorkerPool::global().run(nthreads, merge_worker);

    // Stitch partition arenas + adjust posting offsets & sort exact/lower directory entries
    std::vector<TrigramEntry> exact_entries;
    std::vector<u8> exact_arena;
    std::vector<TrigramEntry> lower_entries;
    std::vector<u8> lower_arena;

    size_t total_exact_entries = 0, total_exact_bytes = 0;
    size_t total_lower_entries = 0, total_lower_bytes = 0;
    for (const auto& po : part_outs) {
        total_exact_entries += po.exact_entries.size();
        total_exact_bytes += align_up(po.exact_arena.size(), 8);
        total_lower_entries += po.lower_entries.size();
        total_lower_bytes += align_up(po.lower_arena.size(), 8);
    }
    exact_entries.reserve(total_exact_entries);
    exact_arena.reserve(total_exact_bytes);
    lower_entries.reserve(total_lower_entries);
    lower_arena.reserve(total_lower_bytes);

    for (auto& po : part_outs) {
        u64 base_off = static_cast<u64>(align_up(exact_arena.size(), 8));
        exact_arena.resize(base_off, 0);
        exact_arena.insert(exact_arena.end(), po.exact_arena.begin(), po.exact_arena.end());
        for (auto e : po.exact_entries) {
            if (e.encoding() != PostingEncoding::Inline) {
                e.offset_or_inline += base_off;
            }
            exact_entries.push_back(e);
        }
        std::vector<u8>().swap(po.exact_arena);
        std::vector<TrigramEntry>().swap(po.exact_entries);

        u64 lbase_off = static_cast<u64>(align_up(lower_arena.size(), 8));
        lower_arena.resize(lbase_off, 0);
        lower_arena.insert(lower_arena.end(), po.lower_arena.begin(), po.lower_arena.end());
        for (auto e : po.lower_entries) {
            if (e.encoding() != PostingEncoding::Inline) {
                e.offset_or_inline += lbase_off;
            }
            lower_entries.push_back(e);
        }
        std::vector<u8>().swap(po.lower_arena);
        std::vector<TrigramEntry>().swap(po.lower_entries);
    }

    auto tri_cmp = [](const TrigramEntry& a, const TrigramEntry& b) {
        return a.trigram() < b.trigram();
    };
    std::sort(exact_entries.begin(), exact_entries.end(), tri_cmp);
    std::sort(lower_entries.begin(), lower_entries.end(), tri_cmp);

    auto exact_prefix = build_prefix_table(exact_entries);
    auto lower_prefix = build_prefix_table(lower_entries);

    // 5. Build File-Path Trigram Index (lowercased paths -> FileId)
    std::vector<TrigramEntry> path_entries;
    std::vector<u8> path_arena_postings;
    std::vector<u32> path_prefix;
    if (opts.build_path_index && num_docs > 0) {
        std::vector<u64> path_pairs;
        path_pairs.reserve(static_cast<size_t>(num_docs) * 24);
        for (DocId d = 0; d < num_docs; ++d) {
            const auto& fe = file_entries[d];
            std::string_view psv(path_arena.data() + fe.path_off, fe.path_len);
            if (psv.size() < 3) continue;
            for (size_t i = 0; i + 3 <= psv.size(); ++i) {
                Trigram t = pack_trigram(ascii_lower(static_cast<u8>(psv[i])),
                                         ascii_lower(static_cast<u8>(psv[i + 1])),
                                         ascii_lower(static_cast<u8>(psv[i + 2])));
                path_pairs.push_back((static_cast<u64>(t) << 32) | d);
            }
        }
        std::sort(path_pairs.begin(), path_pairs.end());
        path_pairs.erase(std::unique(path_pairs.begin(), path_pairs.end()), path_pairs.end());

        std::vector<DocId> pdocs;
        size_t i = 0;
        while (i < path_pairs.size()) {
            Trigram t = static_cast<Trigram>(path_pairs[i] >> 32);
            size_t j = i + 1;
            pdocs.clear();
            pdocs.push_back(static_cast<DocId>(path_pairs[i] & 0xFFFFFFFFu));
            while (j < path_pairs.size() && static_cast<Trigram>(path_pairs[j] >> 32) == t) {
                pdocs.push_back(static_cast<DocId>(path_pairs[j] & 0xFFFFFFFFu));
                ++j;
            }
            path_entries.push_back(encode_posting_list(t, pdocs, num_docs, path_arena_postings));
            i = j;
        }
        path_prefix = build_prefix_table(path_entries);
    }

    // 6. Parallel 16-bucket Symbol Definition Index (compact 16-byte SymbolEntry!)
    std::vector<SymbolEntry> sym_entries;
    std::string sym_arena;
    if (opts.extract_symbols) {
        constexpr u32 kSymBuckets = 16;
        struct RawSym {
            u32 lower_hash;
            DocId doc_id;
            u32 line_and_kind;
            std::string name;
        };
        std::array<std::vector<RawSym>, kSymBuckets> sbuckets;
        size_t total_syms = 0;
        for (const auto& acc : accums) total_syms += acc.symbols.size();
        for (auto& b : sbuckets) b.reserve(total_syms / kSymBuckets + 64);

        char low_buf[144];
        for (auto& acc : accums) {
            for (auto& [old_doc, sym] : acc.symbols) {
                DocId nd = old_to_new[old_doc];
                if (nd == INVALID_DOC_ID) continue;
                u16 nlen = static_cast<u16>(std::min<size_t>(sym.name.size(), sizeof(low_buf)));
                for (u16 k = 0; k < nlen; ++k) {
                    low_buf[k] = static_cast<char>(ascii_lower(static_cast<u8>(sym.name[k])));
                }
                u32 lh = static_cast<u32>(hash_bytes64(low_buf, nlen));
                sbuckets[lh >> 28].push_back({
                    lh, nd, SymbolEntry::pack_line_kind(sym.line_no, sym.kind), std::move(sym.name)
                });
            }
            std::vector<std::pair<u32, ExtractedSymbol>>().swap(acc.symbols);
        }

        // Parallel per-bucket interning + sorting (16 cores 100% parallel!)
        struct SymBucketOut {
            std::vector<SymbolEntry> entries;
            std::string arena;
        };
        std::array<SymBucketOut, kSymBuckets> bouts;

        std::atomic<u32> next_sym_bucket{0};
        WorkerPool::global().run(std::min<u32>(kSymBuckets, pool_max), [&](u32) {
            while (true) {
                u32 b = next_sym_bucket.fetch_add(1, std::memory_order_relaxed);
                if (b >= kSymBuckets) break;
                auto& raw = sbuckets[b];
                auto& bo = bouts[b];
                if (raw.empty()) continue;
                bo.entries.reserve(raw.size());
                std::unordered_map<u64, u32> intern;
                intern.reserve(raw.size() / 2 + 16);

                for (const auto& r : raw) {
                    u64 eh = hash_sv(r.name);
                    u32 off = 0;
                    auto it = intern.find(eh);
                    if (it != intern.end() &&
                        bo.arena.compare(it->second, r.name.size(), r.name) == 0 &&
                        bo.arena[it->second + r.name.size()] == '\0') {
                        off = it->second;
                    } else {
                        off = static_cast<u32>(bo.arena.size());
                        bo.arena.append(r.name);
                        bo.arena.push_back('\0');
                        intern.emplace(eh, off);
                    }
                    bo.entries.push_back({r.lower_hash, off, r.doc_id, r.line_and_kind});
                }
                std::vector<RawSym>().swap(raw);

                std::sort(bo.entries.begin(), bo.entries.end(), [](const SymbolEntry& a, const SymbolEntry& b) {
                    if (a.lower_hash != b.lower_hash) return a.lower_hash < b.lower_hash;
                    if (a.doc_id != b.doc_id) return a.doc_id < b.doc_id;
                    return a.line_no() < b.line_no();
                });
            }
        });

        size_t total_sym_entries = 0, total_arena_bytes = 0;
        for (const auto& bo : bouts) {
            total_sym_entries += bo.entries.size();
            total_arena_bytes += bo.arena.size();
        }
        sym_entries.reserve(total_sym_entries);
        sym_arena.reserve(total_arena_bytes);
        for (auto& bo : bouts) {
            u32 base_off = static_cast<u32>(sym_arena.size());
            sym_arena.append(bo.arena);
            for (auto se : bo.entries) {
                se.name_off += base_off;
                sym_entries.push_back(se);
            }
            std::string().swap(bo.arena);
            std::vector<SymbolEntry>().swap(bo.entries);
        }
    }

    stats.merge_and_encode_ms = merge_sw.elapsed_ms();
    stats.num_exact_trigrams = static_cast<u32>(exact_entries.size());
    stats.num_lower_trigrams = static_cast<u32>(lower_entries.size());
    stats.num_symbols = static_cast<u32>(sym_entries.size());

    // 7. Write binary zero-copy mmap index v2 atomically
    Stopwatch write_sw;
    std::filesystem::path final_path = opts.output_path.empty()
        ? (index_dir_for_root(root) / "index.scry")
        : opts.output_path;
    std::filesystem::create_directories(final_path.parent_path(), ec);
    if (ec) {
        err = "Failed to create index directory: " + final_path.parent_path().string();
        return false;
    }

    std::filesystem::path tmp_path = final_path;
    tmp_path += ".tmp." + std::to_string(::getpid());

    int fd = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        err = "Failed to open temporary index file: " + tmp_path.string();
        return false;
    }

    IndexHeader hdr{};
    std::memcpy(hdr.magic, kIndexMagic, 8);
    hdr.version = kIndexVersion;
    hdr.flags = IDX_FLAG_NONE;
    if (!lower_entries.empty()) hdr.flags |= IDX_FLAG_HAS_LOWER_TABLE;
    if (!path_entries.empty()) hdr.flags |= IDX_FLAG_HAS_PATH_INDEX;
    if (!sym_entries.empty()) hdr.flags |= IDX_FLAG_HAS_SYMBOLS;

    hdr.num_docs = num_docs;
    hdr.num_chunks = num_chunks;
    hdr.num_exact_trigrams = static_cast<u32>(exact_entries.size());
    hdr.num_lower_trigrams = static_cast<u32>(lower_entries.size());
    hdr.num_path_trigrams = static_cast<u32>(path_entries.size());
    hdr.num_symbols = static_cast<u32>(sym_entries.size());
    hdr.total_corpus_bytes = stats.total_corpus_bytes;
    hdr.build_time_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    read_git_freshness(root, hdr.git_index_mtime_ns, hdr.git_head_hash);
    hdr.root_path_hash = hash_sv(root.string());

    size_t off = align_up(sizeof(IndexHeader), 64);

    hdr.file_table_off = off;
    off = align_up(off + file_entries.size() * sizeof(FileEntry), 64);

    hdr.chunk_table_off = off;
    off = align_up(off + chunk_entries.size() * sizeof(ChunkEntry), 64);

    hdr.path_arena_off = off;
    hdr.path_arena_size = path_arena.size();
    off = align_up(off + path_arena.size(), 64);

    hdr.exact_prefix_off = off;
    off = align_up(off + exact_prefix.size() * sizeof(u32), 64);

    hdr.exact_entries_off = off;
    off = align_up(off + exact_entries.size() * sizeof(TrigramEntry), 64);

    hdr.exact_postings_off = off;
    hdr.exact_postings_size = exact_arena.size();
    off = align_up(off + exact_arena.size(), 64);

    if (hdr.flags & IDX_FLAG_HAS_LOWER_TABLE) {
        hdr.lower_prefix_off = off;
        off = align_up(off + lower_prefix.size() * sizeof(u32), 64);
        hdr.lower_entries_off = off;
        off = align_up(off + lower_entries.size() * sizeof(TrigramEntry), 64);
        hdr.lower_postings_off = off;
        hdr.lower_postings_size = lower_arena.size();
        off = align_up(off + lower_arena.size(), 64);
    }

    if (hdr.flags & IDX_FLAG_HAS_PATH_INDEX) {
        hdr.path_prefix_off = off;
        off = align_up(off + path_prefix.size() * sizeof(u32), 64);
        hdr.path_entries_off = off;
        off = align_up(off + path_entries.size() * sizeof(TrigramEntry), 64);
        hdr.path_postings_off = off;
        hdr.path_postings_size = path_arena_postings.size();
        off = align_up(off + path_arena_postings.size(), 64);
    }

    if (hdr.flags & IDX_FLAG_HAS_SYMBOLS) {
        hdr.symbols_off = off;
        off = align_up(off + sym_entries.size() * sizeof(SymbolEntry), 64);
        hdr.symbol_arena_off = off;
        hdr.symbol_arena_size = sym_arena.size();
        off = align_up(off + sym_arena.size(), 64);
    }

    hdr.checksum = hash_bytes64(&hdr, offsetof(IndexHeader, checksum));

    bool ok = true;
    size_t cur = 0;
    auto write_sec = [&](const void* data, size_t sz) {
        if (!ok) return;
        size_t padded = 0;
        if (!write_padding(fd, cur, 64, padded)) { ok = false; return; }
        cur = padded;
        if (sz > 0) {
            if (!write_all(fd, data, sz)) { ok = false; return; }
            cur += sz;
        }
    };

    if (!write_all(fd, &hdr, sizeof(hdr))) ok = false;
    cur = sizeof(hdr);

    write_sec(file_entries.data(), file_entries.size() * sizeof(FileEntry));
    write_sec(chunk_entries.data(), chunk_entries.size() * sizeof(ChunkEntry));
    write_sec(path_arena.data(), path_arena.size());

    write_sec(exact_prefix.data(), exact_prefix.size() * sizeof(u32));
    write_sec(exact_entries.data(), exact_entries.size() * sizeof(TrigramEntry));
    write_sec(exact_arena.data(), exact_arena.size());

    if (hdr.flags & IDX_FLAG_HAS_LOWER_TABLE) {
        write_sec(lower_prefix.data(), lower_prefix.size() * sizeof(u32));
        write_sec(lower_entries.data(), lower_entries.size() * sizeof(TrigramEntry));
        write_sec(lower_arena.data(), lower_arena.size());
    }

    if (hdr.flags & IDX_FLAG_HAS_PATH_INDEX) {
        write_sec(path_prefix.data(), path_prefix.size() * sizeof(u32));
        write_sec(path_entries.data(), path_entries.size() * sizeof(TrigramEntry));
        write_sec(path_arena_postings.data(), path_arena_postings.size());
    }

    if (hdr.flags & IDX_FLAG_HAS_SYMBOLS) {
        write_sec(sym_entries.data(), sym_entries.size() * sizeof(SymbolEntry));
        write_sec(sym_arena.data(), sym_arena.size());
    }

    size_t final_sz = cur;
    if (ok) {
        write_padding(fd, cur, 64, final_sz);
    }
    ::close(fd);

    if (!ok) {
        std::filesystem::remove(tmp_path, ec);
        err = "Failed writing index file";
        return false;
    }

    if (::rename(tmp_path.c_str(), final_path.c_str()) != 0) {
        std::filesystem::remove(tmp_path, ec);
        err = "Failed atomic rename of index file";
        return false;
    }

    {
        std::ofstream meta(final_path.parent_path() / "root.txt");
        if (meta) meta << root.string() << "\n";
    }

    stats.write_ms = write_sw.elapsed_ms();
    stats.total_ms = total_sw.elapsed_ms();
    stats.index_file_bytes = final_sz;
    stats.index_path = final_path;
    if (stats.total_ms > 0.0) {
        stats.throughput_mb_s = (static_cast<double>(stats.total_corpus_bytes) / (1024.0 * 1024.0)) /
                                (stats.total_ms / 1000.0);
    }
#if defined(__GLIBC__)
    ::malloc_trim(0);
#endif
    return true;
}

} // namespace scry
