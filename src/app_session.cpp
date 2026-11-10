// app_session.cpp
// Part of class App (src/app.h): The saved session (snapshot, autosave) and adding tracks in bulk by URL.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "app_state_bulk.h"
#include "video_links.h"
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
// Autosave / session snapshot
// ---------------------------------------------------------------------

SnapshotData App::build_snapshot() const {
    SnapshotData snap;
    snap.play_mode = settings_.play_mode;
    snap.muted = muted_;
    // Save the *real* volume, not the forced-0 muted value, so unmuting
    // next session restores to what it actually was, not silence.
    snap.volume = muted_ ? pre_mute_volume_ : player_.volume();

    if (has_track_) {
        snap.has_now_playing = true;
        snap.now_playing.is_local = current_.is_local;
        snap.now_playing.path = current_.is_local ? path_utf8(current_.path) : std::string();
        snap.now_playing.video_id = current_.is_local ? std::string() : current_.video_id;
        snap.now_playing.title = metadata_.name;
        snap.now_playing.artist = metadata_.artist;
        snap.position_sec = player_.poll_elapsed();
    }

    snap.queue_locked = qst_.locked;
    for (const auto& item : queue_) {
        SnapshotTrack t;
        t.is_local = item.is_local;
        t.path = item.is_local ? path_utf8(item.local_path) : std::string();
        t.video_id = item.is_local ? std::string() : item.video_id;
        t.title = item.title;
        t.artist = item.artist;
        t.playlist = item.playlist;
        snap.queue.push_back(std::move(t));
    }
    return snap;
}

void App::restore_snapshot(const SnapshotData& snap) {
    settings_.play_mode = std::clamp(snap.play_mode, 0, 4);
    // Apply the saved volume first, then re-apply mute on top of it --
    // mirrors what pressing 'x' does at runtime (force 0, remember the
    // real value), just seeded from the snapshot instead of live state.
    pre_mute_volume_ = std::clamp(snap.volume, 0, 100);
    player_.set_volume(pre_mute_volume_);
    if (snap.muted) {
        player_.set_volume(0);
        muted_ = true;
    }

    queue_.clear();
    qst_.locked = snap.queue_locked;
    qst_.next_run = 0;
    // (a snapshot written by an older version may hold audio book chapters: the audio book player has its own place, in the .cue files)
    auto is_book_file = [&](const std::string& path) {
        std::vector<LocalTrack> one{LocalTrack{path_utf8(path_from_utf8(path).stem()), path_from_utf8(path), std::string()}};
        exclude_book_files(one);
        return one.empty();
    };
    for (const auto& t : snap.queue) {
        if (t.is_local && is_book_file(t.path)) continue;
        queue_.push_back({t.is_local, t.title, t.artist, t.is_local ? path_from_utf8(t.path) : fs::path(), t.video_id, false, t.playlist});
    }
    clamp_queue_selected();

    if (snap.has_now_playing) {
        resume_start_sec_ = std::max(0.0, snap.position_sec);
        if (snap.now_playing.is_local) {
            fs::path p = path_from_utf8(snap.now_playing.path);
            std::error_code ec;
            if (is_book_file(snap.now_playing.path)) {
                resume_start_sec_ = 0.0;   // an audio book chapter does not belong to the music player
            } else if (fs::exists(p, ec)) {
                LocalTrack t{path_utf8(p.stem()), p, snap.now_playing.artist};
                start_local_track(t);
                log_event("resuming: " + t.title);
            } else {
                resume_start_sec_ = 0.0; // file's gone -- nothing to resume into
            }
        } else if (!snap.now_playing.video_id.empty()) {
            OnlineResult r{snap.now_playing.video_id, snap.now_playing.title, snap.now_playing.artist};
            start_online_track(r);
        } else {
            resume_start_sec_ = 0.0;
        }
    }
}

void App::maybe_autosave() {
    if (!settings_.autosave_enabled || audiobook_) return;   // (the audio book player must not overwrite the music player's session: it keeps its place in the .cue files)
    double delay = std::max(1, settings_.autosave_delay_sec);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_autosave_at_).count();
    if (elapsed < delay) return;

    last_autosave_at_ = std::chrono::steady_clock::now();
    save_snapshot(build_snapshot());
    autosave_pulse_active_ = true;
    autosave_pulse_started_at_ = last_autosave_at_;
    ConsoleLog::instance().log_basic("autosaved session snapshot");
}

std::string App::autosave_indicator_glyph() const {
    if (!settings_.autosave_enabled || !settings_.autosave_indicator) return "";

    double t = autosave_pulse_active_
             ? std::chrono::duration<double>(std::chrono::steady_clock::now() - autosave_pulse_started_at_).count()
             : kAutosavePulseSeconds + 1.0; // idle -- well past the pulse window

    std::string glyph = settings_.autosave_chr.empty() ? "\u2022" : settings_.autosave_chr;

    if (settings_.autosave_indicator_type == 0) {
        // blink: appear/disappear twice (4 half-cycles) right after a
        // save, then settle back to hidden until the next one.
        if (t >= kAutosavePulseSeconds) return ""; // idle: hidden between saves
        int half_cycle = static_cast<int>(t / (kAutosavePulseSeconds / 4.0));
        bool visible = (half_cycle % 2) == 0;
        return visible ? glyph : " ";
    }

    // color: always visible, pulses C1 -> C2 -> C1 (heartbeat) right
    // after a save, then settles to a steady C1.
    std::string c1 = ansi_for(settings_.autosave_c1.empty() ? settings_.border_color : settings_.autosave_c1, false);
    std::string c2 = ansi_for(settings_.autosave_c2.empty() ? settings_.visualizer_color : settings_.autosave_c2, false);
    if (t >= kAutosavePulseSeconds) return c1 + glyph + "\x1b[0m"; // idle: steady C1
    // Two full heartbeats across the pulse window: C1->C2->C1->C2->C1.
    double phase = std::fmod(t, kAutosavePulseSeconds / 2.0) / (kAutosavePulseSeconds / 2.0); // 0..1 within each half-beat
    bool towards_c2 = phase < 0.5;
    return (towards_c2 ? c2 : c1) + glyph + "\x1b[0m";
}

