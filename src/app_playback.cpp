// app_playback.cpp
// Part of class App (src/app.h): Loading and playing a track: the load worker, search, play next / previous, the queue and its playlists.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "playlist_state.h"
#include "hifi_bars.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "book_art.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include "app_internal.h"
#include "update_check.h"
#include "open_url.h"
#include "app_settings_tables.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace muisc {


// ---------------------------------------------------------------------
// Playback start
// ---------------------------------------------------------------------

void App::write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                 double t_probe, double t_total, const std::string& error) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    fs::path dir = path_from_utf8(home) / ".cache" / "mousiki";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream log(dir / "load_timing.log", std::ios::app);
    if (!log.is_open()) return;

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    log << timebuf << " track=\"" << title << "\" source=" << (is_local ? "local" : "online");
    if (!is_local) log << " resolve=" << t_resolve << "s";
    // This is now "time to first sound", not "time to fully decoded" —
    // decode itself streams in after this point, off the critical path.
    log << " probe=" << t_probe << "s time_to_playback=" << t_total << "s";
    if (!error.empty()) log << " ERROR=\"" << error << "\"";
    log << "\n";
}

void App::launch_load_async(fs::path local_path, std::string title, std::string artist,
                             std::string location_label, bool is_local, std::string video_id) {
    if (load_thread_.joinable()) load_thread_.join(); // previous job already signaled done, safe to reap
    swap_pending_ = false;   // (reload_current_audio() sets it again right after this call)
    load_in_progress_ = true;
    load_ready_ = false;
    load_stage_ = is_local ? 4 : 1;
    load_started_at_ = std::chrono::steady_clock::now();
    if (!is_local) status_line_ = "resolving \"" + title + "\" ...";

    // This thread ONLY resolves (online) and probes metadata/duration —
    // both fast, no full decode. It publishes a result and returns. Full
    // decode is a SEPARATE, detached thread spawned at the bottom, so
    // this thread (the one the main loop's next launch_load_async call
    // will join()) is never blocked waiting on decode — that's what
    // makes it safe to join from launch_load_async without risking a
    // freeze if the user switches tracks again quickly.
    const bool want_stereo = settings_.stereo; // read here, on the calling thread, rather than from inside the load thread
    const int sid_seconds = settings_.sid_play_length;
    // "Use mp4 audio for this song" (Style Option / video options): a song paired with a video plays the audio of that video instead of its
    // own file. Worked out here on the main thread (the pairing looks into the library); a video itself always plays its own audio.
    std::string video_audio;
    if (is_local && settings_.video_mode && !LocalSource::is_video_file(local_path) && cover_track_video_opts(path_utf8(local_path)).mp4audio == 1)
        video_audio = video_source_for(local_path);
    load_thread_ = std::thread([this, local_path, title, artist, location_label, is_local, video_id, want_stereo, sid_seconds, video_audio]() {
      run_guarded("track load", [&] {
        using clock = std::chrono::steady_clock;
        auto t_start = clock::now();
        auto elapsed_s = [](clock::time_point from) {
            return std::chrono::duration<double>(clock::now() - from).count();
        };

        PendingLoad pl;
        pl.title = title;
        pl.artist = artist;
        pl.location_label = location_label;
        pl.is_local = is_local;
        pl.video_id = video_id;

        double t_resolve = 0.0, t_probe = 0.0;

        fs::path path = local_path;
        if (!is_local) {
            load_stage_ = 1;
            auto t0 = clock::now();
            std::string err;
            auto resolved = youtube_.resolve_by_id(video_id, title, artist, &err);
            t_resolve = elapsed_s(t0);
            if (!resolved) {
                pl.error = "download failed: " + err;
                write_load_timing_log(title, is_local, t_resolve, 0, elapsed_s(t_start), pl.error);
                std::lock_guard<std::mutex> lk(load_mutex_);
                pending_load_ = std::move(pl);
                load_ready_ = true;
                return;
            }
            path = resolved->cached_path;
            pl.title = resolved->title;
            pl.artist = resolved->artist;
        }
        pl.path = path;

        // Tracker modules / game music (ffmpeg via libopenmpt / libgme) and SID tunes (rendered by sidplayfp into a
        // cached WAV, which is then decoded and probed instead of the .sid -- pl.path stays the .sid itself).
        fs::path decode_from = path;
        std::string rate_hint;   // the sample rate the video's audio has (set below when it replaces the song's audio)
        if (!video_audio.empty()) {
            decode_from = path_from_utf8(video_audio);
            rate_hint = probe_metadata(decode_from, pl.title, pl.artist, pl.location_label).sampling;
        }
        if (is_local && video_audio.empty() && is_chiptune_file(path)) {
            std::string problem = chiptune_support_problem(path);
            if (problem.empty() && is_sid_file(path)) {
                fs::path wav;
                if (render_sid_to_wav(path, sid_seconds, wav, &problem)) decode_from = wav;
            }
            if (!problem.empty()) {
                pl.error = problem;
                write_load_timing_log(title, is_local, 0, 0, elapsed_s(t_start), pl.error);
                std::lock_guard<std::mutex> lk(load_mutex_);
                pending_load_ = std::move(pl);
                load_ready_ = true;
                return;
            }
        }

        load_stage_ = 4;
        auto t2 = clock::now();
        pl.metadata = probe_metadata(path, pl.title, pl.artist, pl.location_label);
        
        // Ensure lyrics fetch uses the real metadata tags instead of the filename/folder
        if (is_local) {
            if (!pl.metadata.name.empty() && pl.metadata.name != "-") {
                pl.title = pl.metadata.name;
            }
            if (!pl.metadata.artist.empty() && pl.metadata.artist != "-") {
                pl.artist = pl.metadata.artist;
            }
        }
        
        double duration = probe_duration_seconds(decode_from);
        t_probe = elapsed_s(t2);
        pl.total_sec = duration > 0 ? static_cast<size_t>(duration) : 0;

        pl.pcm = std::make_shared<StreamingPcm>();
        // the file's own sample rate: no resampling on the way to the device
        pl.pcm->reserve_for_seconds(duration > 0 ? duration : 300.0,
                                    native_sample_rate(decode_from, video_audio.empty() ? pl.metadata.sampling : rate_hint, duration, settings_.rate_cap_96), want_stereo ? 2 : 1,
                                    static_cast<double>(settings_.track_buffer_mb) * 1024.0 * 1024.0, duration <= 0);
        pl.success = true;

        write_load_timing_log(pl.title, is_local, t_resolve, t_probe, elapsed_s(t_start), "");

        {
            std::lock_guard<std::mutex> lk(load_mutex_);
            pending_load_ = pl;
            load_ready_ = true;
        }

        // Decode continues independently from here — detached because it
        // may still be running when the user switches to a different
        // track, and the StreamingPcm it's filling stays alive via the
        // shared_ptr captured below (and via Player's own reference, if
        // this track is still the one playing) for exactly as long as it
        // needs to. Known tradeoff: if the app quits while a decode is
        // still in flight, that ffmpeg subprocess can be orphaned rather
        // than cleanly killed — worth fixing with real process-group
        // tracking later, not a correctness or crash risk today.
        std::shared_ptr<StreamingPcm> pcm = pl.pcm;
        fs::path decode_path = decode_from;
        std::string wtitle = pl.title, wartist = pl.artist;
        bool waveform_smooth = settings_.waveform_smooth; // captured by value — see below, avoids a cross-thread read of settings_
        spawn_bg("track decode", [this, decode_path, pcm, waveform_smooth]() {
            if (pcm->windowed) {
                // A long track: held a window at a time (see StreamingPcm). This thread keeps the window filled
                // until nobody holds the track any more; a second one goes through the whole file once for the
                // loudness and the waveform.
                // abandoned: nobody but these decode threads holds the track any more (it was replaced, or the
                // load was superseded) -- they count themselves in internal_refs
                std::weak_ptr<StreamingPcm> watch = pcm;
                StreamingPcm* raw = pcm.get();
                auto abandoned = [watch, raw]() { return raw->cancelled.load() || watch.use_count() <= raw->internal_refs.load(); };
                raw->internal_refs.store(1);   // this thread's `pcm`
                spawn_bg("track analysis", [this, decode_path, keep = pcm, waveform_smooth, abandoned]() {
                    analyse_track(decode_path, *keep, abandoned);
                    if (!abandoned() && !keep->env_sumsq.empty()) {
                        auto envelope = WaveformQuantizer::envelope_from_bins(keep->env_sumsq, StreamingPcm::kEnvGran,
                                                                              keep->total_frames.load(), 4096, waveform_smooth);
                        std::lock_guard<std::mutex> lk(waveform_mutex_);
                        pending_waveform_envelope_ = std::move(envelope);
                        waveform_pending_ready_ = true;
                    }
                    keep->internal_refs.fetch_sub(1);   // before `keep` itself goes (never counted low)
                });
                raw->internal_refs.fetch_add(1);   // the analysis thread's `keep` (it exists already: never counted high)
                stream_decode_windowed(decode_path, *pcm, abandoned);
                return;
            }
            stream_decode_ffmpeg(decode_path, *pcm);

            // Deferred mini-waveform pass — only starts once decode is
            // fully done, never gates playback.
            if (!pcm->decode_failed.load()) {
                // PERF: decode is done — pcm->data is no longer being
                // written to, so we pass it directly as a const-ref
                // instead of making a full snapshot copy.  A 5-minute
                // track at 44100 Hz is ~50 MB; that copy was the single
                // biggest reason the waveform appeared so late after
                // playback started, because it doubled the working-set
                // size and stalled the RMS pass behind a large memcpy.
                auto envelope = WaveformQuantizer::generate_high_res_envelope(pcm->data, 4096, waveform_smooth, pcm->channels);
                std::lock_guard<std::mutex> lk(waveform_mutex_);
                pending_waveform_envelope_ = std::move(envelope);
                waveform_pending_ready_ = true;
            }
        });
      });

      // If the guard above swallowed an exception, nothing published a
      // result -- and poll_pending_load() is the only thing that clears
      // load_in_progress_, so the app would refuse to start any track for
      // the rest of the session ("still loading the previous track ...").
      // Publish a failed load instead so the UI recovers and says why.
      if (!load_ready_.load()) {
          std::lock_guard<std::mutex> lk(load_mutex_);
          PendingLoad failed;
          failed.title = title;
          failed.is_local = is_local;
          failed.success = false;
          failed.error = "couldn't load \"" + title + "\" -- see the console log (t)";
          pending_load_ = std::move(failed);
          load_ready_ = true;
      }
    });
}

