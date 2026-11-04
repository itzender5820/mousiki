#include "video_player.h"

#include "console_log.h"
#include "path_utf8.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace muisc {
namespace {

constexpr size_t kQueueMax = 12;   // decoded frames kept ahead (about half a second)

const uint8_t kBayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

std::string fmt_seconds(double t) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.3f", std::max(0.0, t));
    return b;
}

}  // namespace

VideoPlayer::VideoPlayer() = default;

VideoPlayer::~VideoPlayer() { halt(); }

void VideoPlayer::halt() {
    {
        std::lock_guard<std::mutex> lk(m_);
        ++gen_;
    }
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(child_m_);
        if (child_) child_->kill();
    }
    if (th_.joinable()) th_.join();
    {
        std::lock_guard<std::mutex> lk(child_m_);
        child_.reset();
    }
    running_ = false;
}

std::string VideoPlayer::status() {
    std::lock_guard<std::mutex> lk(m_);
    if (cur_valid_ || !q_.empty()) return std::string();
    if (!note_.empty()) return note_;
    return "video: starting ...";
}

void VideoPlayer::stop() {
    halt();
    std::lock_guard<std::mutex> lk(m_);
    q_.clear();
    cur_valid_ = false;
    cur_stale_ = false;
}

bool VideoPlayer::set_source(const std::string& path) {
    if (path == path_ && (path.empty() || has_video())) return has_video();
    halt();
    {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
        cur_valid_ = false;
        cur_stale_ = false;
        eof_ = false;
    }
    path_ = path;
    vw_ = vh_ = 0;
    duration_ = 0.0;
    if (path.empty()) return false;
    // "1280,720,0" (width, height, a picture glued into an audio file = 1) and the duration of the container
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                      "stream=width,height:stream_disposition=attached_pic:format=duration", "-of", "csv=p=0", path});
    if (!r.ok()) return false;
    std::istringstream in(r.out);
    std::string line;
    int w = 0, h = 0, pic = 0;
    bool have = false;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        if (!have && line.find(',') != std::string::npos) {
            if (std::sscanf(line.c_str(), "%d,%d,%d", &w, &h, &pic) >= 2) have = true;
        } else {
            char* end = nullptr;
            const double d = std::strtod(line.c_str(), &end);
            if (end != line.c_str() && d > 0) duration_ = d;
        }
    }
    if (!have || w <= 0 || h <= 0 || pic == 1) return false;
    vw_ = w;
    vh_ = h;
    return true;
}

void VideoPlayer::fit(int box_w, int box_h, double unit_aspect, int& out_w, int& out_h) const {
    const double ar = aspect() > 0 ? aspect() : 16.0 / 9.0;
    const double ua = unit_aspect > 0 ? unit_aspect : 1.0;
    box_w = std::max(2, box_w);
    box_h = std::max(2, box_h);
    double h = box_h, w = ar * h / ua;
    if (w > box_w) { w = box_w; h = w * ua / ar; }
    out_w = std::max(2, static_cast<int>(w) & ~1);
    out_h = std::max(2, static_cast<int>(h) & ~1);
}

void VideoPlayer::start(double from, bool keep_picture) {
    halt();
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(m_);
        q_.clear();
        // a restart (seek, resize, coming back to the video view) keeps the last picture on screen until the new one is there (the
        // caller scales it to the new size meanwhile); only a new file has nothing to show
        cur_stale_ = keep_picture && cur_valid_;
        if (!cur_stale_) cur_valid_ = false;
        eof_ = false;
        gen = gen_;
    }
    running_ = true;
    {
        std::lock_guard<std::mutex> lk(m_);
        note_.clear();
        frames_decoded_ = 0;
        started_at_ = std::chrono::steady_clock::now();
    }
    th_ = std::thread([this, gen, from]() { run(gen, from, run_w_, run_h_); });
}

