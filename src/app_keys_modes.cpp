// app_keys_modes.cpp
// Part of class App (src/app.h): the key handlers of the overlays, one function per mode (split out of App::handle_key).
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "app_state_rl.h"
#include "app_state_sleep.h"
#include "app_state_eq.h"
#include "app_state_qtp.h"
#include "app_state_bulk.h"
#include "app_state_wizard.h"
#include "app_state_cheat.h"
#include "playlist_state.h"
#include "readlyrics_state.h"
#include "bars_rows.h"
#include "text_edit.h"
#include "app_state_cliov.h"
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

// The key handlers of the overlays that used to be inline in App::handle_key (src/app_keys.cpp): one function per mode.

void App::handle_console_key(int key) {
    // ESC, or the console hotkey again, closes it. TAB changes the pane (logs / status messages / command line), the arrow keys scroll
    // it (SHIFT: a page), HOME goes to the oldest line, END back to the newest. In the command pane every key is typed (so [t] does
    // not close it: ESC clears the line first, a second ESC closes), ENTER runs the line, UP / DOWN recall earlier lines.
    const bool arrow = last_key_was_arrow();
    if (console_pane_ == 2 && console_has_[2]) {
        if (!arrow && key == 27) {
            if (!cli_buf_.empty()) { cli_buf_.clear(); cli_caret_ = cli_anchor_ = 0; cli_hist_pos_ = -1; }
            else mode_ = Mode::Browse;
        } else if (!arrow && key == 9) {
            console_pane_ = console_has_[0] ? 0 : 1;
        } else if (!arrow && (key == '\r' || key == '\n')) {
            cli_run(cli_buf_);
            cli_buf_.clear(); cli_caret_ = cli_anchor_ = 0;
        } else if (arrow && (key == 'A' || key == 'B')) {
            if (!cli_hist_.empty()) {
                const int n = static_cast<int>(cli_hist_.size());
                cli_hist_pos_ = key == 'A' ? (cli_hist_pos_ < 0 ? n - 1 : std::max(0, cli_hist_pos_ - 1))
                                           : (cli_hist_pos_ < 0 ? -1 : cli_hist_pos_ + 1 >= n ? -1 : cli_hist_pos_ + 1);
                cli_buf_ = cli_hist_pos_ < 0 ? std::string() : cli_hist_[static_cast<size_t>(cli_hist_pos_)];
                cli_caret_ = cli_anchor_ = cli_buf_.size();
            }
        } else if (key == kKeyCtrlRight) {   // complete (not TAB: that changes the pane)
            const std::string done = cli_complete(cli_buf_);
            if (!done.empty()) { cli_buf_ = done; cli_caret_ = cli_anchor_ = cli_buf_.size(); }
            if (!cli_reply_.empty()) cli_out_.push_back("  " + cli_reply_);
        } else {
            edit_focus("console-cli", cli_buf_);
            if (edit_text_key(cli_buf_, cli_caret_, cli_anchor_, key, 200, &status_line_)) cli_hist_pos_ = -1;
        }
        force_redraw_ = true;
        return;
    }
    if (!arrow && (key == 27 || key == 't' || key == 'T')) mode_ = Mode::Browse;
    else if (!arrow && key == 9) { int p = console_pane_; for (int i = 0; i < 3; ++i) { p = (p + 1) % 3; if (console_has_[p]) break; } console_pane_ = p; }
    else if (!arrow && (key == 'v' || key == 'V')) { ConsoleLog::instance().toggle_level(); status_line_ = ConsoleLog::instance().level() == LogVerbosity::Verbose ? "verbose log: on (this session)" : "verbose log: off"; }
    else if (arrow && (key == 'A' || key == 'B')) {
        const int step = last_key_was_shifted() ? 8 : 1;
        console_up_[console_pane_] = std::max(0, console_up_[console_pane_] + (key == 'A' ? step : -step));   // (the upper end is clamped while drawing)
    } else if (key == kKeyHome) console_up_[console_pane_] = 1 << 20;
    else if (key == kKeyEnd) console_up_[console_pane_] = 0;
    return;
}

void App::handle_update_notice_key(int key) {
    // The new-version popup: [o] opens the release page (and closes it), ESC closes it; other keys do nothing, so a
    // key meant for the player is not lost silently behind it (it is small and one key away from gone).
    const bool arrow = last_key_was_arrow();
    if (!arrow && (key == 'o' || key == 'O')) {
        if (!open_url(update_url_)) { status_line_ = "could not open the browser: " + update_url_; }
        mode_ = Mode::Browse;
    } else if (!arrow && key == 27) {
        mode_ = Mode::Browse;
    }
    force_redraw_ = true;
    return;
}

