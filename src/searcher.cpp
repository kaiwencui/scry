#include "searcher.hpp"
#include "indexer.hpp"
#include "io_uring_reader.hpp"
#include "posting.hpp"
#include "simd.hpp"
#include "thread_pool.hpp"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace scry {

namespace {

bool matches_lang_filter(LangId lang, const SearchOptions& opts) noexcept {
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

bool matches_path_filters(std::string_view rel_path,
                          LangId lang,
                          const SearchOptions& opts,
                          const std::string& subdir_prefix) noexcept {
    if (!subdir_prefix.empty()) {
        if (rel_path.size() < subdir_prefix.size() ||
            rel_path.compare(0, subdir_prefix.size(), subdir_prefix) != 0) {
            return false;
        }
    }
    if (!matches_lang_filter(lang, opts)) return false;
    if (!opts.globs.empty() && !opts.globs.matches(rel_path)) return false;
    return true;
}

// Extract longest literal substring >= 3 chars from a positive glob rule for PathTrigramIndex pre-filtering
std::vector<std::string> extract_glob_literals(const GlobFilter& gf) {
    std::vector<std::string> lits;
    if (gf.rules.empty()) return lits;
    // Only safe if there is a single positive rule (multiple positive rules act as OR!)
    int pos_count = 0;
    const GlobFilter::Rule* pos_rule = nullptr;
    for (const auto& r : gf.rules) {
        if (!r.negated) {
            pos_count++;
            pos_rule = &r;
        }
    }
    if (pos_count != 1 || !pos_rule) return lits;

    std::string cur;
    for (char c : pos_rule->pattern) {
        if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\') {
            if (cur.size() >= 3) lits.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(static_cast<char>(ascii_lower(static_cast<u8>(c))));
        }
    }
    if (cur.size() >= 3) lits.push_back(cur);
    return lits;
}

// Estimate chunk_freq of a TrigramQuery in O(1) for selectivity ordering
u32 estimate_cost(const TrigramQuery& q, const MmapIndex& idx) noexcept {
    switch (q.kind) {
        case TrigramQuery::Kind::None: return 0;
        case TrigramQuery::Kind::All: return idx.num_chunks();
        case TrigramQuery::Kind::Atom: {
            auto r = idx.lookup_trigram(q.trigram, q.icase);
            return r.entry ? r.entry->doc_freq : 0;
        }
        case TrigramQuery::Kind::And: {
            u32 best = idx.num_chunks();
            for (const auto& ch : q.children) {
                best = std::min(best, estimate_cost(ch, idx));
            }
            return best;
        }
        case TrigramQuery::Kind::Or: {
            u64 sum = 0;
            for (const auto& ch : q.children) sum += estimate_cost(ch, idx);
            return static_cast<u32>(std::min<u64>(sum, idx.num_chunks()));
        }
    }
    return idx.num_chunks();
}

// Evaluate TrigramQuery against immutable MmapIndex -> sorted candidate ChunkIds.
// Returns false if query is unrestricted (`All`) -> caller treats all files as candidates.
bool eval_query_base(const TrigramQuery& q,
                     const MmapIndex& idx,
                     std::vector<DocId>& out,
                     const std::atomic<bool>* cancel) {
    out.clear();
    if (cancel && cancel->load(std::memory_order_relaxed)) return true;

    switch (q.kind) {
        case TrigramQuery::Kind::All:
            return false;
        case TrigramQuery::Kind::None:
            return true;
        case TrigramQuery::Kind::Atom: {
            auto r = idx.lookup_trigram(q.trigram, q.icase);
            if (!r.entry) return true; // Empty
            decode_posting_list(*r.entry, r.arena, idx.num_chunks(), out);
            return true;
        }
        case TrigramQuery::Kind::And: {
            std::vector<std::pair<u32, size_t>> order;
            order.reserve(q.children.size());
            for (size_t i = 0; i < q.children.size(); ++i) {
                u32 c = estimate_cost(q.children[i], idx);
                if (c == 0) return true;
                order.emplace_back(c, i);
            }
            std::sort(order.begin(), order.end());

            bool initialized = false;
            std::vector<DocId> sub;
            std::vector<DocId> merged;

            for (const auto& [cost, ci] : order) {
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    out.clear();
                    return true;
                }
                const auto& ch = q.children[ci];
                if (!initialized) {
                    if (eval_query_base(ch, idx, out, cancel)) {
                        initialized = true;
                        if (out.empty()) return true;
                    }
                } else {
                    if (ch.kind == TrigramQuery::Kind::Atom) {
                        auto r = idx.lookup_trigram(ch.trigram, ch.icase);
                        if (!r.entry) {
                            out.clear();
                            return true;
                        }
                        intersect_candidates_with_posting(out, *r.entry, r.arena, idx.num_chunks());
                        if (out.empty()) return true;
                    } else {
                        // Skip expensive OR-union subtrees when candidate set is already tight
                        // relative to the OR subtree's estimated doc_freq (sound: exact verification follows!)
                        if (ch.kind == TrigramQuery::Kind::Or && cost > out.size() * 4 && out.size() <= 1024) {
                            continue;
                        }
                        if (eval_query_base(ch, idx, sub, cancel)) {
                            simd::intersect_sorted_u32(out.data(), out.size(),
                                                       sub.data(), sub.size(), merged);
                            out.swap(merged);
                            if (out.empty()) return true;
                        }
                    }
                }
            }
            return initialized;
        }
        case TrigramQuery::Kind::Or: {
            std::vector<DocId> sub;
            std::vector<DocId> merged;
            for (const auto& ch : q.children) {
                if (cancel && cancel->load(std::memory_order_relaxed)) {
                    out.clear();
                    return true;
                }
                if (!eval_query_base(ch, idx, sub, cancel)) {
                    // Branch is All -> whole OR is All
                    out.clear();
                    return false;
                }
                if (out.empty()) {
                    out.swap(sub);
                } else if (!sub.empty()) {
                    union_sorted_u32(out, sub, merged);
                    out.swap(merged);
                }
            }
            return true;
        }
    }
    return false;
}

