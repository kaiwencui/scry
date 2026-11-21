#pragma once

#include "common.hpp"

#include <bit>

#if defined(__x86_64__) || defined(_M_X64)
  #include <immintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__)
  #include <arm_neon.h>
#endif

namespace scry::simd {

inline constexpr const char* arch_name() noexcept {
#if defined(__AVX512F__) && defined(__AVX512BW__)
    return "AVX-512";
#elif defined(__AVX2__)
    return "AVX2";
#elif defined(__ARM_NEON) || defined(__aarch64__)
    return "NEON";
#else
    return "Scalar";
#endif
}

// Fast NUL-byte detection (binary file check)
inline bool contains_nul(const u8* data, size_t len) noexcept {
    size_t i = 0;
#if defined(__AVX512F__) && defined(__AVX512BW__)
    const __m512i zero = _mm512_setzero_si512();
    for (; i + 64 <= len; i += 64) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(data + i));
        if (_mm512_cmpeq_epi8_mask(v, zero) != 0) return true;
    }
#elif defined(__AVX2__)
    const __m256i zero = _mm256_setzero_si256();
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        if (_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, zero)) != 0) return true;
    }
#elif defined(__ARM_NEON) || defined(__aarch64__)
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8(data + i);
        if (vminvq_u8(v) == 0) return true;
    }
#endif
    return std::memchr(data + i, 0, len - i) != nullptr;
}

// Fast single-byte forward search, returns offset or len if not found
inline size_t find_byte(const u8* data, size_t len, u8 ch) noexcept {
    size_t i = 0;
#if defined(__AVX512F__) && defined(__AVX512BW__)
    const __m512i target = _mm512_set1_epi8(static_cast<char>(ch));
    for (; i + 64 <= len; i += 64) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(data + i));
        __mmask64 mask = _mm512_cmpeq_epi8_mask(v, target);
        if (mask != 0) {
            return i + static_cast<size_t>(std::countr_zero(mask));
        }
    }
#elif defined(__AVX2__)
    const __m256i target = _mm256_set1_epi8(static_cast<char>(ch));
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        u32 mask = static_cast<u32>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, target)));
        if (mask != 0) {
            return i + static_cast<size_t>(std::countr_zero(mask));
        }
    }
#endif
    const void* p = std::memchr(data + i, ch, len - i);
    return p ? static_cast<size_t>(static_cast<const u8*>(p) - data) : len;
}

// Fast reverse single-byte search in [0, pos), returns index of byte or npos (SIZE_MAX)
inline size_t rfind_byte(const u8* data, size_t pos, u8 ch) noexcept {
    size_t i = pos;
#if defined(__AVX512F__) && defined(__AVX512BW__)
    const __m512i target = _mm512_set1_epi8(static_cast<char>(ch));
    while (i >= 64) {
        i -= 64;
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(data + i));
        __mmask64 mask = _mm512_cmpeq_epi8_mask(v, target);
        if (mask != 0) {
            return i + static_cast<size_t>(63 - std::countl_zero(mask));
        }
    }
#elif defined(__AVX2__)
    const __m256i target = _mm256_set1_epi8(static_cast<char>(ch));
    while (i >= 32) {
        i -= 32;
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        u32 mask = static_cast<u32>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, target)));
        if (mask != 0) {
            return i + static_cast<size_t>(31 - std::countl_zero(mask));
        }
    }
#endif
    while (i > 0) {
        --i;
        if (data[i] == ch) return i;
    }
    return SIZE_MAX;
}

// Fast byte occurrence counter (e.g. counting '\n' for line numbers)
inline size_t count_byte(const u8* data, size_t len, u8 ch) noexcept {
    size_t count = 0;
    size_t i = 0;
#if defined(__AVX512F__) && defined(__AVX512BW__)
    const __m512i target = _mm512_set1_epi8(static_cast<char>(ch));
    for (; i + 64 <= len; i += 64) {
        __m512i v = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(data + i));
        __mmask64 mask = _mm512_cmpeq_epi8_mask(v, target);
        count += static_cast<size_t>(std::popcount(mask));
    }
#elif defined(__AVX2__)
    const __m256i target = _mm256_set1_epi8(static_cast<char>(ch));
    for (; i + 32 <= len; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(data + i));
        u32 mask = static_cast<u32>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, target)));
        count += static_cast<size_t>(std::popcount(mask));
    }
#endif
    for (; i < len; ++i) {
        if (data[i] == ch) ++count;
    }
    return count;
}