void App::handle_qtp_key(int key) {
    const bool arrow = last_key_was_arrow();
    force_redraw_ = true;
    if (qtp_->stage == 0) {   // New playlist | Existing playlist
        if (!arrow && key == 27) { mode_ = Mode::Browse; qtp_->track = false; return; }
        if ((arrow && (key == 'C' || key == 'D')) || (!arrow && key == 9)) { qtp_->choice ^= 1; return; }
        if (!arrow && (key == 'n' || key == 'N')) { qtp_->choice = 0; key = '\r'; }
        else if (!arrow && (key == 'e' || key == 'E')) { qtp_->choice = 1; key = '\r'; }
        if (!arrow && (key == '\r' || key == '\n')) {
            if (qtp_->choice == 0 && qtp_->track) { qtp_->stage = 2; qtp_->new_name.clear(); edit_owner_.clear(); }
            else if (qtp_->choice == 0) { mode_ = Mode::Browse; queue_to_playlist_apply(""); }
            else { qtp_->stage = 1; qtp_->query.clear(); qtp_->selected = 0; edit_owner_.clear(); qtp_refresh(); }
        }
        return;
    }
    if (qtp_->stage == 2) {   // SHIFT+a, new playlist: its name
        if (!arrow && key == 27) { qtp_->stage = 0; return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            std::string name = qtp_->new_name;
            while (!name.empty() && name.front() == ' ') name.erase(name.begin());
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (name.empty()) { status_line_ = "type a name first"; return; }
            track_to_playlist_apply(name);
            return;
        }
        std::string st;
        edit_focus("track-to-playlist-name", qtp_->new_name);
        edit_text_key(qtp_->new_name, edit_caret_, edit_anchor_, key, 80, &st);
        return;
    }
    // stage 1: the search field (typing filters) and the list of saved playlists
    if (!arrow && key == 27) { qtp_->stage = 0; return; }
    const int total = static_cast<int>(qtp_->view.size());
    if (arrow && key == 'A') { if (qtp_->selected > 0) --qtp_->selected; return; }
    if (arrow && key == 'B') { if (qtp_->selected < total - 1) ++qtp_->selected; return; }
    if (!arrow && (key == '\r' || key == '\n')) {
        if (total > 0) {
            const std::string name = qtp_->view[static_cast<size_t>(std::clamp(qtp_->selected, 0, total - 1))].name;
            if (qtp_->track) track_to_playlist_apply(name);
            else { mode_ = Mode::Browse; queue_to_playlist_apply(name); }
        }
        return;
    }
    std::string st;
    edit_focus("queue-to-playlist-search", qtp_->query);
    if (edit_text_key(qtp_->query, edit_caret_, edit_anchor_, key, 80, &st)) qtp_refresh();
    return;
}

void App::handle_clear_queue_key(int key) {
    // "Want to clear queue?" -- Left/Right/TAB move between Yes and No,
    // Enter takes the highlighted one, y / n answer directly, ESC is No.
    // Any OTHER key also cancels (nothing is cleared): this mode must never
    // be a place where keys vanish silently, or a popup that is hard to
    // see would look like a frozen UI. Cancelling is always the safe side.
    const bool arrow = last_key_was_arrow();
    if (arrow && (key == 'C' || key == 'D')) { clear_queue_choice_ = (key == 'D') ? 0 : 1; return; } // Left=Yes, Right=No
    if (!arrow && key == 9) { clear_queue_choice_ ^= 1; return; }
    if (!arrow && (key == 'y' || key == 'Y')) { mode_ = Mode::Browse; queue_clear(); return; }
    if (!arrow && (key == '\r' || key == '\n')) {
        mode_ = Mode::Browse;
        if (clear_queue_choice_ == 0) queue_clear();
        return;
    }
    mode_ = Mode::Browse; // n / N / ESC / anything else: No
    status_line_.clear();
    return;
}

void App::handle_spectro_menu_key(int key) {
    // Spectrogram options (SHIFT+i). Like the oscilloscope overlay: playback keeps running, only these keys work.
    const bool arrow = last_key_was_arrow();
    // rows kSrView .. the last (no "Display" row: what the lyrics area shows is the "." key's job)
    if (arrow && (key == 'A' || key == 'B')) {   // only the rows of the view in use (the waterfall has others than the spectrogram)
        const std::vector<int> vis = spectro_visible_rows(video_layout_menu() ? layout_spectro_view() : settings_.spectro);
        int at = 0;
        for (size_t i = 0; i < vis.size(); ++i) if (vis[i] <= spectro_.menu_row) at = static_cast<int>(i);
        const int n = static_cast<int>(vis.size());
        at = key == 'A' ? (at + n - 1) % n : (at + 1) % n;
        spectro_.menu_row = vis[static_cast<size_t>(at)];
        return;
    }
    if (arrow && key == 'D') { spectro_menu_adjust(-1); return; }
    if (arrow && key == 'C') { spectro_menu_adjust(+1); return; }
    if (!arrow && (key == 'r' || key == 'R')) {
        if (video_layout_menu()) { settings_.vl_spec_view = settings_.vl_spec_style = -1; return; }   // the layout's choices back to "as in the options"
        spectro_reset(settings_.spectro); settings_.spectro_sync(); return;
    }
    std::string act = resolve_hotkey_action(key);
    if (act.empty() && key >= 'a' && key <= 'z') act = resolve_hotkey_action(key - 32);
    if (act.empty() && key >= 'A' && key <= 'Z') act = resolve_hotkey_action(key + 32);
    if (video_full_ && (act == "HKeySpectroFull" || act == "HKeyScopeBig")) return;   // (not out of the video layout)
    if (video_full_ && act == "HKeyOscMenu") { if (!vl_has_osci(settings_.video_layout)) return; save_settings(settings_); osci_menu_row_ = 0; mode_ = Mode::OsciMenu; force_redraw_ = true; return; }
    if (!arrow && act == "HKeySpectroFull") {
        spectro_.full = !spectro_.full; if (spectro_.full) scope_big_ = false; force_redraw_ = true; return;
    }
    if (!arrow && act == "HKeyScopeWindow") { scope_window_toggle(); return; }     // the windows: always
    if (!arrow && act == "HKeySpectroWindow") { spectro_window_toggle(); return; }
    // the oscilloscope's overlays take over without closing this one first
    if (!arrow && act == "HKeyOscMenu") { save_settings(settings_); spectro_.full = false; osci_menu_row_ = 0; mode_ = Mode::OsciMenu; force_redraw_ = true; return; }
    if (!arrow && act == "HKeyScopeBig") {
        save_settings(settings_);
        scope_big_ = true; spectro_.full = false; scope_big_color_edit_ = false; scope_metrics_open_ = false;
        osci_menu_row_ = 0; mode_ = Mode::OsciMenu; force_redraw_ = true; return;
    }
    if (!arrow && (key == 27 || act == "HKeySpectroMenu")) {
        mode_ = Mode::Browse;
        save_settings(settings_);
    }
    return;
}