// Evaluate TrigramQuery against DeltaSegment -> sorted delta DocIds.
bool eval_query_delta(const TrigramQuery& q,
                      const DeltaSegment& delta,
                      std::vector<DocId>& out) {
    out.clear();
    switch (q.kind) {
        case TrigramQuery::Kind::All:
            return false;
        case TrigramQuery::Kind::None:
            return true;
        case TrigramQuery::Kind::Atom: {
            const auto* lst = delta.lookup_trigram(q.trigram, q.icase);
            if (lst) out = *lst;
            return true;
        }
        case TrigramQuery::Kind::And: {
            bool initialized = false;
            std::vector<DocId> sub;
            std::vector<DocId> merged;
            for (const auto& ch : q.children) {
                if (!initialized) {
                    if (eval_query_delta(ch, delta, out)) {
                        initialized = true;
                        if (out.empty()) return true;
                    }
                } else {
                    if (eval_query_delta(ch, delta, sub)) {
                        simd::intersect_sorted_u32(out.data(), out.size(),
                                                   sub.data(), sub.size(), merged);
                        out.swap(merged);
                        if (out.empty()) return true;
                    }
                }
            }
            return initialized;
        }
        case TrigramQuery::Kind::Or: {
            std::vector<DocId> sub;
            std::vector<DocId> merged;
            for (const auto& ch : q.children) {
                if (!eval_query_delta(ch, delta, sub)) {
                    out.clear();
                    return false;
                }
                if (out.empty()) out.swap(sub);
                else if (!sub.empty()) {
                    union_sorted_u32(out, sub, merged);
                    out.swap(merged);
                }
            }
            return true;
        }
    }
    return false;
}

// Extract specific context lines around matching lines when -A / -B / -C > 0
void populate_context_lines(const u8* data, size_t len,
                            std::vector<LineMatch>& matches,
                            u32 before_ctx, u32 after_ctx) {
    if ((before_ctx == 0 && after_ctx == 0) || matches.empty()) return;

    // Build line offset table once for this matching file
    std::vector<std::pair<size_t, size_t>> lines;
    lines.reserve(256);
    size_t pos = 0;
    while (pos < len) {
        size_t nl = simd::find_byte(data + pos, len - pos, '\n');
        size_t end = pos + nl;
        size_t tend = (end > pos && data[end - 1] == '\r') ? (end - 1) : end;
        lines.emplace_back(pos, tend - pos);
        pos = (end < len) ? (end + 1) : len;
    }

    const u32 total_lines = static_cast<u32>(lines.size());
    std::unordered_set<u32> matched_set;
    for (const auto& m : matches) matched_set.insert(m.line_no);

    u32 last_emitted = 0;
    for (size_t mi = 0; mi < matches.size(); ++mi) {
        auto& m = matches[mi];
        if (before_ctx > 0) {
            u32 start_ln = (m.line_no > before_ctx) ? (m.line_no - before_ctx) : 1;
            start_ln = std::max(start_ln, last_emitted + 1);
            for (u32 ln = start_ln; ln < m.line_no; ++ln) {
                if (matched_set.count(ln)) continue;
                auto [off, sz] = lines[ln - 1];
                m.before.push_back({ln, std::string(reinterpret_cast<const char*>(data + off), sz)});
                last_emitted = ln;
            }
        }
        last_emitted = std::max(last_emitted, m.line_no);
        if (after_ctx > 0) {
            u32 next_match_ln = (mi + 1 < matches.size()) ? matches[mi + 1].line_no : (total_lines + 1);
            u32 end_ln = std::min<u32>(total_lines, m.line_no + after_ctx);
            end_ln = std::min<u32>(end_ln, next_match_ln - 1);
            for (u32 ln = m.line_no + 1; ln <= end_ln; ++ln) {
                if (matched_set.count(ln)) continue;
                auto [off, sz] = lines[ln - 1];
                m.after.push_back({ln, std::string(reinterpret_cast<const char*>(data + off), sz)});
                last_emitted = ln;
            }
        }
    }
}

// Verify a single file buffer against CompiledPattern
bool verify_file_buffer(const u8* data, size_t len,
                        const std::string& rel_path,
                        const CompiledPattern& compiled,
                        const SearchOptions& opts,
                        FileSearchResult& out_res,
                        const std::atomic<bool>* cancel) {
    if (len == 0) return false;
    // Binary check (unless -a / --text / -uuu)
    if (!opts.search_binary) {
        size_t probe = std::min<size_t>(len, 8192);
        if (simd::contains_nul(data, probe)) return false;
    }

    out_res.rel_path = rel_path;
    out_res.matches.clear();
    out_res.match_count = 0;

    if (!opts.invert_match) {
        compiled.search_buffer(data, len, [&](u32 line_no, std::string_view line,
                                              const std::vector<MatchSpan>& spans) -> bool {
            out_res.match_count++;
            if (opts.files_with_matches) {
                return false; // One match is enough!
            }
            if (!opts.count_only) {
                LineMatch lm;
                lm.line_no = line_no;
                lm.line = std::string(line);
                lm.spans = spans;
                out_res.matches.push_back(std::move(lm));
            }
            if (opts.max_count_per_file > 0 && out_res.match_count >= opts.max_count_per_file) {
                return false;
            }
            return true;
        }, cancel);
    } else {
        // Inverted match (-v): emit lines that do NOT match
        size_t pos = 0;
        u32 line_no = 1;
        std::vector<MatchSpan> spans;
        while (pos < len) {
            if (cancel && (line_no & 63) == 0 && cancel->load(std::memory_order_relaxed)) break;
            size_t nl = simd::find_byte(data + pos, len - pos, '\n');
            size_t end = pos + nl;
            size_t tend = (end > pos && data[end - 1] == '\r') ? (end - 1) : end;
            std::string_view line_sv(reinterpret_cast<const char*>(data + pos), tend - pos);
            if (!compiled.match_line(line_sv, spans)) {
                out_res.match_count++;
                if (opts.files_with_matches) break;
                if (!opts.count_only) {
                    LineMatch lm;
                    lm.line_no = line_no;
                    lm.line = std::string(line_sv);
                    out_res.matches.push_back(std::move(lm));
                }
                if (opts.max_count_per_file > 0 && out_res.match_count >= opts.max_count_per_file) {
                    break;
                }
            }
            pos = (end < len) ? (end + 1) : len;
            ++line_no;
        }
    }

    if (out_res.match_count > 0 && !opts.count_only && !opts.files_with_matches &&
        (opts.before_context > 0 || opts.after_context > 0)) {
        populate_context_lines(data, len, out_res.matches, opts.before_context, opts.after_context);
    }
    return out_res.match_count > 0;
}

