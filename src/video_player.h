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

    // The largest picture with the video's proportions that fits in a box of w x h units (a unit = a pixel, or a braille dot) whose
    // units have the shape unit_w : unit_h (1:1 for pixels / braille dots). Always at least 2 x 2, both even.
    void fit(int box_w, int box_h, double unit_aspect, int& out_w, int& out_h) const;

    // The frame for the audio time `t` at the size w x h. `playing` false: the audio is paused, nothing is taken from the decoder.
    // nullptr: no frame yet (the decoder is starting) -- the caller keeps showing what it showed. `fresh` is true when it differs
    // from the frame returned by the last call.
    const VideoFrame* frame_for(double t, int w, int h, bool playing, bool* fresh = nullptr);

    void stop();                  // ends the decoder (the source stays)
    // What the picture is doing while there is no frame: "starting ..." or why it failed ("ffmpeg: ..."); "" once a frame was shown.
    std::string status();
    int frames_per_second() const { return kFps; }

    // ---- turning a frame into something the terminal shows (pure functions, tested) ----
    // Palette picture: `hue` gets indices into palette() (216 colour cube + 24 greys), `level` is 255 everywhere. x0,y0: where the
    // frame sits in the dither pattern (keeps it still between frames).
    // The frame scaled (nearest neighbour) to w x h: shown while the decoder restarts after a resize.
    static VideoFrame resample(const VideoFrame& f, int w, int h);
    static void quantize(const VideoFrame& f, std::vector<uint8_t>& hue, std::vector<uint8_t>& level);
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

    std::string path_;
    int vw_ = 0, vh_ = 0;
    double duration_ = 0.0;

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<VideoFrame> q_;         // decoded, not shown yet
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
    double run_from_ = 0.0;
    int pend_w_ = 0, pend_h_ = 0;      // a new size waiting to stay the same for a moment before the decoder restarts
    std::chrono::steady_clock::time_point pend_since_{};
    std::chrono::steady_clock::time_point restarted_at_{};
    bool running_ = false;
    std::string note_;                 // (guarded by m_) why there is no frame
    uint64_t frames_decoded_ = 0;
    std::chrono::steady_clock::time_point started_at_;
};

}  // namespace muisc