// The previous track's fetch thread is detached, not joined: joining would block the main thread
// until the abandoned fetch finished, so skipping to another track during a slow lookup would hang
// the program. An epoch guard so a late-arriving stale result
// just gets discarded rather than clobbering the new/retried track's
// lyrics. Shared by the initial per-track fetch (poll_pending_load) and
// the manual retry hotkey (handle_key's 'l' case).
void App::launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network) {
    lyrics_ready_ = false;
    lyrics_path_ = path;
    int my_epoch = ++lyrics_epoch_;
    const double track_secs = static_cast<double>(total_sec_);
    spawn_bg("lyrics fetch", [this, title, artist, path, force_network, my_epoch, track_secs]() {
        LyricsResult r = fetch_synced_lyrics(title, artist, path_utf8(lyrics_script_), path, force_network, track_secs);
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (my_epoch != lyrics_epoch_.load()) return; // a newer/retried fetch has since started — discard
        lyrics_result_ = std::move(r);
        lyrics_ready_ = true;
    });
}

// The audio of the song that plays is loaded again (the choice "Use mp4 audio for this song" changed) and goes on at the time the old one had
// reached; the pause stays. The listening history, the lyrics and the video view are not touched: it is the same play.
void App::reload_current_audio() {
    if (!has_track_ || !current_is_local_ || current_path_.empty()) return;
    if (load_in_progress_.load()) { status_line_ = "still loading -- the audio choice applies when the song starts the next time"; return; }
    const fs::path path = current_path_;
    const double pos = player_.poll_elapsed();
    const bool paused = player_.is_paused();
    launch_load_async(path, path_utf8(path.stem()), "", path_utf8(path.parent_path().filename()) + "/", /*is_local=*/true, /*video_id=*/"");
    swap_pending_ = true; swap_pos_ = pos; swap_paused_ = paused; swap_path_ = path;
}