bool verify_file_on_disk(const std::filesystem::path& root,
                         const std::string& rel_path,
                         u64 size_hint,
                         const CompiledPattern& compiled,
                         const SearchOptions& opts,
                         FileSearchResult& out_res,
                         std::vector<u8>& reusable_buf,
                         const std::atomic<bool>* cancel) {
    std::string abs_path = (root / rel_path).string();
    // For larger files (>= 256 KiB), zero-copy mmap avoids page-cache copy & heap churn;
    // for smaller files, a single open+pread into reusable_buf avoids mmap setup overhead!
    if (size_hint >= 256 * 1024) {
        MmapFile mf;
        if (!mf.open(abs_path)) return false;
        return verify_file_buffer(mf.data(), mf.size(), rel_path, compiled, opts, out_res, cancel);
    } else {
        if (!BatchedFileReader::read_file_sync(abs_path, reusable_buf, size_hint)) return false;
        return verify_file_buffer(reusable_buf.data(), reusable_buf.size(), rel_path, compiled, opts, out_res, cancel);
    }
}

struct ChunkSlice {
    u32 byte_off = 0;
    u32 byte_len = 0;
    u32 start_line_no = 1;
};

// Verify ONLY matching 32 KiB line-aligned byte slices of a large file via pread!
bool verify_chunks_on_disk(const std::filesystem::path& root,
                           const std::string& rel_path,
                           const std::vector<ChunkSlice>& slices,
                           const CompiledPattern& compiled,
                           const SearchOptions& opts,
                           FileSearchResult& out_res,
                           std::vector<u8>& reusable_buf,
                           const std::atomic<bool>* cancel) {
    std::string abs_path = (root / rel_path).string();
    int fd = ::open(abs_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    out_res.rel_path = rel_path;
    out_res.matches.clear();
    out_res.match_count = 0;

    size_t i = 0;
    while (i < slices.size()) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        // Coalesce contiguous adjacent chunks into a single pread!
        u32 off = slices[i].byte_off;
        u32 len = slices[i].byte_len;
        u32 base_line = slices[i].start_line_no;
        size_t j = i + 1;
        while (j < slices.size() && slices[j].byte_off == off + len && len < 256 * 1024) {
            len += slices[j].byte_len;
            ++j;
        }
        i = j;

        reusable_buf.resize(len);
        size_t total_read = 0;
        while (total_read < len) {
            ssize_t n = ::pread(fd, reusable_buf.data() + total_read, len - total_read,
                                static_cast<off_t>(off + total_read));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            total_read += static_cast<size_t>(n);
        }
        if (total_read == 0) continue;

        bool stop_file = false;
        compiled.search_buffer(reusable_buf.data(), total_read,
            [&](u32 rel_line_no, std::string_view line, const std::vector<MatchSpan>& spans) -> bool {
                out_res.match_count++;
                if (opts.files_with_matches) {
                    stop_file = true;
                    return false;
                }
                if (!opts.count_only) {
                    LineMatch lm;
                    lm.line_no = base_line - 1 + rel_line_no;
                    lm.line = std::string(line);
                    lm.spans = spans;
                    out_res.matches.push_back(std::move(lm));
                }
                if (opts.max_count_per_file > 0 && out_res.match_count >= opts.max_count_per_file) {
                    stop_file = true;
                    return false;
                }
                return true;
            }, cancel);
        if (stop_file) break;
    }

    ::close(fd);
    return out_res.match_count > 0;
}

// Fetch a single 1-based line from a file on disk (used for --def fast path)
bool read_single_line(const std::filesystem::path& abs_path, u32 target_line, std::string& out_line) {
    std::vector<u8> buf;
    if (!BatchedFileReader::read_file_sync(abs_path.string(), buf)) return false;
    size_t pos = 0;
    u32 ln = 1;
    while (pos < buf.size()) {
        size_t nl = simd::find_byte(buf.data() + pos, buf.size() - pos, '\n');
        size_t end = pos + nl;
        if (ln == target_line) {
            size_t tend = (end > pos && buf[end - 1] == '\r') ? (end - 1) : end;
            out_line.assign(reinterpret_cast<const char*>(buf.data() + pos), tend - pos);
            return true;
        }
        pos = (end < buf.size()) ? (end + 1) : buf.size();
        ++ln;
    }
    return false;
}

} // namespace

