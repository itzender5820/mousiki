// The radio engine: plays the station streams, keeps a 10 second timeshift ring, records to file and
// switches between stations.
#include "radio_engine.h"
#include "scope_window.h"
#include "spectrogram.h"
#include "loudness_meter.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include "audio_backend.h"
#include "utf8_util.h"   // clean_tag_text(): the station's metadata
#include "miniaudio.h"
#include "process_util.h"
#include "string_util.h"
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace muisc::radio {

namespace {

constexpr int kRate = 48000;
constexpr int kChannels = 2;
constexpr size_t kRingFrames = static_cast<size_t>(kRate) * 10; // 10 s
constexpr size_t kPrebufferFrames = static_cast<size_t>(kRate) * 3 / 2; // start (or resume) at 1.5 s

using Clock = std::chrono::steady_clock;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Ring buffer: mutex-protected, interleaved stereo float. A short lock per
// block on the audio thread is the same trade the oscilloscope already makes.
// A "generation" number lets a cancelled (stale) worker discover it has been
// replaced the moment it tries to write, without any other signalling.
// ---------------------------------------------------------------------------
struct Ring {
    std::mutex m;
    std::vector<float> buf = std::vector<float>(kRingFrames * kChannels);
    size_t head = 0;   // frame index of the oldest sample
    size_t count = 0;  // frames stored
    unsigned gen = 0;
    // Timeshift: the position of the oldest stored frame in the stream since it was tuned (frames counted from the
    // first audio, the same numbering as the TimeshiftStore). What the device plays next = head_abs.
    long long head_abs = 0;

    unsigned reset() {
        std::lock_guard<std::mutex> lk(m);
        head = count = 0;
        head_abs = 0;
        return ++gen;
    }
    // Empties the ring without invalidating the writer (timeshift jumps); the next frame is `abs`.
    void clear(long long abs) {
        std::lock_guard<std::mutex> lk(m);
        head = count = 0;
        head_abs = abs;
    }
    unsigned current_gen() {
        std::lock_guard<std::mutex> lk(m);
        return gen;
    }
    long long heard_abs() {
        std::lock_guard<std::mutex> lk(m);
        return head_abs;
    }
    // false = this writer's generation is stale; it must stop. `abs` = stream position of in[0].
    bool write(unsigned g, const float* in, size_t frames, long long abs = -1) {
        std::lock_guard<std::mutex> lk(m);
        if (g != gen) return false;
        if (count == 0 && abs >= 0) head_abs = abs;
        if (frames >= kRingFrames) { in += (frames - kRingFrames) * kChannels; head_abs += static_cast<long long>(frames - kRingFrames); frames = kRingFrames; head = count = 0; }
        const size_t room = kRingFrames - count;
        if (frames > room) { // live stream running ahead of the device clock: drop the oldest audio
            const size_t drop = frames - room;
            head = (head + drop) % kRingFrames;
            count -= drop;
            head_abs += static_cast<long long>(drop);
        }
        size_t tail = (head + count) % kRingFrames;
        for (size_t i = 0; i < frames; ++i) {
            std::memcpy(&buf[tail * kChannels], in + i * kChannels, sizeof(float) * kChannels);
            if (++tail == kRingFrames) tail = 0;
        }
        count += frames;
        return true;
    }
    size_t read(float* out, size_t frames) {
        std::lock_guard<std::mutex> lk(m);
        const size_t n = std::min(frames, count);
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(out + i * kChannels, &buf[head * kChannels], sizeof(float) * kChannels);
            if (++head == kRingFrames) head = 0;
        }
        count -= n;
        head_abs += static_cast<long long>(n);
        return n;
    }
    size_t fill() {
        std::lock_guard<std::mutex> lk(m);
        return count;
    }
};

// ---------------------------------------------------------------------------
// Timeshift store: the last N minutes of the tuned station as 16-bit stereo PCM in a ring FILE on disk
// (~/.cache/mousiki/radio_timeshift.pcm; 48 kHz * 4 bytes = 11.5 MB per minute, so 30 min = 345 MB). On disk rather
// than in memory so a long buffer costs no RAM, and the write rate (192 KB/s) is nothing for any disk. Frames are
// numbered from the first audio of the tune (`end` = frames written so far); the file keeps
// [max(0, end - cap), end). The worker writes, the timeshift feeder and the recorder read -- one FILE under a mutex,
// with a seek before every access.
// ---------------------------------------------------------------------------
struct TimeshiftStore {
    mutable std::mutex m;
    FILE* f = nullptr;
    std::string path;
    long long cap = 0;                    // frames the file holds
    std::atomic<long long> end{0};        // frames written since the tune
    unsigned gen = 0;                     // bumped by reset(): a stale worker's writes are refused
    std::vector<int16_t> scratch;

    bool open(const std::string& p, long long cap_frames) {
        std::lock_guard<std::mutex> lk(m);
        if (f) { std::fclose(f); f = nullptr; }
        path = p;
        cap = std::max<long long>(cap_frames, kRate * 60);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
        f = std::fopen(p.c_str(), "w+b");
        end.store(0);
        return f != nullptr;
    }
    void close_and_remove() {
        std::lock_guard<std::mutex> lk(m);
        if (f) { std::fclose(f); f = nullptr; }
        std::error_code ec;
        if (!path.empty()) std::filesystem::remove(path, ec);
    }
    unsigned reset() {
        std::lock_guard<std::mutex> lk(m);
        end.store(0);
        return ++gen;
    }
    long long oldest() const { return std::max<long long>(0, end.load() - cap); }
    // Appends `frames` (float stereo); false if `g` is stale.
    bool write(unsigned g, const float* in, size_t frames) {
        std::lock_guard<std::mutex> lk(m);
        if (g != gen) return false;
        if (!f || frames == 0) { if (g == gen) end.fetch_add(static_cast<long long>(frames)); return true; }
        scratch.resize(frames * kChannels);
        for (size_t i = 0; i < scratch.size(); ++i)
            scratch[i] = static_cast<int16_t>(std::lround(std::clamp(in[i], -1.0f, 1.0f) * 32767.0f));
        long long pos = end.load();
        size_t done = 0;
        while (done < frames) {
            const long long slot = pos % cap;
            const size_t n = static_cast<size_t>(std::min<long long>(static_cast<long long>(frames - done), cap - slot));
            fseek_abs(slot);
            std::fwrite(scratch.data() + done * kChannels, sizeof(int16_t) * kChannels, n, f);
            done += n; pos += static_cast<long long>(n);
        }
        end.store(pos);
        return true;
    }
    // Reads up to `frames` frames starting at stream position `abs` as raw 16-bit (the recorder) -- clamped to what is
    // still stored. Returns the frames read.
    size_t read_raw(long long abs, int16_t* out, size_t frames) {
        std::lock_guard<std::mutex> lk(m);
        if (!f) return 0;
        const long long e = end.load(), o = std::max<long long>(0, e - cap);
        if (abs < o || abs >= e) return 0;
        frames = static_cast<size_t>(std::min<long long>(static_cast<long long>(frames), e - abs));
        size_t done = 0;
        while (done < frames) {
            const long long slot = (abs + static_cast<long long>(done)) % cap;
            const size_t n = static_cast<size_t>(std::min<long long>(static_cast<long long>(frames - done), cap - slot));
            fseek_abs(slot);
            const size_t got = std::fread(out + done * kChannels, sizeof(int16_t) * kChannels, n, f);
            done += got;
            if (got < n) break;
        }
        return done;
    }
    size_t read(long long abs, float* out, size_t frames) {
        std::vector<int16_t> tmp(frames * kChannels);
        const size_t n = read_raw(abs, tmp.data(), frames);
        for (size_t i = 0; i < n * kChannels; ++i) out[i] = static_cast<float>(tmp[i]) / 32768.0f;
        return n;
    }
private:
    void fseek_abs(long long slot) {
        const long long byte = slot * static_cast<long long>(sizeof(int16_t) * kChannels);
#if defined(_WIN32)
        _fseeki64(f, byte, SEEK_SET);
#else
        fseeko(f, static_cast<off_t>(byte), SEEK_SET);
#endif
    }
};

// One tune() = one Session. Workers hold it (and the ring) by shared_ptr, so a
// cancelled session can finish winding down after the engine is gone.
struct Session {
    Station station;
    std::atomic<int> index{-1};   // set_tuned_index() moves it when the station list changes under a playing stream
    std::shared_ptr<Ring> ring;
    unsigned gen = 0;
    // Timeshift: the worker writes everything into the store; the ring only gets it while `shifted` is false (live
    // listening). switch_m makes "store, then ring" one step against the feeder's switch back to live.
    std::shared_ptr<TimeshiftStore> store;
    unsigned store_gen = 0;
    std::shared_ptr<std::atomic<bool>> shifted;
    std::shared_ptr<std::mutex> switch_m;
    std::atomic<bool> cancel{false};
    std::atomic<int> state{static_cast<int>(StreamState::Connecting)}; // Connecting/Reconnecting/Failed or 99 = receiving
    std::atomic<long long> first_audio_ms{0};
    std::atomic<int> reconnects{0};
    std::mutex info_m;
    StreamInfo info;
};
constexpr int kReceiving = 99;

std::string host_of(const std::string& url) {
    size_t a = url.find("://");
    a = (a == std::string::npos) ? 0 : a + 3;
    size_t b = url.find_first_of("/:?", a);
    return url.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// Offline test signals, generated by ffmpeg's lavfi so the whole pipeline
// (spawn, pipe, ring, device, visualizers) can be exercised with no network.
Args lavfi_args(const std::string& name) {
    if (name == "tone")
        return {"-re", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"};
    if (name == "noise")
        return {"-re", "-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.25:sample_rate=48000"};
    // "chords": two slowly detuning tones per channel -> a drifting Lissajous figure on the XY scope
    return {"-re", "-f", "lavfi", "-i",
            "aevalsrc=0.30*sin(2*PI*220*t+2*sin(2*PI*0.20*t))+0.20*sin(2*PI*277.18*t)"
            "|0.30*sin(2*PI*329.63*t+2*sin(2*PI*0.13*t))+0.20*sin(2*PI*220*t):s=48000"};
}

Args ffmpeg_cmd(const Station& st) {
    Args cmd = {"ffmpeg", "-nostdin", "-v", "error"};
    if (st.synthetic()) {
        const Args src = lavfi_args(st.url.substr(6));
        cmd.insert(cmd.end(), src.begin(), src.end());
    } else {
        // rw_timeout: give up on a stalled socket after 15 s instead of blocking forever.
        // A web address may only lead to the web (a redirect to file: or another protocol is refused by ffmpeg itself).
        if (Station::web_url(st.url)) cmd.insert(cmd.end(), {"-protocol_whitelist", "http,https,tcp,tls,crypto,httpproxy"});
        cmd.insert(cmd.end(), {"-rw_timeout", "15000000", "-reconnect", "1", "-reconnect_streamed", "1",
                               "-reconnect_delay_max", "5", "-i", st.url});
    }
    // To 48 kHz with a long, sharp resampling filter (ffmpeg's default one is shorter and softer at the top end).
    cmd.insert(cmd.end(), {"-vn", "-af", "aresample=" + std::to_string(kRate) + ":filter_size=64:phase_shift=10:cutoff=0.97",
                           "-f", "f32le", "-ac", "2", "-ar", std::to_string(kRate), "-"});
    return cmd;
}

// ---- recorder: copies a range of the timeshift store into a 16-bit WAV (converted to MP3 when it ends) ---------
// Recording starts at any stream position still in the store -- "from now on" (what was playing when the overlay
// opened) or minutes in the past -- and follows the stream until stop(): its own thread copies from the store, so a
// long backlog ("the last 30 minutes") never holds up the audio or the UI.
struct Recorder {
    std::mutex m;
    FILE* f = nullptr;
    std::string wav, mp3, title;
    std::shared_ptr<TimeshiftStore> store;
    std::thread th;
    std::atomic<long long> frames{0};       // written to the WAV so far
    std::atomic<long long> start_abs{0};
    std::atomic<long long> stop_at{-1};     // -1 = follow the stream
    std::atomic<bool> active{false};
    std::atomic<int> serial{0};
    std::atomic<int> converting{0};   // conversions still running (the destructor waits for them)
    std::string note;           // guarded by m

    static void put_u32(unsigned char* p, unsigned v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i)); }

    bool start(std::shared_ptr<TimeshiftStore> st, long long from, const std::string& wav_path, const std::string& mp3_path,
               const std::string& t, std::string* err) {
        std::lock_guard<std::mutex> lk(m);
        if (f || active.load()) { if (err) *err = "already recording"; return false; }
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(wav_path).parent_path(), ec);
        f = std::fopen(wav_path.c_str(), "wb");
        if (!f) { if (err) *err = "cannot write " + wav_path; return false; }
        unsigned char h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a',0,0,0,0};
        put_u32(h + 24, kRate); put_u32(h + 28, kRate * kChannels * 2);
        std::fwrite(h, 1, sizeof h, f);
        wav = wav_path; mp3 = mp3_path; title = t;
        store = std::move(st);
        frames.store(0);
        start_abs.store(std::max(from, store->oldest()));
        stop_at.store(-1);
        active.store(true);
        if (th.joinable()) th.join();
        th = std::thread([this]() { copy_loop(); });
        return true;
    }
    void copy_loop() {
        std::vector<int16_t> buf(static_cast<size_t>(kRate) * kChannels);
        long long pos = start_abs.load();
        for (;;) {
            const long long stop = stop_at.load();
            const long long end = stop >= 0 ? std::min(stop, store->end.load()) : store->end.load();
            if (pos < store->oldest()) pos = store->oldest();   // fell out of the buffer (cannot happen while it follows)
            if (pos < end) {
                const size_t n = store->read_raw(pos, buf.data(), static_cast<size_t>(std::min<long long>(kRate, end - pos)));
                if (n == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
                std::lock_guard<std::mutex> lk(m);
                if (f) std::fwrite(buf.data(), sizeof(int16_t) * kChannels, n, f);
                frames.fetch_add(static_cast<long long>(n));
                pos += static_cast<long long>(n);
                continue;
            }
            if (stop >= 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    // Ends the recording at stream position `at` (what is being heard; -1 = everything stored so far), closes the WAV
    // and hands the conversion to a detached thread. Waits until the copy has caught up to `at`.
    void stop(long long at = -1) {
        if (!active.load()) return;
        stop_at.store(at >= 0 ? std::max(at, start_abs.load()) : store->end.load());
        if (th.joinable()) th.join();
        std::string w, o, t;
        long long fr;
        {
            std::lock_guard<std::mutex> lk(m);
            active.store(false);
            if (!f) return;
            fr = frames.load();
            const unsigned data = static_cast<unsigned>(fr * kChannels * 2);
            unsigned char b[4];
            std::fseek(f, 4, SEEK_SET); put_u32(b, 36 + data); std::fwrite(b, 1, 4, f);
            std::fseek(f, 40, SEEK_SET); put_u32(b, data); std::fwrite(b, 1, 4, f);
            std::fclose(f); f = nullptr;
            w = wav; o = mp3; t = title;
        }
        converting.fetch_add(1);
        std::thread([this, w, o, t, fr]() {
            std::string result;
            try {
            if (fr < kRate / 2) {   // under half a second: nothing worth keeping
                std::error_code ec; std::filesystem::remove(w, ec);
                result = "recording too short, discarded";
            } else {
                const Args cmd = {"ffmpeg", "-nostdin", "-v", "error", "-y", "-i", w, "-c:a", "libmp3lame", "-q:a", "2",
                                  "-metadata", "title=" + t, o};
                ProcResult r = run_capture(cmd, true);
                std::error_code ec;
                if (r.ok() && std::filesystem::exists(o, ec)) { std::filesystem::remove(w, ec); result = "saved " + o; }
                else result = "mp3 conversion failed, kept " + w;
            }
            } catch (...) { result = "mp3 conversion failed, kept " + w; }   // (the counter below must be counted down whatever happens)
            { std::lock_guard<std::mutex> lk(m); note = result; }
            serial.fetch_add(1);
            converting.fetch_sub(1);
        }).detach();
    }
};
Recorder g_rec;

// ---- worker: ffmpeg -> ring -----------------------------------------------
void sleep_cancellable(const Session& s, int ms) {
    for (int waited = 0; waited < ms && !s.cancel.load(); waited += 100)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

void worker_main(std::shared_ptr<Session> s) {
    int failures_without_audio = 0;
    int backoff_ms = 1000;
    std::vector<char> pending;
    std::vector<float> frames_buf;
    std::vector<char> rd(32768);

    while (!s->cancel.load()) {
        s->state.store(s->reconnects.load() > 0 ? static_cast<int>(StreamState::Reconnecting)
                                                 : static_cast<int>(StreamState::Connecting));
        auto child = spawn_capture(ffmpeg_cmd(s->station), /*merge_stderr=*/false);
        bool got_audio = false;
        if (child) {
            pending.clear();
            std::ptrdiff_t n;
            while (!s->cancel.load() && (n = child->read(rd.data(), rd.size())) > 0) {
                pending.insert(pending.end(), rd.data(), rd.data() + n);
                const size_t frame_bytes = sizeof(float) * kChannels;
                const size_t whole = pending.size() / frame_bytes;
                if (whole == 0) continue;
                frames_buf.resize(whole * kChannels);
                std::memcpy(frames_buf.data(), pending.data(), whole * frame_bytes);
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(whole * frame_bytes));
                {
                    std::lock_guard<std::mutex> sw(*s->switch_m);
                    const long long abs = s->store->end.load();
                    if (!s->store->write(s->store_gen, frames_buf.data(), whole)) { s->cancel.store(true); break; }
                    if (!s->shifted->load() && !s->ring->write(s->gen, frames_buf.data(), whole, abs)) { s->cancel.store(true); break; }
                }
                if (!got_audio) {
                    got_audio = true;
                    failures_without_audio = 0;
                    backoff_ms = 1000;
                    long long expected = 0;
                    s->first_audio_ms.compare_exchange_strong(expected, now_ms());
                    s->state.store(kReceiving);
                }
            }
            // Leaving scope destroys the ChildProcess: closes the pipe (ffmpeg gets SIGPIPE on
            // its next write and exits) and reaps it.
            child.reset();
        }
        if (s->cancel.load()) break;
        if (!got_audio && ++failures_without_audio >= 5) {
            s->state.store(static_cast<int>(StreamState::Failed));
            return;
        }
        s->reconnects.fetch_add(1);
        s->state.store(static_cast<int>(StreamState::Reconnecting));
        sleep_cancellable(*s, backoff_ms);
        backoff_ms = std::min(backoff_ms * 2, 8000);
    }
}

// ---- probe: ffprobe -> station metadata (ICY name/genre/title, codec, bitrate) ----
std::string value_after(const std::string& line, const std::string& key) {
    if (line.compare(0, key.size(), key) == 0 && line.size() > key.size() && line[key.size()] == '=')
        return line.substr(key.size() + 1);
    return "";
}

std::string upper_codec(std::string c) {
    if (c == "aac" || c == "aac_latm") return "AAC";
    if (c == "mp3" || c == "mp3float") return "MP3";
    if (c == "vorbis") return "Vorbis";
    if (c == "opus") return "Opus";
    if (c == "flac") return "FLAC";
    std::transform(c.begin(), c.end(), c.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return c;
}

// The stream's metadata comes from the station (anyone can add one to Radio Browser): clean_tag_text() removes control characters
// (a terminal acts on them: ESC sequences could redraw the screen or, in some terminals, reach the clipboard) and broken UTF-8.
void apply_stream_title(StreamInfo& info, const std::string& raw) {
    const std::string t = clean_tag_text(raw);
    size_t dash = t.find(" - ");
    if (dash != std::string::npos) { info.artist = t.substr(0, dash); info.title = t.substr(dash + 3); }
    else { info.artist.clear(); info.title = t; }
}

void probe_main(std::shared_ptr<Session> s) {
    // Polled, not streamed: ffprobe opens its own short connection to the stream. That is the
    // simplest way to get ICY metadata out of ffmpeg without writing an HTTP client, and it is
    // cheap, but it does show up as one extra (brief) listener on the station every 20 s.
    Args cmd = {"ffprobe", "-v", "error", "-rw_timeout", "10000000",
                "-show_entries", "format_tags:stream=codec_name,bit_rate,sample_rate,channels", "-of", "default=noprint_wrappers=1"};
    if (Station::web_url(s->station.url)) cmd.insert(cmd.end(), {"-protocol_whitelist", "http,https,tcp,tls,crypto,httpproxy"});
    cmd.insert(cmd.end(), {"-i", s->station.url});   // -i: an address that starts with "-" is not read as an option
    while (!s->cancel.load()) {
        ProcResult r = run_capture(cmd, false);
        if (s->cancel.load()) return;
        std::istringstream in(r.out);
        std::string line;
        StreamInfo got;
        std::string stream_title;
        bool any = false;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            std::string v;
            if (!(v = value_after(line, "codec_name")).empty()) { got.codec = upper_codec(v); any = true; }
            else if (!(v = value_after(line, "sample_rate")).empty()) got.sample_rate = muisc::atoi_safe(v.c_str());
            else if (!(v = value_after(line, "channels")).empty()) got.channels = muisc::atoi_safe(v.c_str());
            else if (!(v = value_after(line, "bit_rate")).empty()) { int b = muisc::atoi_safe(v.c_str()); if (b > 0) got.bitrate_kbps = b / 1000; }
            else if (!(v = value_after(line, "TAG:icy-br")).empty()) { int b = muisc::atoi_safe(v.c_str()); if (b > 0) got.bitrate_kbps = b; }
            else if (!(v = value_after(line, "TAG:icy-name")).empty()) got.station = clean_tag_text(v);
            else if (!(v = value_after(line, "TAG:icy-genre")).empty()) got.genre = clean_tag_text(v);
            else if (!(v = value_after(line, "TAG:StreamTitle")).empty()) stream_title = v;
        }
        if (any || !stream_title.empty()) {
            std::lock_guard<std::mutex> lk(s->info_m);
            if (!got.codec.empty()) s->info.codec = got.codec;
            if (got.sample_rate) s->info.sample_rate = got.sample_rate;
            if (got.channels) s->info.channels = got.channels;
            if (got.bitrate_kbps) s->info.bitrate_kbps = got.bitrate_kbps;
            if (!got.station.empty()) s->info.station = got.station;
            if (!got.genre.empty()) s->info.genre = got.genre;
            if (!stream_title.empty()) apply_stream_title(s->info, stream_title);
        }
        sleep_cancellable(*s, 20000);
    }
}

} // namespace

// ===========================================================================
struct RadioEngine::Impl {
    std::shared_ptr<Ring> ring = std::make_shared<Ring>();
    // --- timeshift (pause / rewind live radio) ---
    std::shared_ptr<TimeshiftStore> store = std::make_shared<TimeshiftStore>();
    std::shared_ptr<std::atomic<bool>> shifted = std::make_shared<std::atomic<bool>>(false);   // playing from the store
    std::shared_ptr<std::mutex> switch_m = std::make_shared<std::mutex>();
    std::atomic<bool> paused{false};
    std::atomic<long long> feed_pos{0};        // next store frame the feeder copies into the ring
    std::atomic<bool> feeder_quit{false};
    std::atomic<int> edge_hits{0};             // the feeder had to skip ahead because the buffer's oldest audio was gone
    std::thread feeder;
    int timeshift_minutes = 30;
    std::string timeshift_path;

    // While shifted (and not paused): keeps ~3 s of store audio in the ring; when it has caught up with the stream it
    // hands over to the live worker without a gap (under switch_m, so no chunk is missed or doubled).
    void feeder_main() {
        std::vector<float> buf(static_cast<size_t>(kRate / 4) * kChannels);
        while (!feeder_quit.load()) {
            if (!shifted->load() || paused.load() || ring->fill() >= static_cast<size_t>(kRate) * 3) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            std::lock_guard<std::mutex> sw(*switch_m);
            if (!shifted->load() || paused.load()) continue;
            long long pos = feed_pos.load();
            const long long margin = kRate * 2;   // never read where the writer is about to overwrite
            if (pos < store->oldest() + margin && store->end.load() > store->cap) { pos = store->oldest() + margin; edge_hits.fetch_add(1); ring->clear(pos); }
            const long long end = store->end.load();
            const unsigned g = ring->current_gen();
            if (pos >= end) {          // caught up: from now on the worker writes straight into the ring again
                shifted->store(false);
                continue;
            }
            // Copy at most a quarter second at a time; if the rest up to the live edge is short, copy it all and go live.
            const size_t n = static_cast<size_t>(std::min<long long>(kRate / 4, end - pos));
            const size_t got = store->read(pos, buf.data(), n);
            if (got == 0) continue;
            ring->write(g, buf.data(), got, pos);
            feed_pos.store(pos + static_cast<long long>(got));
            if (pos + static_cast<long long>(got) >= end) shifted->store(false);
        }
    }
    mutable std::mutex session_m;
    std::shared_ptr<Session> session;

    ma_context context{};
    bool context_ready = false;
    ma_device device{};
    bool device_ready = false;
    std::string device_error;
    std::string device_name;        // the chosen output ("" = default)
    std::string active_device;      // the output that is open now
    mutable std::mutex device_m;    // guards device_name / active_device / reopening

    std::atomic<int> volume{70};
    std::atomic<float> fade{1.0f};     // sleep timer fade-out, multiplies the volume (1 = none)
    std::atomic<bool> muted{false};
    std::atomic<bool> primed{false};
    std::atomic<bool> stereo{true};
    // Tuning noise: a burst of radio static that fades in when a station is tuned and fades out once the new stream plays.
    std::atomic<bool> noise_on{false};        // the ON/OFF option
    std::atomic<bool> noise_hold{false};      // set by tune(): static stays up until the stream plays (or the cap runs out)
    std::atomic<bool> noise_restart{false};   // tune() -> the callback restarts its hold timer
    float noise_env = 0.0f;                   // callback only: current static level 0..1
    long long noise_held = 0;                 // callback only: frames the static has been held
    float pk[7] = {0, 0, 0, 0, 0, 0, 0};      // callback only: pink-noise filter state
    uint32_t noise_rng = 0x9E3779B9u;
    int glitch_left = 0, glitch_len = 1, glitch_type = 0, glitch_hold = 8, next_glitch = 4800;   // callback only
    float held = 0.0f;
    std::atomic<bool> norm_on{true};
    std::atomic<float> norm_target{-16.0f};
    std::atomic<float> norm_boost{9.0f};
    std::atomic<bool> meter_reset{true};       // set by tune / stop / the stereo switch; consumed by the callback
    std::atomic<float> lufs{std::numeric_limits<float>::quiet_NaN()};
    std::atomic<float> norm_gain_db{0.0f};
    LoudnessMeter meter{kRate, 2};             // audio thread only
    float norm_cur = -1.0f;                    // applied normalisation gain (linear), audio thread only
    // Equaliser: the UI thread publishes gains through the atomics, the callback re-configures when the generation changed.
    std::atomic<bool> eq_on{false};
    std::array<std::atomic<float>, kEqBands> eq_gains{};
    std::atomic<unsigned> eq_gen{1};
    Equalizer eq;                              // audio thread only
    unsigned eq_seen_gen = 0;                  // audio thread only

    FftVisualizer fft;
    RadioScope scope;
    std::vector<float> mono;   // scratch for the FFT feed, sized before the device starts
    std::vector<float> scope_raw;              // scratch: the unprocessed stream for the scopes (oscilloscope music mode)
    std::atomic<bool> scope_raw_on{false};

    static void data_callback(ma_device* dev, void* output, const void*, ma_uint32 frame_count) {
        auto* self = static_cast<Impl*>(dev->pUserData);
        float* out = static_cast<float*>(output);
        size_t got = 0;
        const size_t want = frame_count;
        if (self->paused.load(std::memory_order_relaxed)) {
            // Timeshift pause: nothing is taken from the ring (the stream keeps going into the store).
        } else if (!self->primed.load()) {
            if (self->ring->fill() >= kPrebufferFrames) self->primed.store(true);
        }
        if (self->primed.load() && !self->paused.load(std::memory_order_relaxed)) {
            got = self->ring->read(out, want);
            if (got < want) self->primed.store(false); // underrun: output silence, refill to the pre-buffer again
        }
        if (got < want) std::memset(out + got * kChannels, 0, (want - got) * kChannels * sizeof(float));
        // Oscilloscope music mode: the scopes get the stream as it comes (before mono fold, equalizer, normalization,
        // volume and the tuning noise).
        const bool raw_scope = self->scope_raw_on.load(std::memory_order_relaxed) && self->scope_raw.size() >= want * kChannels;
        if (raw_scope) std::memcpy(self->scope_raw.data(), out, want * kChannels * sizeof(float));
        muisc::spectro_feed_push(out, want, kRate);   // the spectrogram: the stream as it comes; nothing while none is shown
        muisc::spectro_window_feed_push(out, want, kRate);   // and the spectrogram window (SHIFT+8); nothing while it is closed

        // Mono: fold left and right together (before the loudness measurement, so it measures what is heard).
        if (!self->stereo.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < want; ++i) {
                const float m = 0.5f * (out[i * 2] + out[i * 2 + 1]);
                out[i * 2] = out[i * 2 + 1] = m;
            }
        }
        // Loudness of the stream so far (before volume and normalisation).
        if (self->meter_reset.exchange(false)) {
            self->meter.reset(kRate, 2);
            self->lufs.store(std::numeric_limits<float>::quiet_NaN());
        }
        if (got > 0) {
            self->meter.push(out, got);
            self->lufs.store(static_cast<float>(self->meter.integrated_lufs()), std::memory_order_relaxed);
        }
        // Normalisation target gain, exactly like the player: none until measured; clamp(target - lufs, -30, max boost).
        const float lufs = self->lufs.load(std::memory_order_relaxed);
        float norm_target_lin = 1.0f, norm_db = 0.0f;
        if (self->norm_on.load() && !std::isnan(lufs)) {
            norm_db = std::clamp(self->norm_target.load() - lufs, -30.0f, self->norm_boost.load());
            norm_target_lin = std::pow(10.0f, norm_db / 20.0f);
        }
        self->norm_gain_db.store(norm_db, std::memory_order_relaxed);
        if (self->norm_cur < 0.0f) self->norm_cur = norm_target_lin;
        const float alpha = 1.0f - std::exp(-1.0f / static_cast<float>(kRate));   // ~1 s time constant: no clicks
        const float gain = self->muted.load() ? 0.0f : static_cast<float>(self->volume.load()) / 100.0f * self->fade.load();
        // Equaliser (after the loudness measurement, before volume / normalisation, like the player).
        bool eq_run = false;
        if (self->eq_on.load(std::memory_order_relaxed)) {
            const unsigned gen = self->eq_gen.load(std::memory_order_acquire);
            if (gen != self->eq_seen_gen) {
                EqGains g{};
                for (int b = 0; b < kEqBands; ++b) g[static_cast<size_t>(b)] = self->eq_gains[static_cast<size_t>(b)].load(std::memory_order_relaxed);
                self->eq.configure(g, static_cast<int>(kRate));
                self->eq_seen_gen = gen;
            }
            eq_run = self->eq.active();
        } else if (self->eq_seen_gen != 0) {
            self->eq.reset();
            self->eq_seen_gen = 0;
        }
        auto soft = [](float x) {   // like the player: no hard clipping once the equaliser boosts
            constexpr float kKnee = 0.891f;
            const float a = std::fabs(x);
            if (a <= kKnee) return x;
            const float y = kKnee + (1.0f - kKnee) * std::tanh((a - kKnee) / (1.0f - kKnee));
            return x < 0.0f ? -y : y;
        };
        for (size_t i = 0; i < want; ++i) {
            self->norm_cur += alpha * (norm_target_lin - self->norm_cur);
            if (eq_run) self->eq.process(out[i * 2], out[i * 2 + 1]);
            out[i * 2] *= gain * self->norm_cur;
            out[i * 2 + 1] *= gain * self->norm_cur;
            if (eq_run) { out[i * 2] = soft(out[i * 2]); out[i * 2 + 1] = soft(out[i * 2 + 1]); }
        }

        // Tuning noise: attack ~0.12 s, held until the stream really plays (at least 0.45 s, at most 6 s), release ~0.9 s.
        if (self->noise_on.load(std::memory_order_relaxed) || self->noise_env > 0.0005f) {
            if (self->noise_restart.exchange(false)) self->noise_held = 0;
            const bool want_hold = self->noise_on.load(std::memory_order_relaxed) && self->noise_hold.load(std::memory_order_relaxed);
            if (want_hold) {
                self->noise_held += static_cast<long long>(want);
                const bool playing = got == want && self->primed.load();
                if ((playing && self->noise_held > static_cast<long long>(0.45 * kRate)) || self->noise_held > static_cast<long long>(6.0 * kRate))
                    self->noise_hold.store(false);
            }
            const float vol = self->muted.load() ? 0.0f : static_cast<float>(self->volume.load()) / 100.0f * self->fade.load();
            const float a_att = 1.0f - std::exp(-1.0f / (0.04f * static_cast<float>(kRate)));
            const float a_rel = 1.0f - std::exp(-1.0f / (0.30f * static_cast<float>(kRate)));
            const bool hold_now = self->noise_on.load(std::memory_order_relaxed) && self->noise_hold.load(std::memory_order_relaxed);
            const float tgt = hold_now ? 1.0f : 0.0f;
            auto rnd = [self]() { self->noise_rng = self->noise_rng * 1664525u + 1013904223u; return self->noise_rng >> 8; };   // 24 bit
            auto ms = [](float t) { return static_cast<int>(t * 0.001f * static_cast<float>(kRate)); };
            for (size_t i = 0; i < want; ++i) {
                self->noise_env += (tgt > self->noise_env ? a_att : a_rel) * (tgt - self->noise_env);
                const float w = static_cast<float>(static_cast<int>(rnd()) - (1 << 23)) / static_cast<float>(1 << 23);   // white, -1..1
                float* b = self->pk;                                                                                     // Paul Kellet's pink filter
                b[0] = 0.99886f * b[0] + w * 0.0555179f;  b[1] = 0.99332f * b[1] + w * 0.0750759f;
                b[2] = 0.96900f * b[2] + w * 0.1538520f;  b[3] = 0.86650f * b[3] + w * 0.3104856f;
                b[4] = 0.55000f * b[4] + w * 0.5329522f;  b[5] = -0.7616f * b[5] - w * 0.0168980f;
                const float pink = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.11f;
                b[6] = w * 0.115926f;
                // Steady bed: pink noise plus a little white hiss, no movement in it.
                const float bed = pink * 0.40f + w * 0.10f;
                float n = bed;
                // Glitches: every 25-250 ms something short breaks the bed up (2-16 ms).
                if (self->glitch_left > 0) {
                    --self->glitch_left;
                    const int pos = self->glitch_len - self->glitch_left;
                    switch (self->glitch_type) {
                        case 0: n = bed * 0.5f + ((rnd() % 55u) == 0u ? (w < 0 ? -1.0f : 1.0f) * (0.35f + 0.002f * static_cast<float>(rnd() % 300u)) : 0.0f); break;   // crackle: dense pops
                        case 1: if (pos % self->glitch_hold == 0) self->held = bed; n = self->held * 1.6f; break;                                                      // stutter: sample and hold
                        case 2: n = bed * 0.06f; break;                                                                                                                  // dropout
                        default: n = std::round(bed * 5.0f) / 5.0f * 1.8f; break;                                                                                         // digital crunch
                    }
                } else if (--self->next_glitch <= 0) {
                    self->glitch_type = static_cast<int>(rnd() % 4u);
                    self->glitch_len = self->glitch_left = ms(2.0f + static_cast<float>(rnd() % 1400u) * 0.01f);
                    self->glitch_hold = 4 + static_cast<int>(rnd() % 40u);
                    self->next_glitch = ms(25.0f + static_cast<float>(rnd() % 22500u) * 0.01f);
                    self->held = bed;
                }
                n *= self->noise_env * vol;
                out[i * 2] += n; out[i * 2 + 1] += n;
            }
        }

        // Visualizers get exactly what went to the device, like in the music player.
        const float* scope_src = raw_scope ? self->scope_raw.data() : out;
        self->scope.push_frames(scope_src, want);
        muisc::scope_feed_push(scope_src, want, kRate);   // the scope window (SHIFT+9); nothing while it is closed
        size_t done = 0;
        while (done < want) {
            const size_t chunk = std::min(want - done, self->mono.size());
            for (size_t i = 0; i < chunk; ++i)
                self->mono[i] = 0.5f * (out[(done + i) * 2] + out[(done + i) * 2 + 1]);
            self->fft.push_samples(self->mono.data(), chunk, kRate);
            done += chunk;
        }
    }
};

RadioEngine::RadioEngine() : impl_(new Impl) {
    impl_->mono.assign(8192, 0.0f);
    impl_->scope_raw.assign(2 * 16384, 0.0f);
    impl_->feeder = std::thread([d = impl_.get()]() { d->feeder_main(); });
}

RadioEngine::~RadioEngine() {
    stop();
    impl_->feeder_quit.store(true);
    if (impl_->feeder.joinable()) impl_->feeder.join();
    impl_->store->close_and_remove();   // the buffer file is only scratch space
    for (int i = 0; i < 1200 && g_rec.converting.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let a running MP3 conversion finish
    if (impl_->device_ready) ma_device_uninit(&impl_->device);
    if (impl_->context_ready) ma_context_uninit(&impl_->context);
}

bool RadioEngine::open_device(std::string* err) {
    Impl& d = *impl_;
    ma_context_config cc = ma_context_config_init();
    ma_result r;
    const char* use_null = std::getenv("MOUSIKI_RADIO_NULL");
    if (use_null && *use_null == '1') {
        ma_backend nb[] = { ma_backend_null };
        r = ma_context_init(nb, 1, &cc, &d.context);
    } else {
        r = init_platform_audio_context(d.context) ? MA_SUCCESS : MA_ERROR;
    }
    if (r != MA_SUCCESS) {
        d.device_error = "no audio backend available";
        if (err) *err = d.device_error;
        return false;
    }
    d.context_ready = true;

    std::lock_guard<std::mutex> lk(d.device_m);
    return open_output_locked(err);
}

// The playback device on the chosen output (or the default one when that is not there). device_m held, context ready, device closed.
bool RadioEngine::open_output_locked(std::string* err) {
    Impl& d = *impl_;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = kChannels;
    cfg.sampleRate = kRate;
    cfg.dataCallback = &Impl::data_callback;
    cfg.pUserData = &d;
    ma_device_id chosen{};
    if (find_output_device(d.context, d.device_name, chosen)) cfg.playback.pDeviceID = &chosen;
    if (ma_device_init(&d.context, &cfg, &d.device) != MA_SUCCESS) {
        d.device_error = "could not open the audio device";
        if (err) *err = d.device_error;
        return false;
    }
    d.device_ready = true;
    d.active_device = d.device.playback.name;
    if (ma_device_start(&d.device) != MA_SUCCESS) {
        d.device_error = "could not start the audio device";
        if (err) *err = d.device_error;
        return false;
    }
    d.device_error.clear();
    return true;
}

bool RadioEngine::set_output_device(const std::string& name, std::string* err) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lk(d.device_m);
    d.device_name = name;
    if (!d.context_ready) return true;   // not open yet: open_device() takes it
    if (d.device_ready) { ma_device_uninit(&d.device); d.device_ready = false; }   // waits for the callback to stop
    if (open_output_locked(err)) return true;
    d.device_name.clear();   // the chosen one failed: the default device
    return open_output_locked(err);
}

std::string RadioEngine::active_device() const {
    std::lock_guard<std::mutex> lk(impl_->device_m);
    return impl_->active_device;
}

void RadioEngine::tune(const Station& st, int index) {
    Impl& d = *impl_;
    auto s = std::make_shared<Session>();
    s->station = st;
    s->index = index;
    s->ring = d.ring;
    s->store = d.store;
    s->shifted = d.shifted;
    s->switch_m = d.switch_m;
    s->info.station = st.name;
    s->info.genre = st.genre;
    s->info.country = st.country;
    s->info.codec = st.codec_hint;
    s->info.bitrate_kbps = st.bitrate_hint;
    s->info.host = st.synthetic() ? "ffmpeg lavfi (offline)" : host_of(st.url);
    if (st.synthetic()) { s->info.sample_rate = kRate; s->info.channels = (st.url == "lavfi:tone") ? 1 : 2; }
    g_rec.stop(d.ring->heard_abs());   // a recording belongs to one station (finished before the buffer is reset)
    {
        std::lock_guard<std::mutex> lk(d.session_m);
        if (d.session) d.session->cancel.store(true);
        std::lock_guard<std::mutex> sw(*d.switch_m);
        s->gen = d.ring->reset();      // from here on, the old worker's writes are refused
        s->store_gen = d.store->reset();   // the timeshift buffer starts again with this station
        d.shifted->store(false);
        d.paused.store(false);
        d.feed_pos.store(0);
        d.session = s;
    }
    d.primed.store(false);
    if (d.noise_on.load()) { d.noise_hold.store(true); d.noise_restart.store(true); }
    d.meter_reset.store(true);
    d.scope.reset();
    d.fft.reset();
    // Detached on purpose: a worker may sit in a blocking read() on a dead socket for up to the
    // rw_timeout; tuning must never wait for that. Each holds its own shared_ptr to what it needs.
    // (an exception that left a detached thread would end the whole program: the worker gives up on the station instead)
    std::thread([s] { try { worker_main(s); } catch (...) { s->state.store(static_cast<int>(StreamState::Failed)); } }).detach();
    if (!st.synthetic()) std::thread([s] { try { probe_main(s); } catch (...) {} }).detach();
}

void RadioEngine::reconnect() {
    Station st;
    int index;
    {
        std::lock_guard<std::mutex> lk(impl_->session_m);
        if (!impl_->session) return;
        st = impl_->session->station;
        index = impl_->session->index;
    }
    tune(st, index);
}

void RadioEngine::stop() {
    Impl& d = *impl_;
    g_rec.stop(d.ring->heard_abs());
    std::lock_guard<std::mutex> lk(d.session_m);
    if (d.session) d.session->cancel.store(true);
    d.session.reset();
    {
        std::lock_guard<std::mutex> sw(*d.switch_m);
        d.ring->reset();
        d.store->reset();
        d.shifted->store(false);
        d.paused.store(false);
    }
    d.primed.store(false);
    d.noise_hold.store(false);
    d.meter_reset.store(true);
}

void RadioEngine::set_tune_noise(bool on) { impl_->noise_on.store(on); if (!on) impl_->noise_hold.store(false); }
void RadioEngine::set_fade(float g) { impl_->fade.store(std::clamp(g, 0.0f, 1.0f)); }
void RadioEngine::set_volume(int pct) { impl_->volume.store(std::clamp(pct, 0, 100)); }
bool RadioEngine::start_recording(const std::string& dir, const std::string& stem, const std::string& title,
                                  long long from_frame, std::string* err) {
    {
        std::lock_guard<std::mutex> lk(impl_->session_m);
        if (!impl_->session) { if (err) *err = "nothing tuned"; return false; }
    }
    const std::filesystem::path base = std::filesystem::path(dir) / stem;
    if (from_frame < 0) from_frame = impl_->ring->heard_abs();
    return g_rec.start(impl_->store, from_frame, base.string() + ".wav", base.string() + ".mp3", title, err);
}
void RadioEngine::stop_recording() { g_rec.stop(impl_->ring->heard_abs()); }

// ---- timeshift ----------------------------------------------------------------------------------------------------
void RadioEngine::set_timeshift(int minutes, const std::string& file) {
    Impl& d = *impl_;
    d.timeshift_minutes = std::clamp(minutes, 1, 120);
    d.timeshift_path = file;
    std::lock_guard<std::mutex> sw(*d.switch_m);
    g_rec.stop(d.ring->heard_abs());
    d.store->open(file, static_cast<long long>(d.timeshift_minutes) * 60 * kRate);
    // a new (empty) buffer: whatever played so far is gone, so back to live
    d.store->reset();
    if (d.shifted->load()) { d.shifted->store(false); d.ring->clear(0); d.primed.store(false); }
    d.paused.store(false);
}

void RadioEngine::toggle_pause() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> sw(*d.switch_m);
    if (!d.paused.load()) {
        // Pause: remember what was about to play and keep only the store running. On resume the feeder plays on from there.
        const long long at = d.ring->heard_abs();
        d.shifted->store(true);
        d.feed_pos.store(at);
        d.ring->clear(at);
        d.paused.store(true);
        d.primed.store(false);
    } else {
        d.paused.store(false);
        d.primed.store(false);
    }
}

double RadioEngine::jump(double seconds) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> sw(*d.switch_m);
    const long long end = d.store->end.load();
    const long long margin = kRate * 2;
    const long long oldest = end > d.store->cap ? d.store->oldest() + margin : 0;
    const long long heard = d.shifted->load() ? d.feed_pos.load() - static_cast<long long>(d.ring->fill()) : d.ring->heard_abs();
    long long target = heard + static_cast<long long>(seconds * kRate);
    target = std::clamp(target, oldest, end);
    const double moved = static_cast<double>(target - heard) / kRate;
    // Forward to (almost) the live edge: back to live, playing on from 1.5 s before the edge so there is no gap.
    if (target >= end - static_cast<long long>(kPrebufferFrames)) target = std::max(oldest, end - static_cast<long long>(kPrebufferFrames));
    d.shifted->store(true);
    d.feed_pos.store(target);
    d.ring->clear(target);
    d.primed.store(false);
    return moved;
}

