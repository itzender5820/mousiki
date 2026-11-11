// app_remote.cpp
// Part of class App (src/app.h): Mode switch, the remote control (phone / command line) and the actions they trigger.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "app_state_fo.h"
#include "app_state_phone.h"
#include "meta_state.h"
#include "book_state.h"
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
// Input handling
// ---------------------------------------------------------------------

// What the radio may have changed in config.txt while the player was suspended: Start Mode, Black Background, Share Profiles and,
// with Share Profiles on, the oscilloscope / spectrogram settings (the radio writes its edits of them there).
// SHIFT and + (dir = 1) / SHIFT and - (dir = -1): leave for the next / previous mode that is switched on.
bool App::request_mode_switch(int dir) {
    const int me = audiobook_ ? 2 : 0;
    if (settings_.next_mode(me, dir) == me) {
        status_line_ = "this is the only mode switched on (Settings -> GENERAL -> MODES)";
        return false;
    }
    g_mode_switch_dir = dir;
    if (audiobook_ && book_ask_due()) {   // leaving the audio book: the question about a bookmark first
        book_ask_then(dir > 0 ? "Switching to the next mode" : "Switching to the previous mode", [this]() { switch_mode_ = true; quit_ = true; });
        return true;
    }
    switch_mode_ = true;
    quit_ = true;
    return true;
}

void App::sync_after_radio() {
    settings_.start_mode = read_start_mode();
    {   // the mode switches and their order (the radio's settings change the same lines)
        const ModeConfig mc = read_mode_config();
        for (int i = 0; i < 3; ++i) { settings_.use_mode[i] = mc.use[i]; settings_.mode_order[i] = mc.order[i]; }
    }
    {   // the AUDIO BOOK PATH (the radio's PATHS tab can change it): a new folder changes what the library lists
        settings_.audiobook_path = read_audiobook_path();
        if (settings_.audiobook_path != scanned_book_path_) rescan_library();   // also: the other mode changed it while this one waited
    }
    settings_.latin_letters = translit_enabled();   // the radio has its own key for it (same switch)
    settings_.black_background = read_black_background();
    settings_.share_profiles = read_share_profiles();
    if (settings_.share_profiles && !audiobook_) { VizPrefs v; if (read_player_viz(v)) settings_.take_shared_from(v); }   // the audio book mode keeps its own
    const RemoteAppConfig rc = read_remote_app();
    settings_.remote_app = rc.enabled;
    settings_.remote_port = rc.port;
    settings_.remote_pin = rc.pin;
    settings_.remote_cli = rc.cli;
    settings_.remote_hosts = rc.hosts;
}

void App::remote_app_apply() {
    remote::net::configure({settings_.remote_app, settings_.remote_port, settings_.remote_pin, settings_.remote_hosts, settings_.remote_cli});
}

// The REMOTE APP overlay, centred on the Settings screen (absolute positions, like the screen itself).
remote_ui::Style App::settings_overlay_style() const {
    remote_ui::Style st;
    st.border = ansi_for(settings_.border_color, false);
    st.border_bottom = ansi_for(settings_.border_color_bottom, false);
    st.header = ansi_for(settings_.header_color, true);
    st.legend = ansi_for(settings_.legend_color, false);
    st.text = ansi_for(settings_.list_color, false);
    st.value = st.text;
    st.change = ansi_for(settings_.change_text_color, false) + bg_ansi_for(settings_.change_bg_color);
    st.cursor = "\x1b[7m";
    st.ul = settings_.box_upper_left; st.ur = settings_.box_upper_right;
    st.ll = settings_.box_lower_left; st.lr = settings_.box_lower_right;
    st.h = settings_.box_horizontal;  st.v = settings_.box_vertical;
    if (settings_.border_smooth)
        st.border_at = [this](float t) { return gradient_ansi(settings_.border_color, settings_.border_color_bottom, t, false); };
    return st;
}

