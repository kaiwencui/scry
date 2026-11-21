#include "io_uring_reader.hpp"

#include <atomic>
#include <cerrno>

#if defined(__linux__)
  #include <linux/io_uring.h>
  #include <sys/syscall.h>
#endif

namespace scry {

#if defined(__linux__)
namespace {

inline int sys_io_uring_setup(u32 entries, struct io_uring_params* p) {
    return static_cast<int>(::syscall(__NR_io_uring_setup, entries, p));
}

inline int sys_io_uring_enter(int fd, u32 to_submit, u32 min_complete, u32 flags) {
    return static_cast<int>(::syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, nullptr, 0));
}

} // namespace
#endif

BatchedFileReader::BatchedFileReader(u32 queue_depth) : queue_depth_(queue_depth) {
    uring_active_ = init_io_uring(queue_depth_);
}

BatchedFileReader::~BatchedFileReader() {
    destroy_io_uring();
}

bool BatchedFileReader::init_io_uring(u32 entries) {
#if defined(__linux__)
    struct io_uring_params params {};
    ring_fd_ = sys_io_uring_setup(entries, &params);
    if (ring_fd_ < 0) {
        return false;
    }

    sq_sz_ = params.sq_off.array + params.sq_entries * sizeof(u32);
    cq_sz_ = params.cq_off.cqes + params.cq_entries * sizeof(struct io_uring_cqe);
    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        if (cq_sz_ > sq_sz_) sq_sz_ = cq_sz_;
        cq_sz_ = sq_sz_;
    }

    sq_ptr_ = ::mmap(nullptr, sq_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                     ring_fd_, IORING_OFF_SQ_RING);
    if (sq_ptr_ == MAP_FAILED) {
        sq_ptr_ = nullptr;
        destroy_io_uring();
        return false;
    }

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        cq_ptr_ = sq_ptr_;
    } else {
        cq_ptr_ = ::mmap(nullptr, cq_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         ring_fd_, IORING_OFF_CQ_RING);
        if (cq_ptr_ == MAP_FAILED) {
            cq_ptr_ = nullptr;
            destroy_io_uring();
            return false;
        }
    }

    sqes_sz_ = params.sq_entries * sizeof(struct io_uring_sqe);
    sqes_ptr_ = ::mmap(nullptr, sqes_sz_, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                       ring_fd_, IORING_OFF_SQES);
    if (sqes_ptr_ == MAP_FAILED) {
        sqes_ptr_ = nullptr;
        destroy_io_uring();
        return false;
    }

    auto* sq_base = static_cast<u8*>(sq_ptr_);
    auto* cq_base = static_cast<u8*>(cq_ptr_);
    sq_head_  = reinterpret_cast<u32*>(sq_base + params.sq_off.head);
    sq_tail_  = reinterpret_cast<u32*>(sq_base + params.sq_off.tail);
    sq_mask_  = reinterpret_cast<u32*>(sq_base + params.sq_off.ring_mask);
    sq_array_ = reinterpret_cast<u32*>(sq_base + params.sq_off.array);

    cq_head_  = reinterpret_cast<u32*>(cq_base + params.cq_off.head);
    cq_tail_  = reinterpret_cast<u32*>(cq_base + params.cq_off.tail);
    cq_mask_  = reinterpret_cast<u32*>(cq_base + params.cq_off.ring_mask);
    cqes_     = cq_base + params.cq_off.cqes;

    return true;
#else
    (void)entries;
    return false;
#endif
}

void BatchedFileReader::destroy_io_uring() {
#if defined(__linux__)
    if (sqes_ptr_) {
        ::munmap(sqes_ptr_, sqes_sz_);
        sqes_ptr_ = nullptr;
    }
    if (cq_ptr_ && cq_ptr_ != sq_ptr_) {
        ::munmap(cq_ptr_, cq_sz_);
    }
    cq_ptr_ = nullptr;
    if (sq_ptr_) {
        ::munmap(sq_ptr_, sq_sz_);
        sq_ptr_ = nullptr;
    }
    if (ring_fd_ >= 0) {
        ::close(ring_fd_);
        ring_fd_ = -1;
    }
    uring_active_ = false;
#endif
}