void App::poll_pending_load() {
    if (!sty_video_src_.empty() && std::chrono::steady_clock::now() - sty_video_seen_ > std::chrono::milliseconds(800)) {   // the Style Option preview of another track is not shown any more
        sty_video_.set_source("");
        sty_video_.stop();
        sty_video_src_.clear();
    }
    if (swap_repause_ && device_play_pending_gen_.load() == 0) { swap_repause_ = false; player_.pause(); }   // (the new audio is in: a paused song stays paused)
    if (!load_ready_.load()) return;
    PendingLoad pl;
    {
        std::lock_guard<std::mutex> lk(load_mutex_);
        // The flag is cleared while load_mutex_ is still held, so the load thread cannot write
        // pending_load_ again between the copy and the reset.
        if (!load_ready_.load()) return; // re-check under lock (spurious wakeup guard)
        pl = std::move(pending_load_);   // moved, not copied: a copy left here kept the track's audio alive
        pending_load_ = PendingLoad{};
        load_ready_ = false;
    }
    load_in_progress_ = false;
    load_stage_ = 0;
    const bool was_advancing = advancing_;
    advancing_ = false;

    if (!pl.success) {
        swap_pending_ = false;   // (a failed reload leaves the audio that plays as it is)
        status_line_ = pl.error;
        // An automatic advance whose next track failed to load: the previous
        // track is over, so stop treating it as current (otherwise its
        // finished flag would immediately trigger another advance).
        if (was_advancing) {
            has_track_ = false;
            // ...and the play it left behind is over too -- nothing took its
            // place, so close the record now (it was heard to the end; the
            // next track simply never arrived). A FAILED manual selection is
            // deliberately not recorded: whatever was playing keeps playing
            // and its play must stay live.
            history_end_current_play();
        }
        return;
    }

    if (swap_pending_ && pl.is_local && pl.path == swap_path_ && pl.path == current_path_) {   // the same song, other audio: nothing else changes
        swap_pending_ = false;
        current_pcm_ = pl.pcm;
        total_sec_ = pl.total_sec;
        resume_start_sec_ = swap_pos_;
        waveform_envelope_.clear();
        waveform_ready_ = false;
        waveform_pending_ready_ = false;
        ++waveform_epoch_;
        hifi().reset();
        fft_.reset();
        scope_.reset();
        player_.clear_finished();
        swap_repause_ = swap_paused_;
        launch_device_play_async();
        status_line_.clear();
        return;
    }
    swap_pending_ = false;

    // Player::play() stops whatever it was previously playing as its own
    // first step, so no separate explicit stop() call is needed here —
    // and doing it inside play() (below, off the main thread) is what
    // lets this whole switch never touch the main thread.
    current_pcm_ = pl.pcm;
    total_sec_ = pl.total_sec;
    metadata_ = pl.metadata;
    current_path_ = pl.path;
    current_is_local_ = pl.is_local;
    // Edits left pending by an earlier run belong on this screen too (display_metadata()), not only in the editor.
    if (pl.is_local) meta_ensure_session_loaded();
    current_video_id_ = pl.video_id;
    current_playlist_ = load_playlist_ctx_;
    has_track_ = true;
    // Listening history, in this exact order: the play that just ended is
    // closed BEFORE clear_finished() (its "was it heard to the end?" verdict
    // reads player_.finished(), which the next line resets) and the new one
    // is opened AFTER current_* have been reassigned (they are what
    // history_begin_current_play() records). No history record exists for a
    // failed load -- see the branch above.
    history_end_current_play();
    player_.clear_finished(); // see clear_finished()'s comment — closes the race that caused the double-skip bug
    history_begin_current_play();
    waveform_envelope_.clear();
    waveform_ready_ = false;
    waveform_pending_ready_ = false;
    ++waveform_epoch_; // any waveform still being computed for the previous track is now stale
    last_lyrics_status_.clear();
    hifi().reset();
    fft_.reset(); // don't let the previous track's spectrum tail linger into this one's first frame
    scope_.reset(); // ...and don't let the previous track's waveform linger in the oscilloscope either

    video_auto_apply();   // (Video overlay: Auto Video) before the lyrics are fetched, they depend on the view
    // "Lyrics Engine" (settings_.element_lyrics) off means no lyrics work at all: nothing is
    // fetched, no Python is started and there is no network access.
    // The video view of a track WITHOUT a video that shows a visual ("When No Video Shows" is not the lyrics) needs no lyrics: nothing is fetched
    // (the "." key fetches them when you cycle to the lyrics view).
    const bool video_placeholder = settings_.video_mode && settings_.video_view && settings_.novideo_viz != 4 && video_shown_for(pl.path).empty();
    if (settings_.lyrics_on() && !audiobook_ && !video_placeholder) {   // (no lyrics for audio books)
        launch_lyrics_fetch(pl.title, pl.artist, pl.path);
    } else {
        // nothing fetched for this track: the lyrics of the last one must not count as its own (the karaoke
        // overlay fetches them itself when it opens, see karaoke_ensure_lyrics())
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        ++lyrics_epoch_;   // a fetch still running for the last track is discarded
        lyrics_path_.clear();
        lyrics_result_ = LyricsResult{};
        lyrics_ready_ = false;
    }

    // This is the whole point of the redesign: play() is handed a
    // StreamingPcm that may have zero frames decoded yet. The audio
    // callback plays silence for anything past what's been decoded and
    // self-corrects the instant more arrives — so sound starts the
    // moment decode produces its first chunk, not after the whole track.
    // Dispatched off the main thread — see launch_device_play_async().
    launch_device_play_async();
    status_line_.clear();
}

void App::launch_device_play_async() {
    int my_gen = ++device_gen_;
    // Raise the handoff guard BEFORE the request is visible to the worker:
    // from here until player_.play() returns, finished_ still describes the
    // old track and must not drive advance_track() (see device_play_pending_gen_).
    device_play_pending_gen_.store(my_gen);
    auto pcm = current_pcm_;
    int vol = player_.volume() > 0 ? player_.volume() : 70;
    // One-shot resume position from a restored snapshot -- consumed
    // here exactly once, then zeroed so every subsequent track change
    // (skip, search-and-play, queue advance, ...) starts at 0 like
    // always. Reading+clearing it up front (still on the main thread,
    // before it's handed to the worker) avoids any race with a second
    // restore attempt -- there isn't one, but this keeps that invariant
    // obvious rather than implicit.
    double start_sec = resume_start_sec_;
    resume_start_sec_ = 0.0;

    // Post to device_worker_loop() rather than spawning a thread here.
    // There's only one request slot, not a queue: if the worker is still
    // busy with an older request when a newer one lands, this simply
    // overwrites it in place before the worker ever reads it, so only the
    // latest survives -- the same "a superseded switch is silently
    // dropped" behaviour the old generation-counter design gave, just
    // enforced by construction instead of by a check the stale thread had
    // to remember to perform.
    {
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        device_request_.pcm = std::move(pcm);
        device_request_.volume = vol;
        device_request_.start_sec = start_sec;
        device_request_.generation = my_gen;
        device_request_ready_ = true;
    }
    device_request_cv_.notify_one();
}

void App::start_device_worker() {
    {
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        device_worker_running_ = true;
    }
    device_worker_thread_ = std::thread(&App::device_worker_loop, this);
}

void App::device_run(const std::function<void()>& fn) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> fut = done->get_future();
    {
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        if (device_worker_running_ && !device_worker_stop_) {
            device_tasks_.push_back([fn, done] {
                run_guarded("audio device", fn);
                done->set_value();
            });
        } else {
            done.reset();
        }
    }
    if (!done) {   // no worker: here, as before
        std::lock_guard<std::mutex> lk(player_mutex_);
        run_guarded("audio device", fn);
        return;
    }
    device_request_cv_.notify_one();
    fut.wait();
}

void App::device_stop() {
    {   // a switch that has not reached the worker yet would start the track that is being stopped again
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        if (device_request_ready_) {
            device_request_ready_ = false;
            device_request_.pcm.reset();
            device_play_pending_gen_.store(0);
        }
    }
    device_run([this] { player_.stop(); });
}

void App::stop_device_worker() {
    {
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        device_worker_stop_ = true;
    }
    device_request_cv_.notify_one();
}