// Fast SIMD exact substring search (first + last byte SIMD filter).
// Returns offset in [0, hay_len) or SIZE_MAX if not found.
inline size_t find_substring(const u8* hay, size_t hay_len,
                             const u8* needle, size_t needle_len) noexcept {
    if (needle_len == 0) return 0;
    if (needle_len > hay_len) return SIZE_MAX;
    if (needle_len == 1) {
        size_t pos = find_byte(hay, hay_len, needle[0]);
        return pos < hay_len ? pos : SIZE_MAX;
    }

    const u8 first = needle[0];
    const u8 last = needle[needle_len - 1];
    const size_t last_off = needle_len - 1;
    const size_t limit = hay_len - needle_len + 1;
    size_t i = 0;

#if defined(__AVX512F__) && defined(__AVX512BW__)
    const __m512i vfirst = _mm512_set1_epi8(static_cast<char>(first));
    const __m512i vlast  = _mm512_set1_epi8(static_cast<char>(last));
    for (; i + 64 <= limit; i += 64) {
        __m512i bfirst = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(hay + i));
        __m512i blast  = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(hay + i + last_off));
        __mmask64 mask = _mm512_cmpeq_epi8_mask(bfirst, vfirst) &
                         _mm512_cmpeq_epi8_mask(blast, vlast);
        while (mask != 0) {
            u32 bit = static_cast<u32>(std::countr_zero(mask));
            if (needle_len == 2 || std::memcmp(hay + i + bit + 1, needle + 1, needle_len - 2) == 0) {
                return i + bit;
            }
            mask &= (mask - 1);
        }
    }
#elif defined(__AVX2__)
    const __m256i vfirst = _mm256_set1_epi8(static_cast<char>(first));
    const __m256i vlast  = _mm256_set1_epi8(static_cast<char>(last));
    for (; i + 32 <= limit; i += 32) {
        __m256i bfirst = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hay + i));
        __m256i blast  = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hay + i + last_off));
        u32 mask = static_cast<u32>(_mm256_movemask_epi8(
            _mm256_and_si256(_mm256_cmpeq_epi8(bfirst, vfirst),
                             _mm256_cmpeq_epi8(blast, vlast))));
        while (mask != 0) {
            u32 bit = static_cast<u32>(std::countr_zero(mask));
            if (needle_len == 2 || std::memcmp(hay + i + bit + 1, needle + 1, needle_len - 2) == 0) {
                return i + bit;
            }
            mask &= (mask - 1);
        }
    }
#elif defined(__ARM_NEON) || defined(__aarch64__)
    const uint8x16_t vfirst = vdupq_n_u8(first);
    const uint8x16_t vlast  = vdupq_n_u8(last);
    for (; i + 16 <= limit; i += 16) {
        uint8x16_t bfirst = vld1q_u8(hay + i);
        uint8x16_t blast  = vld1q_u8(hay + i + last_off);
        uint8x16_t eq = vandq_u8(vceqq_u8(bfirst, vfirst), vceqq_u8(blast, vlast));
        if (vmaxvq_u8(eq) != 0) {
            alignas(16) u8 lanes[16];
            vst1q_u8(lanes, eq);
            for (size_t bit = 0; bit < 16; ++bit) {
                if (lanes[bit] && (needle_len == 2 ||
                    std::memcmp(hay + i + bit + 1, needle + 1, needle_len - 2) == 0)) {
                    return i + bit;
                }
            }
        }
    }
#endif

    // Scalar tail using memchr for first byte
    while (i < limit) {
        const void* p = std::memchr(hay + i, first, limit - i);
        if (!p) return SIZE_MAX;
        size_t pos = static_cast<size_t>(static_cast<const u8*>(p) - hay);
        if (hay[pos + last_off] == last &&
            (needle_len == 2 || std::memcmp(hay + pos + 1, needle + 1, needle_len - 2) == 0)) {
            return pos;
        }
        i = pos + 1;
    }
    return SIZE_MAX;
}

inline bool memeq_icase_ascii(const u8* a, const u8* lower_b, size_t len) noexcept {
    for (size_t i = 0; i < len; ++i) {
        if (ascii_lower(a[i]) != lower_b[i]) return false;
    }
    return true;
}