void App::remote_overlay_draw(std::ostringstream& frame, int W) const {
    const remote_ui::Style st = settings_overlay_style();
    const remote_ui::Values v{settings_.remote_app, settings_.remote_port, settings_.remote_pin};
    const int rows = std::max(10, term_rows_ - 1);
    int w = 0;
    const std::vector<std::string> lines = remote_ui::build(remote_ov_, v, st, W - 2, rows, w);
    const int x = std::max(1, (W - w) / 2 + 1), y = std::max(1, (rows - static_cast<int>(lines.size())) / 2 + 1);
    for (size_t i = 0; i < lines.size(); ++i) frame << "\x1b[" << y + static_cast<int>(i) << ";" << x << "H" << lines[i];
    frame << "\x1b[0m";
}

void App::act_cycle_play_mode(int dir) {
    // list -> repeat -> shuffle -> stop -> queue then stop -> list (dir -1: the other way round)
    settings_.play_mode = (settings_.play_mode + (dir < 0 ? 4 : 1)) % 5;
    queue_reset_lap();
    static const char* mode_names[] = {"list", "repeat", "shuffle", "stop", "queue then stop"};
    log_event(std::string("play mode: ") + mode_names[settings_.play_mode]);
}

void App::act_wizard_toggle() {
    if (mode_ == Mode::Wizard) wizard_close();
    else wizard_open();
}

// ---- actions shared by the hotkeys and the remote control ------------------------------------------------------------------
void App::act_play_pause() {
    if (has_track_) { if (player_.is_paused()) player_.resume(); else player_.pause(); }
}

void App::act_volume_change(int delta) {
    if (has_track_) player_.set_volume(std::clamp(player_.volume() + delta, 0, 100));
}

void App::act_mute(int how) {   // force volume to 0 without touching the pause state
    const bool want = how < 0 ? !muted_ : how > 0;
    if (want == muted_) return;
    if (want) {
        pre_mute_volume_ = player_.volume();
        player_.set_volume(0);
        muted_ = true;
        log_event("muted");
    } else {
        player_.set_volume(pre_mute_volume_);
        muted_ = false;
        log_event("unmuted");
    }
}

void App::act_next() {
    if (audiobook_) { book_step(1); return; }
    // The queue (if any) takes priority, same as auto-advance-on-finish does: its next title in order (only # picks a random
    // one), Queue-then-stop respected via play_next_from_queue() -- a manual skip still always actually skips, though: Repeat/Stop/Queue-then-stop only govern
    // *automatic* advance, not an explicit "next" press.
    if (!queue_.empty()) play_next_from_queue();
    else play_relative(1);
}

void App::act_prev() {
    if (audiobook_) { book_step(-1); return; }
    // Relative to what's actually playing (see current_track_list_index()), not the hover cursor. No queue equivalent: a FIFO
    // queue has no well-defined "previous" once an item's been consumed.
    play_relative(-1);
}