void App::handle_osci_menu_key(int key) {
    // Oscilloscope tuning. Playback and the scope keep running underneath;
    // only this overlay takes keys. Unknown keys are ignored (the footer
    // lists every key), ESC or the overlay hotkey again closes and saves.
    const bool arrow = last_key_was_arrow();
    const int osci_n = std::max(1, static_cast<int>(osci_menu_rows().size()));
    if (arrow && key == 'A') { osci_menu_row_ = (osci_menu_row_ + osci_n - 1) % osci_n; return; } // up
    if (arrow && key == 'B') { osci_menu_row_ = (osci_menu_row_ + 1) % osci_n; return; }          // down
    if (arrow && key == 'D') { osci_menu_adjust(-1); return; }                      // left
    if (arrow && key == 'C') { osci_menu_adjust(+1); return; }                      // right
    if (!arrow && (key == 'r' || key == 'R')) {
        if (video_layout_menu()) { settings_.vl_osci_style = settings_.vl_osci_mode = -1; return; }   // the layout's choices back to "as in the options"
        osci_reset(settings_);
        return;
    }
    std::string osc_action = resolve_hotkey_action(key);
    if (osc_action.empty() && key >= 'a' && key <= 'z') osc_action = resolve_hotkey_action(key - 32);
    if (osc_action.empty() && key >= 'A' && key <= 'Z') osc_action = resolve_hotkey_action(key + 32);
    if (video_full_ && (osc_action == "HKeyScopeBig" || osc_action == "HKeySpectroFull")) return;   // (not out of the video layout)
    if (video_full_ && osc_action == "HKeySpectroMenu") { if (vl_has_spectro(settings_.video_layout)) { save_settings(settings_); spectro_.menu_row = kSrView; mode_ = Mode::SpectroMenu; force_redraw_ = true; } return; }
    if (!arrow && osc_action == "HKeyScopeWindow") { scope_window_toggle(); return; }     // the windows: always
    if (!arrow && osc_action == "HKeySpectroWindow") { spectro_window_toggle(); return; }
    if (!arrow && osc_action == "HKeyScopeBig") { scope_big_ = !scope_big_; spectro_.full = false; scope_big_color_edit_ = false; scope_metrics_open_ = false; force_redraw_ = true; return; }
    // the spectrogram's overlays take over without closing this one first
    if (!arrow && osc_action == "HKeySpectroMenu") { save_settings(settings_); scope_big_ = false; spectro_.menu_row = kSrView; mode_ = Mode::SpectroMenu; force_redraw_ = true; return; }
    if (!arrow && osc_action == "HKeySpectroFull") {
        save_settings(settings_);
        spectro_.full = true; scope_big_ = false;
        spectro_.menu_row = kSrView; mode_ = Mode::SpectroMenu; force_redraw_ = true; return;
    }
    if (!arrow && (key == 27 || osc_action == "HKeyOscMenu")) {
        mode_ = Mode::Browse;
        save_settings(settings_);
    }
    return;
}

void App::handle_bars_menu_key(int key) {
    // The spectrum bars' options. Playback and the bars keep running underneath; only these keys work.
    const bool arrow = last_key_was_arrow();
    const std::vector<int> rows = bars_menu_rows();
    const int n = static_cast<int>(rows.size());
    int at = 0;
    for (int i = 0; i < n; ++i) if (rows[static_cast<size_t>(i)] == bars_menu_row_) at = i;   // exact match: the list order need not follow the ids
    if (arrow && key == 'A') { bars_menu_row_ = rows[static_cast<size_t>((at + n - 1) % n)]; return; }
    if (arrow && key == 'B') { bars_menu_row_ = rows[static_cast<size_t>((at + 1) % n)]; return; }
    if (arrow && key == 'D') { bars_menu_adjust(-1); return; }
    if (arrow && key == 'C') { bars_menu_adjust(+1); return; }
    if (!arrow && (key == 'r' || key == 'R')) {   // the Hi-Fi values and the motion back to the defaults; the style is left alone
        bars_reset(settings_);
        return;
    }
    std::string bars_action = resolve_hotkey_action(key);
    if (bars_action.empty() && key >= 'a' && key <= 'z') bars_action = resolve_hotkey_action(key - 32);
    if (bars_action.empty() && key >= 'A' && key <= 'Z') bars_action = resolve_hotkey_action(key + 32);
    if (!arrow && (key == 27 || bars_action == "HKeyBarsMenu" || key_is("HKeyBarsMenu", key) || (audiobook_ && key == 'Y'))) {
        mode_ = Mode::Browse;
        save_settings(settings_);
    }
    return;
}