void App::device_worker_loop() {
    for (;;) {
        DevicePlayRequest req;
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(device_request_mutex_);
            device_request_cv_.wait(lk, [this] { return device_request_ready_ || device_worker_stop_ || !device_tasks_.empty(); });
            if (!device_tasks_.empty()) {   // a stop / device move first (also on the way out: the caller waits for it)
                task = std::move(device_tasks_.front());
                device_tasks_.pop_front();
            } else {
                if (device_worker_stop_) { device_worker_running_ = false; return; }   // quitting -- don't start one more track on the way out
                req = device_request_;
                device_request_ready_ = false;
            }
        }
        if (task) {
            std::lock_guard<std::mutex> lk(player_mutex_);
            task();
            continue;
        }
        // Defensive only: with a single overwritable slot rather than a
        // real queue, req.generation should already equal device_gen_ by
        // construction every time this fires.
        if (req.generation != device_gen_.load()) continue;
        // Guarded like every other worker: this loop lives for the whole
        // session, and an exception escaping it would take the process with
        // it rather than just failing one track.
        run_guarded("audio device start", [&] {
            std::lock_guard<std::mutex> lk(player_mutex_);
            player_.play(req.pcm, req.start_sec, req.volume, &fft_, &scope_);
        });
        // The new track is in and the old pcm (and its latching finished_
        // flag) is gone, so the main loop may look at finished_ again. Only
        // clear if no newer handoff has been posted meanwhile -- that one is
        // still in flight and owns the guard now.
        int expected = req.generation;
        device_play_pending_gen_.compare_exchange_strong(expected, 0);
    }
}

void App::poll_pending_waveform() {
    if (!waveform_pending_ready_.load()) return;
    std::vector<float> envelope;
    {
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        envelope = std::move(pending_waveform_envelope_);
    }
    waveform_pending_ready_ = false;
    waveform_envelope_ = std::move(envelope);
    waveform_ready_ = true;
    waveform_reveal_start_ = std::chrono::steady_clock::now(); // starts the 700ms left-to-right reveal
}

void App::launch_search_async(const std::string& query, int source) {
    if (search_thread_.joinable()) search_thread_.join();
    search_in_progress_ = true;
    search_ready_ = false;
    static const char* const kWhere[3] = {"YouTube", "SoundCloud", "Bandcamp"};
    status_line_ = std::string("searching ") + kWhere[std::clamp(source, 0, 2)] + " for \"" + query + "\" ...";
    const std::string bc_script = path_utf8(locate_script("bandcamp_search.py"));

    search_thread_ = std::thread([this, query, source, bc_script]() { run_guarded("online search", [&] {
        auto results = source == 1 ? online_.search_soundcloud(query, 15)
                     : source == 2 ? online_.search_bandcamp(query, 15, bc_script)
                     : online_.search(query, /*count=*/15, path_utf8(fast_search_script_));
        std::lock_guard<std::mutex> lk(search_mutex_);
        pending_search_results_ = std::move(results);
    }); 
        // Set outside the guard: poll_pending_search() waits on this flag,
        // so it has to be raised even when the search threw, or the UI sits
        // on "searching ..." forever.
        search_ready_ = true;
    });
}

void App::poll_pending_search() {
    if (!search_ready_.load()) return;
    std::vector<OnlineResult> results;
    {
        std::lock_guard<std::mutex> lk(search_mutex_);
        results = std::move(pending_search_results_);
    }
    search_ready_ = false;
    search_in_progress_ = false;

    online_view_ = std::move(results);
    selected_ = 0;
    scroll_ = 0;
    status_line_ = online_view_.empty() ? "no online results" : "";
}

void App::play_selected() {
    // Playlists aren't "played" directly -- there's no single track to
    // start. Enter on a playlist row queues everything in it instead
    // (see playlist_add_selected_to_queue()), same as the user pressing
    // "a" on it would.
    if (list_source_ == ListSource::Playlist) { playlist_add_selected_to_queue(); return; }
    // Folders aren't played either -- Enter opens the folder's files.
    if (list_source_ == ListSource::Folder) { open_selected_folder(); return; }
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) start_local_track(local_view_[selected_]);
    else start_online_track(online_view_[selected_]);
}

int App::current_track_list_index() const {
    if (!has_track_) return -1;
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return -1; // no "now playing" identity in a list of playlist/folder names
    if (list_source_ == ListSource::Local) {
        if (!current_is_local_) return -1; // playing an online track while browsing the local list
        for (size_t i = 0; i < local_view_.size(); ++i) {
            if (local_view_[i].path == current_path_) return static_cast<int>(i);
        }
        return -1;
    } else {
        if (current_is_local_) return -1; // playing a local track while browsing online results
        for (size_t i = 0; i < online_view_.size(); ++i) {
            if (online_view_[i].video_id == current_video_id_) return static_cast<int>(i);
        }
        return -1;
    }
}

void App::play_relative(int delta) {
    // "next/previous track" has no meaning while browsing a list of
    // playlist names rather than tracks.
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return;
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0) return;
    // Relative to what's actually *playing*, not wherever the hover
    // cursor happens to be sitting -- falls back to the hover cursor
    // only when there's no sensible "current" position in this list
    // (nothing playing yet, or what's playing is from a different
    // source/isn't in this view at all).
    int base = current_track_list_index();
    if (base < 0) base = selected_;
    selected_ = std::clamp(base + delta, 0, static_cast<int>(list_len) - 1);
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::play_relative_random() {
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return;
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0) return;
    if (list_len == 1) { selected_ = 0; play_selected(); return; }
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(list_len) - 1);
    int base = current_track_list_index();
    if (base < 0) base = selected_;
    int next;
    do { next = dist(rng); } while (next == base);
    selected_ = next;
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::queue_reset_lap() {
    for (auto& q : queue_) q.played = false;
}

void App::play_next_from_queue(bool random) {
    // The queue plays in its order, whatever the play mode: next (n) and the end of a track take its first item. Only the
    // shuffle key (#, `random`) picks a random item of it.
    // Shuffle: pick a random queue item instead of strictly FIFO order.
    // What happens to the item once it is taken depends on the lock ("!"):
    //   * LOCKED (the default): it goes to the END of the queue, so the queue
    //     keeps all its tracks and loops indefinitely instead of draining.
    //   * UNLOCKED: it leaves the queue for good once it is played.
    // Queue-then-stop (play mode 4) on a locked queue plays the queue ONCE:
    // every item that goes to the back is flagged `played`, the next one is
    // the first item not flagged yet, and advance_track() stops playback once
    // none is left (the order is then back to what it was).
    int idx = 0;
    queue_next_run_ = 0; // the head is moving: "a" starts a fresh run
    const bool queue_once = settings_.play_mode == 4 && queue_locked_;
    if (queue_once) {
        auto first = std::find_if(queue_.begin(), queue_.end(), [](const QueueItem& q) { return !q.played; });
        if (first == queue_.end()) {   // a manual skip after the pass was over: start a new pass
            queue_reset_lap();
            first = queue_.begin();
        }
        idx = static_cast<int>(first - queue_.begin());
    } else {
        queue_reset_lap(); // the flags only mean something in queue-then-stop on a locked queue
        if (random && queue_.size() > 1) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, static_cast<int>(queue_.size()) - 1);
            idx = dist(rng);
        }
    }
    play_queue_item(idx);
}

