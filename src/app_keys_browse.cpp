// app_keys_browse.cpp
// Part of class App (src/app.h): the hotkey actions of the main screen, in themed groups (split out of App::handle_browse_key).
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "app_state_rl.h"
#include "app_state_sleep.h"
#include "app_state_cheat.h"
#include "creator_state.h"
#include "bars_rows.h"
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

bool App::browse_open_key(const std::string& action, int key, int lookup_key, size_t list_len) {
    (void)key; (void)lookup_key; (void)list_len;
    if (action == "HKeyRescan") {   // SHIFT+r: new files copied / uploaded while the app runs
        rescan_now();
    } else if (action == "HKeyTranslit") {   // SHIFT+' : Japanese / Korean / Chinese text in Latin letters, on / off
        toggle_latin_letters();
    } else if (action == "HKeyLyricsTiming") {   // lyrics timing overlay
        lyrics_edit_open();
    } else if (action == "HKeyStyleOptions") {   // SHIFT+s: the Style Option overlay of the playing track
        meta_open_style_playing();
    } else if (action == "HKeyVideoFetch") {      // ALT+v: find and download videos for the songs that have none
        if (audiobook_) status_line_ = "fetch videos: music player only";
        else video_fetch_open();
    } else if (action == "HKeyCoverFetch") {     // ALT+c: fetch album covers (MusicBrainz / Cover Art Archive)
        if (audiobook_) status_line_ = "cover fetch: music player only";
        else cf_open();
    } else if (action == "HKeyReadLyrics") {     // READ LYRICS
        readlyrics_open();
    } else if (action == "HKeyBulkAdd") {           // paste a playlist link, from any pane
        bulk_add_open();
    } else if (action == "HKeyAddToPlaylist") {     // the highlighted library track -> a playlist, saved at once
        track_to_playlist();
    } else if (action == "HKeyQueueToPlaylist") {   // the queue -> the playlist editor
        queue_to_playlist();
    } else if (action == "HKeyQueueUndoClear") {    // bring back the queue that was just cleared
        queue_undo_clear();
    } else if (action == "HKeySetting") {
        settings_open();
        mode_ = Mode::Settings;
        settings_tab_ = 0;
        settings_row_ = 0;
        settings_col_ = 0;
    } else if (action == "HKeyPlaylist") {
        playlist_open_editor();
    } else if (action == "HKeyMetaEditor") { // SHIFT+m: edit file names / tags, fetch metadata
        meta_open();
    } else if (action == "HKeyHistory") { // SHIFT+h: listening history (HISTORY / TOP TRACKS / HABITS)
        history_open();
    } else if (action == "HKeySpectroMenu") { // SHIFT+i: spectrogram options
        spectro_.menu_row = kSrView;
        mode_ = Mode::SpectroMenu;
    } else if (action == "HKeyScopeBig") { // SHIFT+0: the big oscilloscope overlay on / off
        scope_big_ = !scope_big_;
        spectro_.full = false;
        scope_big_color_edit_ = false;
        scope_metrics_open_ = false;
        force_redraw_ = true;
        status_line_ = scope_big_ ? "oscilloscope: full screen (ESC or " + hotkey_text("HKeyScopeBig", "=") + " closes, " + hotkey_text("HKeyOscMenu", "O") + " options)" : "";
    } else if (action == "HKeySpectroFull") { // SHIFT+u: spectrogram full screen on / off
        spectro_.full = !spectro_.full;
        if (spectro_.full) scope_big_ = false;
        force_redraw_ = true;
        status_line_ = spectro_.full ? "spectrogram: full screen (ESC or " + hotkey_text("HKeySpectroFull", "U") + " closes, " + hotkey_text("HKeySpectroMenu", "I") + " options)" : "";
    } else if (action == "HKeyScopeWindow") { // SHIFT+9: the oscilloscope in its own window (always, even with it off on ANIMA/VIZ)
        scope_window_toggle();
    } else if (action == "HKeySpectroWindow") { // SHIFT+8: the spectrogram in its own window (always)
        spectro_window_toggle();
    } else if (action == "HKeyOscMenu") { // SHIFT+o: oscilloscope tuning overlay
        osci_menu_row_ = 0;
        mode_ = Mode::OsciMenu;
    } else if (action == "HKeyBarsMenu") { // SHIFT+y: the spectrum bars' options, under the upper pane
        bars_menu_row_ = 0;
        mode_ = Mode::BarsMenu;
    } else if (action == "HKeyNormMenu") { // SHIFT+v: loudness normalization overlay
        norm_menu_row_ = 0;
        mode_ = Mode::NormMenu;
    } else if (action == "HKeySleepTimer") { // SHIFT+z: sleep timer overlay
        sleep_->menu_row = sleep_->stop_after_track ? 5
                        : sleep_->timer_active ? (sleep_->timer_minutes == 15 ? 0 : sleep_->timer_minutes == 30 ? 1
                                               : sleep_->timer_minutes == 60 ? 2 : sleep_->timer_minutes == 90 ? 3 : 4)
                        : 0;
        mode_ = Mode::SleepTimer;
    } else if (action == "HKeyCreator") { // ^: creator overlay
        creator_->status.clear();
        creator_->list_open = false;
        if (fs::is_directory(creator_dir()) && creator_dir_prepare()) rescan_library();   // files made before the folder was marked leave the library
        creator_load();
        mode_ = Mode::Creator;
    } else if (action == "HKeyEqualizer") { // SHIFT+e: equaliser overlay
        eq_open();
    } else if (action == "HKeyFolderOverlay") { // SHIFT+f: big folder overlay on/off
        if (folder_overlay_open_) folder_overlay_close(); else folder_overlay_open();
    } else if (action == "HKeyConvertOverlay") { // SHIFT+c: file conversion overlay on/off
        if (convert_overlay_open_) convert_overlay_close(); else convert_overlay_open();
    } else if (action == "HKeyListOverlay") { // SHIFT+l: big list overlay on/off
        if (list_overlay_open_) list_overlay_close(); else list_overlay_open();
    } else if (action == "HKeyQueueOverlay") { // SHIFT+k: big queue overlay on/off
        if (qst_.overlay_open) queue_overlay_close(); else queue_overlay_open();
    } else {
        return false;
    }
    return true;
}