bool BatchedFileReader::read_file_sync(const std::string& abs_path, std::vector<u8>& out, u64 size_hint) {
    int fd = ::open(abs_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;

    u64 sz = size_hint;
    if (sz == 0) {
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            ::close(fd);
            return false;
        }
        sz = static_cast<u64>(st.st_size);
    }

    out.resize(sz);
    size_t total = 0;
    while (total < sz) {
        ssize_t n = ::pread(fd, out.data() + total, sz - total, static_cast<off_t>(total));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        total += static_cast<size_t>(n);
    }
    out.resize(total);
    ::close(fd);
    return true;
}

void BatchedFileReader::read_batch(const std::vector<ReadRequest>& reqs,
                                   const std::function<void(u32, const u8*, size_t, bool)>& cb) {
    if (reqs.empty()) return;
    if (uring_active_) {
        read_batch_uring(reqs, cb);
    } else {
        read_batch_sync(reqs, cb);
    }
}

void BatchedFileReader::read_batch_sync(const std::vector<ReadRequest>& reqs,
                                        const std::function<void(u32, const u8*, size_t, bool)>& cb) {
    std::vector<u8> buf;
    for (const auto& r : reqs) {
        bool ok = read_file_sync(r.abs_path, buf, r.expected_size);
        cb(r.tag, ok ? buf.data() : nullptr, ok ? buf.size() : 0, ok);
    }
}

