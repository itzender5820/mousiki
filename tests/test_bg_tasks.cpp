// The helper tasks that nobody waits for (bg_tasks.h): the result comes back, an exception does not end the program, and no more than
// three heavy runs are going at the same time.
#include "test_framework.h"
#include "bg_tasks.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace muisc;

TEST(bg_async_gives_the_result_and_the_exception) {
    auto ok = bg_async([] { return 41 + 1; });
    CHECK_EQ(ok.get(), 42);
    auto bad = bg_async([]() -> int { throw std::runtime_error("boom"); });
    bool threw = false;
    try { bad.get(); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

TEST(bg_slots_let_at_most_three_heavy_runs_go_at_once) {
    std::atomic<int> now{0}, most{0};
    std::vector<std::shared_future<int>> fs;
    for (int i = 0; i < 10; ++i)
        fs.push_back(bg_async([&] {
            BgSlot slot;
            if (!slot.ok()) return 0;
            const int n = ++now;
            int m = most.load();
            while (n > m && !most.compare_exchange_weak(m, n)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            --now;
            return 1;
        }));
    int done = 0;
    for (auto& f : fs) done += f.get();
    CHECK_EQ(done, 10);
    CHECK(most.load() <= 3);
    CHECK(most.load() >= 2);   // (and they do run side by side)
}

#if !defined(_WIN32)
#include "process_util.h"
// kill_all_children(): a program that would run for half a minute is ended, and the one waiting for it returns at once.
TEST(kill_all_children_ends_a_running_program) {
    auto child = spawn_capture({"sleep", "30"}, false);
    CHECK(child != nullptr);
    if (!child) return;
    auto t0 = std::chrono::steady_clock::now();
    auto waiter = bg_async([&] { char b[16]; return static_cast<int>(child->read(b, sizeof b)); });   // (blocks: `sleep` writes nothing)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(waiter.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
    kill_all_children();
    CHECK(waiter.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    child->wait();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10));
}
#endif