bool App::browse_nav_queue_key(const std::string& action, int key, int lookup_key, size_t list_len) {
    (void)key; (void)lookup_key; (void)list_len;
    if (action == "HKeySwitchBetweenCards") { // Tab: toggle Up/Down + reorder focus between the list and the queue
        // The queue pane is hidden behind the big list overlay and the queue
        // always has focus inside the big queue overlay, so there is nothing
        // to switch to while either is up.
        if (!list_overlay_active() && !queue_overlay_active()) qst_.focus = !qst_.focus;
    } else if (action == "HKeyNavigateUp") {   // SHIFT+Up / PgUp: a page
        const bool paged = last_key_was_arrow() && last_key_was_shifted();
        if (qst_.focus) {
            qst_.selected = std::max(0, qst_.selected - (paged ? 5 : 1));
            clamp_queue_selected();
        } else {
            selected_ = std::max(0, selected_ - (paged ? std::max(1, list_nav_rows() - 1) : 1));
            if (selected_ < scroll_) scroll_ = selected_;
        }
    } else if (action == "HKeyNavigateDown") {   // SHIFT+Down / PgDn: a page
        const bool paged = last_key_was_arrow() && last_key_was_shifted();
        if (qst_.focus) {
            if (!queue_.empty()) qst_.selected = std::min(static_cast<int>(queue_.size()) - 1, qst_.selected + (paged ? 5 : 1));
            clamp_queue_selected();
        } else {
            if (list_len > 0) selected_ = std::min(static_cast<int>(list_len) - 1, selected_ + (paged ? std::max(1, list_nav_rows() - 1) : 1));
            // Keep the selected row inside the visible window: the list scrolls to follow it.
            if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
        }
    } else if (action == "HKeySeekForward") {
        if (has_track_) { player_.seek_relative(5.0); seek_flash(1); }
    } else if (action == "HKeySeekBackward") {
        if (has_track_) { player_.seek_relative(-5.0); seek_flash(-1); }
    } else if (action == "HKeyQueueMoveUp") { // move the hovering queue item up (only meaningful once you've Tab'd into the queue)
        queue_move_hovering(-1);
    } else if (action == "HKeyQueueMoveDown") { // move the hovering queue item down (only meaningful once you've Tab'd into the queue)
        queue_move_hovering(1);
    } else if (action == "HKeyQueueMoveTop") {    // Shift+4: hovering queue item to the top
        queue_move_to_edge(-1);
    } else if (action == "HKeyQueueMoveBottom") { // Shift+5: hovering queue item to the bottom
        queue_move_to_edge(1);
    } else if (action == "HKeyQueueLock") {       // "!": locked queue keeps played tracks
        queue_toggle_lock();
    } else if (action == "HKeyQueueAddEnd") {     // "e": hovering list track to the end of the queue
        if (qst_.focus) {
            status_line_ = "focus the track list (TAB) to add a track to the end of the queue";
        } else {
            queue_add_selected_end();
            log_event("added to end of queue");
        }
    } else {
        return false;
    }
    return true;
}

