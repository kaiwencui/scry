#pragma once

#include "common.hpp"
#include "gitignore.hpp"
#include "posting.hpp"
#include "simd.hpp"

#include <array>
#include <cstring>
#include <string_view>

namespace scry {

constexpr char kIndexMagic[8] = {'S', 'C', 'R', 'Y', 'I', 'D', 'X', '\0'};
constexpr u32 kIndexVersion = 2; // v2: line-aligned 32 KiB virtual chunks + compact 16B SymbolEntry

constexpr u32 kChunkTargetBytes = 32 * 1024; // 32 KiB virtual chunk size for large files

enum IndexFlags : u32 {
    IDX_FLAG_NONE = 0,
    IDX_FLAG_HAS_SYMBOLS = 1u << 0,
    IDX_FLAG_HAS_PATH_INDEX = 1u << 1,
    IDX_FLAG_HAS_LOWER_TABLE = 1u << 2,
};

// Fixed 64-byte aligned header at offset 0 of index.scry
struct alignas(64) IndexHeader {
    char magic[8];
    u32 version;
    u32 flags;
    u32 num_docs;           // Number of files in FileTable
    u32 num_chunks;         // Number of virtual line-aligned chunks in ChunkTable (>= num_docs)
    u32 num_exact_trigrams;
    u32 num_lower_trigrams;
    u32 num_path_trigrams;
    u32 num_symbols;

    u64 total_corpus_bytes;
    i64 build_time_ns;
    i64 git_index_mtime_ns;
    u64 git_head_hash;
    u64 root_path_hash;

    // 64-byte aligned section offsets
    u64 file_table_off;
    u64 chunk_table_off;
    u64 path_arena_off;
    u64 path_arena_size;

    u64 exact_prefix_off;   // 65537 * u32 (256 KiB + 4B)
    u64 exact_entries_off;  // num_exact_trigrams * TrigramEntry
    u64 exact_postings_off;
    u64 exact_postings_size;

    u64 lower_prefix_off;   // 65537 * u32
    u64 lower_entries_off;  // num_lower_trigrams * TrigramEntry
    u64 lower_postings_off;
    u64 lower_postings_size;

    u64 path_prefix_off;    // 65537 * u32
    u64 path_entries_off;   // num_path_trigrams * TrigramEntry
    u64 path_postings_off;
    u64 path_postings_size;

    u64 symbols_off;        // num_symbols * SymbolEntry
    u64 symbol_arena_off;
    u64 symbol_arena_size;

    u64 checksum;           // Hash of header bytes [0..offsetof(checksum))
    u8 padding[8];
};
static_assert(sizeof(IndexHeader) <= 320);

// 40-byte file metadata entry per FileId
struct FileEntry {
    u32 path_off;    // Offset into PathArena
    u16 path_len;
    u16 lang_id;     // LangId
    u64 file_size;
    i64 mtime_ns;
    u64 inode;
    u32 first_chunk; // First ChunkId belonging to this file
    u32 chunk_count; // Contiguous number of chunks [first_chunk, first_chunk + chunk_count)
};
static_assert(sizeof(FileEntry) == 40);

// 16-byte line-aligned virtual chunk entry (posting lists reference ChunkId!)
struct ChunkEntry {
    DocId file_id;       // Parent file in FileTable
    u32 byte_off;        // Byte offset in parent file (always starts at a line boundary)
    u32 byte_len;        // Byte length of this chunk (always ends at '\n' or EOF)
    u32 start_line_no;   // 1-based line number of the first line in this chunk
};
static_assert(sizeof(ChunkEntry) == 16);

enum class SymbolKind : u8 {
    Unknown = 0,
    Function = 1,
    Struct = 2,
    Class = 3,
    Enum = 4,
    Macro = 5,
    Trait = 6,
    Interface = 7,
    TypeAlias = 8,
    Const = 9
};

// Compact 16-byte sorted symbol definition entry (4 entries per 64-byte cache line!)
struct SymbolEntry {
    u32 lower_hash;    // Low 32 bits of hash_sv(lowercased name)
    u32 name_off;      // Offset into NUL-terminated SymbolArena
    DocId doc_id;      // Parent FileId
    u32 line_and_kind; // Low 24 bits: 1-based line_no; high 8 bits: SymbolKind