SearchResponse search_snapshot(const IndexSnapshot& snap,
                               const SearchOptions& opts,
                               const std::string& subdir_prefix,
                               const std::atomic<bool>* cancel) {
    Stopwatch total_sw;
    SearchResponse resp;
    if (!snap.base || !snap.base->valid()) {
        resp.error = "Invalid index snapshot";
        return resp;
    }

    const MmapIndex& base = *snap.base;
    const DeltaSegment* delta = snap.delta.get();
    resp.stats.used_index = true;
    resp.stats.total_indexed_docs = base.num_docs() + (delta ? delta->live_delta_docs : 0);

    // 1. `--files` fast mode: list all indexed files matching filters without touching disk!
    if (opts.list_files) {
        for (DocId d = 0; d < base.num_docs(); ++d) {
            if (delta && delta->is_base_tombstoned(d)) continue;
            std::string_view p = base.file_path(d);
            LangId lang = static_cast<LangId>(base.file(d).lang_id);
            if (matches_path_filters(p, lang, opts, subdir_prefix)) {
                resp.listed_files.emplace_back(p);
            }
        }
        if (delta) {
            for (const auto& dd : delta->docs) {
                if (dd.deleted) continue;
                if (matches_path_filters(dd.rel_path, dd.lang, opts, subdir_prefix)) {
                    resp.listed_files.push_back(dd.rel_path);
                }
            }
        }
        std::sort(resp.listed_files.begin(), resp.listed_files.end());
        resp.stats.total_ms = total_sw.elapsed_ms();
        return resp;
    }

    // 2. `--def <symbol>` sub-millisecond symbol definition lookup!
    if (opts.def_only) {
        Stopwatch plan_sw;
        bool ic = opts.regex_opts.case_insensitive;
        if (!ic && opts.regex_opts.smart_case) {
            ic = true;
            for (char c : opts.pattern) {
                if (c >= 'A' && c <= 'Z') { ic = false; break; }
            }
        }

        struct DefHit {
            std::string rel_path;
            u32 line_no;
            SymbolKind kind;
            std::string sym_name;
        };
        std::vector<DefHit> hits;

        std::vector<const SymbolEntry*> base_syms;
        base.find_symbols(opts.pattern, ic, base_syms);
        for (const SymbolEntry* se : base_syms) {
            if (delta && delta->is_base_tombstoned(se->doc_id)) continue;
            std::string_view p = base.file_path(se->doc_id);
            LangId lang = static_cast<LangId>(base.file(se->doc_id).lang_id);
            if (!matches_path_filters(p, lang, opts, subdir_prefix)) continue;
            hits.push_back({std::string(p), se->line_no(), se->kind(),
                            std::string(base.symbol_name(*se))});
        }

        if (delta) {
            std::string low_pat(opts.pattern.size(), '\0');
            for (size_t i = 0; i < opts.pattern.size(); ++i) {
                low_pat[i] = static_cast<char>(ascii_lower(static_cast<u8>(opts.pattern[i])));
            }
            for (const auto& dd : delta->docs) {
                if (dd.deleted) continue;
                if (!matches_path_filters(dd.rel_path, dd.lang, opts, subdir_prefix)) continue;
                for (const auto& s : dd.symbols) {
                    if (s.name.size() != opts.pattern.size()) continue;
                    bool eq = ic
                        ? simd::memeq_icase_ascii(reinterpret_cast<const u8*>(s.name.data()),
                                                  reinterpret_cast<const u8*>(low_pat.data()),
                                                  low_pat.size())
                        : (s.name == opts.pattern);
                    if (eq) {
                        hits.push_back({dd.rel_path, s.line_no, s.kind, s.name});
                    }
                }
            }
        }
        resp.stats.plan_ms = plan_sw.elapsed_ms();

        Stopwatch ver_sw;
        std::sort(hits.begin(), hits.end(), [](const DefHit& a, const DefHit& b) {
            if (a.rel_path != b.rel_path) return a.rel_path < b.rel_path;
            return a.line_no < b.line_no;
        });

        for (const auto& h : hits) {
            if (opts.max_total_matches > 0 && resp.stats.matched_lines >= opts.max_total_matches) break;
            std::string line_text;
            if (!read_single_line(snap.root / h.rel_path, h.line_no, line_text)) continue;

            LineMatch lm;
            lm.line_no = h.line_no;
            lm.line = std::move(line_text);
            lm.is_definition = true;
            lm.def_kind = h.kind;
            // Highlight symbol name span in line
            size_t pos = lm.line.find(h.sym_name);
            if (pos != std::string::npos) {
                lm.spans.push_back({static_cast<u32>(pos), static_cast<u32>(pos + h.sym_name.size())});
            }

            if (resp.files.empty() || resp.files.back().rel_path != h.rel_path) {
                FileSearchResult fr;
                fr.rel_path = h.rel_path;
                fr.has_def = true;
                resp.files.push_back(std::move(fr));
            }
            resp.files.back().matches.push_back(std::move(lm));
            resp.files.back().match_count++;
            resp.stats.matched_lines++;
        }
        resp.stats.candidate_docs = static_cast<u32>(hits.size());
        resp.stats.verified_docs = static_cast<u32>(resp.files.size());
        resp.stats.matched_files = static_cast<u32>(resp.files.size());
        resp.stats.verify_ms = ver_sw.elapsed_ms();
        resp.stats.total_ms = total_sw.elapsed_ms();
        return resp;
    }

    // 3. Compile pattern (AST -> TrigramQuery + SIMD Prefilter + Thompson NFA / Lazy DFA)
    CompiledPattern compiled;
    if (!compiled.compile(opts.pattern, opts.regex_opts, resp.error)) {
        return resp;
    }

    Stopwatch plan_sw;
    const TrigramQuery& tq = compiled.trigram_query();
    resp.stats.trigram_plan_desc = tq.to_string();

    struct CandidateFile {
        std::string rel_path;
        u64 size_hint = 0;
        bool has_def_boost = false;
        bool full_file = true;
        std::vector<ChunkSlice> chunks;
    };
    std::vector<CandidateFile> candidates;

    // Optional path-trigram pre-filter for -g globs (operates on FileIds)!
    bool has_path_filter_files = false;
    std::vector<DocId> path_filtered_files;
    if (!opts.globs.empty() && (base.header().flags & IDX_FLAG_HAS_PATH_INDEX)) {
        auto lits = extract_glob_literals(opts.globs);
        if (!lits.empty()) {
            bool ok = true;
            for (const auto& lit : lits) {
                for (size_t i = 0; i + 3 <= lit.size(); ++i) {
                    Trigram pt = pack_trigram(static_cast<u8>(lit[i]),
                                              static_cast<u8>(lit[i + 1]),
                                              static_cast<u8>(lit[i + 2]));
                    auto r = base.lookup_path_trigram(pt);
                    if (!r.entry) {
                        path_filtered_files.clear();
                        has_path_filter_files = true;
                        ok = false;
                        break;
                    }
                    if (!has_path_filter_files) {
                        decode_posting_list(*r.entry, r.arena, base.num_docs(), path_filtered_files);
                        has_path_filter_files = true;
                    } else {
                        intersect_candidates_with_posting(path_filtered_files, *r.entry, r.arena, base.num_docs());
                    }
                    if (path_filtered_files.empty()) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) break;
            }
        }
    }

    // Evaluate query against Base MmapIndex (returns ChunkIds!)
    std::vector<DocId> base_chunks;
    bool base_restricted = false;
    if (!opts.invert_match) {
        base_restricted = eval_query_base(tq, base, base_chunks, cancel);
    }

    if (cancel && cancel->load(std::memory_order_relaxed)) {
        resp.stats.cancelled = true;
        return resp;
    }

    // Fast O(1) FileId filter bitset when path_filtered_files is active
    std::vector<u64> allowed_file_bits;
    if (has_path_filter_files && base_restricted) {
        allowed_file_bits.assign((static_cast<size_t>(base.num_docs()) + 63) / 64, 0);
        for (DocId fid : path_filtered_files) {
            allowed_file_bits[fid >> 6] |= (1ULL << (fid & 63));
        }
    }

    // Optional symbol boost set
    std::unordered_set<DocId> def_base_docs;
    if (opts.boost_defs && compiled.is_pure_literal() && compiled.literal().size() >= 2) {
        std::vector<const SymbolEntry*> syms;
        base.find_symbols(compiled.literal(), compiled.effective_icase(), syms);
        for (const auto* s : syms) def_base_docs.insert(s->doc_id);
    }

    const bool need_whole_file =
        opts.invert_match || opts.before_context > 0 || opts.after_context > 0;

    if (base_restricted) {
        // Group matching ChunkIds by parent FileId!
        // Because each file's chunks are contiguous and ascending in byte_off,
        // chunks belonging to the same file appear consecutively in `base_chunks`!
        candidates.reserve(std::min<size_t>(base_chunks.size(), base.num_docs()) +
                           (delta ? delta->live_delta_docs : 0));
        DocId cur_fid = INVALID_DOC_ID;
        bool cur_fid_ok = false;
        size_t cur_cand_idx = SIZE_MAX;

        for (DocId cid : base_chunks) {
            const auto& ch = base.chunk(cid);
            DocId fid = ch.file_id;
            if (fid != cur_fid) {
                cur_fid = fid;
                cur_fid_ok = false;
                cur_cand_idx = SIZE_MAX;

                if (delta && delta->is_base_tombstoned(fid)) continue;
                if (has_path_filter_files &&
                    !((allowed_file_bits[fid >> 6] >> (fid & 63)) & 1ULL)) {
                    continue;
                }
                const auto& fe = base.file(fid);
                std::string_view p = base.file_path(fid);
                if (!matches_path_filters(p, static_cast<LangId>(fe.lang_id), opts, subdir_prefix)) {
                    continue;
                }
                cur_fid_ok = true;
                cur_cand_idx = candidates.size();
                CandidateFile cf;
                cf.rel_path = std::string(p);
                cf.has_def_boost = def_base_docs.count(fid) > 0;
                if (need_whole_file || fe.chunk_count <= 1) {
                    cf.full_file = true;
                    cf.size_hint = fe.file_size;
                } else {
                    cf.full_file = false;
                    cf.size_hint = ch.byte_len;
                    cf.chunks.push_back({ch.byte_off, ch.byte_len, ch.start_line_no});
                }
                candidates.push_back(std::move(cf));
            } else if (cur_fid_ok && !candidates[cur_cand_idx].full_file) {
                auto& cf = candidates[cur_cand_idx];
                cf.chunks.push_back({ch.byte_off, ch.byte_len, ch.start_line_no});
                cf.size_hint += ch.byte_len;
            }
        }
    } else if (has_path_filter_files) {
        // Content query unrestricted (e.g. <3 chars), but path glob restricted!
        resp.stats.used_linear_fallback = true;
        candidates.reserve(path_filtered_files.size());
        for (DocId fid : path_filtered_files) {
            if (delta && delta->is_base_tombstoned(fid)) continue;
            std::string_view p = base.file_path(fid);
            const auto& fe = base.file(fid);
            if (!matches_path_filters(p, static_cast<LangId>(fe.lang_id), opts, subdir_prefix)) continue;
            CandidateFile cf;
            cf.rel_path = std::string(p);
            cf.size_hint = fe.file_size;
            cf.has_def_boost = def_base_docs.count(fid) > 0;
            cf.full_file = true;
            candidates.push_back(std::move(cf));
        }
    } else {
        // Query unrestricted (e.g., < 3 chars, `.*`, or `-v`): linear scan over indexed files
        resp.stats.used_linear_fallback = true;
        candidates.reserve(base.num_docs());
        for (DocId d = 0; d < base.num_docs(); ++d) {
            if (delta && delta->is_base_tombstoned(d)) continue;
            std::string_view p = base.file_path(d);
            const auto& fe = base.file(d);
            if (!matches_path_filters(p, static_cast<LangId>(fe.lang_id), opts, subdir_prefix)) continue;
            CandidateFile cf;
            cf.rel_path = std::string(p);
            cf.size_hint = fe.file_size;
            cf.has_def_boost = def_base_docs.count(d) > 0;
            cf.full_file = true;
            candidates.push_back(std::move(cf));
        }
    }

    // Evaluate query against LSM DeltaSegment overlay
    if (delta && delta->live_delta_docs > 0) {
        std::vector<DocId> delta_docs;
        bool delta_restricted = !opts.invert_match && eval_query_delta(tq, *delta, delta_docs);
        if (delta_restricted) {
            for (DocId did : delta_docs) {
                const auto& dd = delta->docs[did];
                if (dd.deleted) continue;
                if (!matches_path_filters(dd.rel_path, dd.lang, opts, subdir_prefix)) continue;
                candidates.push_back({dd.rel_path, dd.file_size, false, true, {}});
            }
        } else {
            for (const auto& dd : delta->docs) {
                if (dd.deleted) continue;
                if (!matches_path_filters(dd.rel_path, dd.lang, opts, subdir_prefix)) continue;
                candidates.push_back({dd.rel_path, dd.file_size, false, true, {}});
            }
        }
    }

    if (opts.boost_defs) {
        std::stable_partition(candidates.begin(), candidates.end(),
                              [](const CandidateFile& c) { return c.has_def_boost; });
    }

    resp.stats.plan_ms = plan_sw.elapsed_ms();
    resp.stats.candidate_docs = static_cast<u32>(candidates.size());

    // 4. Exact Verification Phase (sequential for tiny sets / Top-K, persistent WorkerPool for larger sets!)
    Stopwatch ver_sw;
    const size_t ncand = candidates.size();
    const u32 nthreads = (opts.threads > 0)
        ? opts.threads
        : WorkerPool::global().max_threads();

    if (ncand == 0) {
        resp.stats.verify_ms = 0.0;
        resp.stats.total_ms = total_sw.elapsed_ms();
        return resp;
    }

    u64 total_cand_bytes = 0;
    for (const auto& c : candidates) total_cand_bytes += c.size_hint;

    auto verify_cand = [&](const CandidateFile& cand, FileSearchResult& fr, std::vector<u8>& buf) -> bool {
        if (cand.full_file) {
            return verify_file_on_disk(snap.root, cand.rel_path, cand.size_hint,
                                       compiled, opts, fr, buf, cancel);
        }
        return verify_chunks_on_disk(snap.root, cand.rel_path, cand.chunks,
                                     compiled, opts, fr, buf, cancel);
    };

    const bool use_sequential =
        (opts.max_total_matches > 0 && opts.max_total_matches <= 200) ||
        (ncand < 24 && total_cand_bytes < 512ULL * 1024ULL) ||
        ncand <= 1 ||
        nthreads <= 1;

    if (use_sequential) {
        thread_local std::vector<u8> buf;
        u64 total_matched = 0;
        for (size_t i = 0; i < ncand; ++i) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                resp.stats.cancelled = true;
                break;
            }
            resp.stats.verified_docs++;
            FileSearchResult fr;
            if (verify_cand(candidates[i], fr, buf)) {
                fr.has_def = candidates[i].has_def_boost;
                total_matched += fr.match_count;
                resp.files.push_back(std::move(fr));
                if (opts.max_total_matches > 0 && total_matched >= opts.max_total_matches) {
                    break;
                }
            }
        }
        if (buf.capacity() > 2 * 1024 * 1024) std::vector<u8>().swap(buf);
    } else {
        // Persistent WorkerPool: 0 pthread_create overhead, 0 glibc arena bloat!
        const size_t chunk = (ncand < 128) ? 1 : 4;
        u32 max_w = std::min<u32>(nthreads, WorkerPool::global().max_threads());
        u32 workers_to_use = std::min<u32>(max_w, static_cast<u32>((ncand + chunk - 1) / chunk));
        std::vector<FileSearchResult> slot_results(ncand);
        std::vector<u8> slot_matched(ncand, 0);
        std::atomic<size_t> next_idx{0};
        std::atomic<u64> global_matches{0};
        std::atomic<u32> verified_cnt{0};

        WorkerPool::global().run(workers_to_use, [&](u32) {
            thread_local std::vector<u8> buf;
            while (true) {
                if (cancel && cancel->load(std::memory_order_relaxed)) break;
                if (opts.max_total_matches > 0 &&
                    global_matches.load(std::memory_order_relaxed) >= opts.max_total_matches) {
                    break;
                }
                size_t start = next_idx.fetch_add(chunk, std::memory_order_relaxed);
                if (start >= ncand) break;
                size_t end = std::min(start + chunk, ncand);
                for (size_t i = start; i < end; ++i) {
                    if (cancel && cancel->load(std::memory_order_relaxed)) break;
                    verified_cnt.fetch_add(1, std::memory_order_relaxed);
                    FileSearchResult fr;
                    if (verify_cand(candidates[i], fr, buf)) {
                        fr.has_def = candidates[i].has_def_boost;
                        global_matches.fetch_add(fr.match_count, std::memory_order_relaxed);
                        slot_results[i] = std::move(fr);
                        slot_matched[i] = 1;
                    }
                }
            }
            if (buf.capacity() > 1024 * 1024) std::vector<u8>().swap(buf);
        });

        if (cancel && cancel->load(std::memory_order_relaxed)) {
            resp.stats.cancelled = true;
        }
        resp.stats.verified_docs = verified_cnt.load();
        u64 collected = 0;
        for (size_t i = 0; i < ncand; ++i) {
            if (slot_matched[i]) {
                collected += slot_results[i].match_count;
                resp.files.push_back(std::move(slot_results[i]));
                if (opts.max_total_matches > 0 && collected >= opts.max_total_matches) break;
            }
        }
    }

    if (!opts.boost_defs) {
        std::sort(resp.files.begin(), resp.files.end(),
                  [](const FileSearchResult& a, const FileSearchResult& b) {
                      return a.rel_path < b.rel_path;
                  });
    }

    resp.stats.matched_files = static_cast<u32>(resp.files.size());
    resp.stats.matched_lines = 0;
    for (const auto& fr : resp.files) resp.stats.matched_lines += fr.match_count;
    resp.stats.verify_ms = ver_sw.elapsed_ms();
    resp.stats.total_ms = total_sw.elapsed_ms();
    return resp;
}

