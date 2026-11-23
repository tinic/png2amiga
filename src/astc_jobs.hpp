#pragma once

// Job batches for the helper threads of the ASTC encoder.
//
// The encoder has serial chains (the blocks of a strip under error
// diffusion, the blocks of the RDO pass under its rate model) whose links
// contain independent pieces of work: the candidate groups of one block, the
// donor solves of one block. A Board lets the thread that owns the chain
// (the driver) post such a batch and lets other threads take jobs of it.
//
// Rules that keep the output independent of threads and timing:
//   - a job writes only its own result slot;
//   - the driver combines the slots in the order of the serial code, after
//     run() has returned.
//
// The driver runs jobs itself and never waits for a helper to start, so a
// batch completes with any number of helpers, including none (a nested
// pipeline::parallel_for runs its bodies one after the other).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

namespace png2amiga::astc::jobs {

// One step of a short wait loop: tells the core that the thread spins,
// without a system call.
inline void spin_pause() noexcept {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
}

// Spins until done() holds or about kSpinMicros have passed; returns done().
// A waiting thread spins first because the waits of this file are short
// (the serial work between two batches of the RDO pass, the last job of a
// batch): a sleeping helper wakes up too late for the next batch, and that
// cost half of the pass's speed-up when it was measured. After the spin it
// sleeps on the atomic, so a long wait does not burn a core.
inline constexpr int kSpinMicros = 500;
template<typename Done>
bool spin_until(Done done) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        for (int i = 0; i < 64; ++i) {
            if (done()) return true;
            spin_pause();
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::microseconds(kSpinMicros))
            return done();
    }
}

class Board {
public:
    // Driver: job(i, worker) for every i in [0, n); returns when all have
    // finished. worker = 0 for the driver, else the value a helper passed
    // to join().
    template<typename Job>
    void run(int n, Job& job) {
        if (n <= 0) return;
        if (n == 1) {
            job(0, 0);
            return;
        }
        ctx_ = &job;
        call_ = [](void* c, int i, int worker) {
            (*static_cast<Job*>(c))(i, worker);
        };
        n_ = n;
        next_.store(0);
        done_.store(0);
        open_.store(true);
        wake();
        for (int i = next_.fetch_add(1); i < n; i = next_.fetch_add(1)) {
            job(i, 0);
            done_.fetch_add(1);
        }
        // Helpers may still run jobs they took.
        for (;;) {
            const int d = done_.load();
            if (d >= n) break;
            if (!spin_until([&] { return done_.load() != d; })) done_.wait(d);
        }
        // No helper may enter the batch from here on. Wait for the ones
        // that are inside before the next batch changes the job.
        open_.store(false);
        while (active_.load() != 0)
            spin_pause();
    }

    // Helper: take jobs of the open batch, if there is one; returns when
    // that batch has no job left to take.
    void join(int worker) {
        active_.fetch_add(1);
        // open_ is read after active_ is raised and the driver lowers open_
        // before it reads active_: either the driver waits for this helper
        // or the helper sees the batch closed.
        if (open_.load()) {
            for (int i = next_.fetch_add(1); i < n_; i = next_.fetch_add(1)) {
                call_(ctx_, i, worker);
                done_.fetch_add(1);
                done_.notify_all();
            }
        }
        active_.fetch_sub(1);
    }

    // A hint for helpers that poll several boards.
    bool open() const { return open_.load(std::memory_order_relaxed); }

    // For helpers that sleep between batches: sequence number of the last
    // batch (or wake() call), and a wait until it differs from `seen`.
    std::uint32_t batch() const { return batch_.load(); }
    // Returns when batch() != seen: after a short spin, by sleeping.
    void wait_batch(std::uint32_t seen) const {
        if (spin_until([&] { return batch_.load() != seen; })) return;
        batch_.wait(seen);
    }
    void wake() {
        batch_.fetch_add(1);
        batch_.notify_all();
    }

private:
    std::atomic<std::uint32_t> batch_{0};
    std::atomic<bool> open_{false};
    std::atomic<int> active_{0};
    std::atomic<int> next_{0};
    std::atomic<int> done_{0};
    // Written by run() while no helper is inside a batch.
    int n_ = 0;
    void (*call_)(void*, int, int) = nullptr;
    void* ctx_ = nullptr;
};

}  // namespace png2amiga::astc::jobs