    u32 line_no() const noexcept { return line_and_kind & 0x00FFFFFFu; }
    SymbolKind kind() const noexcept { return static_cast<SymbolKind>(line_and_kind >> 24); }
    static u32 pack_line_kind(u32 line, SymbolKind k) noexcept {
        return (line & 0x00FFFFFFu) | (static_cast<u32>(k) << 24);
    }
};
static_assert(sizeof(SymbolEntry) == 16);

// Zero-copy read-only view over an mmapped index.scry file
class MmapIndex {
public:
    MmapIndex() = default;

    bool open(const std::filesystem::path& index_path, bool willneed = false) {
        if (!mmap_.open(index_path.string(), true, willneed)) return false;
        if (mmap_.size() < sizeof(IndexHeader)) {
            mmap_.close();
            return false;
        }
        hdr_ = reinterpret_cast<const IndexHeader*>(mmap_.data());
        if (std::memcmp(hdr_->magic, kIndexMagic, 8) != 0 || hdr_->version != kIndexVersion) {
            mmap_.close();
            hdr_ = nullptr;
            return false;
        }
        u64 expected_cs = hash_bytes64(hdr_, offsetof(IndexHeader, checksum));
        if (hdr_->checksum != expected_cs) {
            mmap_.close();
            hdr_ = nullptr;
            return false;
        }
        base_ = mmap_.data();
        files_ = reinterpret_cast<const FileEntry*>(base_ + hdr_->file_table_off);
        chunks_ = reinterpret_cast<const ChunkEntry*>(base_ + hdr_->chunk_table_off);
        path_arena_ = reinterpret_cast<const char*>(base_ + hdr_->path_arena_off);

        exact_prefix_ = reinterpret_cast<const u32*>(base_ + hdr_->exact_prefix_off);
        exact_entries_ = reinterpret_cast<const TrigramEntry*>(base_ + hdr_->exact_entries_off);
        exact_postings_ = base_ + hdr_->exact_postings_off;

        if (hdr_->flags & IDX_FLAG_HAS_LOWER_TABLE) {
            lower_prefix_ = reinterpret_cast<const u32*>(base_ + hdr_->lower_prefix_off);
            lower_entries_ = reinterpret_cast<const TrigramEntry*>(base_ + hdr_->lower_entries_off);
            lower_postings_ = base_ + hdr_->lower_postings_off;
        }
        if (hdr_->flags & IDX_FLAG_HAS_PATH_INDEX) {
            path_prefix_ = reinterpret_cast<const u32*>(base_ + hdr_->path_prefix_off);
            path_entries_ = reinterpret_cast<const TrigramEntry*>(base_ + hdr_->path_entries_off);
            path_postings_ = base_ + hdr_->path_postings_off;
        }
        if (hdr_->flags & IDX_FLAG_HAS_SYMBOLS) {
            symbols_ = reinterpret_cast<const SymbolEntry*>(base_ + hdr_->symbols_off);
            symbol_arena_ = reinterpret_cast<const char*>(base_ + hdr_->symbol_arena_off);
        }
        return true;
    }

    bool valid() const noexcept { return hdr_ != nullptr; }
    const IndexHeader& header() const noexcept { return *hdr_; }
    size_t index_file_bytes() const noexcept { return mmap_.size(); }
    u32 num_docs() const noexcept { return hdr_ ? hdr_->num_docs : 0; }
    u32 num_chunks() const noexcept { return hdr_ ? hdr_->num_chunks : 0; }

    const FileEntry& file(DocId id) const noexcept { return files_[id]; }
    const ChunkEntry& chunk(DocId cid) const noexcept { return chunks_[cid]; }
    std::string_view file_path(DocId id) const noexcept {
        const auto& f = files_[id];
        return std::string_view(path_arena_ + f.path_off, f.path_len);
    }

    // O(1) prefix-bucketed lookup in exact trigram table (returns ChunkId posting list!)
    const TrigramEntry* lookup_exact(Trigram t) const noexcept {
        return lookup_in_table(t, exact_prefix_, exact_entries_);
    }