namespace {

// Direct parallel AVX-512/AVX2/NEON linear scanner for unindexed/tiny repos or --no-index
SearchResponse run_linear_search(const SearchOptions& opts, const std::atomic<bool>* cancel) {
    Stopwatch total_sw;
    SearchResponse resp;
    resp.stats.used_linear_fallback = true;

    std::error_code ec;
    std::filesystem::path target = opts.target_path;
    bool single_file = std::filesystem::is_regular_file(target, ec);

    WalkOptions wopts;
    wopts.respect_gitignore = opts.respect_gitignore;
    wopts.include_hidden = opts.include_hidden;
    wopts.follow_symlinks = opts.follow_symlinks;
    wopts.max_file_size = opts.max_file_size;
    wopts.globs = opts.globs;
    wopts.include_types = opts.include_types;
    wopts.exclude_types = opts.exclude_types;

    auto files = walk_directory(target, wopts);

    if (opts.list_files) {
        resp.listed_files.reserve(files.size());
        for (const auto& f : files) resp.listed_files.push_back(f.rel_path);
        resp.stats.total_ms = total_sw.elapsed_ms();
        return resp;
    }

    CompiledPattern compiled;
    if (!compiled.compile(opts.pattern, opts.regex_opts, resp.error)) {
        return resp;
    }

    std::filesystem::path base_dir = single_file ? target.parent_path() : target;
    if (base_dir.empty()) base_dir = ".";

    resp.stats.candidate_docs = static_cast<u32>(files.size());
    Stopwatch ver_sw;

    const size_t n = files.size();
    const u32 nthreads = (opts.threads > 0)
        ? opts.threads
        : WorkerPool::global().max_threads();

    if (n < 32 || (opts.max_total_matches > 0 && opts.max_total_matches <= 200) || nthreads <= 1) {
        std::vector<u8> buf;
        u64 total_matched = 0;
        for (size_t i = 0; i < n; ++i) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                resp.stats.cancelled = true;
                break;
            }
            resp.stats.verified_docs++;
            FileSearchResult fr;
            std::string display = single_file ? target.string() : files[i].rel_path;
            if (verify_file_on_disk(base_dir, files[i].rel_path, files[i].file_size,
                                    compiled, opts, fr, buf, cancel)) {
                fr.rel_path = display;
                total_matched += fr.match_count;
                resp.files.push_back(std::move(fr));
                if (opts.max_total_matches > 0 && total_matched >= opts.max_total_matches) break;
            }
        }
    } else {
        u32 workers_to_use = std::min<u32>(nthreads, static_cast<u32>((n + 7) / 8));
        std::vector<FileSearchResult> slot_results(n);
        std::vector<u8> slot_matched(n, 0);
        std::atomic<size_t> next_idx{0};
        std::atomic<u64> global_matches{0};

        WorkerPool::global().run(workers_to_use, [&](u32) {
            thread_local std::vector<u8> buf;
            while (true) {
                if (cancel && cancel->load(std::memory_order_relaxed)) break;
                if (opts.max_total_matches > 0 &&
                    global_matches.load(std::memory_order_relaxed) >= opts.max_total_matches) {
                    break;
                }
                size_t start = next_idx.fetch_add(16, std::memory_order_relaxed);
                if (start >= n) break;
                size_t end = std::min(start + 16, n);
                for (size_t i = start; i < end; ++i) {
                    if (cancel && cancel->load(std::memory_order_relaxed)) break;
                    FileSearchResult fr;
                    if (verify_file_on_disk(base_dir, files[i].rel_path, files[i].file_size,
                                            compiled, opts, fr, buf, cancel)) {
                        global_matches.fetch_add(fr.match_count, std::memory_order_relaxed);
                        slot_results[i] = std::move(fr);
                        slot_matched[i] = 1;
                    }
                }
            }
            if (buf.capacity() > 1024 * 1024) std::vector<u8>().swap(buf);
        });

        resp.stats.verified_docs = static_cast<u32>(n);
        for (size_t i = 0; i < n; ++i) {
            if (opts.files_without_match) {
                if (!slot_matched[i]) {
                    FileSearchResult fr;
                    fr.rel_path = single_file ? target.string() : files[i].rel_path;
                    fr.match_count = 0;
                    resp.files.push_back(std::move(fr));
                }
            } else if (slot_matched[i]) {
                resp.files.push_back(std::move(slot_results[i]));
            }
        }
    }

    if (opts.files_without_match && (n < 32 || (opts.max_total_matches > 0 && opts.max_total_matches <= 200) || nthreads <= 1)) {
        std::unordered_set<std::string> matched_paths;
        for (const auto& fr : resp.files) matched_paths.insert(fr.rel_path);
        resp.files.clear();
        for (size_t i = 0; i < n; ++i) {
            std::string display = single_file ? target.string() : files[i].rel_path;
            if (!matched_paths.count(display)) {
                FileSearchResult fr;
                fr.rel_path = std::move(display);
                resp.files.push_back(std::move(fr));
            }
        }
    }

    resp.stats.matched_files = static_cast<u32>(resp.files.size());
    for (const auto& fr : resp.files) resp.stats.matched_lines += fr.match_count;
    resp.stats.verify_ms = ver_sw.elapsed_ms();
    resp.stats.total_ms = total_sw.elapsed_ms();
    return resp;
}