void App::handle_norm_menu_key(int key) {
    // Loudness normalisation. Playback keeps running underneath and the
    // level follows every change immediately; only this overlay takes
    // keys. Unknown keys are ignored (the footer lists every key), ESC or
    // the overlay hotkey again closes and saves.
    const bool arrow = last_key_was_arrow();
    if (arrow && key == 'A') { norm_menu_row_ = (norm_menu_row_ + 2) % 3; return; } // up
    if (arrow && key == 'B') { norm_menu_row_ = (norm_menu_row_ + 1) % 3; return; } // down
    if (arrow && key == 'D') { norm_menu_adjust(-1); return; }                      // left
    if (arrow && key == 'C') { norm_menu_adjust(+1); return; }                      // right
    if (!arrow && key == ' ') {
        settings_.normalize = !settings_.normalize;
        norm_apply();
        return;
    }
    if (!arrow && (key == 'r' || key == 'R')) { // target + boost back to the defaults; on/off is left alone
        const Settings defaults{};
        settings_.normalize_target_lufs = defaults.normalize_target_lufs;
        settings_.normalize_max_boost_db = defaults.normalize_max_boost_db;
        norm_apply();
        return;
    }
    std::string norm_action = resolve_hotkey_action(key);
    if (norm_action.empty() && key >= 'a' && key <= 'z') norm_action = resolve_hotkey_action(key - 32);
    if (norm_action.empty() && key >= 'A' && key <= 'Z') norm_action = resolve_hotkey_action(key + 32);
    if (!arrow && (key == 27 || norm_action == "HKeyNormMenu")) {
        mode_ = Mode::Browse;
        save_settings(settings_);
    } else if (!arrow && norm_action == "HKeyToggleNormalize") { // the on/off key works in here too
        settings_.normalize = !settings_.normalize;
        norm_apply();
    }
    return;
}

void App::handle_lyrics_edit_key(int key) {
    // Lyrics timing. Playback keeps running underneath; only this overlay
    // takes keys. Unknown keys are ignored (the footer lists every key).
    const bool arrow = last_key_was_arrow();
    if (arrow && key == 'C') { lyrics_.lyrics_edit_adjust(+0.1); return; } // right: lyrics later
    if (arrow && key == 'D') { lyrics_.lyrics_edit_adjust(-0.1); return; } // left: lyrics earlier
    if (arrow && key == 'A') { lyrics_.lyrics_edit_adjust(+0.5); return; } // up
    if (arrow && key == 'B') { lyrics_.lyrics_edit_adjust(-0.5); return; } // down
    // SHIFT+Left/Right seek the running track (5 s, like the normal seek keys) so a line can be heard again
    // while it is being re-timed.
    if (key == kKeyShiftLeft)  { if (has_track_) player_.seek_relative(-5.0); return; }
    if (key == kKeyShiftRight) { if (has_track_) player_.seek_relative(5.0);  return; }
    if (arrow) return;
    if (key == 'r' || key == 'R') {
        std::lock_guard<std::mutex> lk(lyrics_.mutex);
        lyrics_.result.delay = 0.0;
        return;
    }
    if (key == '\r' || key == '\n' || key == 's' || key == 'S') {
        double d = 0.0;
        { std::lock_guard<std::mutex> lk(lyrics_.mutex); d = lyrics_.result.delay; }
        char num[24];
        std::snprintf(num, sizeof num, "%+.1f s", d);
        if (lyrics_.edit_from_reader) rd_->delay = d;   // (the reader's lines keep the new offset)
        if (save_lyrics_delay(lyrics_.path, d)) status_line_ = std::string("lyrics timing saved: ") + num;
        else status_line_ = std::string("lyrics timing ") + num + " applied, but could not be saved (no lyrics file for this track)";
        lyrics_edit_close();
        return;
    }
    if (key == 27 || key_is("HKeyLyricsTiming", key)) { // cancel: the value from before the overlay opened comes back
        { std::lock_guard<std::mutex> lk(lyrics_.mutex); lyrics_.result.delay = lyrics_.edit_orig_delay; }
        lyrics_edit_close();
    }
    return;
}

void App::handle_sleep_timer_key(int key) {
    // Sleep timer. Playback keeps running underneath; only this overlay takes
    // keys. Unknown keys are ignored (the footer lists every key).
    const int rows = 8; // 15 / 30 / 60 / 90 / 120 min, stop after song, fade out, off
    const bool arrow = last_key_was_arrow();
    if (arrow && key == 'A') { sleep_->menu_row = (sleep_->menu_row + rows - 1) % rows; return; } // up
    if (arrow && key == 'B') { sleep_->menu_row = (sleep_->menu_row + 1) % rows; return; }        // down
    if (!arrow && (key == '\r' || key == '\n') && sleep_->menu_row == 6) {   // Fade out: a toggle, the overlay stays open
        settings_.sleep_fade = !settings_.sleep_fade;
        if (!settings_.sleep_fade) player_.set_fade(1.0f);
        save_settings(settings_);
        return;
    }
    if (!arrow && (key == '\r' || key == '\n')) {
        sleep_timer_apply(sleep_->menu_row);
        mode_ = Mode::Browse;
        return;
    }
    std::string sl_action = resolve_hotkey_action(key);
    if (sl_action.empty() && key >= 'a' && key <= 'z') sl_action = resolve_hotkey_action(key - 32);
    if (sl_action.empty() && key >= 'A' && key <= 'Z') sl_action = resolve_hotkey_action(key + 32);
    if (!arrow && (key == 27 || sl_action == "HKeySleepTimer")) mode_ = Mode::Browse;
    return;
}

