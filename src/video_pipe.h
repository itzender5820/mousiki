// The picture of a video frame for the terminal (the look, the colour reduction, the rounded corners and the lyric hole), made in a thread of its own.
//
// Before, App::video_draw() did all of it on the UI thread: with a heavy look (Glitch, VHS, Technicolor ...) that is 20-40 ms of a frame at 560 x 300 pixels
// and more at a larger picture -- the keys, the other panels and the audio display all waited for it. Now a frame that was just decoded is handed to
// the worker (a copy of the visible part), the screen goes on at once, and the finished picture is taken at the next pass of the loop:
//   * the newest frame wins: a frame that has not been started when the next one comes is dropped (a slow machine shows fewer pictures per
//     second instead of a late picture that gets later and later), so the work can never pile up
//   * a picture is only taken for the geometry (the key) it was made for; after a resize / a new look the picture is made on the spot, as before
//     (the new size must be on screen at once), and what the worker was doing is thrown away
//   * only when it pays: a frame that costs a few milliseconds (a plain colour reduction) is made on the spot, there is no wait for the next
//     pass; the worker is used when the last pictures cost more than kAsyncOnMs, and left again below kAsyncOffMs
//   * the same function makes the picture on the spot and in the worker, so both give the same pixels (tested)
// MOUSIKI_VIDEO_THREAD=0 keeps everything on the UI thread (for a comparison, or a machine with one core).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video_player.h"

namespace muisc {

// What the picture needs besides the frame itself.
struct VideoPicParams {
    VideoLook look;
    double t = 0.0;              // the audio time the frame belongs to (the looks move with it)
    bool truecolor = false;      // Kitty with high / full resolution: the picture keeps its own colours (no palette)
    bool round_corners = false;  // the four corners cut round
    int vcols = 1, vrows = 1;    // the picture's size in cells (the radius of the corners follows the cell size)
    int trim = 0;                // columns of pixels cut off at the right
    int cut_c = 0, cut_r = 0, cut_w = 0, cut_h = 0;   // the lyric box: cells (relative to the picture) that are cut out; cut_w = 0: none
};

// A finished picture: the buffers go straight into the GfxFrame (swapped, so no memory is allocated per frame).
struct VideoPic {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;    // truecolor: w * h * 3 (else empty)
    std::vector<uint8_t> hue;    // palette: w * h (else empty)
    std::vector<uint8_t> level;  // w * h: 255 = opaque, 0 = cut out
    double look_ms = 0.0, quant_ms = 0.0;   // what it cost (the look, the colour reduction with the masks)
};

// The picture of `src` (the visible part of a frame, already cut to its final size): look (`prev` = the frame before it, for the interlace look),
// then the colours (true colour or the palette with the look's dither pattern), then the corners / the trim / the lyric hole cut out of `level`.
// Writes into `out` (its buffers are reused). Records the times in `out` and in video_timing().
void video_make_picture(const VideoFrame& src, const VideoFrame* prev, const VideoPicParams& p, VideoPic& out);

class VideoPipe {
public:
    VideoPipe();
    ~VideoPipe();
    VideoPipe(const VideoPipe&) = delete;
    VideoPipe& operator=(const VideoPipe&) = delete;

    // Is the worker worth it now? (the last pictures cost more than kAsyncOnMs, with a margin back; always false with MOUSIKI_VIDEO_THREAD=0)
    bool wants_thread() const;
    // What a picture just cost (milliseconds, made on the spot or by the worker): decides wants_thread().
    void note_cost(double ms);

    // Hands a frame over (`f` is copied, cut to the visible part cx0, cy0, w x h when that is not all of it; `prev`: the frame before it, already
    // cut and scaled like the picture, or nullptr). A frame that is waiting for the worker is replaced. `key` says what the picture is for.
    // false: the worker could not be started (the caller makes the picture itself, and the worker stays off).
    bool submit(const std::string& key, const VideoFrame& f, int cx0, int cy0, int w, int h, const VideoFrame* prev, const VideoPicParams& p);
    // The finished picture for `key` and the size w x h, if the worker has one: its buffers are swapped into the arguments (the arguments' old
    // buffers go back to the worker). A picture for another key or size is dropped. false: nothing (yet).
    bool take(const std::string& key, int w, int h, std::vector<uint8_t>& rgb, std::vector<uint8_t>& hue, std::vector<uint8_t>& level);
    // Forgets the frame that waits and the picture that is finished (a frame the worker is busy with is thrown away when it is done).
    void cancel();
    // How many frames were dropped for a newer one (for the tests / the log).
    long dropped() const { return dropped_.load(); }
    // The cost (ms, smoothed) and whether the worker is on (for the tests).
    double cost_ms() const { return cost_.load(); }

    static constexpr double kAsyncOnMs = 6.0;
    static constexpr double kAsyncOffMs = 2.5;

private:
    bool start_locked();   // (m_ held) starts the thread (false: it could not be started)
    void loop();

    mutable std::mutex m_;
    std::condition_variable cv_;       // a job came / the pipe stops
    std::thread th_;
    bool stop_ = false;
    // the frame that waits
    struct Job {
        std::string key;
        VideoFrame src, prev;
        bool has_prev = false;
        VideoPicParams p;
        unsigned gen = 0;
    };
    Job pending_;
    bool has_pending_ = false;
    unsigned gen_ = 1;                 // bumped by cancel(): what was started before is stale
    // the finished picture
    VideoPic result_;
    std::string result_key_;
    bool has_result_ = false;

    std::atomic<double> cost_{0.0};
    std::atomic<bool> async_{false};
    std::atomic<long> dropped_{0};
    std::atomic<bool> enabled_{true};
};

}  // namespace muisc