SearchResponse run_search_single_path(const SearchOptions& opts, const std::atomic<bool>* cancel) {
    std::filesystem::path idx_root;
    std::filesystem::path idx_path;
    std::error_code ec;

    bool single_file = std::filesystem::is_regular_file(opts.target_path, ec);

    if (!opts.force_no_index && !single_file && !opts.include_hidden && opts.respect_gitignore &&
        !opts.search_binary && !opts.follow_symlinks && !opts.files_without_match &&
        opts.max_file_size >= 32ULL * 1024 * 1024 &&
        locate_index_for_path(opts.target_path, idx_root, idx_path)) {

        auto base = std::make_shared<MmapIndex>();
        if (base->open(idx_path)) {
            auto canon_target = std::filesystem::weakly_canonical(opts.target_path, ec);
            std::string subdir_prefix;
            if (!ec && canon_target != idx_root) {
                auto rel = std::filesystem::relative(canon_target, idx_root, ec);
                if (!ec && !rel.empty() && rel.string() != ".") {
                    subdir_prefix = rel.string();
                    if (!subdir_prefix.empty() && subdir_prefix.back() != '/') {
                        subdir_prefix.push_back('/');
                    }
                }
            }

            auto delta = build_freshness_delta(idx_root, *base, opts.strict_freshness);

            IndexSnapshot snap;
            snap.root = idx_root;
            snap.index_path = idx_path;
            snap.base = std::move(base);
            snap.delta = std::move(delta);

            auto resp = search_snapshot(snap, opts, subdir_prefix, cancel);
            if (!subdir_prefix.empty()) {
                for (auto& fr : resp.files) {
                    if (fr.rel_path.rfind(subdir_prefix, 0) == 0) {
                        fr.rel_path.erase(0, subdir_prefix.size());
                    }
                }
                for (auto& p : resp.listed_files) {
                    if (p.rfind(subdir_prefix, 0) == 0) {
                        p.erase(0, subdir_prefix.size());
                    }
                }
            }
            return resp;
        }
    }

    return run_linear_search(opts, cancel);
}

} // namespace