void App::handle_equalizer_key(int key) {
    // Equaliser. Playback keeps running underneath and the sound follows
    // every change immediately; only this overlay takes keys. Unknown keys
    // are ignored (the footer lists every key). ESC or the overlay hotkey
    // closes and saves.
    const bool arrow = last_key_was_arrow();

    // The "Save as:" prompt owns every key until ENTER or ESC: typing,
    // caret, marking and clipboard come from the shared single-line editor.
    if (eq_->naming) {
        if (!arrow && key == 27) { // cancel the prompt only, the overlay stays open
            eq_->naming = false;
            eq_->name_buf.clear();
            eq_->status = "Save cancelled";
            return;
        }
        if (!arrow && (key == '\r' || key == '\n')) { eq_commit_name(); return; }
        edit_focus("eq-name", eq_->name_buf);
        edit_text_key(eq_->name_buf, edit_caret_, edit_anchor_, key, kEqNameMaxBytes, nullptr);
        return;
    }

    const bool delete_was_armed = eq_->delete_armed; // a second DEL / X right after the first confirms
    eq_->delete_armed = false;
    eq_->status.clear();

    if (arrow && key == 'C') { eq_->band = (eq_->band + 1) % kEqBands; return; }               // right
    if (arrow && key == 'D') { eq_->band = (eq_->band + kEqBands - 1) % kEqBands; return; }    // left
    if (arrow && key == 'A') { eq_set_gain(eq_->band, settings_.eq_gains[eq_->band] + 1.0f); return; } // up
    if (arrow && key == 'B') { eq_set_gain(eq_->band, settings_.eq_gains[eq_->band] - 1.0f); return; } // down
    if (!arrow && (key == ',' || key == '<')) { eq_select_preset(-1); return; }
    if (!arrow && (key == '.' || key == '>' || key == 9)) { eq_select_preset(+1); return; }
    if (!arrow && key == '0') { eq_set_gain(eq_->band, 0.0f); return; }
    if (!arrow && key == ' ') {
        settings_.eq_enabled = !settings_.eq_enabled;
        eq_apply();
        return;
    }
    if (!arrow && (key == 'r' || key == 'R')) { // back to Flat; the on/off state is left alone
        settings_.eq_gains = kEqPresets[0].gains;
        eq_->last_preset = 0;
        eq_apply();
        return;
    }
    std::string eq_action = resolve_hotkey_action(key);
    if (eq_action.empty() && key >= 'a' && key <= 'z') eq_action = resolve_hotkey_action(key - 32);
    if (eq_action.empty() && key >= 'A' && key <= 'Z') eq_action = resolve_hotkey_action(key + 32);
    const bool eq_close_key = !arrow && (key == 27 || eq_action == "HKeyEqualizer");
    if (!eq_close_key && !arrow && (key == 's' || key == 'S')) { eq_begin_naming(); return; }
    if (!eq_close_key && !arrow && (key == kKeyDelete || key == 'x' || key == 'X')) {
        eq_delete_custom(delete_was_armed);
        return;
    }
    if (eq_close_key) {
        mode_ = Mode::Browse;
        save_settings(settings_);
    }
    return;
}

void App::handle_karaoke_key(int key) {
    // ESC or the karaoke key close it; the playback keys (play/pause,
    // next/previous, seek, volume, mute, ...) work as on the main screen
    // and are simply handed to it; everything else is ignored.
    const bool arrow = last_key_was_arrow();
    auto karaoke_close = [this]() {
        mode_ = Mode::Browse;
        if (karaoke_size_dirty_) { save_settings(settings_); karaoke_size_dirty_ = false; }   // the lyrics size sticks
    };
    if (key == 27) { karaoke_close(); return; }
    if (!arrow && key_is("HKeyTranslit", key)) { toggle_latin_letters(); return; }
    // The lyrics timing overlay opens over the karaoke screen (judging an offset is easiest against the big lyrics); closing it
    // brings the karaoke screen back.
    if (!arrow && key_is("HKeyLyricsTiming", key)) { lyrics_edit_open_over_karaoke(); return; }
    if (key == kKeyCtrlN || (!arrow && (key == 'a' || key == 'A'))) { readlyrics_add_open_from_playing(); return; }   // add a lyrics file by hand (READ LYRICS, overlay open)
    if (!arrow && key_is("HKeyRetryLyrics", key)) { rl_open_over_karaoke(); return; }   // the re-fetch / version control overlay, as on the main screen
    if (arrow && (key == 'A' || key == 'B')) return;
    // SHIFT and + / SHIFT and - (the characters '*' and '_'): lyrics size 1..5. Fixed keys -- on a US keyboard
    // '*' is SHIFT+8. Only the lyrics change size, the pictures stay as they are.
    const bool size_up = key_is("HKeyKaraokeBigger", key), size_down = key_is("HKeyKaraokeSmaller", key);
    if (!arrow && (size_up || size_down)) {
        const int before = settings_.karaoke_lyrics_size;
        settings_.karaoke_lyrics_size = std::clamp(before + (size_up ? 1 : -1), 1, 5);
        if (settings_.karaoke_lyrics_size != before) karaoke_size_dirty_ = true;
        karaoke_flash_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        return;
    }
    std::string act;
    if (!arrow) {
        const int lk = (key == '\r') ? '\n' : key;
        act = resolve_hotkey_action(lk);
        if (act.empty() && lk >= 'a' && lk <= 'z') act = resolve_hotkey_action(lk - 32);
        if (act.empty() && lk >= 'A' && lk <= 'Z') act = resolve_hotkey_action(lk + 32);
    }
    if (act == "HKeyKaraoke") { karaoke_close(); return; }
    static const char* const kPassThrough[] = {
        "HKeyTogglePlayPause", "HKeyPlayNextSong", "HKeyPlayPreviousSong", "HKeyShuffleNext",
        "HKeyIncreaseVolume", "HKeyDecreaseVolume", "HKeyToggleMute", "HKeyCyclePlayMode", "HKeyCyclePlayModeBack", "HKeyToggleNormalize",
    };
    bool pass = arrow && (key == 'C' || key == 'D');   // Left/Right: seek
    for (const char* a : kPassThrough) if (act == a) pass = true;
    if (pass) {
        mode_ = Mode::Browse;
        handle_key(key);
        if (mode_ == Mode::Browse) mode_ = Mode::Karaoke;
    }
    return;
}

