#pragma once

#include "common.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scry {

struct ReadRequest {
    std::string abs_path;
    u64 expected_size = 0;
    u32 tag = 0;
};

struct ReadResult {
    u32 tag = 0;
    std::vector<u8> data;
    bool ok = false;
};

// Direct Linux io_uring batched file reader (zero liburing dependency).
// Automatically falls back to pread() if io_uring is unavailable or blocked.
class BatchedFileReader {
public:
    explicit BatchedFileReader(u32 queue_depth = 128);
    ~BatchedFileReader();

    BatchedFileReader(const BatchedFileReader&) = delete;
    BatchedFileReader& operator=(const BatchedFileReader&) = delete;

    bool using_io_uring() const noexcept { return uring_active_; }

    // Read a batch of files and invoke callback(tag, data, ok) as each completes.
    void read_batch(const std::vector<ReadRequest>& reqs,
                    const std::function<void(u32 tag, const u8* data, size_t len, bool ok)>& cb);

    // Helper for reading a single file cleanly
    static bool read_file_sync(const std::string& abs_path, std::vector<u8>& out, u64 size_hint = 0);

private:
    bool init_io_uring(u32 entries);
    void destroy_io_uring();
    void read_batch_uring(const std::vector<ReadRequest>& reqs,
                          const std::function<void(u32, const u8*, size_t, bool)>& cb);
    void read_batch_sync(const std::vector<ReadRequest>& reqs,
                         const std::function<void(u32, const u8*, size_t, bool)>& cb);

    bool uring_active_ = false;
    u32 queue_depth_ = 128;

#if defined(__linux__)
    int ring_fd_ = -1;
    void* sq_ptr_ = nullptr;
    size_t sq_sz_ = 0;
    void* cq_ptr_ = nullptr;
    size_t cq_sz_ = 0;
    void* sqes_ptr_ = nullptr;
    size_t sqes_sz_ = 0;

    u32* sq_head_ = nullptr;
    u32* sq_tail_ = nullptr;
    u32* sq_mask_ = nullptr;
    u32* sq_array_ = nullptr;

    u32* cq_head_ = nullptr;
    u32* cq_tail_ = nullptr;
    u32* cq_mask_ = nullptr;
    void* cqes_ = nullptr;
#endif
};

} // namespace scry
