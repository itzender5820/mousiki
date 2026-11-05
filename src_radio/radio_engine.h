#pragma once
// Radio mode -- the radio's OWN audio path.
//
//   ffmpeg (child process)  -->  worker thread  -->  RadioRing  -->  miniaudio device
//                                      |                   ^
//                                      v                   | (timeshift: while paused / behind live)
//                               TimeshiftStore  ---->  feeder thread
//                               (disk ring file, the last N minutes)
//   (network, decode, f32le)     (reconnects)        (~10 s, drops     (own ma_device,
//                                                      oldest on         own callback)
//                                                      overflow)
//
// Nothing here touches Player, StreamingPcm or the track decoder: a live
// stream has no duration, no seek position, no end, and StreamingPcm is built
// on exactly those three things. Sharing only the *visualizers* (FFT spectrum
// and XY oscilloscope, both of which just want a block of samples) keeps the
// two audio paths independent: the music player can change without breaking
// radio, and the other way round.
#include <limits>
#include <memory>
#include <string>
#include "equalizer.h"
#include "fft_visualizer.h"
#include "oscilloscope_visualizer.h"

using RadioScope = muisc::OscilloscopeVisualizer;   // the radio uses the player's scope class (one copy; its own settings live in radio_config.txt)
#include "radio_stations.h"

namespace muisc::radio {

enum class StreamState {
    Idle,          // nothing tuned
    Connecting,    // ffmpeg started, no audio yet
    Buffering,     // audio arriving, pre-buffer (or an underrun refill) not full yet
    Live,          // playing
    Reconnecting,  // the stream dropped; waiting to retry
    Failed         // gave up after repeated failures without ever getting audio
};

struct StreamInfo {
    std::string station;     // ICY name, falls back to the station list's name
    std::string genre;
    std::string country;     // from the station entry (the stream does not say)
    std::string codec;       // "AAC", "MP3", ...
    int bitrate_kbps = 0;
    int sample_rate = 0;
    int channels = 0;
    std::string artist;      // split from ICY StreamTitle "Artist - Title"
    std::string title;
    std::string host;
};

struct RadioStatus {
    StreamState state = StreamState::Idle;
    int tuned_index = -1;          // index into the caller's station list; -1 = a station that is not in it (Radio Browser result)
    std::string tuned_name;        // the station entry's own name (info.station may become the stream's ICY name)
    std::string tuned_url;         // its stream URL (how the UI recognises the tuned station in a result list)
    double dial_mhz = 0.0;         // its decorative dial position
    double buffer_sec = 0.0;       // audio waiting in the ring
    double listening_sec = 0.0;    // since the first audio of this tune
    int reconnects = 0;
    StreamInfo info;
    int volume = 70;
    bool muted = false;
    bool device_ok = false;
    std::string device_error;
    // Loudness of the tuned stream so far (LUFS; NaN until about half a second of audio was measured) and the gain
    // the normalisation applies on top of the volume (dB, 0 when it is off or nothing is measured yet).
    float lufs = std::numeric_limits<float>::quiet_NaN();
    float norm_gain_db = 0.0f;
    // Recording (key y): the stream is written to a WAV while it runs and converted to MP3 when it stops.
    bool recording = false;
    double recording_sec = 0.0;
    std::string record_note;       // result of the last finished recording ("saved <file>" / "failed: ..."); empty until one finished
    int record_serial = 0;         // counts finished recordings, so the UI can notice a new note
    // Timeshift (pause / rewind live radio): the last minutes of the station are kept in a buffer on disk.
    bool timeshift_paused = false;      // SPACE: paused, the stream keeps being buffered
    bool timeshift_shifted = false;     // playing from the buffer (paused or behind the live stream)
    double behind_sec = 0.0;            // how far the playback is behind the live stream
    double buffered_sec = 0.0;          // how much of the station is in the buffer (grows from the tune up to the cap)
    double timeshift_cap_sec = 0.0;     // buffer size
    long long heard_frame = 0;          // stream position of what is being heard (frames at 48 kHz since the tune)
    long long oldest_frame = 0;         // the oldest stream position still in the buffer
    int timeshift_edge_hits = 0;        // the playback was pushed forward because its audio had left the buffer
};

class RadioEngine {
public:
    RadioEngine();
    ~RadioEngine();
    RadioEngine(const RadioEngine&) = delete;
    RadioEngine& operator=(const RadioEngine&) = delete;

