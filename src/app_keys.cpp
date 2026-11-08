// app_keys.cpp
// Part of class App (src/app.h): The keyboard: App::handle_key for the main screen.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "playlist_state.h"
#include "meta_state.h"
#include "book_state.h"
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


void App::handle_key(int key) {
    if (key == 0) return;

    // A paste by the terminal (bracketed paste) is one event, not hundreds of keys: with a text field in use it is handled like CTRL+v with
    // that text, otherwise it is dropped (typed as keys, it would have triggered a hotkey per letter).
    if (key == kKeyPaste) {
        const std::string text = take_paste_text();
        if (text.empty() || !text_entry_active()) return;
        set_paste_override(text);
        handle_key(kKeyCtrlV);
        set_paste_override(std::string());
        return;
    }

    // Audio book: the note editor owns every key while it is open (the audio goes on).
    if (audiobook_ && book_->note_edit && mode_ == Mode::Browse) {
        if (key == kKeyCtrlN && !book_->note_discard_ask) { book_note_to_overlay(); return; }   // CTRL+n: on in the big notes overlay
        book_note_key(key);
        return;
    }

    // While the pending edits are written to the files nothing else may happen; ESC stops after the current file.
    if (meta_->save.active) {
        if (key == 27) meta_->save.cancel = true;
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

    // The Latin letters key (SHIFT+' / SHIFT+2 here) works in the meta data menu, the cover and video fetch overlays, the video option and the
    // big folder overlay too -- except while a text field has the keyboard.
    if (!last_key_was_arrow() && key_is("HKeyTranslit", key)) {
        bool ok = false;
        switch (mode_) {
            case Mode::MetaEdit: ok = !text_entry_active() || (meta_->tab == 0 && meta_->focus == 2 && !meta_->field_editing); break;
            case Mode::CoverFetch: ok = !text_entry_active(); break;
            case Mode::VideoFetch: ok = vf_focus_ != 0; break;                       // (the filter field is focus 0; a running download does not matter)
            case Mode::VideoLink: ok = !(key >= '0' && key <= '9') && !(key >= 'a' && key <= 'z') && !(key >= 'A' && key <= 'Z'); break;   // (typing goes into the search fields there: only a symbol key is taken)
            case Mode::Browse: ok = folder_overlay_active() && !fo_search_; break;
            default: break;
        }
        if (ok) { toggle_latin_letters(); return; }
    }

    if (mode_ == Mode::ColorEdit || mode_ == Mode::Settings) {
        handle_settings_key(key);
        return;
    }

    if (mode_ == Mode::Console) { handle_console_key(key); return; }

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

    if (mode_ == Mode::VideoMenu) { video_menu_key(key); return; }
    if (mode_ == Mode::VideoLink) { video_link_key(key); return; }
    if (mode_ == Mode::VideoSearch) { video_search_key(key); return; }
    if (mode_ == Mode::VideoFetch) { video_fetch_key(key); return; }
    if (mode_ == Mode::AudioDevice) { audio_device_key(key); return; }   // CHOOSE AUDIO DEVICE (src/app_audiodevice.cpp)

    if (mode_ == Mode::UpdateNotice) { handle_update_notice_key(key); return; }

    if (mode_ == Mode::QueueToPlaylist) { handle_qtp_key(key); return; }

    if (mode_ == Mode::ClearQueue) { handle_clear_queue_key(key); return; }

    if (mode_ == Mode::SpectroMenu) { handle_spectro_menu_key(key); return; }

    if (creator_->show && mode_ == Mode::Browse) { creator_preview_key(key); return; }   // the creator's preview: ESC stops it

    if (video_full_ && mode_ == Mode::Browse && video_full_key(key)) return;   // the full screen video: UP / DOWN effect, TAB lyrics, ESC closes

    if (spectro_full_ && mode_ == Mode::Browse && last_key_was_arrow() && (key == 'A' || key == 'B' || key == 'C' || key == 'D')) {
        // LEFT / RIGHT: image <-> braille, UP / DOWN: spectrogram <-> waterfall (the same values as the SHIFT+i overlay)
        spectro_adjust(settings_.spectro, (key == 'C' || key == 'D') ? kSrStyle : kSrView, (key == 'C' || key == 'B') ? 1 : -1);
        settings_.spectro_sync();
        if (key == 'C' || key == 'D')
            status_line_ = settings_.spectro.style == 1 ? (gfx_proto_ == GfxProto::None ? "spectrogram style: image (this terminal has neither Kitty graphics nor Sixel: braille is drawn)" : "spectrogram style: image")
                                                        : "spectrogram style: braille";
        else status_line_ = settings_.spectro.view == 1 ? "spectrogram view: waterfall" : "spectrogram view: spectrogram";
        save_settings(settings_);
        force_redraw_ = true;
        return;
    }
    if (spectro_full_ && mode_ == Mode::Browse && key == 27 && !last_key_was_arrow()) {   // ESC leaves the full screen
        spectro_full_ = false;
        force_redraw_ = true;
        return;
    }

    if (scope_big_ && mode_ == Mode::Browse && scope_big_key(key)) return;   // SHIFT+0 overlay: ESC, SHIFT+a, SHIFT+c, the colour prompt

    if (mode_ == Mode::OsciMenu) { handle_osci_menu_key(key); return; }

    if (mode_ == Mode::BarsMenu) { handle_bars_menu_key(key); return; }

    if (mode_ == Mode::NormMenu) { handle_norm_menu_key(key); return; }

    if (mode_ == Mode::LyricsEdit) { handle_lyrics_edit_key(key); return; }

    if (mode_ == Mode::BookOverlay) { book_overlay_key(key); return; }
    if (mode_ == Mode::Creator) { creator_key(key); return; }
    if (mode_ == Mode::MenuIndex) { menu_index_key(key); return; }

    if (mode_ == Mode::SleepTimer) { handle_sleep_timer_key(key); return; }

    if (mode_ == Mode::Equalizer) { handle_equalizer_key(key); return; }

    if (mode_ == Mode::Karaoke) { handle_karaoke_key(key); return; }

    if (mode_ == Mode::Cheatsheet) { handle_cheatsheet_key(key); return; }

    if (mode_ == Mode::Wizard) { handle_wizard_key(key); return; }

    if (mode_ == Mode::BulkAdd) { handle_bulk_add_key(key); return; }

    if (mode_ == Mode::RetryLyrics) { handle_retry_lyrics_key(key); return; }

    if (mode_ == Mode::Search) { handle_search_key(key); return; }

    handle_browse_key(key);
}

// The main screen (Mode::Browse): the big list / queue / folder overlays first, then the hotkey is resolved to an action name and handed
// to the themed groups below (each returns true when it took the key).
void App::handle_browse_key(int key) {
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size()
                     : (list_source_ == ListSource::Online) ? online_view_.size()
                     : (list_source_ == ListSource::Folder) ? folder_view_.size()
                     : playlist_->view.size();

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

    // (A typed capital B and the Down arrow are the same value; resolve_hotkey_action() tells them apart with last_key_was_arrow().)
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
    // SHIFT+b is the bookmark in the audio book mode and the spectrum bars overlay everywhere else (both commands may hold the key)
    // (the settings keep the bindings in an unordered map, so which of the two a shared key resolves to is decided here)
    if (action == "HKeyBarsMenu" || action == "HKeyBookmark" || action == "HKeyFetchTags") {   // (HKeyFetchTags is the meta editor's)
        if (audiobook_ && key_is("HKeyBookmark", key)) action = "HKeyBookmark";
        else if (key_is("HKeyBarsMenu", key)) action = "HKeyBarsMenu";
        else if (!audiobook_) action.clear();
    }
    if (audiobook_ && key == 'Y' && !last_key_was_arrow()) action = "HKeyBarsMenu";   // SHIFT+y: the spectrum bars' options of the audio book (SHIFT+b is the bookmark there)
    if (audiobook_ && book_intercept(action, key)) return;   // the audio book player's own keys (and what it does not offer)

    if (browse_open_key(action, key, lookup_key, list_len)) return;
    if (browse_nav_queue_key(action, key, lookup_key, list_len)) return;
    if (browse_playback_key(action, key, lookup_key, list_len)) return;
    if (browse_queue_misc_key(action, key, lookup_key, list_len)) return;
    if (browse_rest_key(action, key, lookup_key, list_len)) return;
}

} // namespace muisc
