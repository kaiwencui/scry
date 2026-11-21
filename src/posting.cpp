#include "posting.hpp"

#include <bit>

namespace scry {

namespace {

inline void write_varint(std::vector<u8>& buf, u32 val) {
    while (val >= 0x80u) {
        buf.push_back(static_cast<u8>((val & 0x7Fu) | 0x80u));
        val >>= 7;
    }
    buf.push_back(static_cast<u8>(val));
}

inline u32 read_varint(const u8*& p) noexcept {
    u32 val = 0;
    u32 shift = 0;
    while (true) {
        u8 b = *p++;
        val |= static_cast<u32>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) break;
        shift += 7;
    }
    return val;
}

// Header for each 128-doc BP128 block (12 bytes)
struct alignas(4) Bp128BlockHeader {
    u32 first_doc; // doc[0] of this block (delta[0] == 0)
    u32 max_doc;   // doc[127] of this block (enables O(1) block skipping!)
    u8 bit_width;  // 0..32 -> payload is 16 * bit_width bytes (always 4-byte aligned)
    u8 pad[3];
};
static_assert(sizeof(Bp128BlockHeader) == 12);

} // namespace

TrigramEntry encode_posting_list(Trigram t,
                                 std::span<const DocId> docs,
                                 u32 total_docs,
                                 std::vector<u8>& arena,
                                 bool has_upper_variants) {
    TrigramEntry entry;
    entry.doc_freq = static_cast<u32>(docs.size());

    // Tier 0: Inline (<= 2 docs directly inside TrigramEntry!)
    if (docs.size() <= 2) {
        entry.trigram_meta = TrigramEntry::pack_meta(t, PostingEncoding::Inline, has_upper_variants);
        u64 packed = 0;
        if (docs.size() >= 1) packed |= static_cast<u64>(docs[0]);
        if (docs.size() == 2) packed |= (static_cast<u64>(docs[1]) << 32);
        entry.offset_or_inline = packed;
        return entry;
    }

    // Tier 2: Dense Bitset if doc_freq >= total_docs / 16 (and total_docs >= 256)
    if (total_docs >= 256 && docs.size() * 16 >= total_docs) {
        // Align arena offset to 8 bytes for fast u64/AVX-512 loads
        size_t aligned = align_up(arena.size(), 8);
        arena.resize(aligned, 0);
        entry.trigram_meta = TrigramEntry::pack_meta(t, PostingEncoding::Bitset, has_upper_variants);
        entry.offset_or_inline = static_cast<u64>(aligned);

        size_t n_words = (static_cast<size_t>(total_docs) + 63) / 64;
        arena.resize(aligned + n_words * sizeof(u64), 0);
        auto* words = reinterpret_cast<u64*>(arena.data() + aligned);
        for (DocId d : docs) {
            words[d >> 6] |= (1ULL << (d & 63));
        }
        return entry;
    }

    // Tier 1: SIMD-BP128 block-packed deltas + varint tail
    size_t aligned = align_up(arena.size(), 4);
    arena.resize(aligned, 0);
    entry.trigram_meta = TrigramEntry::pack_meta(t, PostingEncoding::BP128, has_upper_variants);
    entry.offset_or_inline = static_cast<u64>(aligned);

    size_t pos = 0;
    const size_t n = docs.size();
    alignas(64) u32 deltas[128];

    while (pos + 128 <= n) {
        u32 first = docs[pos];
        u32 last = docs[pos + 127];
        deltas[0] = 0;
        for (int i = 1; i < 128; ++i) {
            deltas[i] = docs[pos + i] - docs[pos + i - 1];
        }
        u8 bw = simd::max_bit_width_128(deltas);

        Bp128BlockHeader hdr{};
        hdr.first_doc = first;
        hdr.max_doc = last;
        hdr.bit_width = bw;

        size_t hdr_off = arena.size();
        size_t payload_bytes = static_cast<size_t>(bw) * 16;
        arena.resize(hdr_off + sizeof(Bp128BlockHeader) + payload_bytes);
        std::memcpy(arena.data() + hdr_off, &hdr, sizeof(hdr));
        if (payload_bytes > 0) {
            simd::bp128_pack(deltas, bw, arena.data() + hdr_off + sizeof(Bp128BlockHeader));
        }
        pos += 128;
    }

    // Varint delta tail for remaining < 128 docs
    if (pos < n) {
        u32 prev = 0;
        for (; pos < n; ++pos) {
            u32 d = docs[pos] - prev;
            write_varint(arena, d);
            prev = docs[pos];
        }
    }

    return entry;
}

