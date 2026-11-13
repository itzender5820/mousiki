// A persistent pool of worker threads for the pixel loops of the video looks (par_rows in video_player.cpp).
//
// Before, every par_rows() call started its own threads and joined them again -- several times per video frame (every stage of a look is one
// call), 25 times a second: thread creation costs 50-100 us on Linux and more on Windows, per thread and call. Here the threads are started once
// (as many as the looks have asked for so far, at most 16) and sleep on a condition variable between the calls.
//
// run(n, fn, ctx) calls fn(ctx, 0) .. fn(ctx, n - 1), each exactly once, and returns when all have finished. The calling thread takes part (it
// starts with chunk 0 and takes any chunk the workers have not taken yet), so a call never waits for a worker that is busy elsewhere, and the
// pool cannot deadlock: with no worker at all (no thread could be started) the caller does everything. It is safe to call run() from several
// threads at once (the look worker and the UI thread both run looks) and from inside a chunk.
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace muisc {

class ParPool {
public:
    using Fn = void (*)(void* ctx, int chunk);
    static ParPool& instance();
    // n <= 1 runs fn(ctx, 0) in the calling thread (n < 1 does nothing).
    void run(int n, Fn fn, void* ctx);
    // How many worker threads exist now (for the tests).
    int workers() const;
    ParPool() = default;
    ~ParPool();
    ParPool(const ParPool&) = delete;
    ParPool& operator=(const ParPool&) = delete;

private:
    struct Batch {
        Fn fn = nullptr;
        void* ctx = nullptr;
        int n = 0;
        std::atomic<int> next{0};   // the next chunk to be taken
        int done = 0;               // chunks finished (under the pool's mutex)
        int refs = 0;               // workers that hold a pointer to this batch (under the pool's mutex)
    };
    void worker_loop();
    static int take_chunks(Batch& b);   // runs chunks until none is left; returns how many this thread did

    mutable std::mutex m_;
    std::condition_variable work_cv_;   // a batch was added (or the pool stops)
    std::condition_variable done_cv_;   // a batch made progress
    std::vector<Batch*> active_;        // the batches that may still have chunks to take
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

}   // namespace muisc