void BatchedFileReader::read_batch_uring(const std::vector<ReadRequest>& reqs,
                                         const std::function<void(u32, const u8*, size_t, bool)>& cb) {
#if defined(__linux__)
    // Batched async openat -> read -> close pipeline via io_uring!
    // Process in chunks of up to queue_depth_ / 2 to avoid ring overflow.
    const size_t max_inflight = std::max<size_t>(1, queue_depth_ / 2);
    auto* sqes = static_cast<struct io_uring_sqe*>(sqes_ptr_);
    auto* cqes = static_cast<struct io_uring_cqe*>(cqes_);

    struct Slot {
        int fd = -1;
        size_t req_idx = 0;
        std::vector<u8> buf;
        size_t offset = 0;
        enum class Stage : u8 { Open, Read, Close } stage = Stage::Open;
    };

    auto push_sqe = [&](const struct io_uring_sqe& sqe) {
        u32 tail = *sq_tail_;
        u32 idx = tail & (*sq_mask_);
        sqes[idx] = sqe;
        sq_array_[idx] = idx;
        std::atomic_thread_fence(std::memory_order_release);
        *sq_tail_ = tail + 1;
    };

    size_t next_req = 0;
    size_t inflight = 0;
    std::vector<Slot> slots(max_inflight);
    std::vector<u32> free_slots;
    free_slots.reserve(max_inflight);
    for (u32 i = 0; i < max_inflight; ++i) free_slots.push_back(i);

    while (next_req < reqs.size() || inflight > 0) {
        u32 to_submit = 0;

        // Fill ring with new OPENAT requests
        while (next_req < reqs.size() && !free_slots.empty()) {
            const auto& r = reqs[next_req];
            if (r.expected_size == 0) {
                // Empty file fast path
                cb(r.tag, nullptr, 0, true);
                ++next_req;
                continue;
            }
            u32 sid = free_slots.back();
            free_slots.pop_back();
            auto& slot = slots[sid];
            slot.fd = -1;
            slot.req_idx = next_req;
            slot.offset = 0;
            slot.stage = Slot::Stage::Open;
            slot.buf.resize(r.expected_size);

            struct io_uring_sqe sqe {};
            sqe.opcode = IORING_OP_OPENAT;
            sqe.fd = AT_FDCWD;
            sqe.addr = reinterpret_cast<u64>(r.abs_path.c_str());
            sqe.open_flags = O_RDONLY | O_CLOEXEC;
            sqe.user_data = sid;
            push_sqe(sqe);

            ++to_submit;
            ++inflight;
            ++next_req;
        }

        if (inflight == 0) break;

        int ret = sys_io_uring_enter(ring_fd_, to_submit, 1, IORING_ENTER_GETEVENTS);
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0) {
            // Unexpected error; fall back synchronously for remaining items
            for (auto& slot : slots) {
                if (slot.fd >= 0) { ::close(slot.fd); slot.fd = -1; }
            }
            std::vector<ReadRequest> rem(reqs.begin() + static_cast<ptrdiff_t>(next_req), reqs.end());
            read_batch_sync(rem, cb);
            return;
        }

        // Drain completions
        u32 head = *cq_head_;
        std::atomic_thread_fence(std::memory_order_acquire);
        u32 tail = *cq_tail_;
        u32 resubmits = 0;

        while (head != tail) {
            const auto& cqe = cqes[head & (*cq_mask_)];
            u32 sid = static_cast<u32>(cqe.user_data);
            i32 res = cqe.res;
            ++head;

            auto& slot = slots[sid];
            const auto& req = reqs[slot.req_idx];

            if (slot.stage == Slot::Stage::Open) {
                if (res < 0) {
                    if (res == -EMFILE || res == -ENFILE) {
                        // FD limit hit under heavy concurrency; recover synchronously!
                        std::vector<u8> fb;
                        bool ok = read_file_sync(req.abs_path, fb, req.expected_size);
                        cb(req.tag, ok ? fb.data() : nullptr, ok ? fb.size() : 0, ok);
                    } else {
                        cb(req.tag, nullptr, 0, false);
                    }
                    free_slots.push_back(sid);
                    --inflight;
                } else {
                    slot.fd = res;
                    slot.stage = Slot::Stage::Read;
                    struct io_uring_sqe sqe {};
                    sqe.opcode = IORING_OP_READ;
                    sqe.fd = slot.fd;
                    sqe.addr = reinterpret_cast<u64>(slot.buf.data());
                    sqe.len = static_cast<u32>(std::min<size_t>(slot.buf.size(), 1u << 30));
                    sqe.off = 0;
                    sqe.user_data = sid;
                    push_sqe(sqe);
                    ++resubmits;
                }
            } else if (slot.stage == Slot::Stage::Read) {
                if (res > 0) {
                    slot.offset += static_cast<size_t>(res);
                    if (slot.offset < slot.buf.size()) {
                        // Short read: continue reading remainder
                        struct io_uring_sqe sqe {};
                        sqe.opcode = IORING_OP_READ;
                        sqe.fd = slot.fd;
                        sqe.addr = reinterpret_cast<u64>(slot.buf.data() + slot.offset);
                        sqe.len = static_cast<u32>(std::min<size_t>(slot.buf.size() - slot.offset, 1u << 30));
                        sqe.off = slot.offset;
                        sqe.user_data = sid;
                        push_sqe(sqe);
                        ++resubmits;
                        continue;
                    }
                }
                // Deliver completed buffer (even if file shrank -> slot.offset bytes)
                bool ok = (res >= 0);
                cb(req.tag, ok ? slot.buf.data() : nullptr, ok ? slot.offset : 0, ok);

                // Queue async close
                slot.stage = Slot::Stage::Close;
                struct io_uring_sqe sqe {};
                sqe.opcode = IORING_OP_CLOSE;
                sqe.fd = slot.fd;
                sqe.user_data = sid;
                push_sqe(sqe);
                slot.fd = -1;
                ++resubmits;
            } else { // Close
                free_slots.push_back(sid);
                --inflight;
            }
        }

        std::atomic_thread_fence(std::memory_order_release);
        *cq_head_ = head;

        if (resubmits > 0) {
            sys_io_uring_enter(ring_fd_, resubmits, 0, 0);
        }
    }
#else
    read_batch_sync(reqs, cb);
#endif
}

} // namespace scry
