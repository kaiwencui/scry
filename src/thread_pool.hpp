#pragma once

#include "common.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace scry {

// Persistent bounded worker thread pool.
// Eliminates per-query pthread_create/join overhead and prevents glibc thread-arena bloat in scryd.
class WorkerPool {
public:
    static WorkerPool& global() {
        static WorkerPool pool(std::min<u32>(16, std::max<u32>(1, std::thread::hardware_concurrency())));
        return pool;
    }

    explicit WorkerPool(u32 num_threads) : max_threads_(std::max<u32>(1, num_threads)) {
        // Lazily spawn worker threads on first parallel dispatch so tiny single-threaded
        // CLI queries pay ZERO thread-spawn cost!
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
            gen_++;
        }
        cv_work_.notify_all();
        for (auto& th : threads_) {
            if (th.joinable()) th.join();
        }
    }

    u32 max_threads() const noexcept { return max_threads_; }

    // Run `fn(worker_idx)` across `num_workers` threads (calling thread executes worker 0!).
    // Serializes concurrent dispatches cleanly so pool threads never oversubscribe.
    template <typename Fn>
    void run(u32 num_workers, Fn&& fn) {
        if (num_workers <= 1) {
            fn(0);
            return;
        }
        u32 total = std::min(num_workers, max_threads_);
        u32 helper_count = total - 1;

        std::lock_guard<std::mutex> dispatch_lk(dispatch_mu_);
        ensure_started();

        std::function<void(u32)> task_ref = [&](u32 tid) { fn(tid); };
        {
            std::lock_guard<std::mutex> lk(mu_);
            current_task_ = &task_ref;
            active_helpers_ = helper_count;
            remaining_helpers_.store(helper_count, std::memory_order_release);
            gen_++;
        }
        cv_work_.notify_all();

        // Calling thread executes worker 0 directly
        fn(0);

        // Wait for helper workers to finish
        if (remaining_helpers_.load(std::memory_order_acquire) > 0) {
            std::unique_lock<std::mutex> lk(done_mu_);
            cv_done_.wait(lk, [&]() {
                return remaining_helpers_.load(std::memory_order_acquire) == 0;
            });
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            current_task_ = nullptr;
            active_helpers_ = 0;
        }
    }

private:
    void ensure_started() {
        if (started_) return;
        started_ = true;
        u32 helpers = (max_threads_ > 1) ? (max_threads_ - 1) : 0;
        threads_.reserve(helpers);
        for (u32 i = 0; i < helpers; ++i) {
            threads_.emplace_back(&WorkerPool::worker_loop, this, i + 1);
        }
    }

    void worker_loop(u32 tid) {
        u64 my_gen = 0;
        while (true) {
            std::function<void(u32)>* task = nullptr;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_work_.wait(lk, [&]() { return stop_ || gen_ != my_gen; });
                if (stop_) return;
                my_gen = gen_;
                if (tid <= active_helpers_) {
                    task = current_task_;
                }
            }
            if (task) {
                (*task)(tid);
                if (remaining_helpers_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard<std::mutex> dlk(done_mu_);
                    cv_done_.notify_one();
                }
            }
        }
    }

    u32 max_threads_;
    bool started_ = false;
    std::vector<std::thread> threads_;

    std::mutex dispatch_mu_;
    std::mutex mu_;
    std::condition_variable cv_work_;
    std::mutex done_mu_;
    std::condition_variable cv_done_;

    bool stop_ = false;
    u64 gen_ = 0;
    u32 active_helpers_ = 0;
    std::function<void(u32)>* current_task_ = nullptr;
    std::atomic<u32> remaining_helpers_{0};
};

} // namespace scry