// ---------------------------------------------------------------------
void App::bulk_add_open() {
    mode_ = Mode::BulkAdd;
    bulk_->add_buffer.clear();
    bulk_->add_results_ready = false;
    pending_bulk_add_ = BulkAddResult{};
    bulk_->add_selected.clear();
    bulk_->add_cursor = 0;
    bulk_->add_scroll = 0;
    edit_owner_.clear();   // a fresh field: the caret starts at its end
    status_line_.clear();
}

// Bulk add (paste a YouTube playlist link while Queue is focused)
// ---------------------------------------------------------------------

void App::launch_bulk_add_async(const std::string& url) {
    if (bulk_->add_thread.joinable()) bulk_->add_thread.join();
    bulk_->add_in_progress = true;
    bulk_->add_ready = false;
    {
        // Clear the slot before the worker starts: if the fetch throws, the
        // guard below leaves pending_bulk_add_ untouched, and a stale
        // success from a previous playlist would otherwise be re-committed.
        std::lock_guard<std::mutex> lk(bulk_->add_mutex);
        pending_bulk_add_ = BulkAddResult{};
    }
    bulk_->add_thread = std::thread([this, url]() { run_guarded("playlist add", [&] {
        BulkAddResult res;
        res.items = online_.list_playlist(url, &res.error);
        res.success = res.error.empty() && !res.items.empty();
        std::lock_guard<std::mutex> lk(bulk_->add_mutex);
        pending_bulk_add_ = std::move(res);
    }); 
        // Same reasoning as launch_search_async(): the poll loop is gated on
        // this flag, so it is raised whether or not the work succeeded.
        bulk_->add_ready = true;
    });
}

void App::poll_pending_bulk_add() {
    if (!bulk_->add_ready.load()) return;
    BulkAddResult res;
    {
        std::lock_guard<std::mutex> lk(bulk_->add_mutex);
        if (!bulk_->add_ready.load()) return;
        res = std::move(pending_bulk_add_);
        bulk_->add_ready = false;
    }
    bulk_->add_in_progress = false;
    if (bulk_->add_thread.joinable()) bulk_->add_thread.join();

    if (!res.success) {
        status_line_ = res.error.empty() ? "couldn't load that playlist" : res.error;
        return; // stay in Mode::BulkAdd, phase 1 -- let the person edit the link and retry
    }

    // Enter phase 2: show the checklist rather than committing
    // immediately. Starts fully de-selected -- "SELECT" is meant for
    // picking your own favorites out of the playlist, so nothing is
    // pre-starred; "ALL" (the "a" key) still adds every fetched track
    // regardless of star state, unaffected by this default.
    pending_bulk_add_ = std::move(res);
    bulk_->add_selected.assign(pending_bulk_add_.items.size(), false);
    bulk_->add_cursor = 0;
    bulk_->add_scroll = 0;
    bulk_->add_results_ready = true;
    status_line_.clear();
}

void App::commit_bulk_add(bool all) {
    int added = 0;
    for (size_t i = 0; i < pending_bulk_add_.items.size(); ++i) {
        if (!all && (i >= bulk_->add_selected.size() || !bulk_->add_selected[i])) continue;
        const auto& item = pending_bulk_add_.items[i];
        queue_.push_back({false, item.title, item.uploader, {}, item.video_id, false, "", !settings_.bulk_save, settings_.bulk_video});
        ++added;
    }
    clamp_queue_selected();
    queue_reveal(static_cast<int>(queue_.size()) - 1);
    log_event("added " + std::to_string(added) + " track" + (added == 1 ? "" : "s") + " to queue" +
              (settings_.bulk_save ? "" : " (play only: the files are deleted after playing)") + (settings_.bulk_video ? " (with video)" : ""));

    mode_ = Mode::Browse;
    bulk_->add_results_ready = false;
    bulk_->add_buffer.clear();
    pending_bulk_add_ = BulkAddResult{};
    bulk_->add_selected.clear();
    bulk_->add_cursor = 0;
    bulk_->add_scroll = 0;
}

// BULK ADD "play only": deletes the files made for the tracks that have been played (all of them, or all but `keep`).
void App::drop_temp_streams(const fs::path& keep, bool all) {
    for (size_t i = 0; i < temp_streams_.size();) {
        const TempStream t = temp_streams_[i];
        if (!all && t.audio == keep) { ++i; continue; }
        temp_streams_.erase(temp_streams_.begin() + static_cast<std::ptrdiff_t>(i));
        std::error_code ec;
        if (vd_.active && vd_.track == t.audio) { std::lock_guard<std::mutex> lk(vd_.child_m); if (vd_.child) vd_.child->kill(); }   // (its video is not wanted any more)
        if (!t.had_video) {
            const std::string v = VideoLinks::get().video_for(path_utf8(t.audio));
            if (!v.empty() && v != "-" && VideoLinks::get().tracks_for(v).size() <= 1 && cache_.is_video_download_file(path_from_utf8(v))) {   // (a video that other songs use stays)
                fs::remove(path_from_utf8(v), ec);
                VideoLinks::get().remove(path_utf8(t.audio));
                ++video_rev_;
                video_for_track_.clear();
            }
        }
        fs::remove(t.audio, ec);
    }
}

} // namespace muisc