bool App::browse_playback_key(const std::string& action, int key, int lookup_key, size_t list_len) {
    (void)key; (void)lookup_key; (void)list_len;
    if (action == "HKeyTogglePlayPause") {
        act_play_pause();
    } else if (action == "HKeyIncreaseVolume") {
        act_volume_change(+5);
    } else if (action == "HKeyDecreaseVolume") {
        act_volume_change(-5);
    } else if (action == "HKeyPlayNextSong") {
        act_next();
    } else if (action == "HKeyPlayPreviousSong") {
        // With titles in the queue (whichever pane has the focus) the key steps back in the queue; an unlocked queue does not remember
        // played tracks, so it says so only while the queue pane has the focus -- elsewhere the key plays the previous list track as before.
        if (!audiobook_ && (qst_.focus || (!queue_.empty() && qst_.locked))) {
            // Queue titles: only a LOCKED queue knows what was played (the played tracks sit at its end). The last item is
            // the track that plays now (if it came from the queue), the one before it is the previous one: step the queue back so
            // that one is the head again and play it -- the loop order stays as it was.
            if (!qst_.locked) {
                status_line_ = "the queue is unlocked: it does not remember played tracks (! locks it)";
            } else if (queue_.size() < 2) {
                status_line_ = "no previously played queue track";
            } else {
                const QueueItem& last = queue_.back();
                const bool last_is_current = has_track_ && last.is_local == current_.is_local &&
                    (last.is_local ? last.local_path == current_.path : last.video_id == current_.video_id);
                const size_t n = queue_.size();
                const size_t back = (last_is_current ? 2 : 1) % n;
                std::rotate(queue_.begin(), queue_.begin() + (n - back), queue_.end());
                for (QueueItem& q : queue_) q.played = false;
                qst_.next_run = 0;
                qst_.selected = 0;
                play_queue_item(0);
            }
        } else {
            act_prev();
        }
    } else if (action == "HKeyShuffleNext") {
        // Shuffle to a random next track in the current list -- a
        // manual one-off jump, independent of Play Mode
        // (settings_.play_mode). Reuses the same play_relative_random()
        // the automatic Shuffle play mode already calls on auto-advance.
        // With a queue: a random title of the queue instead (it is what plays next anyway).
        if (!queue_.empty()) play_next_from_queue(true);
        else play_relative_random();
    } else if (action == "HKeyToggleLyrics" || action == "HKeyToggleLyricsBack") {   // "." forward, "," backward
        // Cycle the lyrics area without going through Settings > On/Off:
        // sphere -> oscilloscope -> spectrogram -> lyrics ... (the order is Settings -> ANIMA/VIZ -> "Viz Order 1..4"). The visuals are
        // the lyrics engine's "off" states (element_lyrics=false, no network
        // fetching) with settings_.lyric_viz saying which one is drawn
        // (0 = sphere, 1 = osci, 2 = spectro), so the cycle is just a walk over those
        // settings. Returning to the lyrics needs a nudge though: the only
        // other place that starts a fetch is track load (poll_pending_load),
        // so without it the lyrics would not appear until the next track.
        // Views switched off on the ANIMA/VIZ tab (Use Lyrics / Oscilloscope / Spectrogram / Sphere) are left out.
        // The Video view (4) is the fifth place of the order (Viz Order 5) while the Video mode is on; it is the lyrics view with the picture.
        const bool vid = settings_.video_mode && !audiobook_;
        const int cur = settings_.element_lyrics ? ((vid && settings_.video_view) ? 4 : 3) : settings_.lyric_viz == 1 ? 1 : settings_.lyric_viz == 2 ? 2 : 0;
        std::vector<int> ord;
        for (int i = 0; i < 5; ++i) if (settings_.viz_order[i] != 4 || vid) ord.push_back(settings_.viz_order[i]);   // (Viz Order 1..5)
        // the video view is skipped for a track without a video when "When No Video Shows" is *nothing* (it would show an empty field)
        const bool video_view_usable = vid && (settings_.novideo_viz != 3 || (has_track_ && !video_shown_for(current_.path).empty()));
        const int dir = action == "HKeyToggleLyricsBack" ? -1 : 1;
        int next = cur;
        {
            int pos = 0;
            for (size_t i = 0; i < ord.size(); ++i) if (ord[i] == cur) pos = static_cast<int>(i);
            const int n = static_cast<int>(ord.size());
            for (int k = 1; k <= n; ++k) {
                const int c = ord[static_cast<size_t>(((pos + dir * k) % n + n) % n)];
                if (c == 4 ? video_view_usable : settings_.viz_usable(c)) { next = c; break; }
            }
        }
        static const char* const view_names[5] = {"sphere", "oscilloscope", "spectrogram", "lyrics", "video"};
        if (!settings_.element_vizfield) {
            status_line_ = "the Viz/Lyrics field is switched off (Settings > ANIMA/VIZ > Viz/Lyrics Field)";
        } else if (next == cur) {
            status_line_ = std::string("lyrics area: ") + view_names[cur] + " (the other views are switched off on Settings -> ANIMA/VIZ)";
        } else if (next >= 3) {                    // -> lyrics, or the video (lyrics with the picture)
            // lyrics -> video (or back) keeps the lyrics that are there: fetching again would blank them for a moment (the no-lyrics visual with a "cached" caption)
            const bool have_lyrics = settings_.element_lyrics && lyrics_.ready && lyrics_.path == current_.path;
            if (next == 4 && cur != 4) { auto_prev_view_ = cur; auto_prev_known_ = true; }   // (the view the video took the field from: "last used" brings it back on a track without a video)
            settings_.element_lyrics = true;
            settings_.video_view = next == 4;
            const bool placeholder = next == 4 && settings_.novideo_viz != 4 && has_track_ && video_shown_for(current_.path).empty();   // the video view of a track without a video shows a visual: no lyrics needed
            if (has_track_ && !have_lyrics && !placeholder) {
                std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
                last_lyrics_status_.clear(); // fresh 1.75s caption window, not a leftover from before it was off
                launch_lyrics_fetch(metadata_.name, artist, current_.path);
            }
            status_line_ = std::string("lyrics area: ") + view_names[next];
        } else {
            settings_.element_lyrics = false;
            settings_.video_view = false;
            settings_.lyric_viz = next;   // 0 sphere, 1 osci, 2 spectro
            status_line_ = std::string("lyrics area: ") + view_names[next];
        }
    } else {
        return false;
    }
    return true;
}