// Fast SIMD ASCII case-insensitive substring search.
// `lower_needle` MUST already be ASCII-lowercased!
inline size_t find_substring_icase(const u8* hay, size_t hay_len,
                                   const u8* lower_needle, size_t needle_len) noexcept {
    if (needle_len == 0) return 0;
    if (needle_len > hay_len) return SIZE_MAX;

    const u8 first_lo = lower_needle[0];
    const u8 first_hi = (first_lo >= 'a' && first_lo <= 'z') ? static_cast<u8>(first_lo - 32) : first_lo;
    const u8 last_lo  = lower_needle[needle_len - 1];
    const u8 last_hi  = (last_lo >= 'a' && last_lo <= 'z') ? static_cast<u8>(last_lo - 32) : last_lo;
    const size_t last_off = needle_len - 1;
    const size_t limit = hay_len - needle_len + 1;
    size_t i = 0;

#if defined(__AVX512F__) && defined(__AVX512BW__)
    const __m512i vf_lo = _mm512_set1_epi8(static_cast<char>(first_lo));
    const __m512i vf_hi = _mm512_set1_epi8(static_cast<char>(first_hi));
    const __m512i vl_lo = _mm512_set1_epi8(static_cast<char>(last_lo));
    const __m512i vl_hi = _mm512_set1_epi8(static_cast<char>(last_hi));
    for (; i + 64 <= limit; i += 64) {
        __m512i bf = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(hay + i));
        __mmask64 mf = _mm512_cmpeq_epi8_mask(bf, vf_lo) | _mm512_cmpeq_epi8_mask(bf, vf_hi);
        if (mf == 0) continue;
        __mmask64 mask = mf;
        if (needle_len > 1) {
            __m512i bl = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(hay + i + last_off));
            mask &= (_mm512_cmpeq_epi8_mask(bl, vl_lo) | _mm512_cmpeq_epi8_mask(bl, vl_hi));
        }
        while (mask != 0) {
            u32 bit = static_cast<u32>(std::countr_zero(mask));
            if (needle_len <= 2 || memeq_icase_ascii(hay + i + bit + 1, lower_needle + 1, needle_len - 2)) {
                return i + bit;
            }
            mask &= (mask - 1);
        }
    }
#elif defined(__AVX2__)
    const __m256i vf_lo = _mm256_set1_epi8(static_cast<char>(first_lo));
    const __m256i vf_hi = _mm256_set1_epi8(static_cast<char>(first_hi));
    const __m256i vl_lo = _mm256_set1_epi8(static_cast<char>(last_lo));
    const __m256i vl_hi = _mm256_set1_epi8(static_cast<char>(last_hi));
    for (; i + 32 <= limit; i += 32) {
        __m256i bf = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hay + i));
        __m256i eqf = _mm256_or_si256(_mm256_cmpeq_epi8(bf, vf_lo), _mm256_cmpeq_epi8(bf, vf_hi));
        u32 mask = static_cast<u32>(_mm256_movemask_epi8(eqf));
        if (mask == 0) continue;
        if (needle_len > 1) {
            __m256i bl = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(hay + i + last_off));
            __m256i eql = _mm256_or_si256(_mm256_cmpeq_epi8(bl, vl_lo), _mm256_cmpeq_epi8(bl, vl_hi));
            mask &= static_cast<u32>(_mm256_movemask_epi8(eql));
        }
        while (mask != 0) {
            u32 bit = static_cast<u32>(std::countr_zero(mask));
            if (needle_len <= 2 || memeq_icase_ascii(hay + i + bit + 1, lower_needle + 1, needle_len - 2)) {
                return i + bit;
            }
            mask &= (mask - 1);
        }
    }
#endif

    for (; i < limit; ++i) {
        if ((hay[i] == first_lo || hay[i] == first_hi) &&
            (needle_len == 1 || hay[i + last_off] == last_lo || hay[i + last_off] == last_hi)) {
            if (needle_len <= 2 || memeq_icase_ascii(hay + i + 1, lower_needle + 1, needle_len - 2)) {
                return i;
            }
        }
    }
    return SIZE_MAX;
}

// SIMD Bitset operations (64-bit word arrays)
inline void bitset_and(u64* dst, const u64* src, size_t n_words) noexcept {
    size_t i = 0;
#if defined(__AVX512F__)
    for (; i + 8 <= n_words; i += 8) {
        __m512i a = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(dst + i));
        __m512i b = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(src + i));
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(dst + i), _mm512_and_si512(a, b));
    }
#elif defined(__AVX2__)
    for (; i + 4 <= n_words; i += 4) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_and_si256(a, b));
    }