// Starts the queue item at `idx` (the head for the next key, a random one for shuffle, the highlighted one for ENTER in the queue pane;
// with `move_along` (SHIFT+ENTER) the items in front of it move to the end of the queue first).
void App::play_queue_item(int idx, bool move_along) {
    if (idx < 0 || idx >= static_cast<int>(queue_.size())) return;
    const bool queue_once = settings_.play_mode == 4 && queue_locked_;
    if (move_along && idx > 0) {   // SHIFT+ENTER: the items in front of it are passed over -- they go to the end in their order (as if they had been played)
        std::rotate(queue_.begin(), queue_.begin() + idx, queue_.end());
        for (size_t i = queue_.size() - static_cast<size_t>(idx); i < queue_.size(); ++i) queue_[i].played = queue_once;
        idx = 0;
        queue_selected_ = 0;
    }
    QueueItem item = queue_[idx];
    queue_.erase(queue_.begin() + idx);
    if (queue_locked_) {                       // played track -> end of the list
        QueueItem back = item;
        back.played = queue_once;
        queue_.push_back(std::move(back));
    }
    if (queue_selected_ >= idx && queue_selected_ > 0) --queue_selected_; // index shifted down by the erase
    clamp_queue_selected();
    pending_queue_playlist_ = item.playlist;   // consumed by start_*_track() below; any other start leaves it empty
    if (item.is_local) {
        LocalTrack t{path_utf8(item.local_path.stem()), item.local_path, item.artist};
        start_local_track(t);
    } else {
        OnlineResult r{item.video_id, item.title, item.artist};
        start_online_track(r);
    }
}

void App::advance_track() {
    // has_track_ is deliberately NOT cleared up front. Clearing it here made
    // the metadata panel show "no track loaded" for the gap between one track
    // ending and the next one's load finishing (and made play_relative()
    // measure "next" from the hover cursor instead of from what was actually
    // playing). It's only cleared where playback really ends: Stop mode, or
    // when nothing could be started / the load failed (see below and
    // poll_pending_load()).

    // Repeat: keep replaying whatever just finished -- whether it came
    // from the queue or the library -- without touching the queue or
    // advancing through any list at all. Stop: don't auto-advance into
    // anything, queue or not. Both apply uniformly regardless of the
    // queue's state now -- previously these were only ever consulted
    // once the queue was already empty, which was the other half of the
    // "queue mode won't respect repeat" bug.
    // Sleep timer's "stop after current song": a one-shot, checked before
    // everything else so it beats Repeat and the queue. It is NOT the Stop play
    // mode -- play_mode is left exactly as it was -- it just ends playback the
    // same way Stop mode does and clears itself.
    if (sleep_stop_after_track_) {
        sleep_stop_after_track_ = false;
        has_track_ = false;
        history_end_current_play(); // playback really ends here: nothing follows it
        status_line_ = "sleep timer: stopped after the song";
        return;
    }
    if (settings_.play_mode == 1 /*loop*/) {
        launch_device_play_async(); // same track, already fully decoded, no reload needed
        has_track_ = true;
        // One play of it just ended and looping it starts another one -- the
        // record is closed (verdict still readable: clear_finished() is
        // right below) and a fresh one opened, which is what makes a looped
        // track count as a replay instead of one endless play.
        history_end_current_play();
        player_.clear_finished();
        history_begin_current_play();
        return;
    }
    if (settings_.play_mode == 3 /*stop*/) {
        has_track_ = false; // the only mode where "no track loaded" is the right message
        history_end_current_play(); // playback really ends here: nothing follows it
        return; // no auto-advance -- queue or not
    }

    // Queue then stop (play mode 4): the queue is played through once and
    // playback ends there -- it never falls through to the library. Unlocked,
    // that is simply "the queue is empty" (played tracks leave it); locked,
    // played tracks stay (at the back), so the pass is over once every item
    // is flagged as played.
    if (settings_.play_mode == 4) {
        bool pass_over = queue_.empty();
        if (!pass_over && queue_locked_) {
            pass_over = std::none_of(queue_.begin(), queue_.end(), [](const QueueItem& q) { return !q.played; });
        }
        if (pass_over) {
            queue_reset_lap();  // the next pass starts fresh
            has_track_ = false;
            history_end_current_play(); // playback really ends here: nothing follows it
            log_event("queue finished: stopped");
            return;
        }
    }

    if (audiobook_) { book_advance(); return; }   // the next chapter (or the end of the book)

    // The queue always takes priority over the library — it's an
    // explicit user-built-up-next list.
    advancing_ = true; // suppress re-entry until poll_pending_load() reports back
    if (!queue_.empty()) {
        play_next_from_queue();
    } else {
        switch (settings_.play_mode) {
            case 2: // shuffle
                play_relative_random();
                break;
            default: // list (sequential); queue-then-stop (4) never gets here,
                     // it stopped above once the queue was used up
                play_relative(1);
                break;
        }
    }
    // Nothing got started (empty list, ...): playback has genuinely ended.
    if (!load_in_progress_.load()) {
        advancing_ = false;
        has_track_ = false;
        history_end_current_play(); // the finished track has no successor: close its record
    }
}

char App::play_mode_letter() const {
    switch (settings_.play_mode) {
        case 1: return 'R';  // repeat (loop current track)
        case 2: return 'S';  // shuffle
        case 3: return 'O';  // stop (play, then stop -- not "S", shuffle already owns that)
        case 4: return 'Q';  // queue, then stop
        default: return 'L'; // list (normal sequential)
    }
}

// "a" -- the hovering track goes in as NEXT: the front of the queue (the head
// is always what plays next, locked or not). Consecutive presses
// keep their order (queue_next_run_), so A, B, C play as A, B, C.
void App::queue_add_selected() { queue_add_selected_impl(false); }

// "e" -- the hovering track goes to the END of the queue.
void App::queue_add_selected_end() { queue_add_selected_impl(true); }

void App::queue_add_selected_impl(bool at_end) {
    // A playlist row queues the whole playlist; that always appends (there is
    // no meaningful "next" for a block of tracks), for "a" and "e" alike.
    if (list_source_ == ListSource::Playlist) { playlist_add_selected_to_queue(); return; }
    if (list_source_ == ListSource::Folder) { status_line_ = "Enter opens the folder -- then add its tracks"; return; }
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    QueueItem item;
    if (list_source_ == ListSource::Local) {
        const auto& t = local_view_[selected_];
        item = {true, t.title, t.folder_artist, t.path, "", false, ""};
    } else {
        const auto& r = online_view_[selected_];
        item = {false, r.title, r.uploader, {}, r.video_id, false, ""};
    }
    if (at_end) {
        queue_.push_back(std::move(item));
    } else {
        const int pos = std::clamp(queue_next_run_, 0, static_cast<int>(queue_.size()));
        queue_.insert(queue_.begin() + pos, std::move(item));
        ++queue_next_run_;
    }
    clamp_queue_selected();
    queue_reveal(at_end ? static_cast<int>(queue_.size()) - 1 : std::clamp(queue_next_run_ - 1, 0, static_cast<int>(queue_.size()) - 1));
}