void App::handle_cheatsheet_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (cheat_->searching) {   // SHIFT+s: the search field has the keyboard
        const int n = static_cast<int>(cheat_->result_rows.size());
        if (!arrow && key == 27) {   // ESC clears the query first, then leaves the search
            if (!cheat_->query.empty()) { cheat_->query.clear(); cheat_->sel = 0; cheat_->first = 0; edit_owner_.clear(); }
            else cheat_->searching = false;
        } else if (arrow && (key == 'A' || key == 'B')) {
            const int step = last_key_was_shifted() ? 5 : 1;
            cheat_->sel = std::clamp(cheat_->sel + (key == 'B' ? step : -step), 0, std::max(0, n - 1));
        } else if (!arrow && (key == '\r' || key == '\n')) {
            if (n > 0) cheat_->jump_row = cheat_->result_rows[static_cast<size_t>(std::clamp(cheat_->sel, 0, n - 1))];
            cheat_->searching = false;
        } else {
            edit_focus("cheat-search", cheat_->query);
            if (edit_text_key(cheat_->query, edit_caret_, edit_anchor_, key, 80, nullptr)) { cheat_->sel = 0; cheat_->first = 0; }
        }
        return;
    }
    if (!arrow && key_is("HKeySearch", key)) { cheat_->searching = true; cheat_->query.clear(); cheat_->sel = 0; cheat_->first = 0; edit_owner_.clear(); }
    else if (key == 27 || key_is("HKeyCheatsheet", key)) mode_ = Mode::Browse;
    else if (key == 'A') cheatsheet_scroll_ -= (last_key_was_shifted() ? 10 : 1); // up (SHIFT / PgUp: a page) -- the table is longer than the screen
    else if (key == 'B') cheatsheet_scroll_ += (last_key_was_shifted() ? 10 : 1); // down (clamped while rendering)
    return;
}

void App::handle_wizard_key(int key) {
    // The wizard keeps its own page / part / contents state (src/wizard.cpp); here the key only has to be named.
    // Nothing reaches the player underneath: the wizard is a full-screen view like the cheat sheet, and playback
    // simply keeps running.
    const bool arrow = last_key_was_arrow();
    if (wizard_->st.searching) { wizard::search_key(wizard_->st, key, arrow, arrow && last_key_was_shifted()); return; }   // SHIFT+s: the query has the keyboard
    wizard::Key wk = wizard::Key::None;
    if (arrow) {
        const bool shifted = last_key_was_shifted();
        if (key == 'C') wk = wizard::Key::Right;
        else if (key == 'D') wk = wizard::Key::Left;
        else if (key == 'A') wk = shifted ? wizard::Key::PageUp : wizard::Key::Up;
        else if (key == 'B') wk = shifted ? wizard::Key::PageDown : wizard::Key::Down;
    } else if (key == 27 || key == 'q') wk = wizard::Key::Close;
    else if (key == '\r' || key == '\n') wk = wizard::Key::Enter;
    else if (key == '\b' || key == 127) wk = wizard::Key::Back;           // BACKSPACE and [t]: the table of content
    else if (key == 't' || key == 'T') wk = wizard::Key::Contents;
    else if (key_is("HKeySearch", key)) wk = wizard::Key::Search;         // the search key ("/", SHIFT+7 on a German keyboard): search the whole text
    else if (key_is("HKeyWizard", key)) wk = wizard::Key::Close;
    if (wizard::handle_key(wizard_->st, wk)) wizard_close();
    return;
}

void App::handle_bulk_add_key(int key) {
    if (key == 27) { // cancel entirely -- discard buffer, results, and selection state
        mode_ = Mode::Browse;
        status_line_.clear();
        bulk_->add_buffer.clear();
        bulk_->add_results_ready = false;
        pending_bulk_add_ = BulkAddResult{};
        bulk_->add_selected.clear();
        return;
    }

    // The two options, in both phases (they apply to what is added next; both are settings: BulkAddSave=, BulkAddVideo=).
    if (key == kKeyCtrlS) {
        settings_.bulk_save = !settings_.bulk_save;
        status_line_ = settings_.bulk_save ? "bulk add: the tracks are saved in the download folder when they play"
                                           : "bulk add: play only -- a downloaded file is deleted again after the track";
        return;
    }
    if (key == kKeyCtrlO) {
        settings_.bulk_video = !settings_.bulk_video;
        status_line_ = settings_.bulk_video ? (settings_.video_mode ? "bulk add: the video of each track is downloaded when it plays"
                                                                    : "bulk add: with video -- but video mode is off (video overlay), so it will not be shown")
                                            : "bulk add: no videos";
        return;
    }
    if (bulk_->add_results_ready) {
        // --- Phase 2: navigate/star the fetched checklist. ---
        int total = static_cast<int>(pending_bulk_add_.items.size());
        if (key == 'A' && total > 0) { // up
            bulk_->add_cursor = std::max(0, bulk_->add_cursor - 1);
            if (bulk_->add_cursor < bulk_->add_scroll) bulk_->add_scroll = bulk_->add_cursor;
            return;
        }
        if (key == 'B' && total > 0) { // down
            bulk_->add_cursor = std::min(total - 1, bulk_->add_cursor + 1);
            if (bulk_->add_cursor >= bulk_->add_scroll + kBulkAddVisibleRows) bulk_->add_scroll = bulk_->add_cursor - kBulkAddVisibleRows + 1;
            return;
        }
        if (key == ' ' && total > 0) { // toggle star on the hovered row
            if (bulk_->add_cursor < static_cast<int>(bulk_->add_selected.size())) {
                bulk_->add_selected[bulk_->add_cursor] = !bulk_->add_selected[bulk_->add_cursor];
            }
            return;
        }
        if (key == 'a') { commit_bulk_add(/*all=*/true); return; }        // "ALL"
        if (key == '\r' || key == '\n') { commit_bulk_add(/*all=*/false); return; } // "[SELECT]"
        return;
    }

    // --- Phase 1: typing the link. ---
    if (key == '\r' || key == '\n') {
        if (!bulk_->add_buffer.empty() && !bulk_->add_in_progress.load()) {
            status_line_ = "fetching playlist ...";
            launch_bulk_add_async(bulk_->add_buffer);
        }
        return; // stays open -- poll_pending_bulk_add() moves to phase 2 once the fetch resolves
    }
    // The shared single-line editor: caret, SHIFT+arrows marking, HOME / END, DEL, CTRL+C / X / V. It also tells
    // a typed A-D from an arrow key (arrows collapse to those letters).
    edit_focus("bulk-add", bulk_->add_buffer);
    edit_text_key(bulk_->add_buffer, edit_caret_, edit_anchor_, key, 200, &status_line_);
    return;
}

