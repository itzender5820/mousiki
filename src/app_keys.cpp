// app_keys.cpp
// Part of class App (src/app.h): The keyboard: App::handle_key for the main screen.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
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


void App::handle_key(int key) {
    if (key == 0) return;

    // Audio book: the note editor owns every key while it is open (the audio goes on).
    if (audiobook_ && book_note_edit_ && mode_ == Mode::Browse) {
        if (key == kKeyCtrlN && !book_note_discard_ask_) { book_note_to_overlay(); return; }   // CTRL+n: on in the big notes overlay
        book_note_key(key);
        return;
    }

    // While the pending edits are written to the files nothing else may happen; ESC stops after the current file.
    if (meta_save_.active) {
        if (key == 27) meta_save_.cancel = true;
        return;
    }

    // A meta-editor confirmation -- Shift+B's AcoustID disclaimer (raised
    // here in Browse) or Ctrl+Shift+S/D raised from the menu -- swallows every
    // key until it's answered, so nothing underneath it can be triggered by
    // accident. Checked before the mode dispatch on purpose: the prompt can
    // outlive the key that raised it in either of two modes.
    if (handle_meta_prompt_key(key)) return;

    // The help wizard opens from every menu and overlay that is not taking typed text (the main screen has its own
    // route further down); closing it brings the menu back.
    if (wizard::kEnabled && mode_ != Mode::Wizard && mode_ != Mode::Browse && key_is("HKeyWizard", key) && wizard_allowed_over_mode()) {
        wizard_open();
        return;
    }

    if (mode_ == Mode::ColorEdit || mode_ == Mode::Settings) {
        handle_settings_key(key);
        return;
    }

    if (mode_ == Mode::Console) {
        // ESC, or the console hotkey again, closes it. TAB changes the pane (logs / status messages), the arrow keys scroll it
        // (SHIFT: a page), HOME goes to the oldest line, END back to the newest. A read-only view otherwise.
        const bool arrow = last_key_was_arrow();
        if (!arrow && (key == 27 || key == 't' || key == 'T')) mode_ = Mode::Browse;
        else if (!arrow && key == 9) console_pane_ ^= 1;
        else if (!arrow && (key == 'v' || key == 'V')) { ConsoleLog::instance().toggle_level(); status_line_ = ConsoleLog::instance().level() == LogVerbosity::Verbose ? "verbose log: on (this session)" : "verbose log: off"; }
        else if (arrow && (key == 'A' || key == 'B')) {
            const int step = last_key_was_shifted() ? 8 : 1;
            console_up_[console_pane_] = std::max(0, console_up_[console_pane_] + (key == 'A' ? step : -step));   // (the upper end is clamped while drawing)
        } else if (key == kKeyHome) console_up_[console_pane_] = 1 << 20;
        else if (key == kKeyEnd) console_up_[console_pane_] = 0;
        return;
    }

    if (mode_ == Mode::Playlist) {
        handle_playlist_key(key);
        return;
    }

    if (mode_ == Mode::MetaEdit) {
        handle_meta_key(key);
        return;
    }
    if (mode_ == Mode::CoverFetch) {
        handle_cf_key(key);
        return;
    }

    if (mode_ == Mode::History) {
        handle_history_key(key);
        return;
    }

    if (mode_ == Mode::ReadLyrics) {
        handle_readlyrics_key(key);
        return;
    }

    if (mode_ == Mode::AudioDevice) { audio_device_key(key); return; }   // CHOOSE AUDIO DEVICE (src/app_audiodevice.cpp)

    if (mode_ == Mode::UpdateNotice) {
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

    if (mode_ == Mode::QueueToPlaylist) {
        const bool arrow = last_key_was_arrow();
        force_redraw_ = true;
        if (qtp_stage_ == 0) {   // New playlist | Existing playlist
            if (!arrow && key == 27) { mode_ = Mode::Browse; qtp_track_ = false; return; }
            if ((arrow && (key == 'C' || key == 'D')) || (!arrow && key == 9)) { qtp_choice_ ^= 1; return; }
            if (!arrow && (key == 'n' || key == 'N')) { qtp_choice_ = 0; key = '\r'; }
            else if (!arrow && (key == 'e' || key == 'E')) { qtp_choice_ = 1; key = '\r'; }
            if (!arrow && (key == '\r' || key == '\n')) {
                if (qtp_choice_ == 0 && qtp_track_) { qtp_stage_ = 2; qtp_new_name_.clear(); edit_owner_.clear(); }
                else if (qtp_choice_ == 0) { mode_ = Mode::Browse; queue_to_playlist_apply(""); }
                else { qtp_stage_ = 1; qtp_query_.clear(); qtp_selected_ = 0; edit_owner_.clear(); qtp_refresh(); }
            }
            return;
        }
        if (qtp_stage_ == 2) {   // SHIFT+a, new playlist: its name
            if (!arrow && key == 27) { qtp_stage_ = 0; return; }
            if (!arrow && (key == '\r' || key == '\n')) {
                std::string name = qtp_new_name_;
                while (!name.empty() && name.front() == ' ') name.erase(name.begin());
                while (!name.empty() && name.back() == ' ') name.pop_back();
                if (name.empty()) { status_line_ = "type a name first"; return; }
                track_to_playlist_apply(name);
                return;
            }
            std::string st;
            edit_focus("track-to-playlist-name", qtp_new_name_);
            edit_text_key(qtp_new_name_, edit_caret_, edit_anchor_, key, 80, &st);
            return;
        }
        // stage 1: the search field (typing filters) and the list of saved playlists
        if (!arrow && key == 27) { qtp_stage_ = 0; return; }
        const int total = static_cast<int>(qtp_view_.size());
        if (arrow && key == 'A') { if (qtp_selected_ > 0) --qtp_selected_; return; }
        if (arrow && key == 'B') { if (qtp_selected_ < total - 1) ++qtp_selected_; return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            if (total > 0) {
                const std::string name = qtp_view_[static_cast<size_t>(std::clamp(qtp_selected_, 0, total - 1))].name;
                if (qtp_track_) track_to_playlist_apply(name);
                else { mode_ = Mode::Browse; queue_to_playlist_apply(name); }
            }
            return;
        }
        std::string st;
        edit_focus("queue-to-playlist-search", qtp_query_);
        if (edit_text_key(qtp_query_, edit_caret_, edit_anchor_, key, 80, &st)) qtp_refresh();
        return;
    }

    if (mode_ == Mode::ClearQueue) {
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

    if (mode_ == Mode::SpectroMenu) {
        // Spectrogram options (SHIFT+i). Like the oscilloscope overlay: playback keeps running, only these keys work.
        const bool arrow = last_key_was_arrow();
        // rows kSrView .. the last (no "Display" row: what the lyrics area shows is the "." key's job)
        if (arrow && (key == 'A' || key == 'B')) {   // only the rows of the view in use (the waterfall has others than the spectrogram)
            const std::vector<int> vis = spectro_visible_rows(settings_.spectro);
            int at = 0;
            for (size_t i = 0; i < vis.size(); ++i) if (vis[i] <= spectro_menu_row_) at = static_cast<int>(i);
            const int n = static_cast<int>(vis.size());
            at = key == 'A' ? (at + n - 1) % n : (at + 1) % n;
            spectro_menu_row_ = vis[static_cast<size_t>(at)];
            return;
        }
        if (arrow && key == 'D') { spectro_menu_adjust(-1); return; }
        if (arrow && key == 'C') { spectro_menu_adjust(+1); return; }
        if (!arrow && (key == 'r' || key == 'R')) { spectro_reset(settings_.spectro); settings_.spectro_sync(); return; }
        std::string act = resolve_hotkey_action(key);
        if (act.empty() && key >= 'a' && key <= 'z') act = resolve_hotkey_action(key - 32);
        if (act.empty() && key >= 'A' && key <= 'Z') act = resolve_hotkey_action(key + 32);
        if (!arrow && act == "HKeySpectroFull") {
            spectro_full_ = !spectro_full_; if (spectro_full_) scope_big_ = false; force_redraw_ = true; return;
        }
        if (!arrow && act == "HKeyScopeWindow") { scope_window_toggle(); return; }     // the windows: always
        if (!arrow && act == "HKeySpectroWindow") { spectro_window_toggle(); return; }
        // the oscilloscope's overlays take over without closing this one first
        if (!arrow && act == "HKeyOscMenu") { save_settings(settings_); spectro_full_ = false; osci_menu_row_ = 0; mode_ = Mode::OsciMenu; force_redraw_ = true; return; }
        if (!arrow && act == "HKeyScopeBig") {
            save_settings(settings_);
            scope_big_ = true; spectro_full_ = false; scope_big_color_edit_ = false; scope_metrics_open_ = false;
            osci_menu_row_ = 0; mode_ = Mode::OsciMenu; force_redraw_ = true; return;
        }
        if (!arrow && (key == 27 || act == "HKeySpectroMenu")) {
            mode_ = Mode::Browse;
            save_settings(settings_);
        }
        return;
    }

    if (creator_show_ && mode_ == Mode::Browse) { creator_preview_key(key); return; }   // the creator's preview: ESC stops it

    if (spectro_full_ && mode_ == Mode::Browse && key == 27 && !last_key_was_arrow()) {   // ESC leaves the full screen
        spectro_full_ = false;
        force_redraw_ = true;
        return;
    }

    if (scope_big_ && mode_ == Mode::Browse && scope_big_key(key)) return;   // SHIFT+0 overlay: ESC, SHIFT+a, SHIFT+c, the colour prompt

    if (mode_ == Mode::OsciMenu) {
        // Oscilloscope tuning. Playback and the scope keep running underneath;
        // only this overlay takes keys. Unknown keys are ignored (the footer
        // lists every key), ESC or the overlay hotkey again closes and saves.
        const bool arrow = last_key_was_arrow();
        const int osci_n = static_cast<int>(osci_visible_rows(settings_).size());
        if (arrow && key == 'A') { osci_menu_row_ = (osci_menu_row_ + osci_n - 1) % osci_n; return; } // up
        if (arrow && key == 'B') { osci_menu_row_ = (osci_menu_row_ + 1) % osci_n; return; }          // down
        if (arrow && key == 'D') { osci_menu_adjust(-1); return; }                      // left
        if (arrow && key == 'C') { osci_menu_adjust(+1); return; }                      // right
        if (!arrow && (key == 'r' || key == 'R')) {
            osci_reset(settings_);
            return;
        }
        std::string osc_action = resolve_hotkey_action(key);
        if (osc_action.empty() && key >= 'a' && key <= 'z') osc_action = resolve_hotkey_action(key - 32);
        if (osc_action.empty() && key >= 'A' && key <= 'Z') osc_action = resolve_hotkey_action(key + 32);
        if (!arrow && osc_action == "HKeyScopeWindow") { scope_window_toggle(); return; }     // the windows: always
        if (!arrow && osc_action == "HKeySpectroWindow") { spectro_window_toggle(); return; }
        if (!arrow && osc_action == "HKeyScopeBig") { scope_big_ = !scope_big_; spectro_full_ = false; scope_big_color_edit_ = false; scope_metrics_open_ = false; force_redraw_ = true; return; }
        // the spectrogram's overlays take over without closing this one first
        if (!arrow && osc_action == "HKeySpectroMenu") { save_settings(settings_); scope_big_ = false; spectro_menu_row_ = kSrView; mode_ = Mode::SpectroMenu; force_redraw_ = true; return; }
        if (!arrow && osc_action == "HKeySpectroFull") {
            save_settings(settings_);
            spectro_full_ = true; scope_big_ = false;
            spectro_menu_row_ = kSrView; mode_ = Mode::SpectroMenu; force_redraw_ = true; return;
        }
        if (!arrow && (key == 27 || osc_action == "HKeyOscMenu")) {
            mode_ = Mode::Browse;
            save_settings(settings_);
        }
        return;
    }

    if (mode_ == Mode::NormMenu) {
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

    if (mode_ == Mode::LyricsEdit) {
        // Lyrics timing. Playback keeps running underneath; only this overlay
        // takes keys. Unknown keys are ignored (the footer lists every key).
        const bool arrow = last_key_was_arrow();
        if (arrow && key == 'C') { lyrics_edit_adjust(+0.1); return; } // right: lyrics later
        if (arrow && key == 'D') { lyrics_edit_adjust(-0.1); return; } // left: lyrics earlier
        if (arrow && key == 'A') { lyrics_edit_adjust(+0.5); return; } // up
        if (arrow && key == 'B') { lyrics_edit_adjust(-0.5); return; } // down
        // SHIFT+Left/Right seek the running track (5 s, like the normal seek keys) so a line can be heard again
        // while it is being re-timed.
        if (key == kKeyShiftLeft)  { if (has_track_) player_.seek_relative(-5.0); return; }
        if (key == kKeyShiftRight) { if (has_track_) player_.seek_relative(5.0);  return; }
        if (arrow) return;
        if (key == 'r' || key == 'R') {
            std::lock_guard<std::mutex> lk(lyrics_mutex_);
            lyrics_result_.delay = 0.0;
            return;
        }
        if (key == '\r' || key == '\n' || key == 's' || key == 'S') {
            double d = 0.0;
            { std::lock_guard<std::mutex> lk(lyrics_mutex_); d = lyrics_result_.delay; }
            char num[24];
            std::snprintf(num, sizeof num, "%+.1f s", d);
            if (lyrics_edit_from_reader_) rd_delay_ = d;   // (the reader's lines keep the new offset)
            if (save_lyrics_delay(lyrics_path_, d)) status_line_ = std::string("lyrics timing saved: ") + num;
            else status_line_ = std::string("lyrics timing ") + num + " applied, but could not be saved (no lyrics file for this track)";
            lyrics_edit_close();
            return;
        }
        if (key == 27 || key_is("HKeyLyricsTiming", key)) { // cancel: the value from before the overlay opened comes back
            { std::lock_guard<std::mutex> lk(lyrics_mutex_); lyrics_result_.delay = lyrics_edit_orig_delay_; }
            lyrics_edit_close();
        }
        return;
    }

    if (mode_ == Mode::BookOverlay) { book_overlay_key(key); return; }
    if (mode_ == Mode::Creator) { creator_key(key); return; }
    if (mode_ == Mode::MenuIndex) { menu_index_key(key); return; }

    if (mode_ == Mode::SleepTimer) {
        // Sleep timer. Playback keeps running underneath; only this overlay takes
        // keys. Unknown keys are ignored (the footer lists every key).
        const int rows = 8; // 15 / 30 / 60 / 90 / 120 min, stop after song, fade out, off
        const bool arrow = last_key_was_arrow();
        if (arrow && key == 'A') { sleep_menu_row_ = (sleep_menu_row_ + rows - 1) % rows; return; } // up
        if (arrow && key == 'B') { sleep_menu_row_ = (sleep_menu_row_ + 1) % rows; return; }        // down
        if (!arrow && (key == '\r' || key == '\n') && sleep_menu_row_ == 6) {   // Fade out: a toggle, the overlay stays open
            settings_.sleep_fade = !settings_.sleep_fade;
            if (!settings_.sleep_fade) player_.set_fade(1.0f);
            save_settings(settings_);
            return;
        }
        if (!arrow && (key == '\r' || key == '\n')) {
            sleep_timer_apply(sleep_menu_row_);
            mode_ = Mode::Browse;
            return;
        }
        std::string sl_action = resolve_hotkey_action(key);
        if (sl_action.empty() && key >= 'a' && key <= 'z') sl_action = resolve_hotkey_action(key - 32);
        if (sl_action.empty() && key >= 'A' && key <= 'Z') sl_action = resolve_hotkey_action(key + 32);
        if (!arrow && (key == 27 || sl_action == "HKeySleepTimer")) mode_ = Mode::Browse;
        return;
    }

    if (mode_ == Mode::Equalizer) {
        // Equaliser. Playback keeps running underneath and the sound follows
        // every change immediately; only this overlay takes keys. Unknown keys
        // are ignored (the footer lists every key). ESC or the overlay hotkey
        // closes and saves.
        const bool arrow = last_key_was_arrow();

        // The "Save as:" prompt owns every key until ENTER or ESC: typing,
        // caret, marking and clipboard come from the shared single-line editor.
        if (eq_naming_) {
            if (!arrow && key == 27) { // cancel the prompt only, the overlay stays open
                eq_naming_ = false;
                eq_name_buf_.clear();
                eq_status_ = "Save cancelled";
                return;
            }
            if (!arrow && (key == '\r' || key == '\n')) { eq_commit_name(); return; }
            edit_focus("eq-name", eq_name_buf_);
            edit_text_key(eq_name_buf_, edit_caret_, edit_anchor_, key, kEqNameMaxBytes, nullptr);
            return;
        }

        const bool delete_was_armed = eq_delete_armed_; // a second DEL / X right after the first confirms
        eq_delete_armed_ = false;
        eq_status_.clear();

        if (arrow && key == 'C') { eq_band_ = (eq_band_ + 1) % kEqBands; return; }               // right
        if (arrow && key == 'D') { eq_band_ = (eq_band_ + kEqBands - 1) % kEqBands; return; }    // left
        if (arrow && key == 'A') { eq_set_gain(eq_band_, settings_.eq_gains[eq_band_] + 1.0f); return; } // up
        if (arrow && key == 'B') { eq_set_gain(eq_band_, settings_.eq_gains[eq_band_] - 1.0f); return; } // down
        if (!arrow && (key == ',' || key == '<')) { eq_select_preset(-1); return; }
        if (!arrow && (key == '.' || key == '>' || key == 9)) { eq_select_preset(+1); return; }
        if (!arrow && key == '0') { eq_set_gain(eq_band_, 0.0f); return; }
        if (!arrow && key == ' ') {
            settings_.eq_enabled = !settings_.eq_enabled;
            eq_apply();
            return;
        }
        if (!arrow && (key == 'r' || key == 'R')) { // back to Flat; the on/off state is left alone
            settings_.eq_gains = kEqPresets[0].gains;
            eq_last_preset_ = 0;
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

    if (mode_ == Mode::Karaoke) {
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
            "HKeyIncreaseVolume", "HKeyDecreaseVolume", "HKeyToggleMute", "HKeyCyclePlayMode", "HKeyToggleNormalize",
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

    if (mode_ == Mode::Cheatsheet) {
        const bool arrow = last_key_was_arrow();
        if (cheat_searching_) {   // SHIFT+s: the search field has the keyboard
            const int n = static_cast<int>(cheat_result_rows_.size());
            if (!arrow && key == 27) {   // ESC clears the query first, then leaves the search
                if (!cheat_query_.empty()) { cheat_query_.clear(); cheat_sel_ = 0; cheat_first_ = 0; edit_owner_.clear(); }
                else cheat_searching_ = false;
            } else if (arrow && (key == 'A' || key == 'B')) {
                const int step = last_key_was_shifted() ? 5 : 1;
                cheat_sel_ = std::clamp(cheat_sel_ + (key == 'B' ? step : -step), 0, std::max(0, n - 1));
            } else if (!arrow && (key == '\r' || key == '\n')) {
                if (n > 0) cheat_jump_row_ = cheat_result_rows_[static_cast<size_t>(std::clamp(cheat_sel_, 0, n - 1))];
                cheat_searching_ = false;
            } else {
                edit_focus("cheat-search", cheat_query_);
                if (edit_text_key(cheat_query_, edit_caret_, edit_anchor_, key, 80, nullptr)) { cheat_sel_ = 0; cheat_first_ = 0; }
            }
            return;
        }
        if (!arrow && key_is("HKeySearch", key)) { cheat_searching_ = true; cheat_query_.clear(); cheat_sel_ = 0; cheat_first_ = 0; edit_owner_.clear(); }
        else if (key == 27 || key_is("HKeyCheatsheet", key)) mode_ = Mode::Browse;
        else if (key == 'A') cheatsheet_scroll_ -= (last_key_was_shifted() ? 10 : 1); // up (SHIFT / PgUp: a page) -- the table is longer than the screen
        else if (key == 'B') cheatsheet_scroll_ += (last_key_was_shifted() ? 10 : 1); // down (clamped while rendering)
        return;
    }

    if (mode_ == Mode::Wizard) {
        // The wizard keeps its own page / part / contents state (src/wizard.cpp); here the key only has to be named.
        // Nothing reaches the player underneath: the wizard is a full-screen view like the cheat sheet, and playback
        // simply keeps running.
        const bool arrow = last_key_was_arrow();
        if (wizard_st_.searching) { wizard::search_key(wizard_st_, key, arrow, arrow && last_key_was_shifted()); return; }   // SHIFT+s: the query has the keyboard
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
        if (wizard::handle_key(wizard_st_, wk)) wizard_close();
        return;
    }

    if (mode_ == Mode::BulkAdd) {
        if (key == 27) { // cancel entirely -- discard buffer, results, and selection state
            mode_ = Mode::Browse;
            status_line_.clear();
            bulk_add_buffer_.clear();
            bulk_add_results_ready_ = false;
            pending_bulk_add_ = BulkAddResult{};
            bulk_add_selected_.clear();
            return;
        }

        if (bulk_add_results_ready_) {
            // --- Phase 2: navigate/star the fetched checklist. ---
            int total = static_cast<int>(pending_bulk_add_.items.size());
            if (key == 'A' && total > 0) { // up
                bulk_add_cursor_ = std::max(0, bulk_add_cursor_ - 1);
                if (bulk_add_cursor_ < bulk_add_scroll_) bulk_add_scroll_ = bulk_add_cursor_;
                return;
            }
            if (key == 'B' && total > 0) { // down
                bulk_add_cursor_ = std::min(total - 1, bulk_add_cursor_ + 1);
                if (bulk_add_cursor_ >= bulk_add_scroll_ + kBulkAddVisibleRows) bulk_add_scroll_ = bulk_add_cursor_ - kBulkAddVisibleRows + 1;
                return;
            }
            if (key == ' ' && total > 0) { // toggle star on the hovered row
                if (bulk_add_cursor_ < static_cast<int>(bulk_add_selected_.size())) {
                    bulk_add_selected_[bulk_add_cursor_] = !bulk_add_selected_[bulk_add_cursor_];
                }
                return;
            }
            if (key == 'a') { commit_bulk_add(/*all=*/true); return; }        // "ALL"
            if (key == '\r' || key == '\n') { commit_bulk_add(/*all=*/false); return; } // "[SELECT]"
            return;
        }

        // --- Phase 1: typing the link. ---
        if (key == '\r' || key == '\n') {
            if (!bulk_add_buffer_.empty() && !bulk_add_in_progress_.load()) {
                status_line_ = "fetching playlist ...";
                launch_bulk_add_async(bulk_add_buffer_);
            }
            return; // stays open -- poll_pending_bulk_add() moves to phase 2 once the fetch resolves
        }
        // The shared single-line editor: caret, SHIFT+arrows marking, HOME / END, DEL, CTRL+C / X / V. It also tells
        // a typed A-D from an arrow key (arrows collapse to those letters).
        edit_focus("bulk-add", bulk_add_buffer_);
        edit_text_key(bulk_add_buffer_, edit_caret_, edit_anchor_, key, 200, &status_line_);
        return;
    }

    if (mode_ == Mode::RetryLyrics) {
        if (rl_confirm_delete_) {   // "delete these lyrics for good?" -- only y confirms, anything else backs out
            if (key == 'y' || key == 'Y') { rl_confirm_delete_ = false; rl_delete_version(); }
            else rl_confirm_delete_ = false;
            return;
        }
        if (key == 27) { rl_close(); return; } // cancel entirely, nothing submitted
        if (key == kKeyCtrlN) { readlyrics_add_open_from_playing(); return; }   // CTRL+n: add a lyrics file by hand (plain letters go into the text fields)

        std::vector<RLField> fields = rl_visible_fields();
        auto cur_pos = std::find(fields.begin(), fields.end(), rl_focus_);
        int idx = (cur_pos != fields.end()) ? static_cast<int>(cur_pos - fields.begin()) : 0;

        const int nver = static_cast<int>(rl_versions_.size());
        if (rl_focus_ == RLField::Source) {   // the version list: up / down walk the rows, SPACE picks one, d deletes one
            if (key == 'B' && last_key_was_arrow() && rl_src_sel_ < nver - 1) { ++rl_src_sel_; rl_fix_scroll(); return; }
            if (key == 'A' && last_key_was_arrow() && rl_src_sel_ > 0) { --rl_src_sel_; rl_fix_scroll(); return; }
            if (key == ' ') { rl_choose_version(); return; }
            if (key == 'd' && !last_key_was_arrow()) {
                if (rl_src_sel_ < 0 || rl_src_sel_ >= nver) return;
                if (rl_versions_[static_cast<size_t>(rl_src_sel_)].kind == LyricsVersion::Kind::Embedded) {
                    status_line_ = "lyrics embedded in the file cannot be deleted here";
                    return;
                }
                rl_confirm_delete_ = true;
                return;
            }
        }
        if (key == 9 || (key == 'B' && last_key_was_arrow())) { // Tab / down -- next field (a typed capital B goes into the text)
            idx = (idx + 1) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            if (rl_focus_ == RLField::Source) { rl_src_sel_ = 0; rl_fix_scroll(); }
            return;
        }
        if (key == 'A' && last_key_was_arrow()) { // up -- previous field
            idx = (idx - 1 + static_cast<int>(fields.size())) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            if (rl_focus_ == RLField::Source) { rl_src_sel_ = std::max(0, nver - 1); rl_fix_scroll(); }   // arrived from below: the last row
            return;
        }
        if (key == '\r' || key == '\n') { rl_submit(); return; } // [ENTER] Refetch, works from any field (the old copy is kept)
        if (rl_focus_ == RLField::Source) return;                // nothing to type into the list

        if (bool* b = rl_bool_ptr(rl_focus_)) {
            if (key == ' ') { // toggle -- checkboxes are the only thing Space does anything to
                bool new_val = !*b;
                // Enforce the radio group: turning one of
                // slowed/ultra-slowed/spedup on clears the other two.
                // Reverb/remix/other stay independent of this and of
                // each other.
                if (rl_focus_ == RLField::TypeSlowed || rl_focus_ == RLField::TypeUltraSlowed || rl_focus_ == RLField::TypeSpedup) {
                    rl_slowed_ = rl_ultra_slowed_ = rl_spedup_ = false;
                }
                *b = new_val;
                // Turning RemixText/OtherText off (unchecking) drops
                // them from rl_visible_fields() -- if focus was sitting
                // on one of those now-hidden text fields, land back on
                // the checkbox that just hid it instead of a field that
                // no longer exists in the nav order.
                return;
            }
            return; // typing/backspace do nothing on a checkbox field
        }

        if (std::string* t = rl_text_ptr(rl_focus_)) {
            // Caret, SHIFT+arrows marking, HOME / END, DEL, CTRL+C / X / V as in every other text box.
            edit_focus(rl_owner(rl_focus_), *t);
            edit_text_key(*t, edit_caret_, edit_anchor_, key, 200, &status_line_);
            return;
        }
        return;
    }

    if (mode_ == Mode::Search) {
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
        if (key == '\r' || key == '\n') {
            // Moved through the results with Up/Down while typing: ENTER plays the highlighted entry right away (the
            // search is kept as the list filter, the list keeps the focus and the highlight). Without that it only
            // commits the search, as before.
            const bool picked = search_nav_moved_;
            const int sel = selected_, scr = scroll_;
            const ListSource src = list_source_;
            submit_search();
            mode_ = Mode::Browse;
            const size_t len = (list_source_ == ListSource::Local) ? local_view_.size()
                             : (list_source_ == ListSource::Folder) ? folder_view_.size()
                             : (list_source_ == ListSource::Playlist) ? playlist_view_.size() : 0;
            if (picked && list_source_ == src && sel >= 0 && static_cast<size_t>(sel) < len) {
                selected_ = sel;
                scroll_ = std::clamp(scr, std::max(0, sel - list_nav_rows() + 1), sel);
                queue_focus_ = false;
                play_selected();
            }
            search_nav_moved_ = false;
            return;
        }
        // Up/Down navigate the live preview below the box -- claimed before
        // the caret keys because they are exactly the two arrow letters
        // edit_text_key() would otherwise drop ("nowhere to go vertically"),
        // and navigating the list you are filtering is worth more here than
        // a second way to do nothing. Left/Right are the caret keys in this box (they do not seek), since a
        // text field needs them more and seek still works from Browse.
        if (last_key_was_arrow() && (key == 'A' || key == 'B')) { // up / down
            search_nav_moved_ = true;
            // SHIFT+Up/Down: whole page at a time while the big list overlay is open.
            if (list_overlay_active() && last_key_was_shifted()) { list_overlay_page(key == 'A' ? -1 : 1); return; }
            if (key == 'A') {
                if (selected_ > 0) --selected_;
                if (selected_ < scroll_) scroll_ = selected_;
                return;
            }
            // While actively typing "p:...", the live preview below is
            // already showing playlist_view_ (see update_live_search_preview())
            // -- navigate that instead of local_view_ in that case, same
            // as Enter/submit_search() would commit to.
            size_t list_len = (list_source_ == ListSource::Playlist) ? playlist_view_.size()
                            : (list_source_ == ListSource::Folder) ? folder_view_.size()
                            : local_view_.size();
            if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
            if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
            return;
        }
        edit_focus("browse-search", search_buffer_);
        const bool search_changed = edit_text_key(search_buffer_, edit_caret_, edit_anchor_, key, 120, &status_line_);
        if (search_changed) { update_live_search_preview(); search_nav_moved_ = false; }
        return;
    }

    // Mode::Browse
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size()
                     : (list_source_ == ListSource::Online) ? online_view_.size()
                     : (list_source_ == ListSource::Folder) ? folder_view_.size()
                     : playlist_view_.size();

    // Big list overlay (SHIFT+l): SHIFT+Up/Down turn a whole page, ESC closes
    // it. Matched on the raw arrow codes + the shift flag, like SHIFT+b below
    // -- the input layer reports Shift+Up/Down as a plain 'A'/'B' arrow with
    // last_key_was_shifted() set, so there is no hotkey string for them. With
    // the overlay closed both keys fall through and behave exactly like plain
    // Up/Down, as they always did.
    // Big folder overlay (SHIFT+f): takes the keys it needs (tree navigation, search, `f`), the playback keys fall through.
    if (folder_overlay_active() && folder_overlay_key(key)) return;
    if (convert_overlay_active() && convert_overlay_key(key)) return;
    if (list_overlay_active()) {
        if (last_key_was_arrow() && last_key_was_shifted() && (key == 'A' || key == 'B')) {
            list_overlay_page(key == 'A' ? -1 : 1);
            return;
        }
        if (key == 27) { list_overlay_close(); return; }
    }
    // Big queue overlay (SHIFT+k): same keys as the list overlay above.
    if (queue_overlay_active()) {
        if (queue_find_edit_) { queue_find_key(key); return; }   // the caret is in the search bar: every key is for it
        if (key_is("HKeySearch", key) && !last_key_was_arrow()) {   // "/": search the queue (the bar at the bottom)
            queue_find_edit_ = true;
            edit_focus("queue-find", queue_find_);
            text_engage("queue-find");
            return;
        }
        if (last_key_was_arrow() && last_key_was_shifted() && (key == 'A' || key == 'B')) {
            queue_overlay_page(key == 'A' ? -1 : 1);
            return;
        }
        if (!last_key_was_arrow() && key == 'S') { queue_shuffle_order(); return; }   // SHIFT+s: shuffle the order
        if (!last_key_was_arrow() && key == 'U') { queue_undo_shuffle(); return; }    // SHIFT+u: undo the shuffle
        if (key == 27) {
            if (!queue_find_.empty()) { queue_find_.clear(); return; }   // the first ESC takes the search away
            queue_overlay_close();
            return;
        }
    }

    // Ctrl+Shift+U / Ctrl+Shift+Z: queue -> playlist editor / undo the last
    // clear. Sentinel keys (see terminal_ui.h), so they are matched directly.

    // (SHIFT+b, the AcoustID lookup of the hovered title, is the HKeyFetchTags action below. A typed capital B and the
    // Down arrow are the same value; resolve_hotkey_action() tells them apart with last_key_was_arrow().)
    // Hotkeys are resolved to an action name via settings_.hotkeys /
    // resolve_hotkey_action() instead of switching on the raw key
    // directly, so a rebinding in Settings > Reference (or config.txt)
    // actually changes what a keypress does. This closes a gap that
    // exists in the *original* Linux/macOS codebase too, not something
    // the Windows port introduced: resolve_hotkey_action() was already
    // there, fully implemented, but nothing ever called it -- the
    // switch below was hardcoded on literal characters no matter what
    // config.txt or the Settings UI said. Two small normalizations keep
    // every default binding behaving exactly as it did before this
    // change: CR (13, what Windows' _getch() actually sends for Enter)
    // is treated as LF (10, what hotkey_string_to_key("ENTER") maps to)
    // so Enter keeps working regardless of which one a given terminal
    // reports; and a letter that doesn't resolve on its own also tries
    // its opposite case, so rebinding an action to "n" still fires on
    // Shift+N the same way the old hardcoded "case \'n\': case \'N\':"
    // pairs always did, without having to special-case every letter
    // action individually.
    // SHIFT and the + key (the character '*' on a German keyboard): switch to the radio mode. Fixed key. Only here, in the
    // Browse view -- text fields and menus handled their keys above.
    if (key_is("HKeyModeNext", key)) { request_mode_switch(1); return; }
    if (key_is("HKeyModePrev", key)) { request_mode_switch(-1); return; }
    if (key_is("HKeyCoverPrev", key)) { cycle_cover_look(-1); return; }   // CTRL+t / CTRL+z: the cover look back / forward
    if (key_is("HKeyCoverNext", key)) { cycle_cover_look(1); return; }
    int lookup_key = (key == '\r') ? '\n' : key;
    std::string action = resolve_hotkey_action(lookup_key);
    if (action.empty() && lookup_key >= 'a' && lookup_key <= 'z') action = resolve_hotkey_action(lookup_key - 32);
    if (action.empty() && lookup_key >= 'A' && lookup_key <= 'Z') action = resolve_hotkey_action(lookup_key + 32);
    if (audiobook_ && book_intercept(action, key)) return;   // the audio book player's own keys (and what it does not offer)

    if (action == "HKeyRescan") {   // SHIFT+r: new files copied / uploaded while the app runs
        rescan_now();
    } else if (action == "HKeyTranslit") {   // SHIFT+' : Japanese / Korean / Chinese text in Latin letters, on / off
        toggle_latin_letters();
    } else if (action == "HKeyFetchTags") {   // SHIFT+b: AcoustID lookup (audio fingerprint) of the hovered title
        std::string path;
        if (queue_focus_) {
            if (queue_selected_ >= 0 && queue_selected_ < static_cast<int>(queue_.size()) && queue_[queue_selected_].is_local) {
                path = path_utf8(queue_[queue_selected_].local_path);
            }
        } else if (list_source_ == ListSource::Local && !local_view_.empty() &&
                   selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
            path = path_utf8(local_view_[selected_].path);
        }
        if (path.empty()) {
            status_line_ = "fetch tags: only a local file can be fingerprinted";
        } else {
            meta_prompt_single_fetch(path); // asks the AcoustID disclaimer first
        }
    } else if (action == "HKeyLyricsTiming") {   // lyrics timing overlay
        lyrics_edit_open();
    } else if (action == "HKeyStyleOptions") {   // SHIFT+s: the Style Option overlay of the playing track
        meta_open_style_playing();
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
        spectro_menu_row_ = kSrView;
        mode_ = Mode::SpectroMenu;
    } else if (action == "HKeyScopeBig") { // SHIFT+0: the big oscilloscope overlay on / off
        scope_big_ = !scope_big_;
        spectro_full_ = false;
        scope_big_color_edit_ = false;
        scope_metrics_open_ = false;
        force_redraw_ = true;
        status_line_ = scope_big_ ? "oscilloscope: full screen (ESC or " + hotkey_text("HKeyScopeBig", "=") + " closes, " + hotkey_text("HKeyOscMenu", "O") + " options)" : "";
    } else if (action == "HKeySpectroFull") { // SHIFT+u: spectrogram full screen on / off
        spectro_full_ = !spectro_full_;
        if (spectro_full_) scope_big_ = false;
        force_redraw_ = true;
        status_line_ = spectro_full_ ? "spectrogram: full screen (ESC or " + hotkey_text("HKeySpectroFull", "U") + " closes, " + hotkey_text("HKeySpectroMenu", "I") + " options)" : "";
    } else if (action == "HKeyScopeWindow") { // SHIFT+9: the oscilloscope in its own window (always, even with it off on ANIMA/VIZ)
        scope_window_toggle();
    } else if (action == "HKeySpectroWindow") { // SHIFT+8: the spectrogram in its own window (always)
        spectro_window_toggle();
    } else if (action == "HKeyOscMenu") { // SHIFT+o: oscilloscope tuning overlay
        osci_menu_row_ = 0;
        mode_ = Mode::OsciMenu;
    } else if (action == "HKeyNormMenu") { // SHIFT+v: loudness normalization overlay
        norm_menu_row_ = 0;
        mode_ = Mode::NormMenu;
    } else if (action == "HKeySleepTimer") { // SHIFT+z: sleep timer overlay
        sleep_menu_row_ = sleep_stop_after_track_ ? 5
                        : sleep_timer_active_ ? (sleep_timer_minutes_ == 15 ? 0 : sleep_timer_minutes_ == 30 ? 1
                                               : sleep_timer_minutes_ == 60 ? 2 : sleep_timer_minutes_ == 90 ? 3 : 4)
                        : 0;
        mode_ = Mode::SleepTimer;
    } else if (action == "HKeyCreator") { // ^: creator overlay
        creator_status_.clear();
        creator_list_open_ = false;
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
        if (queue_overlay_open_) queue_overlay_close(); else queue_overlay_open();
    } else if (action == "HKeySwitchBetweenCards") { // Tab: toggle Up/Down + reorder focus between the list and the queue
        // The queue pane is hidden behind the big list overlay and the queue
        // always has focus inside the big queue overlay, so there is nothing
        // to switch to while either is up.
        if (!list_overlay_active() && !queue_overlay_active()) queue_focus_ = !queue_focus_;
    } else if (action == "HKeyNavigateUp") {   // SHIFT+Up / PgUp: a page
        const bool paged = last_key_was_arrow() && last_key_was_shifted();
        if (queue_focus_) {
            queue_selected_ = std::max(0, queue_selected_ - (paged ? 5 : 1));
            clamp_queue_selected();
        } else {
            selected_ = std::max(0, selected_ - (paged ? std::max(1, list_nav_rows() - 1) : 1));
            if (selected_ < scroll_) scroll_ = selected_;
        }
    } else if (action == "HKeyNavigateDown") {   // SHIFT+Down / PgDn: a page
        const bool paged = last_key_was_arrow() && last_key_was_shifted();
        if (queue_focus_) {
            if (!queue_.empty()) queue_selected_ = std::min(static_cast<int>(queue_.size()) - 1, queue_selected_ + (paged ? 5 : 1));
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
        if (queue_focus_) {
            status_line_ = "focus the track list (TAB) to add a track to the end of the queue";
        } else {
            queue_add_selected_end();
            log_event("added to end of queue");
        }
    } else if (action == "HKeyTogglePlayPause") {
        act_play_pause();
    } else if (action == "HKeyIncreaseVolume") {
        act_volume_change(+5);
    } else if (action == "HKeyDecreaseVolume") {
        act_volume_change(-5);
    } else if (action == "HKeyPlayNextSong") {
        act_next();
    } else if (action == "HKeyPlayPreviousSong") {
        if (queue_focus_ && !audiobook_) {
            // Queue pane in focus: only a LOCKED queue knows what was played (the played tracks sit at its end). The last item is
            // the track that plays now (if it came from the queue), the one before it is the previous one: step the queue back so
            // that one is the head again and play it -- the loop order stays as it was.
            if (!queue_locked_) {
                status_line_ = "the queue is unlocked: it does not remember played tracks (! locks it)";
            } else if (queue_.size() < 2) {
                status_line_ = "no previously played queue track";
            } else {
                const QueueItem& last = queue_.back();
                const bool last_is_current = has_track_ && last.is_local == current_is_local_ &&
                    (last.is_local ? last.local_path == current_path_ : last.video_id == current_video_id_);
                const size_t n = queue_.size();
                const size_t back = (last_is_current ? 2 : 1) % n;
                std::rotate(queue_.begin(), queue_.begin() + (n - back), queue_.end());
                for (QueueItem& q : queue_) q.played = false;
                queue_next_run_ = 0;
                queue_selected_ = 0;
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
        const int cur = settings_.element_lyrics ? 3 : settings_.lyric_viz == 1 ? 1 : settings_.lyric_viz == 2 ? 2 : 0;
        const int next = settings_.viz_step(cur, action == "HKeyToggleLyricsBack" ? -1 : 1, false);
        static const char* const view_names[4] = {"sphere", "oscilloscope", "spectrogram", "lyrics"};
        if (!settings_.element_vizfield) {
            status_line_ = "the Viz/Lyrics field is switched off (Settings > ANIMA/VIZ > Viz/Lyrics Field)";
        } else if (next == cur) {
            status_line_ = std::string("lyrics area: ") + view_names[cur] + " (the other views are switched off on Settings -> ANIMA/VIZ)";
        } else if (next == 3) {                    // -> lyrics
            settings_.element_lyrics = true;
            if (has_track_) {
                std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
                last_lyrics_status_.clear(); // fresh 1.75s caption window, not a leftover from before it was off
                launch_lyrics_fetch(metadata_.name, artist, current_path_);
            }
            status_line_ = "lyrics area: lyrics";
        } else {
            settings_.element_lyrics = false;
            settings_.lyric_viz = next;   // 0 sphere, 1 osci, 2 spectro
            status_line_ = std::string("lyrics area: ") + view_names[next];
        }
    } else if (action == "HKeyAddHoveringSongToQueue") {
        // Add hovering song to queue. Only for the library pane: with the Queue pane itself focused there is
        // nothing hovering in the list to add, so the key does nothing there (bulk add has its own key).
        if (!queue_focus_) {
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
    } else if (action == "HKeyWizard") {
        wizard_open();
    } else if (action == "HKeyCheatsheet") {
        mode_ = Mode::Cheatsheet;
        cheat_searching_ = false;
    } else if (action == "HKeyMenus") {   // "<": the list of all menus and overlays
        menu_index_sel_ = 0;
        mode_ = Mode::MenuIndex;
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
            rl_from_reader_ = rl_from_karaoke_ = false;
            mode_ = Mode::RetryLyrics;
        }
    } else if (action == "HKeyToggleWaveform") { // toggle waveform style (raw/smooth) directly, without going into Settings
        settings_.waveform_smooth = !settings_.waveform_smooth;
        recompute_waveform_for_current_track();
        log_event(settings_.waveform_smooth ? "waveform: smooth" : "waveform: raw");
    } else if (action == "HKeyDownloadStream") { // save cached stream to the configured download folder
        if (has_track_) {
            if (path_utf8(current_path_).find(".cache") != std::string::npos || metadata_.location == "youtube" ||
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
                filename += path_utf8(current_path_.extension());

                fs::path dest_path = path_from_utf8(dest_dir) / path_from_utf8(filename);
                if (fs::exists(dest_path, ec)) {
                    status_line_ = "already saved: " + path_utf8(dest_path.filename());
                } else {
                    fs::copy_file(current_path_, dest_path, fs::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        fs::remove(current_path_, ec);
                        current_path_ = dest_path; // update so sidecar lyrics go to the new folder
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
    } else if (action == "HKeyCycleSortMode") { // cycle local-list sort mode (folder order -> title A-Z -> artist A-Z -> recently added)
        local_sort_mode_ = (local_sort_mode_ + 1) % 4;
        if (local_sort_mode_ == 3) { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }   // fresh times each time the mode is chosen
        refresh_local_view();
        log_event(std::string("sort: ") + sort_mode_name(local_sort_mode_, settings_.meta_only));
    } else if (action == "HKeyPlay") {
        if (queue_focus_) { queue_next_run_ = 0; if (!queue_.empty()) play_queue_item(std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1), last_key_was_shifted()); }   // ENTER in the queue pane (small or big): play the highlighted item; SHIFT+ENTER also moves the queue along up to it
        else play_selected();
    } else if (action == "HKeySearch") {
        if (queue_overlay_open_) queue_overlay_close(); // the search filters the list underneath, which the queue overlay covers
        mode_ = Mode::Search;
        search_buffer_.clear();
        search_nav_moved_ = false;
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
    }
}

} // namespace muisc