void VideoPlayer::run(uint64_t gen, double from, int w, int h) {
    const std::vector<std::string> args = {"ffmpeg", "-v", "error", "-nostdin", "-ss", fmt_seconds(from), "-i", path_, "-an", "-sn", "-dn",
                                           "-vf", "fps=" + std::to_string(kFps) + ",scale=" + std::to_string(w) + ":" + std::to_string(h) + ":flags=bilinear",
                                           "-pix_fmt", "rgb24", "-f", "rawvideo", "-"};
    ConsoleLog::instance().log_basic("video: " + command_text(args));
    std::unique_ptr<ChildProcess> child = spawn_capture(args, false);
    if (!child) {
        ConsoleLog::instance().log_basic("video: ffmpeg could not be started (is it installed and on the PATH?)");
        std::lock_guard<std::mutex> lk(m_);
        if (gen == gen_) { eof_ = true; note_ = "video: ffmpeg could not be started"; }
        return;
    }
    ChildProcess* raw = child.get();
    {
        std::lock_guard<std::mutex> lk(child_m_);
        child_ = std::move(child);
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        if (gen != gen_) { std::lock_guard<std::mutex> lk2(child_m_); if (child_) child_->kill(); return; }
    }
    const size_t frame_bytes = static_cast<size_t>(w) * h * 3;
    std::vector<uint8_t> buf(frame_bytes);
    uint64_t n = 0;
    for (;;) {
        size_t got = 0;
        while (got < frame_bytes) {
            const std::ptrdiff_t r = raw->read(reinterpret_cast<char*>(buf.data()) + got, frame_bytes - got);
            if (r <= 0) break;
            got += static_cast<size_t>(r);
        }
        if (got < frame_bytes) break;   // the end (or the process was killed)
        VideoFrame f;
        f.w = w; f.h = h;
        f.rgb = buf;
        f.pts = from + static_cast<double>(n) / kFps;
        ++n;
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return gen != gen_ || q_.size() < kQueueMax; });
        if (gen != gen_) return;
        if (n == 1) ConsoleLog::instance().log_basic("video: first frame after " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_at_).count()) + " ms (" + std::to_string(w) + "x" + std::to_string(h) + ")");
        q_.push_back(std::move(f));
    }
    bool none = false;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (gen == gen_) { eof_ = true; none = n == 0; }
    }
    if (none) {   // not a single frame: ask ffmpeg again for its complaint (it was not part of the pipe) and show it
        const ProcResult r = run_capture({"ffmpeg", "-v", "error", "-nostdin", "-ss", fmt_seconds(from), "-i", path_, "-an", "-frames:v", "1", "-f", "null", "-"}, true);
        std::string msg = r.out;
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
        const size_t nl = msg.find('\n');
        const std::string first = nl == std::string::npos ? msg : msg.substr(0, nl);
        ConsoleLog::instance().log_basic("video: no frame from ffmpeg (exit " + std::to_string(r.exit_code) + "): " + (msg.empty() ? std::string("no message") : msg.substr(0, 600)));
        std::lock_guard<std::mutex> lk(m_);
        if (gen == gen_) note_ = "video: ffmpeg gave no picture" + (first.empty() ? std::string() : ": " + first.substr(0, 80)) + " (see the console log)";
    }
}

const VideoFrame* VideoPlayer::frame_for(double t, int w, int h, bool playing, bool* fresh) {
    if (fresh) *fresh = false;
    if (!has_video() || w < 2 || h < 2) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    bool restart = !running_;
    if (running_ && (w != run_w_ || h != run_h_)) {
        // resized (window, fullscreen, lyrics on / off): the decoder is restarted at the new size only once the size has stayed the
        // same for a moment -- while the window is being dragged the picture on screen is simply scaled
        if (w != pend_w_ || h != pend_h_) { pend_w_ = w; pend_h_ = h; pend_since_ = now; }
        if (now - pend_since_ >= std::chrono::milliseconds(250)) restart = true;
    } else {
        pend_w_ = pend_h_ = 0;
    }
    bool eof = false;
    double last = 0.0;
    {
        std::lock_guard<std::mutex> lk(m_);
        eof = eof_;
        // how far the decoder has come: the newest queued frame, else the shown one -- but a picture left over from before a restart
        // says nothing about the new decoder (it starts at run_from_)
        last = !q_.empty() ? q_.back().pts : (cur_valid_ && !cur_stale_ ? cur_.pts : run_from_);
        const bool decoded = !q_.empty() || (cur_valid_ && !cur_stale_);
        const bool settled = now - restarted_at_ >= std::chrono::milliseconds(400);   // (a seek burst makes one restart, not one per key repeat)
        if (!restart && cur_valid_ && !cur_stale_ && t < cur_.pts - 0.5) restart = true;     // sought back
        // sought ahead: the decoder waits with a full queue far behind the clock (a slow decoder is not restarted: it catches up by
        // showing the newest frame, a restart would throw its work away); or the clock is very far ahead of anything decoded
        if (!restart && !eof && decoded && settled && t > last + 2.0 && (q_.size() >= kQueueMax || t > last + 10.0)) restart = true;
        // the new decoder has not delivered anything yet and the clock has left its start behind (a second seek while it started)
        if (!restart && !decoded && !eof && settled && std::fabs(t - run_from_) > 3.0 && now - restarted_at_ >= std::chrono::milliseconds(1500)) restart = true;
        if (!restart && !decoded && !eof && t > run_from_ + 20.0) restart = true;   // nothing at all for a very long time
    }
    if (restart) {
        run_w_ = w; run_h_ = h;
        run_from_ = std::max(0.0, t);
        restarted_at_ = now;
        pend_w_ = pend_h_ = 0;
        start(run_from_, true);
    }
    std::lock_guard<std::mutex> lk(m_);
    bool changed = false;
    if ((!cur_valid_ || cur_stale_) && !q_.empty()) { cur_ = std::move(q_.front()); q_.pop_front(); cur_valid_ = true; cur_stale_ = false; changed = true; }
    if (playing) {
        while (!q_.empty() && q_.front().pts <= t) { cur_ = std::move(q_.front()); q_.pop_front(); cur_valid_ = true; changed = true; }
        if (changed) cv_.notify_all();
    }
    if (changed) cur_returned_ = false;
    if (fresh) *fresh = changed || (cur_valid_ && !cur_returned_);
    if (cur_valid_) cur_returned_ = true;
    return cur_valid_ ? &cur_ : nullptr;
}