// One command of the remote control (`mousiki --cmd`, the phone app, media keys) -- also what the CLI (/c: and the console's
// command pane) runs for the commands they share.
void App::remote_apply(const remote::Command& c) {
    const std::string& n = c.name;
    if (n == "cli") { cli_run(c.arg, c.from_net ? cli::Origin::Network : cli::Origin::Remote, c.id); force_redraw_ = true; return; }   // a command line (text) from outside
    if (n == "play-pause") act_play_pause();
    else if (n == "play") { if (has_track_ && player_.is_paused()) player_.resume(); }
    else if (n == "pause" || n == "stop") { if (has_track_ && !player_.is_paused()) player_.pause(); }
    else if (n == "next") { if (audiobook_) book_step(1, true); else act_next(); }
    else if (n == "prev") { if (audiobook_) book_step(-1, true); else act_prev(); }
    else if (n == "shuffle") { if (audiobook_) status_line_ = "no shuffle in the audio book mode"; else if (!queue_.empty()) play_next_from_queue(true); else play_relative_random(); }
    else if (n == "volume-up") act_volume_change(static_cast<int>(remote::number(c.arg, 5)));
    else if (n == "volume-down") act_volume_change(-static_cast<int>(remote::number(c.arg, 5)));
    else if (n == "volume") {
        if (has_track_) {
            const int v = static_cast<int>(remote::number(c.arg, player_.volume()));
            player_.set_volume(std::clamp((!c.arg.empty() && (c.arg[0] == '+' || c.arg[0] == '-')) ? player_.volume() + v : v, 0, 100));
        }
    }
    else if (n == "mute") act_mute(1);
    else if (n == "unmute") act_mute(0);
    else if (n == "toggle-mute") act_mute(-1);
    else if (n == "seek") { if (has_track_) player_.seek_relative(remote::number(c.arg, 0.0)); }
    else if (n == "playback-mode") act_cycle_play_mode();
    else if (n == "wizard") act_wizard_toggle();
    else if (n == "remote-view") {
        phone_->view = remote::number(c.arg, 0) > 0.5 ? 1 : 0;
        phone_->note.clear();
        phone_->clear_armed = {};
        if (phone_->view == 1 && audiobook_) phone_book_reveal();   // the library tree opens at the book that is open
        else if (phone_->view == 1) {   // the local list opens on the page of the playing title
            int cols = 44, rows = 44;
            remote::net::screen_wanted(cols, rows);
            int per = 0, per2 = 0;
            remote::net::phone_lists_rows(rows, per, per2);
            int at = -1;
            if (has_track_ && current_.is_local)
                for (size_t i = 0; i < local_view_.size(); ++i) if (local_view_[i].path == current_.path) { at = static_cast<int>(i); break; }
            phone_->page = at >= 0 ? at / per : 0;
            phone_->page2 = 0;
            phone_->focus = 0;
        }
    }
    else if (n == "remote-focus") { phone_->focus = remote::number(c.arg, 0) > 0.5 ? 1 : 0; }
    else if (n == "remote-scroll") {   // a whole page of the pane in focus
        int cols = 44, rows = 44;
        remote::net::screen_wanted(cols, rows);
        int per = 0, per2 = 0;
        remote::net::phone_lists_rows(rows, per, per2);
        const int step = static_cast<int>(remote::number(c.arg, 0));
        if (audiobook_) {   // one pane: the whole library tree
            const int pages = std::max(1, (static_cast<int>(phone_book_rows().size()) + per + per2 - 2) / std::max(1, per + per2 - 1));
            phone_->page = std::clamp(phone_->page + step, 0, pages - 1);
        } else if (phone_->focus == 0) {
            const int pages = std::max(1, (static_cast<int>(local_view_.size()) + per - 1) / per);
            phone_->page = std::clamp(phone_->page + step, 0, pages - 1);
        } else {
            const int pages = std::max(1, (static_cast<int>(playlist_summaries().size()) + per2 - 1) / per2);
            phone_->page2 = std::clamp(phone_->page2 + step, 0, pages - 1);
        }
    }
    else if (n == "book-open" || (n == "track" && audiobook_)) {   // the phone's library tree (1 = its first row)
        const std::vector<FoRow> rows = phone_book_rows();
        const int k = static_cast<int>(remote::number(c.arg, 0)) - 1;
        if (k >= 0 && k < static_cast<int>(rows.size())) {
            const FoRow& r = rows[static_cast<size_t>(k)];
            if (r.is_dir) {   // a folder opens / closes
                if (!phone_->book_open.erase(r.path)) phone_->book_open.insert(r.path);
            } else if (book_open_dir(path_utf8(path_from_utf8(r.path).parent_path()))) {   // a chapter plays from its start (no questions)
                for (size_t i = 0; i < chapters_.size(); ++i)
                    if (path_utf8(chapters_[i].path) == r.path) {
                        book_automark_tick(true);
                        book_->prompt_paused = false;
                        book_play_chapter(static_cast<int>(i));
                        phone_->note = "playing " + path_utf8(path_from_utf8(r.path).stem());
                        break;
                    }
            } else phone_->note = "that book is no longer in the library";
        }
    }
    else if (n == "track") {   // the phone's local list: play that title (1 = the first title of the list)
        const int k = static_cast<int>(remote::number(c.arg, 0)) - 1;
        if (k >= 0 && k < static_cast<int>(local_view_.size())) {
            const LocalTrack t = local_view_[static_cast<size_t>(k)];
            if (list_source_ == ListSource::Local) {   // the main list follows
                selected_ = k;
                if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
                if (selected_ < scroll_) scroll_ = selected_;
            }
            start_local_track(t);
            phone_->note = "playing " + t.title;
        }
    }
    else if (n == "clear-queue") {   // the phone taps twice (arg 0 arms, 1 clears); the command line clears at once
        if (queue_.empty()) phone_->note = "the queue is empty";
        else if (c.arg == "0") {
            phone_->clear_armed = std::chrono::steady_clock::now();
            phone_->note = "tap again to clear " + std::to_string(queue_.size()) + (queue_.size() == 1 ? " title" : " titles");
        } else {
            const size_t was = queue_.size();
            queue_clear();
            phone_->clear_armed = {};
            phone_->note = "queue cleared (" + std::to_string(was) + (was == 1 ? " title)" : " titles)");
            status_line_ = phone_->note;
        }
    }
    else if (n == "playlist") {   // the phone's PLAYLISTS list (1 = the first playlist in A-Z order)
        const std::vector<PlaylistSummary> pls = playlist_summaries();
        const int k = static_cast<int>(remote::number(c.arg, 0)) - 1;
        if (k >= 0 && k < static_cast<int>(pls.size())) {
            const bool idle = !has_track_;
            phone_->note = queue_playlist(pls[static_cast<size_t>(k)].name);
            status_line_ = phone_->note;
            if (idle && !queue_.empty()) { play_next_from_queue(); phone_->note += " -- playing"; }
        }
    }
    else if (n == "switch-mode") request_mode_switch(remote::number(c.arg, 1) < 0 ? -1 : 1);   // (-1: the previous mode)
    else if (n == "quit") quit_ = true;
    // "live" belongs to the radio: ignored here
    force_redraw_ = true;
}