bool App::browse_queue_misc_key(const std::string& action, int key, int lookup_key, size_t list_len) {
    (void)key; (void)lookup_key; (void)list_len;
    if (action == "HKeyAddHoveringSongToQueue") {
        // Add hovering song to queue. Only for the library pane: with the Queue pane itself focused there is
        // nothing hovering in the list to add, so the key does nothing there (bulk add has its own key).
        if (!qst_.focus) {
            queue_add_selected();
            log_event("added to queue");
        }
    } else if (action == "HKeyClearQueue") { // SHIFT+x: clear the whole queue, after a Yes/No confirmation
        if (queue_.empty()) {
            status_line_ = "queue is already empty";
        } else {
            clear_queue_choice_ = 1; // default to No so a stray Enter never wipes the queue
            mode_ = Mode::ClearQueue;
        }
    } else if (action == "HKeyRemoveHoveringSongFromQueue") {
        queue_remove_hovering();
        log_event("removed from queue");
    } else if (action == "HKeyCyclePlayMode") {
        act_cycle_play_mode();   // one key for all five play modes
    } else if (action == "HKeyCyclePlayModeBack") {
        act_cycle_play_mode(-1);   // ... and the same the other way round
    } else if (action == "HKeyKaraoke") {
        // The karaoke overlay: the lyrics of the playing track over the whole screen.
        mode_ = Mode::Karaoke;
    } else if (action == "HKeyRefreshUi") {
        // Force a full redraw, for when a resize or terminal-session
        // switch raced the render loop and left a torn/stale frame on
        // screen. hard_clear is normally only set on a detected width or
        // mode change; this forces it once unconditionally on the very
        // next frame.
        force_redraw_ = true;
        log_event("ui refreshed");
    } else if (action == "HKeyConsole") {
        mode_ = Mode::Console;
    } else if (action == "HKeyAudioDevice") {   // "°" (SHIFT+^) / "&": the sound outputs of this computer
        audio_device_open(false);
    } else if (action == "HKeyToggleNormalize") { // loudness normalisation on/off, for A/B-ing it by ear
        settings_.normalize = !settings_.normalize;
        player_.set_normalization(settings_.normalize,
                                  static_cast<float>(settings_.normalize_target_lufs),
                                  static_cast<float>(settings_.normalize_max_boost_db));
        std::string msg = settings_.normalize ? "normalize: on" : "normalize: off";
        const float lufs = player_.track_lufs();
        if (settings_.normalize && has_track_ && !std::isnan(lufs)) {
            char buf[96];
            std::snprintf(buf, sizeof buf, " (track %.1f LUFS -> target %.0f, %+.1f dB)",
                          lufs, settings_.normalize_target_lufs,
                          static_cast<double>(settings_.normalize_target_lufs) - lufs);
            msg += buf;
        }
        status_line_ = msg;
        log_event(msg);
    } else if (action == "HKeyToggleMetaOnly") { // SHIFT+n: swap the lists between filename+metadata and metadata-only rows
        settings_.meta_only = !settings_.meta_only;
        std::string msg = settings_.meta_only ? "lists: metadata only (no filename)"
                                              : "lists: filename + metadata";
        status_line_ = msg;
        log_event(msg);
        resort_local_view_keep_selection(); // "title A-Z" follows the displayed title
        force_redraw_ = true;
    } else if (action == "HKeyToggleMute") { // force volume to 0 without touching pause state
        act_mute(-1);
    } else {
        return false;
    }
    return true;
}