SearchResponse search_stdin_buffer(const u8* data, size_t len, const SearchOptions& opts) {
    Stopwatch sw;
    SearchResponse resp;
    resp.stats.used_linear_fallback = true;
    CompiledPattern compiled;
    if (!compiled.compile(opts.pattern, opts.regex_opts, resp.error)) {
        return resp;
    }
    resp.stats.candidate_docs = 1;
    resp.stats.verified_docs = 1;
    FileSearchResult fr;
    if (verify_file_buffer(data, len, "<stdin>", compiled, opts, fr, nullptr)) {
        resp.stats.matched_files = 1;
        resp.stats.matched_lines = fr.match_count;
        resp.files.push_back(std::move(fr));
    }
    resp.stats.total_ms = sw.elapsed_ms();
    return resp;
}

SearchResponse run_search(const SearchOptions& opts, const std::atomic<bool>* cancel) {
    if (opts.search_stdin && opts.target_paths.empty()) {
        std::vector<u8> buf;
        u8 chunk[65536];
        while (true) {
            ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            buf.insert(buf.end(), chunk, chunk + n);
        }
        return search_stdin_buffer(buf.data(), buf.size(), opts);
    }

    if (opts.target_paths.size() <= 1 && !opts.search_stdin) {
        return run_search_single_path(opts, cancel);
    }

    // Multiple target paths (and optional stdin '-')
    Stopwatch total_sw;
    SearchResponse merged;
    std::error_code ec;

    if (opts.search_stdin) {
        SearchOptions sin_opts = opts;
        sin_opts.target_paths.clear();
        auto r = run_search(sin_opts, cancel);
        if (!r.error.empty()) return r;
        merged.files.insert(merged.files.end(),
                            std::make_move_iterator(r.files.begin()),
                            std::make_move_iterator(r.files.end()));
        merged.stats.matched_files += r.stats.matched_files;
        merged.stats.matched_lines += r.stats.matched_lines;
    }

    for (const auto& tp : opts.target_paths) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            merged.stats.cancelled = true;
            break;
        }
        SearchOptions sub = opts;
        sub.target_path = tp;
        sub.target_paths.clear();
        sub.search_stdin = false;

        auto r = run_search_single_path(sub, cancel);
        if (!r.error.empty()) {
            merged.error = r.error;
            return merged;
        }

        bool is_dir = std::filesystem::is_directory(tp, ec);
        std::string prefix;
        if (is_dir && tp.string() != ".") {
            prefix = tp.string();
            while (prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();
            prefix.push_back('/');
        }

        for (auto& fr : r.files) {
            if (!prefix.empty()) fr.rel_path = prefix + fr.rel_path;
            merged.files.push_back(std::move(fr));
        }
        for (auto& p : r.listed_files) {
            if (!prefix.empty()) p = prefix + p;
            merged.listed_files.push_back(std::move(p));
        }
        merged.stats.used_index = merged.stats.used_index || r.stats.used_index;
        merged.stats.used_linear_fallback = merged.stats.used_linear_fallback || r.stats.used_linear_fallback;
        merged.stats.total_indexed_docs += r.stats.total_indexed_docs;
        merged.stats.candidate_docs += r.stats.candidate_docs;
        merged.stats.verified_docs += r.stats.verified_docs;
        merged.stats.matched_files += r.stats.matched_files;
        merged.stats.matched_lines += r.stats.matched_lines;
        merged.stats.plan_ms += r.stats.plan_ms;
        merged.stats.verify_ms += r.stats.verify_ms;

        if (opts.max_total_matches > 0 && merged.stats.matched_lines >= opts.max_total_matches) {
            break;
        }
    }

    merged.stats.total_ms = total_sw.elapsed_ms();
    return merged;
}