// Main UI: Enter (or "a") on a playlist row while list_source_==Playlist.
// Loads the playlist from disk and queues every track that's still
// present on disk, skipping (and reporting) any that aren't.
void App::playlist_add_selected_to_queue() {
    if (playlist_->view.empty() || selected_ < 0 || selected_ >= static_cast<int>(playlist_->view.size())) return;
    status_line_ = queue_playlist(playlist_->view[selected_].name);
}

std::string App::queue_playlist(const std::string& name) {
    auto pl = load_playlist(name);
    if (!pl) return "could not load \"" + name + "\"";
    int added = 0, skipped = 0;
    for (auto& t : pl->tracks) {
        if (t.missing) { ++skipped; continue; }
        queue_.push_back({true, t.title, t.artist, t.path, "", false, name});
        ++added;
    }
    clamp_queue_selected();
    queue_reveal(static_cast<int>(queue_.size()) - 1);
    return "queued " + std::to_string(added) + " track" + (added == 1 ? "" : "s") + " from \"" + name + "\""
         + (skipped > 0 ? " (" + std::to_string(skipped) + " missing, skipped)" : "");
}

void App::queue_remove_last() {
    if (!queue_.empty()) queue_.pop_back();
    queue_next_run_ = 0;
    clamp_queue_selected();
}

// Moves the queue window so that entry `idx` (just added) is visible -- the new track is the confirmation that "a" / "e" worked.
// The cursor stays where it was; clamp_queue_selected() would pull the window back to it, so this runs after that.
void App::queue_reveal(int idx) {
    if (queue_.empty()) return;
    idx = std::clamp(idx, 0, static_cast<int>(queue_.size()) - 1);
    const int rows = std::max(1, queue_nav_rows());
    if (idx < queue_scroll_) queue_scroll_ = idx;
    else if (idx >= queue_scroll_ + rows) queue_scroll_ = idx - rows + 1;
    queue_scroll_ = std::clamp(queue_scroll_, 0, std::max(0, static_cast<int>(queue_.size()) - rows));
}

void App::clamp_queue_selected() {
    if (queue_.empty()) { queue_selected_ = 0; queue_scroll_ = 0; return; }
    queue_selected_ = std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1);
    // queue_nav_rows() is the big overlay's row count while it is open, the
    // small pane's otherwise. The window is also kept inside the queue so
    // no empty rows trail the last entry once the queue is long enough.
    const int rows = std::max(1, queue_nav_rows());
    queue_scroll_ = std::clamp(queue_scroll_, 0, std::max(0, static_cast<int>(queue_.size()) - rows));
    if (queue_selected_ >= queue_scroll_ + rows) queue_scroll_ = queue_selected_ - rows + 1;
    if (queue_selected_ < queue_scroll_) queue_scroll_ = queue_selected_;
}

void App::queue_remove_hovering() {
    if (queue_.empty() || queue_selected_ < 0 || queue_selected_ >= static_cast<int>(queue_.size())) return;
    const int idx = queue_selected_;
    queue_.erase(queue_.begin() + idx);
    queue_next_run_ = 0;
    clamp_queue_selected();
}

void App::queue_clear() {
    const size_t n = queue_.size();
    // One level of undo (Ctrl+Shift+Z): what was just cleared.
    if (n > 0) queue_undo_ = queue_;
    queue_shuffle_undo_.clear();
    queue_.clear();
    queue_next_run_ = 0;
    clamp_queue_selected(); // empty queue -> cursor and scroll back to 0
    const std::string undo_key = hotkey_text("HKeyQueueUndoClear", "CTRL+SHIFT+Z");
    log_event("queue cleared (" + std::to_string(n) + " track" + (n == 1 ? "" : "s") + " removed)"
              + (undo_key == "-" ? std::string() : " -- " + undo_key + " undoes it"));
}

// Ctrl+Shift+Z. Puts the last cleared queue back in FRONT of whatever was
// queued since, then forgets the backup (single level).
void App::queue_undo_clear() {
    if (queue_undo_.empty()) { status_line_ = "nothing to undo -- the queue has not been cleared"; return; }
    const int n = static_cast<int>(queue_undo_.size());
    queue_.insert(queue_.begin(), queue_undo_.begin(), queue_undo_.end());
    queue_undo_.clear();
    queue_next_run_ = 0;
    clamp_queue_selected();
    log_event("queue restored (" + std::to_string(n) + " track" + (n == 1 ? "" : "s") + ")");
}

// "!" -- lock / unlock (locked is the default). Locked: a played track goes to
// the end of the queue, so the queue loops. Unlocked: a played track leaves it.
void App::queue_toggle_lock() {
    queue_locked_ = !queue_locked_;
    queue_next_run_ = 0;
    queue_reset_lap();
    log_event(queue_locked_ ? "queue locked: played tracks move to the end of the queue"
                            : "queue unlocked: played tracks leave the queue");
}

// The head of the queue is always what plays next, so a manual move only has
// to end the current "a" run.
void App::queue_after_move(int /*from*/, int /*to*/) {
    queue_next_run_ = 0;
}

void App::queue_move_hovering(int dir) {
    if (queue_.empty()) return;
    int target = queue_selected_ + dir;
    if (target < 0 || target >= static_cast<int>(queue_.size())) return; // already at an edge
    std::swap(queue_[queue_selected_], queue_[target]);
    queue_after_move(queue_selected_, target);
    queue_selected_ = target;
    clamp_queue_selected();
}

// SHIFT+s in the big queue overlay: the queue is put in a random order. On a locked queue the track that plays now sits at its
// end (the played tracks do): that one stays there, so "previous" still finds it.
bool App::queue_playing_is_last() const {
    if (!queue_locked_ || !has_track_ || queue_.empty()) return false;
    const QueueItem& last = queue_.back();
    return last.is_local == current_is_local_ && (last.is_local ? last.local_path == current_path_ : last.video_id == current_video_id_);
}

void App::queue_shuffle_order() {
    if (queue_.size() < 2) { status_line_ = "shuffle order: the queue needs at least two items"; return; }
    size_t n = queue_.size();
    if (queue_playing_is_last()) --n;
    queue_shuffle_undo_ = queue_;   // one level of undo (SHIFT+u)
    queue_shuffle_undo_sel_ = queue_selected_;
    std::random_device rd;
    std::mt19937 rng(rd());
    std::shuffle(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(n), rng);
    queue_after_move(0, 0);
    queue_selected_ = 0;
    clamp_queue_selected();
    status_line_ = "queue shuffled: " + std::to_string(queue_.size()) + " items in a new random order -- SHIFT+u undoes it";
}