void App::handle_retry_lyrics_key(int key) {
    if (rl_->confirm_delete) {   // "delete these lyrics for good?" -- only y confirms, anything else backs out
        if (key == 'y' || key == 'Y') { rl_->confirm_delete = false; rl_delete_version(); }
        else rl_->confirm_delete = false;
        return;
    }
    if (key == 27) { rl_close(); return; } // cancel entirely, nothing submitted
    if (key == kKeyCtrlN) { readlyrics_add_open_from_playing(); return; }   // CTRL+n: add a lyrics file by hand (plain letters go into the text fields)

    std::vector<RLField> fields = rl_->rl_visible_fields();
    auto cur_pos = std::find(fields.begin(), fields.end(), rl_->focus);
    int idx = (cur_pos != fields.end()) ? static_cast<int>(cur_pos - fields.begin()) : 0;

    const int nver = static_cast<int>(rl_->versions.size());
    if (rl_->focus == RLField::Source) {   // the version list: up / down walk the rows, SPACE picks one, d deletes one
        if (key == 'B' && last_key_was_arrow() && rl_->src_sel < nver - 1) { ++rl_->src_sel; rl_->rl_fix_scroll(); return; }
        if (key == 'A' && last_key_was_arrow() && rl_->src_sel > 0) { --rl_->src_sel; rl_->rl_fix_scroll(); return; }
        if (key == ' ') { rl_choose_version(); return; }
        if (key == 'd' && !last_key_was_arrow()) {
            if (rl_->src_sel < 0 || rl_->src_sel >= nver) return;
            if (rl_->versions[static_cast<size_t>(rl_->src_sel)].kind == LyricsVersion::Kind::Embedded) {
                status_line_ = "lyrics embedded in the file cannot be deleted here";
                return;
            }
            rl_->confirm_delete = true;
            return;
        }
    }
    if (key == 9 || (key == 'B' && last_key_was_arrow())) { // Tab / down -- next field (a typed capital B goes into the text)
        idx = (idx + 1) % static_cast<int>(fields.size());
        rl_->focus = fields[idx];
        if (rl_->focus == RLField::Source) { rl_->src_sel = 0; rl_->rl_fix_scroll(); }
        return;
    }
    if (key == 'A' && last_key_was_arrow()) { // up -- previous field
        idx = (idx - 1 + static_cast<int>(fields.size())) % static_cast<int>(fields.size());
        rl_->focus = fields[idx];
        if (rl_->focus == RLField::Source) { rl_->src_sel = std::max(0, nver - 1); rl_->rl_fix_scroll(); }   // arrived from below: the last row
        return;
    }
    if (key == '\r' || key == '\n') { rl_submit(); return; } // [ENTER] Refetch, works from any field (the old copy is kept)
    if (rl_->focus == RLField::Source) return;                // nothing to type into the list

    if (bool* b = rl_->rl_bool_ptr(rl_->focus)) {
        if (key == ' ') { // toggle -- checkboxes are the only thing Space does anything to
            bool new_val = !*b;
            // Enforce the radio group: turning one of
            // slowed/ultra-slowed/spedup on clears the other two.
            // Reverb/remix/other stay independent of this and of
            // each other.
            if (rl_->focus == RLField::TypeSlowed || rl_->focus == RLField::TypeUltraSlowed || rl_->focus == RLField::TypeSpedup) {
                rl_->slowed = rl_->ultra_slowed = rl_->spedup = false;
            }
            *b = new_val;
            // Turning RemixText/OtherText off (unchecking) drops
            // them from rl_->rl_visible_fields() -- if focus was sitting
            // on one of those now-hidden text fields, land back on
            // the checkbox that just hid it instead of a field that
            // no longer exists in the nav order.
            return;
        }
        return; // typing/backspace do nothing on a checkbox field
    }

    if (std::string* t = rl_->rl_text_ptr(rl_->focus)) {
        // Caret, SHIFT+arrows marking, HOME / END, DEL, CTRL+C / X / V as in every other text box.
        edit_focus(rl_owner(rl_->focus), *t);
        edit_text_key(*t, edit_caret_, edit_anchor_, key, 200, &status_line_);
        return;
    }
    return;
}

