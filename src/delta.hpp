#pragma once

#include "common.hpp"
#include "gitignore.hpp"
#include "index_format.hpp"
#include "symbols.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace scry {

struct DeltaDoc {
    std::string rel_path;
    LangId lang = LangId::Unknown;
    u64 file_size = 0;
    i64 mtime_ns = 0;
    u64 inode = 0;
    bool deleted = false;
    std::vector<ExtractedSymbol> symbols;
};

// Immutable in-memory LSM delta overlay layered on top of an MmapIndex base.
struct DeltaSegment {
    // Bitset of base DocIds superseded or deleted by this delta
    std::vector<u64> base_tombstones;
    std::vector<DeltaDoc> docs;
    std::unordered_map<std::string, u32> path_to_delta_id;

    // Inverted trigram lists over delta doc IDs [0..docs.size())
    std::unordered_map<Trigram, std::vector<DocId>> exact_postings;
    std::unordered_map<Trigram, std::vector<DocId>> lower_postings;

    u32 live_delta_docs = 0;
    u32 tombstoned_base_docs = 0;

    bool is_base_tombstoned(DocId base_id) const noexcept {
        size_t w = base_id >> 6;
        if (w >= base_tombstones.size()) return false;
        return ((base_tombstones[w] >> (base_id & 63)) & 1ULL) != 0;
    }

    const std::vector<DocId>* lookup_trigram(Trigram t, bool icase) const noexcept {
        if (icase) {
            auto it = lower_postings.find(lower_trigram(t));
            return it != lower_postings.end() ? &it->second : nullptr;
        }
        auto it = exact_postings.find(t);
        return it != exact_postings.end() ? &it->second : nullptr;
    }

    bool empty() const noexcept {
        return docs.empty() && tombstoned_base_docs == 0;
    }
};

// Immutable atomic snapshot combining zero-copy mmapped base + LSM delta overlay
struct IndexSnapshot {
    std::filesystem::path root;
    std::filesystem::path index_path;
    std::shared_ptr<const MmapIndex> base;
    std::shared_ptr<const DeltaSegment> delta;
    u64 generation = 0;
};

// Manages live incremental updates + lock-free atomic snapshot swaps for a repository.
// Readers call `snapshot()` with zero writer contention; writers serialize via `write_mu_`
// and atomically publish a new immutable `IndexSnapshot`.
class LiveIndex {
public:
    LiveIndex() = default;

    // Load initial base mmap index for `root`
    bool load(const std::filesystem::path& root, const std::filesystem::path& index_path);

    // Swap in a freshly built immutable base MmapIndex and clear delta
    bool reload_base(const std::filesystem::path& index_path);

    // Wait-free / atomic snapshot read for concurrent searchers
    std::shared_ptr<const IndexSnapshot> snapshot() const noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        return current_.load(std::memory_order_acquire);
#else
        std::lock_guard<std::mutex> lk(snap_mu_);
        return current_;
#endif
    }

    // Batch-apply modified/created/deleted relative paths in a single atomic generation swap!
    // Handles both single-file saves (<1 ms) and 2,000-file git checkout storms (<200 ms).
    struct UpdateStats {
        u32 updated = 0;
        u32 deleted = 0;
        u32 skipped = 0;
        double elapsed_ms = 0.0;
    };
    UpdateStats apply_batch(const std::vector<std::string>& rel_paths);

    // Returns true if delta has grown large enough to merit background compaction
    bool needs_compaction() const noexcept;

    // Trigger full base compaction & atomic swap
    bool compact(std::string& err);

private:
    void rebuild_base_path_map(const MmapIndex& base);

    std::filesystem::path root_;
    std::filesystem::path index_path_;
    GitignoreMatcher gi_;

    // Mutable writer-only state guarded by write_mu_
    std::mutex write_mu_;
    std::unordered_map<std::string, DocId> base_path_to_id_;
    // Current live delta document map (rel_path -> DeltaDoc + exact/lower trigrams)
    struct LiveDeltaEntry {
        DeltaDoc doc;
        std::vector<Trigram> exact_trigrams;
        std::vector<Trigram> lower_trigrams;
    };
    std::unordered_map<std::string, LiveDeltaEntry> live_delta_entries_;
    std::vector<u64> live_tombstones_;
    u32 live_tombstone_count_ = 0;
    u64 gen_ = 0;

    void store_snapshot(std::shared_ptr<const IndexSnapshot> snap) noexcept {
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
        current_.store(std::move(snap), std::memory_order_release);
#else
        std::lock_guard<std::mutex> lk(snap_mu_);
        current_ = std::move(snap);
#endif
    }

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
    std::atomic<std::shared_ptr<const IndexSnapshot>> current_;
#else
    mutable std::mutex snap_mu_;
    std::shared_ptr<const IndexSnapshot> current_;
#endif
};

// Ephemeral freshness check for daemon-less CLI invocations:
// Checks git index / HEAD in ~5 us; if dirty (or strict_freshness=true),
// builds an on-the-fly DeltaSegment patching modified/untracked/deleted files.
std::shared_ptr<const DeltaSegment> build_freshness_delta(
    const std::filesystem::path& root,
    const MmapIndex& base,
    bool strict_freshness);

} // namespace scry