void RadioEngine::go_live() {
    Impl& d = *impl_;
    {
        std::lock_guard<std::mutex> sw(*d.switch_m);
        d.paused.store(false);
    }
    jump(1e9);
}

void RadioEngine::set_muted(bool m) { impl_->muted.store(m); }
void RadioEngine::set_scope_raw(bool on) { impl_->scope_raw_on.store(on, std::memory_order_relaxed); }

void RadioEngine::set_stereo(bool on) {
    if (impl_->stereo.exchange(on) != on) impl_->meter_reset.store(true);   // mono and stereo measure differently
}
void RadioEngine::set_normalization(bool on, float target_lufs, float max_boost_db) {
    impl_->norm_target.store(std::clamp(target_lufs, -40.0f, 0.0f));
    impl_->norm_boost.store(std::clamp(max_boost_db, 0.0f, 24.0f));
    impl_->norm_on.store(on);
}

void RadioEngine::set_equalizer(bool on, const EqGains& gains_db) {
    for (int b = 0; b < kEqBands; ++b)
        impl_->eq_gains[static_cast<size_t>(b)].store(std::clamp(gains_db[static_cast<size_t>(b)], kEqMinDb, kEqMaxDb), std::memory_order_relaxed);
    impl_->eq_gen.fetch_add(1, std::memory_order_release);   // publish AFTER the gains are stored
    impl_->eq_on.store(on);
}