void App::handle_search_key(int key) {
    if (key == 27) {
        // Cancel: put the view back exactly as it was before '/' was
        // pressed, discarding whatever the live preview below was
        // showing.
        list_source_ = pre_search_list_source_;
        last_local_query_ = pre_search_local_query_;
        refresh_local_view();
        mode_ = Mode::Browse;
        return;
    }
    if (cli_in_search()) {   // "/c:" -- the search bar is the command line: ENTER runs the line, UP / DOWN recall, ESC (above) leaves
        if (key == '\r' || key == '\n') {
            std::string b = search_.buffer;
            const size_t c = b.find(':');
            cli_run(b.substr(c + 1));
            search_.buffer = "c:";
            edit_caret_ = edit_anchor_ = search_.buffer.size();
            return;
        }
        if (last_key_was_arrow() && (key == 'A' || key == 'B')) {
            if (!cli_hist_.empty()) {
                const int n = static_cast<int>(cli_hist_.size());
                cli_hist_pos_ = key == 'A' ? (cli_hist_pos_ < 0 ? n - 1 : std::max(0, cli_hist_pos_ - 1))
                                           : (cli_hist_pos_ < 0 ? -1 : cli_hist_pos_ + 1 >= n ? -1 : cli_hist_pos_ + 1);
                search_.buffer = "c:" + (cli_hist_pos_ < 0 ? std::string() : cli_hist_[static_cast<size_t>(cli_hist_pos_)]);
                edit_caret_ = edit_anchor_ = search_.buffer.size();
            }
            return;
        }
        if (key == kKeyCtrlRight) {   // complete the command word / the title (CTRL+RIGHT, not TAB)
            const std::string done = cli_complete(search_.buffer.substr(search_.buffer.find(':') + 1));
            if (!done.empty()) { search_.buffer = "c:" + done; edit_caret_ = edit_anchor_ = search_.buffer.size(); force_redraw_ = true; return; }
            force_redraw_ = true;
            return;   // (candidates or a "no match" note are in the bar's border; CTRL+RIGHT is not a word jump here)
        }
        edit_focus("browse-search", search_.buffer);
        if (edit_text_key(search_.buffer, edit_caret_, edit_anchor_, key, 120, &status_line_)) { if (!cli_in_search()) update_live_search_preview(); cli_hist_pos_ = -1; }
        return;
    }
    if (cs_in_search() && (key == '\r' || key == '\n')) {   // "/cs:words" -- ENTER opens the script overlay with that search
        const std::string q = cli::trim(search_.buffer.substr(search_.buffer.find(':') + 1));
        list_source_ = pre_search_list_source_;
        last_local_query_ = pre_search_local_query_;
        refresh_local_view();
        cliov_open("", q);
        return;
    }
    if (key == '\r' || key == '\n') {
        // Moved through the results with Up/Down while typing: ENTER plays the highlighted entry right away (the
        // search is kept as the list filter, the list keeps the focus and the highlight). Without that it only
        // commits the search, as before.
        const bool picked = search_.nav_moved;
        const int sel = selected_, scr = scroll_;
        const ListSource src = list_source_;
        submit_search();
        if (mode_ == Mode::Search) mode_ = Mode::Browse;   // ("/yv:" opened SEARCH YT: that stays)
        const size_t len = (list_source_ == ListSource::Local) ? local_view_.size()
                         : (list_source_ == ListSource::Folder) ? folder_view_.size()
                         : (list_source_ == ListSource::Playlist) ? playlist_->view.size() : 0;
        if (picked && list_source_ == src && sel >= 0 && static_cast<size_t>(sel) < len) {
            selected_ = sel;
            scroll_ = std::clamp(scr, std::max(0, sel - list_nav_rows() + 1), sel);
            qst_.focus = false;
            play_selected();
        }
        search_.nav_moved = false;
        return;
    }
    // Up/Down navigate the live preview below the box -- claimed before
    // the caret keys because they are exactly the two arrow letters
    // edit_text_key() would otherwise drop ("nowhere to go vertically"),
    // and navigating the list you are filtering is worth more here than
    // a second way to do nothing. Left/Right are the caret keys in this box (they do not seek), since a
    // text field needs them more and seek still works from Browse.
    if (last_key_was_arrow() && (key == 'A' || key == 'B')) { // up / down
        search_.nav_moved = true;
        // SHIFT+Up/Down: whole page at a time while the big list overlay is open.
        if (list_overlay_active() && last_key_was_shifted()) { list_overlay_page(key == 'A' ? -1 : 1); return; }
        if (key == 'A') {
            if (selected_ > 0) --selected_;
            if (selected_ < scroll_) scroll_ = selected_;
            return;
        }
        // While actively typing "p:...", the live preview below is
        // already showing playlist_->view (see update_live_search_preview())
        // -- navigate that instead of local_view_ in that case, same
        // as Enter/submit_search() would commit to.
        size_t list_len = (list_source_ == ListSource::Playlist) ? playlist_->view.size()
                        : (list_source_ == ListSource::Folder) ? folder_view_.size()
                        : local_view_.size();
        if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
        if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
        return;
    }
    edit_focus("browse-search", search_.buffer);
    const bool search_changed = edit_text_key(search_.buffer, edit_caret_, edit_anchor_, key, 120, &status_line_);
    if (search_changed) { update_live_search_preview(); search_.nav_moved = false; }
    return;
}

} // namespace muisc