#elif defined(__ARM_NEON) || defined(__aarch64__)
    for (; i + 2 <= n_words; i += 2) {
        uint64x2_t a = vld1q_u64(dst + i);
        uint64x2_t b = vld1q_u64(src + i);
        vst1q_u64(dst + i, vandq_u64(a, b));
    }
#endif
    for (; i < n_words; ++i) dst[i] &= src[i];
}

inline void bitset_or(u64* dst, const u64* src, size_t n_words) noexcept {
    size_t i = 0;
#if defined(__AVX512F__)
    for (; i + 8 <= n_words; i += 8) {
        __m512i a = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(dst + i));
        __m512i b = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(src + i));
        _mm512_storeu_si512(reinterpret_cast<__m512i*>(dst + i), _mm512_or_si512(a, b));
    }
#elif defined(__AVX2__)
    for (; i + 4 <= n_words; i += 4) {
        __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_or_si256(a, b));
    }
#elif defined(__ARM_NEON) || defined(__aarch64__)
    for (; i + 2 <= n_words; i += 2) {
        uint64x2_t a = vld1q_u64(dst + i);
        uint64x2_t b = vld1q_u64(src + i);
        vst1q_u64(dst + i, vorrq_u64(a, b));
    }
#endif
    for (; i < n_words; ++i) dst[i] |= src[i];
}

// Fast galloping / SIMD sorted u32 intersection
inline size_t gallop_search(const u32* arr, size_t pos, size_t len, u32 target) noexcept {
    if (pos >= len || arr[pos] >= target) return pos;
    size_t step = 1;
    size_t lo = pos;
    size_t hi = pos + step;
    while (hi < len && arr[hi] < target) {
        lo = hi;
        step <<= 1;
        hi = pos + step;
    }
    if (hi > len) hi = len;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        if (arr[mid] < target) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

inline void intersect_sorted_u32(const u32* a, size_t na,
                                 const u32* b, size_t nb,
                                 std::vector<u32>& out) {
    out.clear();
    if (na == 0 || nb == 0) return;

    // Ensure a is the smaller array
    if (na > nb) {
        std::swap(a, b);
        std::swap(na, nb);
    }

    // Galloping when sizes are skewed
    if (na * 16 < nb) {
        size_t ib = 0;
        for (size_t ia = 0; ia < na && ib < nb; ++ia) {
            u32 target = a[ia];
            ib = gallop_search(b, ib, nb, target);
            if (ib < nb && b[ib] == target) {
                out.push_back(target);
                ++ib;
            }
        }
        return;
    }

    size_t ia = 0, ib = 0;
#if defined(__AVX512F__)
    // Vectorized 16-vs-16 comparison when blocks overlap
    while (ia + 16 <= na && ib + 16 <= nb) {
        u32 a_max = a[ia + 15];
        u32 b_max = b[ib + 15];
        if (a_max < b[ib]) {
            ia += 16;
            continue;
        }
        if (b_max < a[ia]) {
            ib += 16;
            continue;
        }
        __m512i vb = _mm512_loadu_si512(reinterpret_cast<const __m512i*>(b + ib));
        for (size_t k = 0; k < 16; ++k) {
            u32 val = a[ia + k];
            if (val > b_max) break;
            if (val < b[ib]) continue;
            __m512i va = _mm512_set1_epi32(static_cast<int>(val));
            if (_mm512_cmpeq_epi32_mask(va, vb) != 0) {
                out.push_back(val);
            }
        }
        if (a_max == b_max) {
            ia += 16;
            ib += 16;
        } else if (a_max < b_max) {
            ia += 16;
        } else {
            ib += 16;
        }
    }
#elif defined(__AVX2__)
    while (ia + 8 <= na && ib + 8 <= nb) {
        u32 a_max = a[ia + 7];
        u32 b_max = b[ib + 7];
        if (a_max < b[ib]) {
            ia += 8;
            continue;
        }
        if (b_max < a[ia]) {
            ib += 8;
            continue;
        }
        __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + ib));
        for (size_t k = 0; k < 8; ++k) {
            u32 val = a[ia + k];
            if (val > b_max) break;
            if (val < b[ib]) continue;
            __m256i va = _mm256_set1_epi32(static_cast<int>(val));
            if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(va, vb)) != 0) {
                out.push_back(val);
            }
        }
        if (a_max == b_max) {
            ia += 8;
            ib += 8;
        } else if (a_max < b_max) {
            ia += 8;
        } else {
            ib += 8;
        }
    }
