#pragma once
// The picture of a video for the music player's "Video" view: ffmpeg decodes the file to raw RGB frames (a pipe, in a thread of its
// own), the screen asks for "the frame that belongs to the audio's time" and gets it. The sound is NOT played from here: the audio
// comes from the player as always (the track itself, or its own sound when the file is a video); this only follows its clock.
//   * a jump of the audio time (seek, next track) restarts the decoder at the new time
//   * while the audio is paused no frame is taken (the last one stays)
//   * the frame size is chosen by the caller (the field's pixels), the video's own proportions are kept
// Two ways to show a frame: as a picture (Kitty / Sixel: a palette of 240 colours, ordered dither) or as braille dots.
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "process_util.h"
#include "video_fx.h"

namespace muisc {

struct VideoFrame {
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;   // w * h * 3
    double pts = 0.0;           // seconds from the start of the file
};

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // The file to show ("" = none). The proportions are probed at once (ffprobe, short); false: no video stream / unreadable.
    bool set_source(const std::string& path);
    const std::string& source() const { return path_; }
    bool has_video() const { return !path_.empty() && vw_ > 0 && vh_ > 0; }
    double aspect() const { return vh_ > 0 ? static_cast<double>(vw_) / vh_ : 0.0; }   // width / height of the picture
    double duration() const { return duration_; }
    int source_width() const { return vw_; }   // the video's own picture width in pixels (0 = unknown)

    // The largest picture with the video's proportions that fits in a box of w x h units (a unit = a pixel, or a braille dot) whose
    // units have the shape unit_w : unit_h (1:1 for pixels / braille dots). Always at least 2 x 2, both even.
    void fit(int box_w, int box_h, double unit_aspect, int& out_w, int& out_h) const;

    // The smallest picture with the video's proportions that COVERS a box of w x h units (the rest is cropped away by the caller).
    void fit_cover(int box_w, int box_h, double unit_aspect, int& out_w, int& out_h) const;
    // A piece of a frame (x0, y0, w, h are clamped to the frame).
    static VideoFrame crop(const VideoFrame& f, int x0, int y0, int w, int h);

    // The frame for the audio time `t` at the size w x h. `playing` false: the audio is paused, nothing is taken from the decoder.
    // nullptr: no frame yet (the decoder is starting) -- the caller keeps showing what it showed. `fresh` is true when it differs
    // from the frame returned by the last call.
    const VideoFrame* frame_for(double t, int w, int h, bool playing, bool* fresh = nullptr);

    // The decoder's buffer: how many megabytes of decoded frames it keeps ahead (at least a few frames), and whether the picture is
    // scaled with the sharper filter (Video Resolution high / full). A new filter restarts the decoder; the buffer changes at once.
    void set_buffer_mb(int mb);
    void set_sharp(bool sharp) { sharp_ = sharp; }
    void stop();                  // ends the decoder (the source stays)
    // What the picture is doing while there is no frame: "starting ..." or why it failed ("ffmpeg: ..."); "" once a frame was shown.
    std::string status();
    int frames_per_second() const { return kFps; }

