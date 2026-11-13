#include "video_pipe.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

#include "video_timing.h"

namespace muisc {

void video_make_picture(const VideoFrame& src0, const VideoFrame* prev, const VideoPicParams& p, VideoPic& out) {
    using Clock = std::chrono::steady_clock;
    const VideoFrame* src = &src0;
    VideoFrame fxf;
    const bool looked = p.look.active();
    const auto tm0 = Clock::now();
    if (looked) { fxf = src0; VideoPlayer::apply_look(fxf, p.look, p.t, false, prev); src = &fxf; }
    const auto tm1 = Clock::now();
    out.w = src->w;
    out.h = src->h;
    if (p.truecolor) {
        if (looked) out.rgb.swap(fxf.rgb);   // (the looked frame is ours: no copy)
        else out.rgb = src->rgb;
        out.level.assign(static_cast<size_t>(out.w) * out.h, 255);
        out.hue.clear();
    } else {
        VideoPlayer::quantize(*src, out.hue, out.level, p.look.dither);
        out.rgb.clear();
    }
    const int w = out.w, h = out.h;
    if (p.round_corners && w > 0 && h > 0) {   // the corners cut round, like the corners of the UI: a quarter circle of about one cell row
        const int hi = std::max(1, std::min(w, h) / 2);
        const int rp = std::min(std::max(w / std::max(1, p.vcols) * 11 / 20, 2), hi);   // (the curve of a terminal corner such as "─╮": about half a cell width, a little more)
        const float rf = static_cast<float>(rp);
        for (int y = 0; y < rp; ++y)
            for (int x = 0; x < rp; ++x) {
                const float dx = rf - static_cast<float>(x) - 0.5f, dy = rf - static_cast<float>(y) - 0.5f;
                if (dx * dx + dy * dy <= rf * rf) continue;
                const int xs[2] = {x, w - p.trim - 1 - x}, ys[2] = {y, h - 1 - y};
                for (int a = 0; a < 2; ++a)
                    for (int b = 0; b < 2; ++b) {
                        if (xs[a] < 0 || xs[a] >= w || ys[b] < 0 || ys[b] >= h) continue;
                        out.level[static_cast<size_t>(ys[b]) * w + xs[a]] = 0;
                    }
            }
    }
    if (p.trim > 0)
        for (int y = 0; y < h; ++y)
            for (int x = std::max(0, w - p.trim); x < w; ++x) out.level[static_cast<size_t>(y) * w + x] = 0;
    if (p.cut_w > 0) {
        const int xa = p.cut_c * w / std::max(1, p.vcols), xb = (p.cut_c + p.cut_w) * w / std::max(1, p.vcols);
        const int ya = p.cut_r * h / std::max(1, p.vrows), yb = (p.cut_r + p.cut_h) * h / std::max(1, p.vrows);
        for (int y = std::max(0, ya); y < yb && y < h; ++y)
            for (int x = std::max(0, xa); x < xb && x < w; ++x) out.level[static_cast<size_t>(y) * w + x] = 0;
    }
    const auto tm2 = Clock::now();
    out.look_ms = std::chrono::duration<double, std::milli>(tm1 - tm0).count();
    out.quant_ms = std::chrono::duration<double, std::milli>(tm2 - tm1).count();
    video_timing().look_ms = out.look_ms;
    video_timing().quant_ms = out.quant_ms;
}

VideoPipe::VideoPipe() {
    if (const char* e = std::getenv("MOUSIKI_VIDEO_THREAD"))
        if (e[0] == '0' && e[1] == '\0') enabled_ = false;
}

VideoPipe::~VideoPipe() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

bool VideoPipe::wants_thread() const { return enabled_.load() && async_.load(); }

void VideoPipe::note_cost(double ms) {
    const double c0 = cost_.load();
    const double c = c0 <= 0.0 ? ms : 0.7 * c0 + 0.3 * ms;   // (smoothed: a single slow frame does not switch)
    cost_.store(c);
    if (c > kAsyncOnMs) async_.store(true);
    else if (c < kAsyncOffMs) async_.store(false);
}

bool VideoPipe::start_locked() {
    if (th_.joinable()) return true;
    try {
        th_ = std::thread([this] { loop(); });
    } catch (...) {
        enabled_.store(false);   // (no thread: everything stays on the UI thread)
        return false;
    }
    return true;
}

bool VideoPipe::submit(const std::string& key, const VideoFrame& f, int cx0, int cy0, int w, int h, const VideoFrame* prev, const VideoPicParams& p) {
    if (!enabled_.load()) return false;
    VideoFrame cut;
    const bool whole = cx0 == 0 && cy0 == 0 && w == f.w && h == f.h;
    if (!whole) cut = VideoPlayer::crop(f, cx0, cy0, w, h);   // (made outside the lock)
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!start_locked()) return false;
        if (has_pending_) dropped_.fetch_add(1);   // (a frame that was not started: the newer one wins)
        pending_.key = key;
        if (whole) pending_.src = f;               // (assigned into the buffers of the last job: no allocation)
        else pending_.src = std::move(cut);
        if (prev) { pending_.prev = *prev; pending_.has_prev = true; }
        else pending_.has_prev = false;
        pending_.p = p;
        pending_.gen = gen_;
        has_pending_ = true;
    }
    cv_.notify_one();
    return true;
}

bool VideoPipe::take(const std::string& key, int w, int h, std::vector<uint8_t>& rgb, std::vector<uint8_t>& hue, std::vector<uint8_t>& level) {
    std::lock_guard<std::mutex> lk(m_);
    if (!has_result_) return false;
    has_result_ = false;
    if (result_key_ != key || result_.w != w || result_.h != h || result_.level.size() != static_cast<size_t>(w) * h) return false;   // (made for a size / a look that is gone)
    rgb.swap(result_.rgb);
    hue.swap(result_.hue);
    level.swap(result_.level);
    return true;
}

void VideoPipe::cancel() {
    std::lock_guard<std::mutex> lk(m_);
    ++gen_;
    has_pending_ = false;
    has_result_ = false;
}

void VideoPipe::loop() {
    Job job;
    VideoPic work;
    std::unique_lock<std::mutex> lk(m_);
    for (;;) {
        cv_.wait(lk, [&] { return stop_ || has_pending_; });
        if (stop_) return;
        std::swap(job, pending_);   // (the buffers of the last job go back to submit())
        has_pending_ = false;
        lk.unlock();
        video_make_picture(job.src, job.has_prev ? &job.prev : nullptr, job.p, work);
        note_cost(work.look_ms + work.quant_ms);
        lk.lock();
        if (job.gen == gen_) {   // (cancel() in the meantime: the picture is for a geometry that is gone)
            std::swap(result_, work);
            result_key_ = job.key;
            has_result_ = true;
        }
    }
}

}  // namespace muisc