// Commands from `mousiki --cmd ...` and the status file for `mousiki --status`. Called every frame.
void App::remote_poll() {
    const char* const mode_name = audiobook_ ? "audiobook" : "player";   // (the audio book is its own mode for the remote app)
    remote::tick(mode_name);
    cli_tick();
    {   // the answer of a manual check ("Check Update" in Settings): one line on the settings screen
        update_check::CheckResult cr;
        if (update_check::take_check_result(cr)) {
            std::string v = cr.latest;
            while (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
            if (!cr.ok) status_line_ = "could not check for a newer release: " + cr.error;
            else if (cr.newer) {
                status_line_ = "a newer release is out: Mousiki " + v + " (you have " + cr.current + "): " + update_check::release_url({cr.latest, cr.current, cr.url});
                // the same popup as at start-up ([o] release page, [ESC] close); it waits for the main screen, so it opens
                // when Settings is left (leaving Settings with ESC discards unsaved changes, so it is not forced open)
                update_latest_ = cr.latest; update_current_ = cr.current; update_url_ = update_check::release_url({cr.latest, cr.current, cr.url});
                update_pending_ = true;
            }
            else status_line_ = "Mousiki " + cr.current + " is the newest release (checked just now)";
            force_redraw_ = true;
        }
    }
    {   // "a newer version is out": once; the popup waits until the main screen is showing
        update_check::Notice n;
        if (!update_pending_ && update_check::take(n)) {
            update_latest_ = n.latest; update_current_ = n.current; update_url_ = update_check::release_url(n);
            update_pending_ = true;
        }
        if (update_pending_ && mode_ == Mode::Browse && !meta_->save.active) {
            update_pending_ = false;
            mode_ = Mode::UpdateNotice;
            force_redraw_ = true;
        }
    }
    for (const remote::Command& c : remote::poll()) remote_apply(c);
    phone_publish();   // the remote app's screen, when a phone asks for it
    const TrackMetadata md = has_track_ ? display_metadata() : TrackMetadata{};
    char pos[32], dur[32];
    std::snprintf(pos, sizeof pos, "%.1f", has_track_ ? player_.poll_elapsed() : 0.0);
    std::snprintf(dur, sizeof dur, "%zu", has_track_ ? total_sec_ : static_cast<size_t>(0));
    remote::publish_status({{"mode", mode_name},
                            {"state", !has_track_ ? "stopped" : player_.is_paused() ? "paused" : muted_ ? "muted" : "playing"},
                            {"title", has_track_ ? md.name : std::string()},
                            {"artist", has_track_ && md.artist != "-" ? md.artist : std::string()},
                            {"album", has_track_ && md.album != "-" ? md.album : std::string()},
                            {"position_sec", pos}, {"duration_sec", dur},
                            {"volume", std::to_string(player_.volume())}, {"muted", muted_ ? "yes" : "no"}, {"queue_len", std::to_string(queue_.size())},
                            {"playback_mode", play_mode_name(settings_.play_mode)}});
}


// The phone's audio book LISTS: the library overlay's tree (folders, books, chapters), walked with phone_->book_open.
std::vector<App::FoRow> App::phone_book_rows() const {
    std::vector<FoRow> out;
    std::function<void(const std::string&, int)> emit = [&](const std::string& d, int depth) {
        FoRow r;
        r.path = d;
        r.label = fo_name_of(d);
        r.is_dir = true;
        r.depth = depth;
        r.expanded = phone_->book_open.count(d) > 0;
        const auto c = fo_->count.find(d);
        r.count = c == fo_->count.end() ? 0 : c->second;
        out.push_back(r);
        if (!r.expanded) return;
        const auto sd = fo_->subdirs.find(d);
        if (sd != fo_->subdirs.end()) for (const auto& x : sd->second) emit(x, depth + 1);
        const auto f = fo_->files.find(d);
        if (f != fo_->files.end())
            for (const auto& file : f->second) {
                FoRow fr;
                fr.path = file;
                fr.label = path_utf8(path_from_utf8(file).stem());
                fr.depth = depth + 1;
                out.push_back(fr);
            }
    };
    for (const auto& r : fo_->roots) emit(r, 0);
    return out;
}

// LISTS was opened: the tree is built again (the library may have been scanned meanwhile) and the way down to the open book
// is opened, the page is the one that shows it.
void App::phone_book_reveal() {
    folder_overlay_build_tree();
    phone_->page = 0;
    phone_->note.clear();
    if (book_->dir.empty()) return;
    phone_->book_open.insert(book_->dir);
    std::set<std::string> roots(fo_->roots.begin(), fo_->roots.end());
    std::string k = path_utf8(path_from_utf8(book_->dir).parent_path());
    while (!k.empty()) {
        if (fo_->subdirs.count(k)) phone_->book_open.insert(k);
        if (roots.count(k)) break;
        const std::string par = path_utf8(path_from_utf8(k).parent_path());
        if (par.empty() || par == k) break;
        k = par;
    }
    int cols = 44, rows = 44;
    remote::net::screen_wanted(cols, rows);
    int per = 0, per2 = 0;
    remote::net::phone_lists_rows(rows, per, per2);
    const int page_rows = std::max(1, per + per2 - 1);
    const std::vector<FoRow> v = phone_book_rows();
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].path == book_->dir) { phone_->page = static_cast<int>(i) / page_rows; break; }
}

} // namespace muisc

namespace muisc {
// The seek keys: the << or >> button's bottom border lights up for a moment (see build_progress_panel), so a run of
// seeks shows which way it goes. Each press extends the moment.
void App::seek_flash(int dir) {
    seek_flash_dir_ = dir < 0 ? -1 : 1;
    seek_flash_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    force_redraw_ = true;
}
}  // namespace muisc