// SHIFT+u in the big queue overlay: the order from before the last shuffle comes back. Tracks that were added since keep their
// place behind the others, tracks that were removed or played are simply gone from it; the track that plays now (locked queue)
// stays at the end. One level: the backup is forgotten afterwards.
void App::queue_undo_shuffle() {
    if (queue_shuffle_undo_.empty()) { status_line_ = "nothing to undo -- the queue has not been shuffled"; return; }
    auto key_of = [](const QueueItem& q) { return q.is_local ? "L" + path_utf8(q.local_path) : "V" + q.video_id; };
    std::unordered_map<std::string, std::vector<int>> ranks;   // per track: its places in the old order (the first is used first)
    for (int i = static_cast<int>(queue_shuffle_undo_.size()) - 1; i >= 0; --i) ranks[key_of(queue_shuffle_undo_[static_cast<size_t>(i)])].push_back(i);
    const size_t n = queue_.size() - (queue_playing_is_last() ? 1 : 0);
    std::vector<std::pair<int, size_t>> order;
    for (size_t j = 0; j < n; ++j) {
        auto it = ranks.find(key_of(queue_[j]));
        int r;
        if (it != ranks.end() && !it->second.empty()) { r = it->second.back(); it->second.pop_back(); }
        else r = static_cast<int>(queue_shuffle_undo_.size() + j);   // new since the shuffle
        order.emplace_back(r, j);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<QueueItem> back;
    back.reserve(queue_.size());
    for (const auto& o : order) back.push_back(std::move(queue_[o.second]));
    for (size_t j = n; j < queue_.size(); ++j) back.push_back(std::move(queue_[j]));
    queue_ = std::move(back);
    queue_selected_ = queue_shuffle_undo_sel_;
    queue_shuffle_undo_.clear();
    queue_after_move(0, 0);
    clamp_queue_selected();
    status_line_ = "queue shuffle undone: the order from before is back";
}

// Shift+4 / Shift+5: the hovering item to the very top / bottom.
void App::queue_move_to_edge(int dir) {
    if (queue_.empty()) return;
    const int from = std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1);
    const int to = (dir < 0) ? 0 : static_cast<int>(queue_.size()) - 1;
    if (from == to) return; // already there
    QueueItem item = std::move(queue_[from]);
    queue_.erase(queue_.begin() + from);
    queue_.insert(queue_.begin() + to, std::move(item));
    queue_after_move(from, to);
    queue_selected_ = to;
    clamp_queue_selected();
}

// Ctrl+Shift+U: the queue becomes a new playlist in the playlist editor, with
// the name field focused so it only needs a name and HOME. Playlists are
// local-files-only, so streamed (online) queue entries are left out and
// counted in the status line.
std::vector<PlaylistTrack> App::queue_local_tracks(int& skipped) const {
    std::vector<PlaylistTrack> tracks;
    skipped = 0;
    for (const auto& q : queue_) {
        if (!q.is_local) { ++skipped; continue; }
        bool dup = false;
        for (const auto& t : tracks) { if (t.path == q.local_path) { dup = true; break; } }
        if (dup) continue;
        PlaylistTrack pt;
        pt.title = q.title;
        pt.artist = q.artist;
        pt.path = q.local_path;
        pt.missing = false;
        tracks.push_back(std::move(pt));
    }
    return tracks;
}

// The small overlay first (Mode::QueueToPlaylist); the work is queue_to_playlist_apply().
void App::queue_to_playlist() {
    if (queue_.empty()) { status_line_ = "the queue is empty -- nothing to save as a playlist"; return; }
    int skipped = 0;
    if (queue_local_tracks(skipped).empty()) { status_line_ = "no local tracks in the queue (playlists hold local files only)"; return; }
    qtp_track_ = false;
    qtp_stage_ = 0;
    qtp_choice_ = 0;
    qtp_query_.clear();
    qtp_selected_ = 0;
    mode_ = Mode::QueueToPlaylist;
    force_redraw_ = true;
}

// ---------------------------------------------------------------------
// SHIFT+a: the highlighted library track -> a playlist (saved at once)
// ---------------------------------------------------------------------

void App::track_to_playlist() {
    if (queue_focus_) { status_line_ = "focus the library pane first (TAB), then press this key on a track"; return; }
    if (list_source_ == ListSource::Playlist) { status_line_ = "a playlist can't be added to a playlist -- pick a track"; return; }
    if (list_source_ == ListSource::Folder) { status_line_ = "a folder can't be added to a playlist -- open it and pick a track"; return; }
    const size_t len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (len == 0 || selected_ < 0 || selected_ >= static_cast<int>(len)) { status_line_ = "no track highlighted"; return; }
    if (atp_busy_.load()) { status_line_ = "still downloading the last track for a playlist -- wait for it"; return; }
    AtpTrack t;
    if (list_source_ == ListSource::Local) {
        const auto& r = local_view_[static_cast<size_t>(selected_)];
        t.title = r.title; t.artist = r.folder_artist; t.path = r.path; t.is_local = true;
    } else {
        const auto& r = online_view_[static_cast<size_t>(selected_)];
        t.title = r.title; t.artist = r.uploader; t.video_id = r.video_id; t.is_local = false;
    }
    atp_track_ = std::move(t);
    qtp_track_ = true;
    qtp_stage_ = 0;
    qtp_choice_ = 0;
    qtp_query_.clear();
    qtp_new_name_.clear();
    qtp_selected_ = 0;
    mode_ = Mode::QueueToPlaylist;
    force_redraw_ = true;
}

// Adds one local file to a playlist (a new one when it does not exist yet) and saves it; no editor.
void App::atp_add_file(const std::string& playlist, const std::string& title, const std::string& artist, const fs::path& file) {
    Playlist pl;
    bool existed = false;
    if (auto loaded = PlaylistManager::load(playlists_dir(), playlist)) { pl = std::move(*loaded); existed = true; }
    else pl.name = playlist;
    const std::string p = path_utf8(file);
    for (const auto& t : pl.tracks) {
        if (t.path == p) { status_line_ = "\"" + title + "\" is already in \"" + playlist + "\""; return; }
    }
    PlaylistTrack pt;
    pt.title = title; pt.artist = artist; pt.path = p; pt.missing = false;
    pl.tracks.push_back(std::move(pt));
    std::string err;
    if (!PlaylistManager::save(playlists_dir(), pl, &err)) { status_line_ = "could not save \"" + playlist + "\": " + err; return; }
    status_line_ = "added \"" + title + "\" to " + std::string(existed ? "" : "the new playlist ") + "\"" + playlist + "\" (" + std::to_string(pl.tracks.size()) + " tracks, saved)";
    if (list_source_ == ListSource::Playlist) playlist_->view = filter_playlists(last_playlist_query_);   // a shown /p: list gets the new count
}