    struct LookupResult {
        const TrigramEntry* entry = nullptr;
        const u8* arena = nullptr;
    };

    LookupResult lookup_trigram(Trigram t, bool icase) const noexcept {
        if (!icase) {
            const TrigramEntry* e = lookup_exact(t);
            return {e, exact_postings_};
        }
        Trigram tl = lower_trigram(t);
        if (lower_prefix_) {
            const TrigramEntry* e = lookup_in_table(tl, lower_prefix_, lower_entries_);
            if (e) return {e, lower_postings_};
        }
        const TrigramEntry* e = lookup_exact(tl);
        if (e && !e->has_upper_variants()) {
            return {e, exact_postings_};
        }
        return {nullptr, nullptr};
    }

    LookupResult lookup_path_trigram(Trigram t_lower) const noexcept {
        if (!path_prefix_) return {nullptr, nullptr};
        const TrigramEntry* e = lookup_in_table(lower_trigram(t_lower), path_prefix_, path_entries_);
        return {e, path_postings_};
    }

    std::span<const SymbolEntry> symbols() const noexcept {
        return symbols_ ? std::span<const SymbolEntry>(symbols_, hdr_->num_symbols)
                        : std::span<const SymbolEntry>{};
    }

    std::string_view symbol_name(const SymbolEntry& s) const noexcept {
        const char* p = symbol_arena_ + s.name_off;
        return std::string_view(p, std::strlen(p));
    }

    void find_symbols(std::string_view name, bool icase,
                      std::vector<const SymbolEntry*>& out) const {
        out.clear();
        if (!symbols_ || hdr_->num_symbols == 0) return;
        std::string low(name.size(), '\0');
        for (size_t i = 0; i < name.size(); ++i) {
            low[i] = static_cast<char>(ascii_lower(static_cast<u8>(name[i])));
        }
        u32 target_h = static_cast<u32>(hash_sv(low));
        u32 lo = 0, hi = hdr_->num_symbols;
        while (lo < hi) {
            u32 mid = lo + ((hi - lo) >> 1);
            if (symbols_[mid].lower_hash < target_h) lo = mid + 1;
            else hi = mid;
        }
        for (u32 i = lo; i < hdr_->num_symbols && symbols_[i].lower_hash == target_h; ++i) {
            std::string_view sn = symbol_name(symbols_[i]);
            if (sn.size() != name.size()) continue;
            bool eq = icase
                ? simd::memeq_icase_ascii(reinterpret_cast<const u8*>(sn.data()),
                                          reinterpret_cast<const u8*>(low.data()), low.size())
                : (sn == name);
            if (eq) out.push_back(&symbols_[i]);
        }
    }

private:
    static const TrigramEntry* lookup_in_table(Trigram t,
                                               const u32* prefix,
                                               const TrigramEntry* entries) noexcept {
        u32 p2 = (t >> 8) & 0xFFFFu;
        u32 lo = prefix[p2];
        u32 hi = prefix[p2 + 1];
        if (lo == hi) return nullptr;
        while (lo < hi) {
            u32 mid = lo + ((hi - lo) >> 1);
            Trigram mt = entries[mid].trigram();
            if (mt == t) return &entries[mid];
            if (mt < t) lo = mid + 1;
            else hi = mid;
        }
        return nullptr;
    }

    MmapFile mmap_;
    const u8* base_ = nullptr;
    const IndexHeader* hdr_ = nullptr;
    const FileEntry* files_ = nullptr;
    const ChunkEntry* chunks_ = nullptr;
    const char* path_arena_ = nullptr;

    const u32* exact_prefix_ = nullptr;
    const TrigramEntry* exact_entries_ = nullptr;
    const u8* exact_postings_ = nullptr;

    const u32* lower_prefix_ = nullptr;
    const TrigramEntry* lower_entries_ = nullptr;
    const u8* lower_postings_ = nullptr;

    const u32* path_prefix_ = nullptr;
    const TrigramEntry* path_entries_ = nullptr;
    const u8* path_postings_ = nullptr;

    const SymbolEntry* symbols_ = nullptr;
    const char* symbol_arena_ = nullptr;
};

} // namespace scry
