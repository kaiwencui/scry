#pragma once

#include "common.hpp"
#include "simd.hpp"

#include <vector>

namespace scry {

enum class PostingEncoding : u8 {
    Inline = 0, // doc_freq <= 2 stored directly inside TrigramEntry::offset_or_inline
    BP128  = 1, // 128-doc SIMD bit-packed delta blocks + varint tail
    Bitset = 2  // Dense 64-bit word bitset over [0, total_docs)
};

// 16-byte on-disk trigram directory entry (64-byte cache line holds 4 entries)
struct TrigramEntry {
    // Low 24 bits: Trigram (b0<<16 | b1<<8 | b2)
    // Bits 24..25: PostingEncoding
    // Bit 26: has_upper_variants (for exact table: if 0, lower lookup can reuse this entry directly!)
    u32 trigram_meta = 0;
    u32 doc_freq = 0;
    u64 offset_or_inline = 0;

    Trigram trigram() const noexcept { return trigram_meta & 0x00FFFFFFu; }
    PostingEncoding encoding() const noexcept {
        return static_cast<PostingEncoding>((trigram_meta >> 24) & 0x3u);
    }
    bool has_upper_variants() const noexcept { return ((trigram_meta >> 26) & 1u) != 0; }

    static u32 pack_meta(Trigram t, PostingEncoding enc, bool has_upper = false) noexcept {
        return (t & 0x00FFFFFFu) | (static_cast<u32>(enc) << 24) | (has_upper ? (1u << 26) : 0u);
    }
};
static_assert(sizeof(TrigramEntry) == 16);

// Encode a sorted, deduplicated list of DocIds into `arena` and return a TrigramEntry.
TrigramEntry encode_posting_list(Trigram t,
                                 std::span<const DocId> docs,
                                 u32 total_docs,
                                 std::vector<u8>& arena,
                                 bool has_upper_variants = false);

// Decode a full posting list into `out` (sorted ascending).
void decode_posting_list(const TrigramEntry& entry,
                         const u8* arena,
                         u32 total_docs,
                         std::vector<DocId>& out);

// Intersect an already-sorted candidate vector `candidates` with a compressed posting list in-place!
// Uses:
// - O(1) direct check for Inline
// - O(|candidates|) bit-test for Bitset
// - Block-skipping BP128 unpack + galloping/SIMD intersection for BP128
void intersect_candidates_with_posting(std::vector<DocId>& candidates,
                                       const TrigramEntry& entry,
                                       const u8* arena,
                                       u32 total_docs);

// Union two sorted DocId lists into `out`
void union_sorted_u32(std::span<const DocId> a, std::span<const DocId> b, std::vector<DocId>& out);

} // namespace scry