    // ---- turning a frame into something the terminal shows (pure functions, tested) ----
    // Palette picture: `hue` gets indices into palette() (216 colour cube + 24 greys), `level` is 255 everywhere. x0,y0: where the
    // frame sits in the dither pattern (keeps it still between frames).
    // The frame scaled (nearest neighbour) to w x h: shown while the decoder restarts after a resize.
    // (when the source is at least 1.5 times as large in both directions the pixels are averaged, a plain pick would alias)
    static VideoFrame resample(const VideoFrame& f, int w, int h);
    // The pixels of a frame averaged down to w x h (a box filter; a size that is not smaller is a plain copy / nearest pick).
    static VideoFrame downscale(const VideoFrame& f, int w, int h);
    // Retro looks for a frame (in place). fx: 1 scanlines, 2 crt, 3 vhs, 4 glitch, 5 pixel, 6 gradient (0 = nothing). `t` = the audio time (the effects move with it:
    // paused = they stand still). `braille`: the frame becomes dots (every effect is kept mild there: dots are only on or off).
    //   scanlines  the dark gaps between the lines of a tube (2-3 pixel rows per line, a soft beam profile)
    //   crt        scanlines + a slight horizontal softness + the RGB phosphor stripes + dark corners (a vignette) + a faint flicker
    //   vhs        colour fringes (red left, blue right), a wobbling picture, a tracking band that rolls up, head-switching noise at the
    //              bottom, film grain and paler colours
    //   glitch     now and then (about every second, for a split second) slices of the picture slip sideways with split colours and a
    //              flash of inverted colours; in between a light scanline look
    // Deterministic for the same frame / time (tested); no allocations beyond one copy of the frame.
    // `prev`: the frame shown before this one (the interlace look combs where the two differ; nullptr = none).
    static void apply_fx(VideoFrame& f, int fx, double t, bool braille = false, const VideoFxGradient* grad = nullptr, const VideoFrame* prev = nullptr, int dither = 0, int base = 0);   // dither: 0 auto, else a pattern of video_fx.h; base: the base layer of the tube looks (video_fx.h)
    // The grade: contrast (0..100 %: 0 = as it is, 100 = 2.5 times the distance from the middle grey), colours (0 = greyscale .. 100 = as they
    // are) and chromatic aberration (0 off .. 3 strong: red and blue are pushed apart, more towards the corners).
    static void apply_grade(VideoFrame& f, int contrast, int colors, int chroma, int brightness = 50);   // brightness 0..100: 50 = as it is, lower darker, higher brighter (a gamma curve: black and white stay)
    // The whole look: the grade, then the effect.
    // How many threads share the rows of the looks: 0 = automatic (half of the cores, at most 4; MOUSIKI_FX_THREADS= overrides), 1 = none.
    static void set_fx_threads(int n);
    static void apply_look(VideoFrame& f, const VideoLook& look, double t, bool braille = false, const VideoFrame* prev = nullptr);
    // The frame that was on screen before the current one (nullptr: none, or another size).
    const VideoFrame* previous_frame() const;
    static void quantize(const VideoFrame& f, std::vector<uint8_t>& hue, std::vector<uint8_t>& level, int dither = 0);   // (dither: see apply_fx)
    static const std::vector<std::array<uint8_t, 3>>& palette();
    // Braille: `cols` x `rows` cells from a frame that has (cols * 2) x (rows * 4) dots. Each cell: the dot mask (0x2800 + mask) and the
    // mean brightness 0..255 of its dots.
    struct Cell { uint8_t mask = 0; uint8_t level = 0; };
    static std::vector<std::vector<Cell>> braille(const VideoFrame& f, int cols, int rows);

private:
    static constexpr int kFps = 25;
    void start(double from, bool keep_picture = false);
    void run(uint64_t gen, double from, int w, int h);
    void halt();
    size_t queue_max(size_t frame_bytes) const;

    std::string path_;
    int vw_ = 0, vh_ = 0;
    double duration_ = 0.0;

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<VideoFrame> q_;         // decoded, not shown yet
    VideoFrame prev_;                  // the one before it (for the interlace look)
    bool prev_valid_ = false;
    VideoFrame cur_;                   // the frame on screen
    bool cur_valid_ = false;
    bool cur_returned_ = false;
    bool cur_stale_ = false;           // cur_ is the picture from before a restart: it stays on screen until the first new frame replaces it
    uint64_t gen_ = 0;                 // a restart bumps it: a thread of an older generation ends
    bool eof_ = false;
    std::thread th_;
    std::unique_ptr<ChildProcess> child_;
    std::mutex child_m_;
    int run_w_ = 0, run_h_ = 0;
    bool sharp_ = false, run_sharp_ = false;
    size_t buffer_bytes_ = 32u << 20;   // (guarded by m_)
    double run_from_ = 0.0;
    // The decoder keeps the LARGEST size asked for (the big overlay after the main screen, or the other way round): a smaller size is
    // the same decoded frame averaged down (scaled_), so switching between the two needs no restart -- no wait for ffmpeg, no frame that
    // moves. Only when a much smaller size has been the only one asked for for a long time the decoder is started again at that size.
    VideoFrame scaled_;                // cur_ averaged down to scaled_w_ x scaled_h_ (valid while scaled_valid_)
    bool scaled_valid_ = false;
    std::chrono::steady_clock::time_point oversize_since_{};
    // After a restart that only changed the size (the picture stays): the frames of the new decoder start a little before the one on
    // screen and the one that looks most like it (a pixel compare at the smaller of the two sizes) takes its place, so a paused video does not move by a frame.
    bool match_active_ = false;
    VideoFrame match_ref_;             // the picture that was on screen (compared with the first frames of the new decoder)
    double match_pts_ = 0.0;
    int pend_w_ = 0, pend_h_ = 0;      // a new size waiting to stay the same for a moment before the decoder restarts
    std::chrono::steady_clock::time_point pend_since_{};
    std::chrono::steady_clock::time_point restarted_at_{};
    bool running_ = false;
    std::string note_;                 // (guarded by m_) why there is no frame
    uint64_t frames_decoded_ = 0;
    std::chrono::steady_clock::time_point started_at_;
};

}  // namespace muisc
