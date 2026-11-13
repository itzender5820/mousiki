// The persistent thread pool of the video looks (par_pool.h): every chunk exactly once, no deadlock, several callers at once.
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "par_pool.h"
#include "test_framework.h"

using namespace muisc;

namespace {
struct Count {
    std::vector<std::atomic<int>> hit;
    explicit Count(int n) : hit(static_cast<size_t>(n)) { for (auto& h : hit) h = 0; }
};
void bump(void* ctx, int i) { static_cast<Count*>(ctx)->hit[static_cast<size_t>(i)].fetch_add(1); }
}  // namespace

TEST(par_pool_runs_every_chunk_exactly_once) {
    ParPool& pool = ParPool::instance();
    for (int n : {1, 2, 3, 4, 7, 16, 40}) {
        for (int round = 0; round < 50; ++round) {
            Count c(n);
            pool.run(n, bump, &c);
            for (int i = 0; i < n; ++i) CHECK_EQ(c.hit[static_cast<size_t>(i)].load(), 1);
        }
    }
    Count none(1);
    pool.run(0, bump, &none);   // nothing to do
    CHECK_EQ(none.hit[0].load(), 0);
}

TEST(par_pool_starts_its_threads_once_and_reuses_them) {
    ParPool& pool = ParPool::instance();
    Count c(4);
    pool.run(4, bump, &c);
    const int w = pool.workers();
    CHECK(w >= 0 && w <= 15);
    for (int i = 0; i < 200; ++i) { Count d(4); pool.run(4, bump, &d); }
    CHECK_EQ(pool.workers(), w);   // (200 more calls started no thread)
}

TEST(par_pool_is_safe_from_several_threads_and_from_inside_a_chunk) {
    ParPool& pool = ParPool::instance();
    std::atomic<long> total{0};
    auto caller = [&] {
        for (int round = 0; round < 100; ++round) {
            Count c(8);
            pool.run(8, bump, &c);
            for (int i = 0; i < 8; ++i) total += c.hit[static_cast<size_t>(i)].load();
        }
    };
    std::vector<std::thread> ts;
    for (int k = 0; k < 4; ++k) ts.emplace_back(caller);
    for (auto& t : ts) t.join();
    CHECK_EQ(total.load(), 4L * 100 * 8);
    // a chunk that runs a pool call of its own (the looks do not, but it must not deadlock)
    struct Outer { ParPool* p; std::atomic<int> inner{0}; } o{&pool, {0}};
    pool.run(4, [](void* ctx, int) {
        Outer& oo = *static_cast<Outer*>(ctx);
        Count c(3);
        oo.p->run(3, bump, &c);
        oo.inner += c.hit[0].load() + c.hit[1].load() + c.hit[2].load();
    }, &o);
    CHECK_EQ(o.inner.load(), 4 * 3);
}

TEST(par_pool_caller_finishes_alone_when_the_workers_are_busy) {
    // two long chunks keep two threads busy; a third call must still return (the caller takes the chunks nobody took)
    ParPool& pool = ParPool::instance();
    std::atomic<bool> release{false};
    struct Gate { std::atomic<bool>* r; } g{&release};
    std::thread blocker([&] {
        pool.run(2, [](void* ctx, int) {
            auto* gg = static_cast<Gate*>(ctx);
            while (!gg->r->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }, &g);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Count c(6);
    pool.run(6, bump, &c);
    for (int i = 0; i < 6; ++i) CHECK_EQ(c.hit[static_cast<size_t>(i)].load(), 1);
    release = true;
    blocker.join();
}