    // Opens the output device (48 kHz stereo float). With MOUSIKI_RADIO_NULL=1
    // in the environment the "null" backend is used instead (consumes audio in
    // real time, plays nothing) -- for headless tests. Returns false and fills
    // `err` on failure; the UI still works, it just stays silent.
    bool open_device(std::string* err = nullptr);
    // The sound output (Settings -> GENERAL -> AUDIO "Choose Audio Device", config.txt AudioDevice=; "" = the system default).
    // Before open_device() it only names the device to open; afterwards the device is opened again on the new output at once
    // (the stream goes on). A device that is not there plays on the default one. active_device(): the one that is open now.
    bool set_output_device(const std::string& name, std::string* err = nullptr);
    std::string active_device() const;

    // Tunes to a station. Returns immediately; the previous stream is cancelled
    // and everything else happens on worker threads.
    void tune(const Station& st, int index);
    void stop();
    // The station list changed (a station was deleted or came back): the playing stream goes on, only its index in the list moves (-1 = not in the list).
    void set_tuned_index(int index);
    // Starts the current station again from scratch (works for stations that are not in the list too).
    void reconnect();

    void set_volume(int pct);   // 0..100
    void set_muted(bool m);
    // Extra gain 0..1 on top of the volume, used by the sleep timer's fade-out (1 = untouched).
    void set_fade(float gain);
    // Radio static that fades in when a station is tuned and fades out once the new stream plays (off by default).
    void set_tune_noise(bool on);
    // Records what the tuned station delivers (before volume / mono fold) into `<dir>/<stem>.wav`, then converts it to
    // `<stem>.mp3` (ID3 title = `title`) in the background and removes the WAV; if the conversion fails the WAV stays.
    // Returns false (and fills `err`) when nothing is tuned or the file cannot be created. Tuning another station or
    // stop() ends the recording too.
    // `from_frame`: stream position the recording starts at (status().heard_frame minus N seconds for "the last N
    // minutes"; -1 = what is being heard right now). It is copied from the timeshift buffer, follows the stream until
    // stop_recording(), and ends at what is being heard then.
    bool start_recording(const std::string& dir, const std::string& stem, const std::string& title,
                         long long from_frame = -1, std::string* err = nullptr);
    void stop_recording();

    // --- timeshift: pause / rewind live radio ---
    // Buffer size in minutes and the file it lives in (call once at start and when the setting changes; empties it).
    void set_timeshift(int minutes, const std::string& file);
    void toggle_pause();                // pause / resume; the stream keeps being buffered while paused
    double jump(double seconds);        // -30 = back 30 s, +30 = forward; clamped to the buffer; returns the real move
    void go_live();                     // back to the live stream
    // false = left and right are folded together (mono); the loudness measurement restarts.
    // Oscilloscope music mode: the scopes get the stream before mono fold, equalizer, normalization and volume.
    void set_scope_raw(bool on);
    void set_stereo(bool on);
    // Loudness normalisation like the music player's: the stream's measured loudness is brought to `target_lufs`
    // (-40..0), amplifying by at most `max_boost_db` (0..24). The gain glides over about a second.
    void set_normalization(bool on, float target_lufs, float max_boost_db);
    // The player's 10-band graphic equaliser (equalizer.h), applied to the stream before volume and normalisation (the
    // loudness is still measured on the untouched stream). Safe to call from the UI thread at any time.
    void set_equalizer(bool on, const EqGains& gains_db);

    RadioStatus status() const;

    // Same visualizers the music player uses, fed from the radio callback.
    FftVisualizer& fft();
    RadioScope& scope();

private:
    bool open_output_locked(std::string* err);   // see radio_engine.cpp
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace muisc::radio