bool App::browse_rest_key(const std::string& action, int key, int lookup_key, size_t list_len) {
    (void)key; (void)lookup_key; (void)list_len;
    if (action == "HKeyWizard") {
        wizard_open();
    } else if (action == "HKeyCheatsheet") {
        mode_ = Mode::Cheatsheet;
        cheat_->searching = false;
    } else if (action == "HKeyMenus") {   // "<": the list of all menus and overlays
        menu_index_sel_ = 1;
        mode_ = Mode::MenuIndex;
        force_redraw_ = true;
    } else if (action == "HKeyFilterForFolder") {
        if (list_source_ == ListSource::Local && !local_view_.empty() &&
            selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
            folder_filter_ = path_utf8(local_view_[selected_].path.parent_path());
            // Filtering by folder also drops back to folder order (T cycles
            // away from it again), so the pane title reads "sort: folder
            // order" and the files sit in the order they are on disk.
            local_sort_mode_ = 0;
            refresh_local_view();
            log_event("filtered: " + path_utf8(path_from_utf8(folder_filter_).filename()));
        }
    } else if (action == "HKeyClearFilter") {
        if (!folder_filter_.empty()) {
            folder_filter_.clear();
            refresh_local_view();
            log_event("filter cleared");
        }
    } else if (action == "HKeyRetryLyrics") { // opens the manual title/artist override form
        if (has_track_) {   // also with the lyrics engine off: the fetched lyrics are kept for karaoke / READ LYRICS
            rl_open_from_current_track();
            rl_->from_reader = rl_->from_karaoke = false;
            mode_ = Mode::RetryLyrics;
        }
    } else if (action == "HKeyToggleWaveform") { // toggle waveform style (raw/smooth) directly, without going into Settings
        settings_.waveform_smooth = !settings_.waveform_smooth;
        recompute_waveform_for_current_track();
        log_event(settings_.waveform_smooth ? "waveform: smooth" : "waveform: raw");
    } else if (action == "HKeyDownloadStream") { // save cached stream to the configured download folder
        if (has_track_) {
            if (path_utf8(current_.path).find(".cache") != std::string::npos || metadata_.location == "youtube" ||
                metadata_.location == "soundcloud" || metadata_.location == "bandcamp") {
                // Same folder the DOWNLOAD PATH setting promises everywhere
                // else (see load_library()'s cache-dir injection and the
                // Settings > PATHS tab's Download Path field): the
                // configured path, or ~/.cache/mousiki when none is set --
                // never settings_.local_music_paths[0] or $HOME/Music: those have nothing to do with the
                // folder the user configured for downloads.
                std::string dest_dir = path_utf8(cache_.download_dir());
                std::error_code ec;
                fs::create_directories(path_from_utf8(dest_dir), ec);

                std::string safe_name = metadata_.name;
                for (char& c : safe_name) if (c == '/' || c == '\\') c = '_';
                std::string safe_artist = (metadata_.artist == "-" ? "" : metadata_.artist);
                for (char& c : safe_artist) if (c == '/' || c == '\\') c = '_';

                std::string filename = safe_artist.empty() ? safe_name : safe_name + " - " + safe_artist;
                filename += path_utf8(current_.path.extension());

                fs::path dest_path = path_from_utf8(dest_dir) / path_from_utf8(filename);
                if (fs::exists(dest_path, ec)) {
                    status_line_ = "already saved: " + path_utf8(dest_path.filename());
                } else {
                    fs::copy_file(current_.path, dest_path, fs::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        fs::remove(current_.path, ec);
                        current_.path = dest_path; // update so sidecar lyrics go to the new folder
                        metadata_.location = dest_dir;
                        status_line_ = "saved to " + path_utf8(dest_path);
                        refresh_local_view();
                    } else {
                        status_line_ = "failed to save: " + ec.message();
                    }
                }
            } else {
                status_line_ = "not a cached stream";
            }
        }
    } else if (action == "HKeyVideoFx" || action == "HKeyVideoFxBack") {   // the next / previous effect for this track's video (small field)
        const int fxdir = action == "HKeyVideoFxBack" ? -1 : 1;
        if (audiobook_) status_line_ = "video effect: not available in the audio book player";
        else if (!settings_.video_mode || !settings_.video_view || video_shown_for(current_.path).empty()) status_line_ = "video effect: needs the video view in the field with a track that has a video";
        else video_track_fx_step(fxdir);
    } else if (action == "HKeyVideoFull") {   // ">": the video over the whole screen (on / off)
        video_full_toggle();
    } else if (action == "HKeyVideo") {
        video_menu_open();   // the VIDEO overlay (its first row is the Video mode switch)
    } else if (action == "HKeyCycleSortMode") { // cycle local-list sort mode (folder order -> title A-Z -> artist A-Z -> recently added [-> only videos, while the Video mode is on])
        local_sort_mode_ = (local_sort_mode_ + 1) % (settings_.video_mode ? 5 : 4);
        if (local_sort_mode_ == 3) { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }   // fresh times each time the mode is chosen
        refresh_local_view();
        log_event(std::string("sort: ") + sort_mode_name(local_sort_mode_, settings_.meta_only));
    } else if (action == "HKeyPlay") {
        if (qst_.focus) { qst_.next_run = 0; if (!queue_.empty()) play_queue_item(std::clamp(qst_.selected, 0, static_cast<int>(queue_.size()) - 1), last_key_was_shifted()); }   // ENTER in the queue pane (small or big): play the highlighted item; SHIFT+ENTER also moves the queue along up to it
        else play_selected();
    } else if (action == "HKeySearch") {
        if (qst_.overlay_open) queue_overlay_close(); // the search filters the list underneath, which the queue overlay covers
        mode_ = Mode::Search;
        search_.buffer.clear();
        search_.nav_moved = false;
        pre_search_list_source_ = list_source_;
        pre_search_local_query_ = last_local_query_;
    } else if (action == "HKeyQuit") {
        // Only the bound key itself quits: the case fallback above would also make SHIFT+q quit, which is too easy to hit.
        auto qk = settings_.hotkeys.find("HKeyQuit");
        if (qk != settings_.hotkeys.end() && hotkey_string_to_key(qk->second) == lookup_key) quit_ = true;
    } else if (key == 27) {
        // ESC -- back to the home view: full local library, no filter,
        // from the top. Same destination regardless of how buried you
        // are (mid search results, viewing online results, scrolled
        // deep into the list). Not in settings_.hotkeys / kRefRows at all,
        // on purpose: this mirrors the original, which likewise has no
        // HKeyEsc entry -- ESC is a fixed shortcut, not something meant
        // to be rebound (see the SYSTEM (MAIN UI) section of
        // the cheat sheet, '?', which is where fixed keys like this one
        // are documented -- the Reference tab only lists rebindable ones).
        list_source_ = ListSource::Local;
        last_local_query_.clear();
        folder_filter_.clear();
        refresh_local_view();
        status_line_.clear();
    } else {
        return false;
    }
    return true;
}

} // namespace muisc