// ============================================================
// Output Formatting (rg-compatible text, color, context, JSON)
// ============================================================
namespace {

void write_json_escaped(std::ostream& out, std::string_view s) {
    out.put('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                } else {
                    out.put(static_cast<char>(c));
                }
        }
    }
    out.put('"');
}

void write_highlighted_line(std::ostream& out,
                            std::string_view line,
                            const std::vector<MatchSpan>& spans,
                            bool color) {
    if (!color || spans.empty()) {
        out << line;
        return;
    }
    size_t pos = 0;
    for (const auto& sp : spans) {
        if (sp.start > pos && sp.start <= line.size()) {
            out << line.substr(pos, sp.start - pos);
        }
        size_t end = std::min<size_t>(sp.end, line.size());
        if (end > sp.start) {
            out << "\033[1;31m" << line.substr(sp.start, end - sp.start) << "\033[0m";
        }
        pos = std::max(pos, end);
    }
    if (pos < line.size()) out << line.substr(pos);
}

} // namespace

void format_search_response(const SearchResponse& resp,
                            const SearchOptions& opts,
                            bool is_tty,
                            std::ostream& out) {
    if (opts.quiet) return;

    const char path_term = opts.null_sep ? '\0' : '\n';
    const char path_sep  = opts.null_sep ? '\0' : ':';
    const char ctx_sep   = opts.null_sep ? '\0' : '-';

    if (opts.list_files) {
        for (const auto& p : resp.listed_files) {
            out << p << path_term;
        }
        return;
    }

    if (opts.json_output) {
        for (const auto& fr : resp.files) {
            for (const auto& m : fr.matches) {
                out << "{\"type\":\"match\",\"data\":{\"path\":{\"text\":";
                write_json_escaped(out, fr.rel_path);
                out << "},\"lines\":{\"text\":";
                write_json_escaped(out, m.line + "\n");
                out << "},\"line_number\":" << m.line_no << ",\"submatches\":[";
                for (size_t i = 0; i < m.spans.size(); ++i) {
                    if (i > 0) out << ',';
                    const auto& sp = m.spans[i];
                    std::string_view sub = (sp.start <= m.line.size())
                        ? std::string_view(m.line).substr(sp.start, std::min<size_t>(sp.end, m.line.size()) - sp.start)
                        : std::string_view{};
                    out << "{\"match\":{\"text\":";
                    write_json_escaped(out, sub);
                    out << "},\"start\":" << sp.start << ",\"end\":" << sp.end << '}';
                }
                out << "]}}\n";
            }
        }
        return;
    }

    const bool use_color = (opts.color == ColorMode::Always) ||
                           (opts.color == ColorMode::Auto && is_tty);

    const char* c_path  = use_color ? "\033[35m" : "";
    const char* c_lno   = use_color ? "\033[32m" : "";
    const char* c_def   = use_color ? "\033[36m" : "";
    const char* c_reset = use_color ? "\033[0m" : "";

    const bool use_heading = opts.heading_explicit
        ? opts.heading
        : (is_tty && opts.with_filename && !opts.vimgrep && !opts.count_only &&
           !opts.files_with_matches && !opts.files_without_match && !opts.null_sep);

    bool has_context = (opts.before_context > 0 || opts.after_context > 0);
    bool first_file = true;

    for (const auto& fr : resp.files) {
        if (opts.files_with_matches || opts.files_without_match) {
            out << c_path << fr.rel_path << c_reset << path_term;
            continue;
        }
        if (opts.count_only) {
            if (opts.with_filename) {
                out << c_path << fr.rel_path << c_reset << path_sep;
            }
            out << fr.match_count << '\n';
            continue;
        }

        if (use_heading) {
            if (!first_file) out << '\n';
            if (opts.with_filename) {
                out << c_path << fr.rel_path << c_reset << '\n';
            }
        } else if (has_context && !first_file) {
            out << "--\n";
        }
        first_file = false;

        const bool prefix_file = opts.with_filename && !use_heading;

        for (const auto& m : fr.matches) {
            for (const auto& b : m.before) {
                if (prefix_file) out << c_path << fr.rel_path << c_reset << ctx_sep;
                if (opts.show_line_numbers) out << c_lno << b.line_no << c_reset << '-';
                out << b.text << '\n';
            }

            if (opts.vimgrep && !m.spans.empty()) {
                for (const auto& sp : m.spans) {
                    if (prefix_file) out << c_path << fr.rel_path << c_reset << path_sep;
                    out << c_lno << m.line_no << c_reset << ':' << (sp.start + 1) << ':';
                    write_highlighted_line(out, m.line, {sp}, use_color);
                    out << '\n';
                }
            } else if (opts.only_matching && !m.spans.empty()) {
                for (const auto& sp : m.spans) {
                    if (prefix_file) out << c_path << fr.rel_path << c_reset << path_sep;
                    if (opts.show_line_numbers) out << c_lno << m.line_no << c_reset << ':';
                    if (opts.show_column) out << (sp.start + 1) << ':';
                    size_t end = std::min<size_t>(sp.end, m.line.size());
                    if (sp.start <= end) {
                        out << m.line.substr(sp.start, end - sp.start);
                    }
                    out << '\n';
                }
            } else {
                if (prefix_file) out << c_path << fr.rel_path << c_reset << path_sep;
                if (opts.show_line_numbers) out << c_lno << m.line_no << c_reset << ':';
                if (opts.show_column && !m.spans.empty()) {
                    out << (m.spans[0].start + 1) << ':';
                }
                if (m.is_definition) {
                    out << c_def << "[" << symbol_kind_name(m.def_kind) << "]" << c_reset << ' ';
                }
                write_highlighted_line(out, m.line, m.spans, use_color);
                out << '\n';
            }

            for (const auto& a : m.after) {
                if (prefix_file) out << c_path << fr.rel_path << c_reset << ctx_sep;
                if (opts.show_line_numbers) out << c_lno << a.line_no << c_reset << '-';
                out << a.text << '\n';
            }
        }
    }
}

} // namespace scry
