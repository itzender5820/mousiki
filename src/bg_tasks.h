// Background work that nobody waits for: helper tasks of the cover loading and the previews. They run on detached threads (a std::async
// future would block the end of the program until its ffmpeg has finished), at most a few at a time (scrolling through pictures does not start
// thirty ffmpeg runs), and a task that finds the program ending (or its result no longer wanted) does not start its work at all.
#pragma once
#include <atomic>
#include <exception>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>

namespace muisc {

// True from the moment the program starts to end (set by bg_wait_all()).
std::atomic<bool>& bg_stopping();

// Counting of the running tasks and the limit of parallel "heavy" runs (an ffmpeg decode).
void bg_task_started();
void bg_task_finished();

// Waits (at most `ms`) until the tasks started with bg_async() are done, after having set bg_stopping(). false: some are still running (the
// caller ends the process without running destructors, so that no task meets a destroyed object).
bool bg_wait_all(int ms);
// Waits (at most `ms`) until no task is running, without asking them to stop (tests: nothing of one test runs into the next).
bool bg_idle_wait(int ms);

// Holds one of the places for heavy runs (3): the constructor waits for a free one (or for bg_stopping()); ok() says whether it got one.
class BgSlot {
public:
    BgSlot();
    ~BgSlot();
    BgSlot(const BgSlot&) = delete;
    BgSlot& operator=(const BgSlot&) = delete;
    bool ok() const { return got_; }
private:
    bool got_ = false;
};

// Runs fn() on a detached thread; the result (or the exception) is in the returned future. The future can be dropped at any time.
template <class F>
auto bg_async(F fn) -> std::shared_future<std::invoke_result_t<F>> {
    using R = std::invoke_result_t<F>;
    auto prom = std::make_shared<std::promise<R>>();
    std::shared_future<R> fut = prom->get_future().share();
    bg_task_started();
    std::thread([prom, fn = std::move(fn)]() mutable {
        try { prom->set_value(fn()); }
        catch (...) { try { prom->set_exception(std::current_exception()); } catch (...) {} }
        bg_task_finished();
    }).detach();
    return fut;
}

} // namespace muisc
