#include "bg_tasks.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace muisc {
namespace {
struct BgState {
    std::mutex mu;
    std::condition_variable cv;
    int running = 0;   // tasks started with bg_async() and not finished
    int slots = 0;     // heavy runs holding a place
    std::atomic<bool> stopping{false};
};
BgState& st() {
    static BgState* s = new BgState();   // (never destroyed: a task may still count itself out while the program ends)
    return *s;
}
constexpr int kMaxHeavy = 3;
}  // namespace

std::atomic<bool>& bg_stopping() { return st().stopping; }

void bg_task_started() {
    BgState& s = st();
    std::lock_guard<std::mutex> lk(s.mu);
    ++s.running;
}

void bg_task_finished() {
    BgState& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        --s.running;
    }
    s.cv.notify_all();
}

bool bg_wait_all(int ms) {
    BgState& s = st();
    s.stopping.store(true);
    s.cv.notify_all();
    std::unique_lock<std::mutex> lk(s.mu);
    return s.cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return s.running == 0; });
}

bool bg_idle_wait(int ms) {
    BgState& s = st();
    std::unique_lock<std::mutex> lk(s.mu);
    return s.cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return s.running == 0; });
}

BgSlot::BgSlot() {
    BgState& s = st();
    std::unique_lock<std::mutex> lk(s.mu);
    s.cv.wait(lk, [&] { return s.slots < kMaxHeavy || s.stopping.load(); });
    if (s.stopping.load()) return;
    ++s.slots;
    got_ = true;
}

BgSlot::~BgSlot() {
    if (!got_) return;
    BgState& s = st();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        --s.slots;
    }
    s.cv.notify_all();
}

} // namespace muisc