VideoFrame VideoPlayer::resample(const VideoFrame& f, int w, int h) {
    VideoFrame o;
    o.w = w; o.h = h; o.pts = f.pts;
    if (w <= 0 || h <= 0 || f.w <= 0 || f.h <= 0 || f.rgb.size() < static_cast<size_t>(f.w) * f.h * 3) return o;
    o.rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const int sy = std::min(f.h - 1, static_cast<int>((static_cast<int64_t>(y) * f.h) / h));
        for (int x = 0; x < w; ++x) {
            const int sx = std::min(f.w - 1, static_cast<int>((static_cast<int64_t>(x) * f.w) / w));
            const size_t si = (static_cast<size_t>(sy) * f.w + sx) * 3, di = (static_cast<size_t>(y) * w + x) * 3;
            o.rgb[di] = f.rgb[si]; o.rgb[di + 1] = f.rgb[si + 1]; o.rgb[di + 2] = f.rgb[si + 2];
        }
    }
    return o;
}

// ---- terminal output ---------------------------------------------------------------------------------------------------------

const std::vector<std::array<uint8_t, 3>>& VideoPlayer::palette() {
    static const std::vector<std::array<uint8_t, 3>> pal = [] {
        std::vector<std::array<uint8_t, 3>> p;
        for (int r = 0; r < 6; ++r)
            for (int g = 0; g < 6; ++g)
                for (int b = 0; b < 6; ++b) p.push_back({static_cast<uint8_t>(r * 51), static_cast<uint8_t>(g * 51), static_cast<uint8_t>(b * 51)});
        for (int i = 0; i < 24; ++i) { const uint8_t v = static_cast<uint8_t>(std::lround(i * 255.0 / 23.0)); p.push_back({v, v, v}); }
        return p;
    }();
    return pal;
}

void VideoPlayer::quantize(const VideoFrame& f, std::vector<uint8_t>& hue, std::vector<uint8_t>& level) {
    const size_t n = static_cast<size_t>(f.w) * f.h;
    hue.assign(n, 0);
    level.assign(n, 255);
    if (f.rgb.size() < n * 3) return;
    for (int y = 0; y < f.h; ++y) {
        for (int x = 0; x < f.w; ++x) {
            const size_t i = static_cast<size_t>(y) * f.w + x;
            const int r = f.rgb[i * 3], g = f.rgb[i * 3 + 1], b = f.rgb[i * 3 + 2];
            const int bias = static_cast<int>(kBayer4[y & 3][x & 3]) - 8;   // -8 .. 7
            const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
            if (mx - mn < 14) {   // near grey: the finer ramp of 24 greys
                const int v = std::clamp((r + g + b) / 3 + bias * 2, 0, 255);
                hue[i] = static_cast<uint8_t>(216 + (v * 23 + 127) / 255);
            } else {
                const int step = 51;
                const int qr = std::clamp((r + bias * 3 + step / 2) / step, 0, 5);
                const int qg = std::clamp((g + bias * 3 + step / 2) / step, 0, 5);
                const int qb = std::clamp((b + bias * 3 + step / 2) / step, 0, 5);
                hue[i] = static_cast<uint8_t>(36 * qr + 6 * qg + qb);
            }
        }
    }
}

std::vector<std::vector<VideoPlayer::Cell>> VideoPlayer::braille(const VideoFrame& f, int cols, int rows) {
    std::vector<std::vector<Cell>> out(static_cast<size_t>(std::max(0, rows)), std::vector<Cell>(static_cast<size_t>(std::max(0, cols))));
    static const uint8_t bit[2][4] = {{1, 2, 4, 64}, {8, 16, 32, 128}};   // [dot column][dot row]
    auto luma = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= f.w || y >= f.h) return 0;
        const size_t i = (static_cast<size_t>(y) * f.w + x) * 3;
        return (f.rgb[i] * 77 + f.rgb[i + 1] * 150 + f.rgb[i + 2] * 29) >> 8;
    };
    for (int cy = 0; cy < rows; ++cy)
        for (int cx = 0; cx < cols; ++cx) {
            int mask = 0, sum = 0;
            for (int dx = 0; dx < 2; ++dx)
                for (int dy = 0; dy < 4; ++dy) {
                    const int x = cx * 2 + dx, y = cy * 4 + dy;
                    const int l = luma(x, y);
                    sum += l;
                    const int thr = (static_cast<int>(kBayer4[y & 3][x & 3]) * 16 + 8);   // 8 .. 248
                    if (l > thr) mask |= bit[dx][dy];
                }
            Cell& c = out[static_cast<size_t>(cy)][static_cast<size_t>(cx)];
            c.mask = static_cast<uint8_t>(mask);
            c.level = static_cast<uint8_t>(sum / 8);
        }
    return out;
}

}  // namespace muisc