RadioStatus RadioEngine::status() const {
    const Impl& d = *impl_;
    RadioStatus st;
    st.volume = d.volume.load();
    st.muted = d.muted.load();
    st.device_ok = d.device_ready;
    st.device_error = d.device_error;
    st.recording = g_rec.active.load();
    if (st.recording) st.recording_sec = static_cast<double>(std::max(0LL, d.store->end.load() - g_rec.start_abs.load())) / kRate;
    // timeshift
    st.timeshift_paused = d.paused.load();
    st.timeshift_shifted = d.shifted->load() || st.timeshift_paused;
    {
        const long long end = d.store->end.load();
        const long long heard = st.timeshift_shifted
            ? (st.timeshift_paused ? d.feed_pos.load() : d.ring->heard_abs())
            : end - static_cast<long long>(d.ring->fill());
        st.heard_frame = d.ring->heard_abs();
        if (st.timeshift_paused) st.heard_frame = d.feed_pos.load();
        st.behind_sec = st.timeshift_shifted ? static_cast<double>(std::max(0LL, end - heard)) / kRate : 0.0;
        st.buffered_sec = static_cast<double>(end - d.store->oldest()) / kRate;
        st.timeshift_cap_sec = static_cast<double>(d.store->cap) / kRate;
        st.timeshift_edge_hits = d.edge_hits.load();
        st.oldest_frame = d.store->oldest();
    }
    st.record_serial = g_rec.serial.load();
    if (st.record_serial > 0) { std::lock_guard<std::mutex> lk(g_rec.m); st.record_note = g_rec.note; }
    st.lufs = d.lufs.load();
    st.norm_gain_db = d.norm_gain_db.load();
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lk(d.session_m);
        s = d.session;
    }
    if (!s) return st;
    st.tuned_index = s->index;
    st.tuned_name = s->station.name;
    st.tuned_url = s->station.url;
    st.dial_mhz = s->station.dial_mhz;
    st.reconnects = s->reconnects.load();
    st.buffer_sec = static_cast<double>(d.ring->fill()) / kRate;
    const int raw = s->state.load();
    if (raw == kReceiving) st.state = (d.primed.load() || d.paused.load()) ? StreamState::Live : StreamState::Buffering;
    else st.state = static_cast<StreamState>(raw);
    const long long t0 = s->first_audio_ms.load();
    if (t0 > 0) st.listening_sec = static_cast<double>(now_ms() - t0) / 1000.0;
    {
        std::lock_guard<std::mutex> lk(s->info_m);
        st.info = s->info;
    }
    return st;
}

void RadioEngine::set_tuned_index(int index) {
    std::lock_guard<std::mutex> lk(impl_->session_m);
    if (impl_->session) impl_->session->index = index;
}

FftVisualizer& RadioEngine::fft() { return impl_->fft; }
RadioScope& RadioEngine::scope() { return impl_->scope; }

} // namespace muisc::radio