#endif

    // Scalar merge tail
    while (ia < na && ib < nb) {
        if (a[ia] == b[ib]) {
            out.push_back(a[ia]);
            ++ia;
            ++ib;
        } else if (a[ia] < b[ib]) {
            ++ia;
        } else {
            ++ib;
        }
    }
}

// Compute max bit width needed for an array of 128 u32 deltas
inline u8 max_bit_width_128(const u32* deltas) noexcept {
    u32 or_all = 0;
#if defined(__AVX512F__)
    __m512i acc = _mm512_setzero_si512();
    for (int i = 0; i < 128; i += 16) {
        acc = _mm512_or_si512(acc, _mm512_loadu_si512(reinterpret_cast<const __m512i*>(deltas + i)));
    }
    or_all = static_cast<u32>(_mm512_reduce_or_epi32(acc));
#else
    for (int i = 0; i < 128; ++i) or_all |= deltas[i];
#endif
    return or_all == 0 ? 0 : static_cast<u8>(32 - std::countl_zero(or_all));
}

// Pack 128 u32 values using `bw` bits per value into `out` (128 * bw / 8 bytes = 16 * bw bytes).
// Uses 4-lane word-striped packing for fast branchless unpack!
inline void bp128_pack(const u32* in, u8 bw, u8* out) noexcept {
    if (bw == 0) return;
    if (bw == 32) {
        std::memcpy(out, in, 128 * sizeof(u32));
        return;
    }
    // Pack into 4*bw u32 words (32 groups of 4 values -> 4 parallel bitstreams)
    auto* words = reinterpret_cast<u32*>(out);
    std::memset(words, 0, static_cast<size_t>(bw) * 16);

    for (u32 lane = 0; lane < 4; ++lane) {
        u32 bit_pos = 0;
        for (u32 j = 0; j < 32; ++j) {
            u32 val = in[j * 4 + lane];
            u32 word_idx = (bit_pos >> 5) * 4 + lane;
            u32 shift = bit_pos & 31;
            words[word_idx] |= (val << shift);
            if (shift + bw > 32) {
                words[word_idx + 4] |= (val >> (32 - shift));
            }
            bit_pos += bw;
        }
    }
}

// Unpack 128 delta values of `bw` bits each and reconstruct sorted docIDs starting from `init_val`
// (`out[0] = init_val + d[0]`, `out[i] = out[i-1] + d[i]`).
inline void bp128_unpack_deltas(const u8* in, u8 bw, u32 init_val, u32* out) noexcept {
    if (bw == 0) {
        for (int i = 0; i < 128; ++i) out[i] = init_val;
        return;
    }
    if (bw == 32) {
        u32 acc = init_val;
        for (int i = 0; i < 128; ++i) {
            u32 d;
            std::memcpy(&d, in + i * 4, 4);
            acc += d;
            out[i] = acc;
        }
        return;
    }
    const auto* words = reinterpret_cast<const u32*>(in);
    const u32 mask = (bw == 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);

    alignas(64) u32 deltas[128];
    u32 bit_pos = 0;
    for (u32 j = 0; j < 32; ++j) {
        u32 w = (bit_pos >> 5) * 4;
        u32 shift = bit_pos & 31;
        if (shift + bw <= 32) {
            deltas[j * 4 + 0] = (words[w + 0] >> shift) & mask;
            deltas[j * 4 + 1] = (words[w + 1] >> shift) & mask;
            deltas[j * 4 + 2] = (words[w + 2] >> shift) & mask;
            deltas[j * 4 + 3] = (words[w + 3] >> shift) & mask;
        } else {
            u32 rshift = 32 - shift;
            deltas[j * 4 + 0] = ((words[w + 0] >> shift) | (words[w + 4] << rshift)) & mask;
            deltas[j * 4 + 1] = ((words[w + 1] >> shift) | (words[w + 5] << rshift)) & mask;
            deltas[j * 4 + 2] = ((words[w + 2] >> shift) | (words[w + 6] << rshift)) & mask;
            deltas[j * 4 + 3] = ((words[w + 3] >> shift) | (words[w + 7] << rshift)) & mask;
        }
        bit_pos += bw;
    }

    u32 acc = init_val;
    for (int i = 0; i < 128; ++i) {
        acc += deltas[i];
        out[i] = acc;
    }
}

} // namespace scry::simd