void decode_posting_list(const TrigramEntry& entry,
                         const u8* arena,
                         u32 total_docs,
                         std::vector<DocId>& out) {
    out.clear();
    if (entry.doc_freq == 0) return;
    out.reserve(entry.doc_freq);

    switch (entry.encoding()) {
        case PostingEncoding::Inline: {
            if (entry.doc_freq >= 1) {
                out.push_back(static_cast<u32>(entry.offset_or_inline & 0xFFFFFFFFu));
            }
            if (entry.doc_freq >= 2) {
                out.push_back(static_cast<u32>(entry.offset_or_inline >> 32));
            }
            return;
        }
        case PostingEncoding::Bitset: {
            const size_t n_words = (static_cast<size_t>(total_docs) + 63) / 64;
            const auto* words = reinterpret_cast<const u64*>(arena + entry.offset_or_inline);
            for (size_t wi = 0; wi < n_words; ++wi) {
                u64 w = words[wi];
                u32 base = static_cast<u32>(wi << 6);
                while (w != 0) {
                    u32 bit = static_cast<u32>(std::countr_zero(w));
                    out.push_back(base + bit);
                    w &= (w - 1);
                }
            }
            return;
        }
        case PostingEncoding::BP128: {
            const u8* p = arena + entry.offset_or_inline;
            u32 remaining = entry.doc_freq;
            alignas(64) u32 block[128];

            while (remaining >= 128) {
                Bp128BlockHeader hdr;
                std::memcpy(&hdr, p, sizeof(hdr));
                p += sizeof(hdr);
                simd::bp128_unpack_deltas(p, hdr.bit_width, hdr.first_doc, block);
                p += static_cast<size_t>(hdr.bit_width) * 16;
                out.insert(out.end(), block, block + 128);
                remaining -= 128;
            }
            u32 acc = 0;
            for (u32 i = 0; i < remaining; ++i) {
                acc += read_varint(p);
                out.push_back(acc);
            }
            return;
        }
    }
}

void intersect_candidates_with_posting(std::vector<DocId>& candidates,
                                       const TrigramEntry& entry,
                                       const u8* arena,
                                       u32 total_docs) {
    if (candidates.empty()) return;
    if (entry.doc_freq == 0) {
        candidates.clear();
        return;
    }

    switch (entry.encoding()) {
        case PostingEncoding::Inline: {
            u32 d0 = static_cast<u32>(entry.offset_or_inline & 0xFFFFFFFFu);
            u32 d1 = static_cast<u32>(entry.offset_or_inline >> 32);
            std::vector<DocId> res;
            if (std::binary_search(candidates.begin(), candidates.end(), d0)) res.push_back(d0);
            if (entry.doc_freq == 2 && d1 != d0 &&
                std::binary_search(candidates.begin(), candidates.end(), d1)) {
                res.push_back(d1);
            }
            candidates.swap(res);
            return;
        }
        case PostingEncoding::Bitset: {
            const auto* words = reinterpret_cast<const u64*>(arena + entry.offset_or_inline);
            size_t write_idx = 0;
            for (DocId d : candidates) {
                if (d < total_docs && ((words[d >> 6] >> (d & 63)) & 1ULL)) {
                    candidates[write_idx++] = d;
                }
            }
            candidates.resize(write_idx);
            return;
        }
        case PostingEncoding::BP128: {
            // Block-skipping intersection! Never unpacks blocks whose [first_doc, max_doc]
            // cannot overlap with remaining candidates!
            std::vector<DocId> res;
            res.reserve(std::min<size_t>(candidates.size(), entry.doc_freq));

            const u8* p = arena + entry.offset_or_inline;
            u32 remaining = entry.doc_freq;
            size_t ci = 0;
            const size_t cn = candidates.size();
            alignas(64) u32 block[128];

            while (remaining >= 128 && ci < cn) {
                Bp128BlockHeader hdr;
                std::memcpy(&hdr, p, sizeof(hdr));
                p += sizeof(hdr);
                const size_t payload = static_cast<size_t>(hdr.bit_width) * 16;

                u32 target = candidates[ci];
                if (hdr.max_doc < target) {
                    // Skip entire 128-doc block in O(1)!
                    p += payload;
                    remaining -= 128;
                    continue;
                }
                if (hdr.first_doc > candidates.back()) {
                    // No further blocks can overlap!
                    candidates.swap(res);
                    return;
                }

                // Advance ci to first candidate >= hdr.first_doc
                if (candidates[ci] < hdr.first_doc) {
                    ci = simd::gallop_search(candidates.data(), ci, cn, hdr.first_doc);
                    if (ci >= cn || candidates[ci] > hdr.max_doc) {
                        p += payload;
                        remaining -= 128;
                        continue;
                    }
                }

                // Unpack this block and intersect with overlapping slice of candidates
                simd::bp128_unpack_deltas(p, hdr.bit_width, hdr.first_doc, block);
                p += payload;
                remaining -= 128;

                size_t bi = 0;
                while (ci < cn && candidates[ci] <= hdr.max_doc && bi < 128) {
                    u32 cval = candidates[ci];
                    u32 bval = block[bi];
                    if (cval == bval) {
                        res.push_back(cval);
                        ++ci;
                        ++bi;
                    } else if (cval < bval) {
                        ++ci;
                    } else {
                        bi = simd::gallop_search(block, bi, 128, cval);
                    }
                }
            }

            if (remaining > 0 && ci < cn) {
                u32 acc = 0;
                for (u32 i = 0; i < remaining && ci < cn; ++i) {
                    acc += read_varint(p);
                    while (ci < cn && candidates[ci] < acc) ++ci;
                    if (ci < cn && candidates[ci] == acc) {
                        res.push_back(acc);
                        ++ci;
                    }
                }
            }

            candidates.swap(res);
            return;
        }
    }
}

void union_sorted_u32(std::span<const DocId> a, std::span<const DocId> b, std::vector<DocId>& out) {
    out.clear();
    out.reserve(a.size() + b.size());
    size_t ia = 0, ib = 0;
    while (ia < a.size() && ib < b.size()) {
        if (a[ia] < b[ib]) {
            out.push_back(a[ia++]);
        } else if (b[ib] < a[ia]) {
            out.push_back(b[ib++]);
        } else {
            out.push_back(a[ia]);
            ++ia;
            ++ib;
        }
    }
    while (ia < a.size()) out.push_back(a[ia++]);
    while (ib < b.size()) out.push_back(b[ib++]);
}

} // namespace scry