void App::track_to_playlist_apply(const std::string& playlist) {
    mode_ = Mode::Browse;
    qtp_track_ = false;
    if (atp_track_.is_local) { atp_add_file(playlist, atp_track_.title, atp_track_.artist, atp_track_.path); return; }
    // An online track: download it into the download folder in the background, then add the file it became.
    status_line_ = "downloading \"" + atp_track_.title + "\" for \"" + playlist + "\" ...";
    atp_busy_ = true;
    AtpTrack t = atp_track_;
    spawn_bg("playlist download", [this, t, playlist]() {
        AtpJob job;
        job.playlist = playlist; job.title = t.title; job.artist = t.artist;
        std::string err;
        auto got = youtube_.resolve_by_id(t.video_id, t.title, t.artist, &err);
        if (!got) {
            job.error = "download failed: " + err;
        } else {
            std::error_code ec;
            const fs::path dest_dir = cache_.download_dir();
            fs::create_directories(dest_dir, ec);
            std::string safe_name = got->title.empty() ? t.title : got->title;
            std::string safe_artist = got->artist.empty() ? t.artist : got->artist;
            for (char& c : safe_name) if (c == '/' || c == '\\') c = '_';
            for (char& c : safe_artist) if (c == '/' || c == '\\') c = '_';
            const std::string stem = safe_artist.empty() ? safe_name : safe_name + " - " + safe_artist;
            const fs::path dest = dest_dir / path_from_utf8(stem + path_utf8(got->cached_path.extension()));
            if (fs::exists(dest, ec)) {
                job.ok = true; job.file = dest;           // downloaded before: use that file
            } else {
                fs::copy_file(got->cached_path, dest, fs::copy_options::overwrite_existing, ec);
                if (ec) job.error = "could not save the download: " + ec.message();
                else { job.ok = true; job.file = dest; }
            }
            // the cache copy goes (the file lives in the download folder now) unless it is the track being played
            if (job.ok) {
                std::error_code ec2;
                if (!(has_track_ && current_path_ == got->cached_path)) fs::remove(got->cached_path, ec2);
            }
        }
        std::lock_guard<std::mutex> lk(atp_mutex_);
        atp_done_ = std::move(job);
        atp_ready_ = true;
    });
}

void App::poll_pending_atp() {
    if (!atp_ready_.load()) return;
    AtpJob job;
    {
        std::lock_guard<std::mutex> lk(atp_mutex_);
        if (!atp_ready_.load()) return;
        job = std::move(atp_done_);
        atp_done_ = AtpJob{};
        atp_ready_ = false;
    }
    atp_busy_ = false;
    if (!job.ok) { status_line_ = job.error.empty() ? "download failed" : job.error; return; }
    refresh_local_view();   // the new file is part of the library now
    atp_add_file(job.playlist, job.title, job.artist, job.file);
}

void App::qtp_refresh() {
    qtp_view_ = filter_playlists(qtp_query_);   // empty query = all of them
    qtp_selected_ = std::clamp(qtp_selected_, 0, std::max(0, static_cast<int>(qtp_view_.size()) - 1));
}

void App::queue_to_playlist_apply(const std::string& existing) {
    int skipped = 0;
    std::vector<PlaylistTrack> tracks = queue_local_tracks(skipped);
    if (tracks.empty()) { status_line_ = "no local tracks in the queue (playlists hold local files only)"; return; }
    const int n = static_cast<int>(tracks.size());
    const std::string skipped_note = skipped > 0 ? " (" + std::to_string(skipped) + " online skipped)" : std::string();
    playlist_open_editor();           // fresh editor, tab 0
    if (existing.empty()) {
        playlist_->edit_tracks = std::move(tracks);
        resolve_playlist_tracks_async();
        playlist_->edit_track_selected = 0;
        playlist_->edit_focus = 0;         // the name field
        playlist_->edit_dirty = true;
        playlist_->status = "queue -> playlist: " + std::to_string(n) + " track" + (n == 1 ? "" : "s") + skipped_note
                         + " -- type a name, Ctrl+S saves";
        return;
    }
    playlist_load_into_editor(existing);
    if (playlist_->edit_name != existing) return;   // could not be loaded: the status line says so
    const int first_new = static_cast<int>(playlist_->edit_tracks.size());
    int added = 0;
    for (auto& t : tracks) {
        bool dup = false;
        for (const auto& e : playlist_->edit_tracks) if (e.path == t.path) { dup = true; break; }
        if (dup) continue;
        playlist_->edit_tracks.push_back(std::move(t));
        ++added;
    }
    playlist_->edit_track_selected = std::min(first_new, std::max(0, static_cast<int>(playlist_->edit_tracks.size()) - 1));
    playlist_->edit_focus = 2;          // the track list, on the first added track
    playlist_->edit_dirty = added > 0;
    playlist_->status = "queue -> \"" + existing + "\": " + std::to_string(added) + " track" + (added == 1 ? "" : "s") + " added"
                     + (n - added > 0 ? ", " + std::to_string(n - added) + " already in it" : std::string()) + skipped_note
                     + (added > 0 ? " -- Ctrl+S saves" : "");
}

// Word-wraps `text` into lines of at most `width` columns (a longer single word is cut).
std::vector<std::string> wrap_words(const std::string& text, int width) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t j = text.find(' ', i);
        std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        while (width > 0 && display_width(word) > width) {      // one word longer than the line: cut it
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            const std::string head = truncate_str(word, width);
            if (head.empty()) break;
            out.push_back(head);
            word = word.substr(head.size());
        }
        if (cur.empty()) cur = word;
        else if (display_width(cur) + 1 + display_width(word) <= width) cur += " " + word;
        else { out.push_back(cur); cur = word; }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}



void App::start_local_track(const LocalTrack& track) {
    const std::string playlist_ctx = std::move(pending_queue_playlist_);
    pending_queue_playlist_.clear();
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    load_playlist_ctx_ = playlist_ctx;
    fs::path parent = track.path.parent_path().filename();
    // folder_artist is only a cheap guess for the browse-list Artist column
    // (see LocalTrack's comment in local_source.h) -- it's the same string
    // as the Location field below (both are just the parent folder name),
    // so feeding it in here as the fallback artist made an untagged file's
    // metadata panel show the folder path twice, once as "Artist" and once
    // as "Location". Pass "" instead: probe_metadata() already falls back
    // to "-" when there's no real artist tag, same as start_online_track
    // does for YouTube tracks below.
    launch_load_async(track.path, track.title, "",
                       path_utf8(parent) + "/", /*is_local=*/true, /*video_id=*/"");
}

void App::start_online_track(const OnlineResult& result) {
    const std::string playlist_ctx = std::move(pending_queue_playlist_);
    pending_queue_playlist_.clear();
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    load_playlist_ctx_ = playlist_ctx;
    // Pass "" for artist so the lyrics search queries just the YouTube video title
    // (which usually contains "Artist - Song Name" perfectly), instead of appending the channel name.
    // where it comes from (the meta data panel's Location; also what the download key looks at)
    const std::string& id = result.video_id;
    const char* where = id.find("soundcloud.com") != std::string::npos ? "soundcloud"
                      : id.find("bandcamp.com") != std::string::npos ? "bandcamp" : "youtube";
    launch_load_async({}, result.title, "", where, /*is_local=*/false, result.video_id);
}

} // namespace muisc
