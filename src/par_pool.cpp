#include "par_pool.h"

#include <algorithm>
#include <exception>

namespace muisc {

ParPool& ParPool::instance() {
    static ParPool pool;   // (made on first use, joined when the program ends)
    return pool;
}

ParPool::~ParPool() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    work_cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
}

int ParPool::workers() const {
    std::lock_guard<std::mutex> lk(m_);
    return static_cast<int>(threads_.size());
}

int ParPool::take_chunks(Batch& b) {
    int did = 0;
    for (;;) {
        const int i = b.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= b.n) break;
        try { b.fn(b.ctx, i); }
        catch (...) { std::terminate(); }   // (as a thread of its own did before: a chunk does not throw; and no batch may be left behind on a stack that is gone)
        ++did;
    }
    return did;
}

void ParPool::run(int n, Fn fn, void* ctx) {
    if (n < 1) return;
    if (n == 1) { fn(ctx, 0); return; }
    Batch b;
    b.fn = fn; b.ctx = ctx; b.n = n;
    {
        std::lock_guard<std::mutex> lk(m_);
        // as many workers as this call can use (the caller is one more); a thread that cannot be started just means more chunks for the caller
        while (static_cast<int>(threads_.size()) < std::min(n - 1, 15)) {
            try { threads_.emplace_back([this] { worker_loop(); }); }
            catch (...) { break; }
        }
        active_.push_back(&b);
    }
    if (n == 2) work_cv_.notify_one(); else work_cv_.notify_all();
    const int mine = take_chunks(b);
    std::unique_lock<std::mutex> lk(m_);
    b.done += mine;
    done_cv_.wait(lk, [&] { return b.done >= b.n && b.refs == 0; });
    active_.erase(std::remove(active_.begin(), active_.end(), &b), active_.end());
}

void ParPool::worker_loop() {
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        Batch* b = nullptr;
        work_cv_.wait(lk, [&] {
            if (stop_) return true;
            for (Batch* c : active_) if (c->next.load(std::memory_order_relaxed) < c->n) { b = c; return true; }
            return false;
        });
        if (stop_) return;
        ++b->refs;   // (the caller does not leave before every holder is done: the batch lives on its stack)
        lk.unlock();
        const int did = take_chunks(*b);
        lk.lock();
        b->done += did;
        --b->refs;
        done_cv_.notify_all();
    }
}

}   // namespace muisc
