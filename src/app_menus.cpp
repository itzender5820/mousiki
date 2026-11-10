// app_menus.cpp
// Part of class App (src/app.h): Small menus and floating panels: retry lyrics, oscilloscope / normalization options, lyrics timing, sleep timer, MENUS index, ...
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "app_state_rl.h"
#include "app_state_sleep.h"
#include "app_state_device.h"
#include "app_state_qtp.h"
#include "readlyrics_state.h"
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
#include "organigram_trees.h"
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
// Retry Lyrics (HKeyRetryLyrics, 'l') -- manual title/artist override
// ---------------------------------------------------------------------

std::vector<App::RLField> App::RlState::rl_visible_fields() const {
    std::vector<RLField> f;
    f.insert(f.end(), {
        RLField::Title, RLField::Artist, RLField::Ft,
        RLField::TypeReverb, RLField::TypeSlowed, RLField::TypeUltraSlowed,
        RLField::TypeSpedup, RLField::TypeRemix, RLField::TypeOther,
    });
    if (remix) f.push_back(RLField::RemixText);
    if (other) f.push_back(RLField::OtherText);
    if (!versions.empty()) f.push_back(RLField::Source);   // the list sits at the bottom of the overlay
    return f;
}

bool* App::RlState::rl_bool_ptr(RLField f) {
    switch (f) {
        case RLField::TypeReverb: return &reverb;
        case RLField::TypeSlowed: return &slowed;
        case RLField::TypeUltraSlowed: return &ultra_slowed;
        case RLField::TypeSpedup: return &spedup;
        case RLField::TypeRemix: return &remix;
        case RLField::TypeOther: return &other;
        default: return nullptr;
    }
}

std::string* App::RlState::rl_text_ptr(RLField f) {
    switch (f) {
        case RLField::Title: return &title;
        case RLField::Artist: return &artist;
        case RLField::Ft: return &ft;
        case RLField::RemixText: return &remix_text;
        case RLField::OtherText: return &other_text;
        default: return nullptr;
    }
}

// Pre-fills from the current track's metadata_ rather than opening
// blank, and resets every other bit of state -- so reopening after a
// previous override (or a previous cancel) never leaks stale values
// from last time.
void App::rl_open_from_current_track() {
    edit_owner_.clear();   // fresh fields: the caret starts at the end of the one in focus
    rl_->title = metadata_.name;
    rl_->artist = (metadata_.artist == "-") ? "" : metadata_.artist;
    rl_->ft.clear();

    // Best-effort ft./feat. split: if the title itself carries a
    // "feat."/"ft." tag, pull it out into its own field rather than
    // leaving it embedded (so it doesn't get double-appended once the
    // TYPE tags get tacked on after the title at submit time).
    static const std::vector<std::string> markers = {" feat. ", " feat ", " ft. ", " ft "};
    std::string lower_title = ascii_lower_str(metadata_.name);
    for (const auto& marker : markers) {
        size_t pos = lower_title.find(marker);
        if (pos != std::string::npos) {
            rl_->title = metadata_.name.substr(0, pos);
            rl_->ft = metadata_.name.substr(pos + marker.size());
            // trim a trailing ')' if the split landed inside "(feat. X)"
            if (!rl_->ft.empty() && rl_->ft.back() == ')') rl_->ft.pop_back();
            while (!rl_->title.empty() && (rl_->title.back() == ' ' || rl_->title.back() == '(')) rl_->title.pop_back();
            break;
        }
    }

    rl_->remix_text.clear();
    rl_->other_text.clear();
    rl_->reverb = rl_->slowed = rl_->ultra_slowed = rl_->spedup = rl_->remix = rl_->other = false;
    rl_->focus = RLField::Title;
    rl_->versions = list_lyrics_versions(current_.path);
    rl_->src_sel = 0;
    for (size_t i = 0; i < rl_->versions.size(); ++i) if (rl_->versions[i].active) { rl_->src_sel = static_cast<int>(i); break; }
    rl_->confirm_delete = false;
    rl_->src_scroll = 0;
    rl_->rl_fix_scroll();
}

// Over READ LYRICS and over the karaoke screen: the same overlay, for the track that plays (the lyrics engine belongs to it).
// Closing returns to the screen it came from; the reader loads the new lyrics when the fetch is done (readlyrics_poll()).
void App::rl_open_over_reader() {
    if (!readlyrics_track_plays()) { rd_->status = "re-fetch: only for the track that plays -- start it with [p]"; rd_->status_warn = true; return; }
    rl_open_from_current_track();
    rl_->from_reader = true;
    rl_->from_karaoke = false;
    rd_->watch_epoch = lyrics_.epoch.load();
    mode_ = Mode::RetryLyrics;
}

void App::rl_open_over_karaoke() {
    if (!has_track_) return;
    rl_open_from_current_track();
    rl_->from_reader = false;
    rl_->from_karaoke = true;
    mode_ = Mode::RetryLyrics;
}

void App::rl_close() {
    mode_ = rl_->from_reader ? Mode::ReadLyrics : rl_->from_karaoke ? Mode::Karaoke : Mode::Browse;
    rl_->from_reader = false;
    rl_->from_karaoke = false;
    force_redraw_ = true;
}

void App::RlState::rl_fix_scroll() {
    const int n = static_cast<int>(versions.size());
    src_sel = std::clamp(src_sel, 0, std::max(0, n - 1));
    src_scroll = std::clamp(src_scroll, 0, std::max(0, n - 3));
    if (src_sel < src_scroll) src_scroll = src_sel;
    if (src_sel > src_scroll + 2) src_scroll = src_sel - 2;
}

// [d] + y on the list: the highlighted saved copy is deleted. When it was the copy in use, the lyrics are reloaded
// without the network (the lyrics embedded in the file if there are any, otherwise none) -- a deliberate delete must
// not bring the same lyrics straight back.
void App::rl_delete_version() {
    if (rl_->src_sel < 0 || rl_->src_sel >= static_cast<int>(rl_->versions.size())) return;
    const LyricsVersion v = rl_->versions[static_cast<size_t>(rl_->src_sel)];
    if (!delete_lyrics_version(v)) { status_line_ = "could not delete those lyrics"; return; }
    rl_->versions = list_lyrics_versions(current_.path);
    rl_->rl_fix_scroll();
    if (v.active) {
        bool embedded = false;
        for (const auto& x : rl_->versions) if (x.kind == LyricsVersion::Kind::Embedded) embedded = true;
        if (embedded) {
            std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
            launch_lyrics_fetch(metadata_.name, artist, current_.path);
        } else {
            ++lyrics_.epoch;
            std::lock_guard<std::mutex> lock(lyrics_.mutex);
            lyrics_.result = LyricsResult();
            lyrics_.result.status = LyricsStatus::NotFound;
            lyrics_.result.message = "no lyrics";
            lyrics_.path = current_.path;
            lyrics_.ready = true;
        }
    }
    status_line_ = "lyrics deleted";
    log_event("lyrics: deleted " + v.label);
}

// SPACE on the version list: the highlighted version becomes the one that loads for this track. The copy that was in use
// is put aside as an older version, so every switch can be undone.
void App::rl_choose_version() {
    if (rl_->src_sel < 0 || rl_->src_sel >= static_cast<int>(rl_->versions.size())) return;
    const LyricsVersion v = rl_->versions[static_cast<size_t>(rl_->src_sel)];
    if (v.active) { status_line_ = "those lyrics are in use already"; return; }
    if (!activate_lyrics_version(current_.path, v)) { status_line_ = "could not switch the lyrics (is the lyrics folder writable?)"; return; }
    rl_->versions = list_lyrics_versions(current_.path);
    rl_->src_sel = 0;
    for (size_t i = 0; i < rl_->versions.size(); ++i) if (rl_->versions[i].active) { rl_->src_sel = static_cast<int>(i); break; }
    rl_->rl_fix_scroll();
    std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
    launch_lyrics_fetch(metadata_.name, artist, current_.path);   // loads whatever is in use now (no network)
    log_event("lyrics: using " + v.label);
}

// Builds "title [ft. X] [tags...]" and launches the override fetch --
// this is the actual point of the whole form: feeding a corrected
// query into fetch_synced_lyrics() instead of the track's real
// metadata, for tracks whose auto-fetched lyrics are wrong/missing.
void App::rl_submit() {
    std::string query = rl_->title;
    if (!rl_->ft.empty()) query += " ft. " + rl_->ft;

    // Tags go after the title (per your note: "you usually need to place
    // them after title in url"). Slowed/Ultra Slowed/Spedup are mutually
    // exclusive so at most one of these three contributes; Reverb/Remix/
    // Other are independent and can stack with it and each other.
    if (rl_->slowed) query += " slowed";
    else if (rl_->ultra_slowed) query += " ultra slowed";
    else if (rl_->spedup) query += " sped up";
    if (rl_->reverb) query += " reverb";
    if (rl_->remix) query += rl_->remix_text.empty() ? " remix" : (" " + rl_->remix_text + " remix");
    if (rl_->other && !rl_->other_text.empty()) query += " " + rl_->other_text;

    launch_lyrics_fetch(query, rl_->artist, current_.path, /*force_network=*/true);
    log_event("retrying lyrics: \"" + query + "\"");

    rl_close();
}

// Oscilloscope tuning overlay (SHIFT+o). The rows come from osci_visible_rows() (osci_settings.cpp): which ones are
// shown depends on the style (braille / image). The ranges match what settings.cpp clamps on load.
void App::osci_menu_adjust(int dir) {
    const std::vector<int> rows = osci_menu_rows();
    if (rows.empty()) return;
    osci_menu_row_ = std::clamp(osci_menu_row_, 0, static_cast<int>(rows.size()) - 1);
    const int id = rows[osci_menu_row_];
    if (video_layout_menu() && (id == kOrStyle || id == kOrMode)) {   // the layout's own choice (-1 = as in the options): default, then the values
        int& v = id == kOrStyle ? settings_.vl_osci_style : settings_.vl_osci_mode;
        const int top = id == kOrStyle ? 1 : 1;
        v += dir > 0 ? 1 : -1;
        if (v > top) v = -1;
        if (v < -1) v = top;
        return;
    }
    osci_adjust(settings_, id, dir);
    // the style row changes which rows exist: keep the cursor on the same one
    const std::vector<int> now = osci_menu_rows();
    for (size_t i = 0; i < now.size(); ++i) if (now[i] == id) { osci_menu_row_ = static_cast<int>(i); return; }
    osci_menu_row_ = std::clamp(osci_menu_row_, 0, static_cast<int>(now.size()) - 1);
}

std::vector<std::string> App::build_osci_menu_panel() const {
    const int W = kOsciMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    const std::vector<int> rows = osci_menu_rows();
    const bool lay = video_layout_menu();
    const VizPrefs lv = layout_osci_view();
    std::vector<std::string> lines;
    lines.push_back(box_top(lay ? "Oscilloscope (video layout)" : "Oscilloscope", W, border));
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool sel = static_cast<int>(i) == osci_menu_row_;
        std::string val = lay ? osci_row_value(lv, rows[i]) : osci_row_value(settings_, rows[i]);
        if (lay && rows[i] == kOrStyle && settings_.vl_osci_style < 0) val += " (default)";
        if (lay && rows[i] == kOrMode) {
            val = settings_.vl_osci_mode == 0 ? "XY" : settings_.vl_osci_mode == 1 ? "YT" : (lv.osci_mode == 1 ? "YT" : "XY") + std::string(" (default)");
        }
        const std::string plain = std::string(sel ? "> " : "  ") + pad_right(osci_row_label(rows[i]), 22) +
                                  pad_left(val, inner - 24);
        lines.push_back(row(plain, sel));
    }
    lines.push_back(row("", false));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] select  [LEFT/RIGHT] change", "[R] reset  [" + hotkey_text("HKeyOscMenu", "O") + " / ESC] close"}, leg_in, leg_bot);
    return lines;
}

// Loudness normalisation overlay (SHIFT+v). Rows: 0 on/off, 1 target level
// (LUFS), 2 max boost (dB). The ranges match what settings.cpp clamps on
// load and what Player::set_normalization() accepts.
namespace {
struct NormKnob { const char* label; double lo, hi, step; };
constexpr NormKnob kNormKnobs[3] = {
    {"Normalize",     0.0,   1.0,  1.0},   // on/off, handled separately
    {"Target level",  -40.0, 0.0,  1.0},   // LUFS
    {"Max boost",     0.0,   24.0, 1.0},   // dB
};

// "-16" instead of "-16.0": whole numbers drop the decimal, a value that was
// typed into config.txt with a fraction ("-14.5") keeps it.
std::string fmt_loudness(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    std::string s(buf);
    if (s.size() > 2 && s.back() == '0' && s[s.size() - 2] == '.') {
        s.pop_back(); // the zero
        s.pop_back(); // ...and the point that only existed to show it
    }
    return s;
}
} // namespace

// Pushes the normalisation settings into the audio side. The player glides to
// the new gain within about a second, so there is no click.
void App::norm_apply() {
    player_.set_normalization(settings_.normalize,
                              static_cast<float>(settings_.normalize_target_lufs),
                              static_cast<float>(settings_.normalize_max_boost_db));
}

void App::norm_menu_adjust(int dir) {
    if (norm_menu_row_ == 0) {          // Left = off, Right = on
        settings_.normalize = dir > 0;
        norm_apply();
        return;
    }
    double* v = norm_menu_row_ == 1 ? &settings_.normalize_target_lufs
                                    : &settings_.normalize_max_boost_db;
    const NormKnob& k = kNormKnobs[std::clamp(norm_menu_row_, 0, 2)];
    // Round to the step grid so repeated presses never accumulate drift (and a
    // fractional value from config.txt snaps onto whole numbers).
    const double next = std::round((*v + dir * k.step) / k.step) * k.step;
    *v = std::clamp(next, k.lo, k.hi);
    norm_apply();
}

std::vector<std::string> App::build_norm_menu_panel() const {
    const int W = kNormMenuPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m", DIM = "\x1b[90m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    auto dim_row = [&](const std::string& plain) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + DIM + body + R + " " + bar;
    };

    const std::string vals[3] = {
        settings_.normalize ? "on" : "off",
        fmt_loudness(settings_.normalize_target_lufs) + " LUFS",
        fmt_loudness(settings_.normalize_max_boost_db) + " dB",
    };

    // What the player is doing right now with the track that is playing: its
    // measured loudness and the gain that results (capped by Max boost).
    std::string live;
    const float lufs = player_.track_lufs();
    if (!has_track_) {
        live = "No track playing";
    } else if (std::isnan(lufs)) {
        live = "Track: measuring loudness...";
    } else if (!settings_.normalize) {
        char num[48];
        std::snprintf(num, sizeof num, "Track %.1f LUFS   normalization is off", static_cast<double>(lufs));
        live = num;
    } else {
        char num[48];
        std::snprintf(num, sizeof num, "Track %.1f LUFS   applied gain %+.1f dB",
                      static_cast<double>(lufs), static_cast<double>(player_.normalization_gain_db()));
        live = num;
    }

    std::vector<std::string> lines;
    lines.push_back(box_top("Loudness normalization", W, border));
    for (int i = 0; i < 3; ++i) {
        const std::string plain = std::string(i == norm_menu_row_ ? "> " : "  ") +
                                  pad_right(kNormKnobs[i].label, 18) + pad_left(vals[i], 12);
        lines.push_back(row(plain, i == norm_menu_row_));
    }
    lines.push_back(dim_row(live));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + "\x1b[0m" + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(lines, W, border_bottom, {"[UP/DOWN] select   [LEFT/RIGHT] change", "[SPACE] on/off  [R] reset  [" + hotkey_text("HKeyNormMenu", "V") + " / ESC] close"}, leg_in, leg_bot);
    return lines;
}

// ---------------------------------------------------------------------
// Lyrics timing overlay (Alt+L) -- see app.h for the design.
// ---------------------------------------------------------------------
void App::lyrics_edit_open() {
    std::lock_guard<std::mutex> lk(lyrics_.mutex);
    if (!settings_.lyrics_on() || !has_track_ || !lyrics_.ready || lyrics_.result.lines.empty()) {
        status_line_ = "lyrics timing: no synced lyrics are loaded for this track";
        return;
    }
    lyrics_.edit_orig_delay = lyrics_.result.delay;
    lyrics_.edit_from_karaoke = false;
    lyrics_.edit_from_reader = false;
    mode_ = Mode::LyricsEdit;
}

// Over READ LYRICS: the same overlay, for the lyrics of the track that plays (the reader's lines light up with the new offset while it is
// open). The lyrics engine is started for the track like the karaoke screen does, so the main screen's lyrics field may be off.
void App::lyrics_edit_open_over_reader() {
    if (!readlyrics_track_plays()) { rd_->status = "lyrics timing: only for the track that plays -- start it with [p]"; rd_->status_warn = true; return; }
    karaoke_ensure_lyrics();
    std::lock_guard<std::mutex> lk(lyrics_.mutex);
    if (!lyrics_.ready || lyrics_.path != current_.path || lyrics_.result.lines.empty()) {
        rd_->status = "lyrics timing: no synced lyrics are loaded for this track (yet)";
        rd_->status_warn = true;
        return;
    }
    lyrics_.edit_orig_delay = lyrics_.result.delay;
    lyrics_.edit_from_karaoke = false;
    lyrics_.edit_from_reader = true;
    mode_ = Mode::LyricsEdit;
}

// Over the karaoke screen. The karaoke overlay runs the lyrics engine itself, so the lyrics field of the main screen may be off.
void App::lyrics_edit_open_over_karaoke() {
    std::lock_guard<std::mutex> lk(lyrics_.mutex);
    if (!has_track_ || !lyrics_.ready || lyrics_.path != current_.path || lyrics_.result.lines.empty()) {
        status_line_ = "lyrics timing: no synced lyrics are loaded for this track";
        return;
    }
    lyrics_.edit_orig_delay = lyrics_.result.delay;
    lyrics_.edit_from_karaoke = true;
    lyrics_.edit_from_reader = false;
    mode_ = Mode::LyricsEdit;
}

// Back to the screen it was opened from: the main screen, or the karaoke screen.
void App::lyrics_edit_close() {
    mode_ = lyrics_.edit_from_reader ? Mode::ReadLyrics : lyrics_.edit_from_karaoke ? Mode::Karaoke : Mode::Browse;
    lyrics_.edit_from_karaoke = false;
    lyrics_.edit_from_reader = false;
    force_redraw_ = true;
}

void App::LyricsState::lyrics_edit_adjust(double step) {
    std::lock_guard<std::mutex> lk(mutex);
    // Round to hundredths so repeated presses never accumulate float drift.
    const double next = std::round((result.delay + step) * 100.0) / 100.0;
    result.delay = std::clamp(next, -120.0, 120.0);
}

std::vector<std::string> App::build_lyrics_edit_panel() const {
    const int W = kLyricsEditPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m", DIM = "\x1b[90m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, const std::string& sgr) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (sgr.empty() ? body : sgr + body + R) + " " + bar;
    };

    std::vector<LyricLine> lines;
    double delay = 0.0;
    {
        std::lock_guard<std::mutex> lk(lyrics_.mutex);
        lines = lyrics_.result.lines;
        delay = lyrics_.result.delay;
        latinize_lyrics(lines);
    }
    const double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    const double lyric_t = elapsed - delay;
    int active = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].start_time <= lyric_t) active = static_cast<int>(i); else break;
    }
    auto text_at = [&](int i) { return (i >= 0 && i < static_cast<int>(lines.size())) ? lines[i].full_text : std::string(); };

    char num[32];
    std::snprintf(num, sizeof num, "%+.1f s", delay);
    const std::string state = delay > 0.0049 ? "lyrics appear later" : delay < -0.0049 ? "lyrics appear earlier" : "as in the lyrics file";

    std::vector<std::string> out;
    out.push_back(box_top("Lyrics Timing", W, border));
    out.push_back(row(std::string("Offset  ") + num + "   (" + state + ")", ""));
    out.push_back(row("", ""));
    out.push_back(row("  " + text_at(active - 1), DIM));
    out.push_back(row("> " + text_at(active), HI));
    out.push_back(row("  " + text_at(active + 1), DIM));
    auto leg_in = [&](const std::string& plain) {
        const std::string tx = truncate_str(plain, inner);
        const int left = std::max(0, (inner - display_width(tx)) / 2);
        return bar + " " + border_bottom + std::string(static_cast<size_t>(left), ' ') + tx + std::string(static_cast<size_t>(std::max(0, inner - left - display_width(tx))), ' ') + R + " " + bar;
    };
    auto leg_bot = [&](const std::string& tx) { return centred_bottom(settings_, W, tx, border_bottom); };
    push_legend_block(out, W, border_bottom, {"[LEFT/RIGHT] -/+ 0.1 s   [DOWN/UP] -/+ 0.5 s", "[SHIFT+LEFT/RIGHT] seek the track -/+ 5 s", "[R] reset  [ENTER] save  [ESC] cancel"}, leg_in, leg_bot);
    return out;
}

// ---------------------------------------------------------------------
// Sleep timer (HKeySleepTimer = Shift+Z) -- see app.h for the design.
// ---------------------------------------------------------------------
namespace {
constexpr int kSleepMinutes[5] = {15, 30, 60, 90, 120};
std::string fmt_countdown(long long secs) {
    if (secs < 0) secs = 0;
    char buf[24];
    if (secs >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", secs / 3600, (secs / 60) % 60, secs % 60);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld", secs / 60, secs % 60);
    return buf;
}
} // namespace

void App::sleep_timer_cancel() {
    player_.set_fade(1.0f);
    sleep_->timer_active = false;
    sleep_->timer_minutes = 0;
    sleep_->stop_after_track = false;
}

void App::sleep_timer_apply(int row) {
    // The two kinds of timer are mutually exclusive: whatever was armed is
    // replaced (picking the running minute entry again simply restarts it).
    sleep_timer_cancel();
    if (row >= 0 && row < 5) {
        sleep_->timer_minutes = kSleepMinutes[row];
        sleep_->timer_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(sleep_->timer_minutes);
        sleep_->timer_active = true;
        status_line_ = "sleep timer: playback pauses in " + std::to_string(sleep_->timer_minutes) + " min";
    } else if (row == 5) {
        if (!has_track_) {
            status_line_ = "sleep timer: nothing is playing, so there is no current song to stop after";
            return;
        }
        sleep_->stop_after_track = true;
        status_line_ = settings_.play_mode == 3
            ? "sleep timer: stops after this song (Stop mode is on as well)"
            : "sleep timer: stops after this song";
    } else {
        status_line_ = "sleep timer: off";
    }
}

// Fade length of a minute timer: 10 % of the time, 30 s .. 10 min (the radio's rule).
static double sleep_fade_seconds(int minutes) { return std::clamp(minutes * 6.0, 30.0, 600.0); }

void App::sleep_timer_tick() {
    // A pending "stop after song" with nothing playing (and nothing about to
    // start) has no song left to wait for -- drop it rather than letting it
    // hit whatever track is started next.
    if (sleep_->stop_after_track && !has_track_ && !advancing_ && !load_in_progress_.load())
        sleep_->stop_after_track = false;
    if (!sleep_->timer_active) return;
    if (std::chrono::steady_clock::now() < sleep_->timer_deadline) {
        const double left = std::chrono::duration<double>(sleep_->timer_deadline - std::chrono::steady_clock::now()).count();
        const double len = sleep_fade_seconds(sleep_->timer_minutes);
        if (settings_.sleep_fade && left < len) { const double t = left / len; player_.set_fade(static_cast<float>(t * t)); }
        else player_.set_fade(1.0f);
        return;
    }
    if (has_track_ && !advancing_ && device_->play_pending_gen.load() == 0) {
        // Pause, not stop: the position survives and one press of play resumes.
        if (!player_.is_paused()) player_.pause();
        player_.set_fade(1.0f);   // the next press of play comes back at the normal volume
        sleep_->timer_active = false;
        sleep_->timer_minutes = 0;
        status_line_ = "sleep timer: playback paused";
    } else if (!has_track_ && !advancing_ && !load_in_progress_.load()) {
        // Nothing is playing any more (Stop mode, end of the list, ...).
        sleep_->timer_active = false;
        sleep_->timer_minutes = 0;
        status_line_ = "sleep timer: finished";
    }
    // else: a track is loading / being handed over -- try again next frame, so
    // the pause lands on the new track instead of racing its start.
}

std::string App::SleepState::sleep_timer_label() const {
    if (timer_active) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
            timer_deadline - std::chrono::steady_clock::now()).count();
        return "SLEEP " + fmt_countdown(left);
    }
    if (stop_after_track) return "SLEEP after song";
    return "";
}

std::vector<std::string> App::build_sleep_timer_panel() const {
    const int W = kSleepTimerPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    // "label ........ right" within `inner` columns.
    auto two = [&](bool cursor, const std::string& left, const std::string& right) {
        const std::string l = std::string(cursor ? "> " : "  ") + left;
        const int gap = std::max(1, inner - display_width(l) - display_width(right));
        return l + std::string(static_cast<size_t>(gap), ' ') + right;
    };

    long long left_secs = 0;
    if (sleep_->timer_active)
        left_secs = std::chrono::duration_cast<std::chrono::seconds>(sleep_->timer_deadline - std::chrono::steady_clock::now()).count();

    std::vector<std::string> lines;
    lines.push_back(box_top("Sleep Timer", W, border));
    for (int i = 0; i < 5; ++i) {
        const bool running = sleep_->timer_active && sleep_->timer_minutes == kSleepMinutes[i];
        lines.push_back(row(two(i == sleep_->menu_row, std::to_string(kSleepMinutes[i]) + " minutes",
                                running ? fmt_countdown(left_secs) + " left" : ""), i == sleep_->menu_row));
    }
    lines.push_back(row(two(sleep_->menu_row == 5, "Stop after current song", sleep_->stop_after_track ? "on" : ""), sleep_->menu_row == 5));
    lines.push_back(row(two(sleep_->menu_row == 6, "Fade out", settings_.sleep_fade ? "on" : "off"), sleep_->menu_row == 6));
    lines.push_back(row(two(sleep_->menu_row == 7, "Off", (!sleep_->timer_active && !sleep_->stop_after_track) ? "(no timer)" : ""), sleep_->menu_row == 7));
    lines.push_back(row(settings_.sleep_fade ? "Volume glides down over the last 10 % of" : "Playback is paused when the time is up.", false));
    lines.push_back(row(settings_.sleep_fade ? "the time (30 s - 10 min), then it pauses." : "Minute timers pause, they do not stop.", false));
    lines.push_back(centred_bottom(settings_, W, "[UP/DOWN] [ENTER] set  [ESC] close", border_bottom));
    return lines;
}

// ---------------------------------------------------------------------
// MENUS index (HKeyMenus, "<"): every menu and overlay of the player (or the audio book) as a 3D organigram over a cyberspace grid
// (organigram.h, the trees in organigram_trees.h). The arrows steer, ENTER opens the menu of a box (it closes the index and presses
// the key the menu is bound to, so a rebinding is followed) or goes down into a group.
// ---------------------------------------------------------------------
namespace {
org::Tree menu_org_tree(bool book) { return book ? org::book_tree() : org::player_tree(); }
}  // namespace

org::Theme App::menu_index_theme() const {
    org::Theme th;
    th.line = ansi_for(settings_.border_color, false);
    th.path = header_sgr(settings_);
    th.key = legend_sgr(settings_);
    // the branches on the path are in the header colour like its boxes (th.walked stays empty = `path`); everything off the path
    // is the progress bar's "remaining" colour
    th.pending = ansi_for(settings_.progress_remaining_color, false);
    th.key_text = [this](const org::Node& n) -> std::string {
        if (!n.action) return "";
        return n.action[0] == '#' ? std::string(n.fallback) : hotkey_text(n.action, n.fallback);
    };
    return th;
}

std::string App::build_menu_index_full(int W) const {
    const org::Tree tree = menu_org_tree(audiobook_);
    const int sel = org::clamp_sel(tree, menu_index_sel_);
    const org::Theme th = menu_index_theme();
    const auto key_of = th.key_text;
    const org::Node& n = tree.n[sel];
    std::string info = n.title;
    const std::string key = key_of(n);
    if (n.action && !key.empty()) info += "   [" + key + "]";
    info += n.action ? (org::children_of(tree, sel).empty() ? "   -   ENTER opens it" : "   -   ENTER opens it, DOWN goes to its menus")
                     : "   -   ENTER or DOWN goes in";
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    const int H = std::max(8, term_rows_ - 1);
    const auto rows = org::render(tree, sel, W - 4, H - 2, 0.0, th, info);
    std::ostringstream f;
    f << "\x1b[?25l";
    f << box_top(audiobook_ ? "AUDIO BOOK MENUS & OVERLAYS" : "MENUS & OVERLAYS", W, border) << "\n";
    for (const auto& r : rows) f << bar << " " << r << " " << bar << "\n";
    f << box_bottom(W, "[\xe2\x86\x90\xe2\x86\x92] Neighbours  [\xe2\x86\x91] Back  [\xe2\x86\x93] In  [ENTER] Open  [SHIFT+T] Classic  [ESC / " + hotkey_text("HKeyMenus", "<") + "] Close", border_bottom);
    return f.str();
}

// The classic view ("<", then SHIFT+t): a regular overlay over the main screen, as big as its two columns need.
std::vector<std::string> App::build_menu_index_classic_panel(int W, int& panel_w) const {
    const org::Tree tree = menu_org_tree(audiobook_);
    const org::Theme th = menu_index_theme();
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    const std::string legend = "[\xe2\x86\x91\xe2\x86\x93] Entry  [\xe2\x86\x90\xe2\x86\x92] Column  [ENTER] Open  [SHIFT+T] Tree  [ESC / " + hotkey_text("HKeyMenus", "<") + "] Close";
    const org::ClassicPanel pnl = org::render_classic(tree, menu_index_sel_, th, std::max(8, term_rows_ - 2 - 2), std::max(24, W - 6));
    const int inner = std::max(pnl.inner_w, std::min(W - 4, display_width(legend) + 2));
    panel_w = inner + 4;
    std::vector<std::string> lines;
    lines.push_back(box_top(audiobook_ ? "AUDIO BOOK MENUS & OVERLAYS" : "MENUS & OVERLAYS", panel_w, border));
    for (const auto& r : pnl.rows) lines.push_back(bar + " " + r + std::string(static_cast<size_t>(inner - pnl.inner_w), ' ') + " " + bar);
    lines.push_back(box_bottom(panel_w, legend, border_bottom));
    return lines;
}

void App::menu_index_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (!arrow && (key == 27 || key_is("HKeyMenus", key))) { mode_ = Mode::Browse; force_redraw_ = true; return; }
    const org::Tree tree = menu_org_tree(audiobook_);
    menu_index_sel_ = settings_.menu_index_classic ? org::classic_sel(tree, menu_index_sel_) : org::clamp_sel(tree, menu_index_sel_);
    if (arrow && (key == 'A' || key == 'B' || key == 'C' || key == 'D')) {   // up: back, down: in, right / left: the neighbours
        const int dir = key == 'A' ? 0 : key == 'B' ? 1 : key == 'D' ? 2 : 3;
        menu_index_sel_ = settings_.menu_index_classic ? org::move_classic(tree, menu_index_sel_, dir) : org::move(tree, menu_index_sel_, dir);
        return;
    }
    if (!arrow && key == 'T') {   // SHIFT+t: the tree or the classic category boxes (the last view is kept)
        settings_.menu_index_classic = !settings_.menu_index_classic;
        set_config_value("MenuIndexClassic", settings_.menu_index_classic ? "true" : "false");
        if (settings_.menu_index_classic) menu_index_sel_ = org::classic_sel(tree, menu_index_sel_);
        force_redraw_ = true;   // (the tree takes the whole screen, the classic view is an overlay over the main screen)
        return;
    }
    if (!arrow && (key == '\r' || key == '\n')) {
        const org::Node& e = tree.n[menu_index_sel_];
        if (!e.action) { menu_index_sel_ = org::move(tree, menu_index_sel_, 1); return; }   // a group: go in
        const auto it = e.action[0] == '#' ? settings_.hotkeys.end() : settings_.hotkeys.find(e.action);
        mode_ = Mode::Browse;
        force_redraw_ = true;
        if (e.action[0] == '#') {   // a fixed key
            handle_key(static_cast<unsigned char>(e.fallback[0]));
        } else if (it != settings_.hotkeys.end()) {
            const int k = hotkey_string_to_key(it->second);
            if (k > 0) handle_key(k);
        }
    }
}

// "Want to clear queue?" -- small Yes/No confirmation, stamped over the live
// Browse view by draw_floating_panel() like Bulk Add / Retry Lyrics. Fixed
// size (8 rows x kClearQueuePanelWidth) so it overwrites cleanly without a clear.
std::vector<std::string> App::build_clear_queue_panel() const {
    const int W = kClearQueuePanelWidth;
    const int inner = W - 4;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;

    // Centred plain-text row. Only ever fed plain text (display_width() counts
    // escape bytes as columns), so the highlighted buttons are built below.
    auto centred = [&](const std::string& plain) {
        const int fill = std::max(0, inner - display_width(plain));
        const int left = fill / 2;
        return bar + " " + std::string(static_cast<size_t>(left), ' ') + plain +
               std::string(static_cast<size_t>(fill - left), ' ') + " " + bar;
    };

    const size_t n = queue_.size();
    const std::string yes = "  Yes  ", no = "  No  ", gap = "    ";
    const int btn_w = display_width(yes) + display_width(gap) + display_width(no);
    const int fill = std::max(0, inner - btn_w);
    const int left = fill / 2;
    const std::string buttons =
        bar + " " + std::string(static_cast<size_t>(left), ' ') +
        (clear_queue_choice_ == 0 ? HI + yes + R : yes) + gap +
        (clear_queue_choice_ == 1 ? HI + no + R : no) +
        std::string(static_cast<size_t>(fill - left), ' ') + " " + bar;

    std::vector<std::string> lines;
    lines.push_back(box_top("Clear Queue", W, border));
    lines.push_back(centred(""));
    lines.push_back(centred("Want to clear queue?"));
    lines.push_back(centred(std::to_string(n) + " track" + (n == 1 ? "" : "s") + " will be removed"));
    lines.push_back(centred(""));
    lines.push_back(buttons);
    lines.push_back(centred(""));
    lines.push_back(box_bottom(W, "[y / n]  [ENTER] confirm", border_bottom));
    return lines;
}

// Ctrl+Shift+U: "New playlist / Existing playlist"; the second step has a search field and the list of saved playlists.
std::vector<std::string> App::build_queue_to_playlist_panel() const {
    const int W = kQtpPanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        return bar + " " + t + std::string(static_cast<size_t>(std::max(0, inner - display_width(t))), ' ') + " " + bar;
    };
    int skipped = 0;
    const int n = qtp_->track ? 1 : static_cast<int>(queue_local_tracks(skipped).size());
    std::vector<std::string> lines;
    if (qtp_->stage == 2) {   // SHIFT+a, a new playlist: its name
        lines.push_back(box_top("New playlist", W, border));
        lines.push_back(row(""));
        lines.push_back(row("Add \"" + atp_track_.title + "\" to a new playlist."));
        lines.push_back(row(""));
        {
            const std::string prefix = "Name: ";
            const int field_w = std::max(1, inner - static_cast<int>(prefix.size()));
            const bool typed = !qtp_->new_name.empty();
            EditPaint p = paint_edit_field(qtp_->new_name, edit_caret_, edit_anchor_, field_w, typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
            lines.push_back(box_line_field(prefix, search_run(settings_, p.s, typed), p.cols, W, border, search_idle_sgr(settings_)));
        }
        lines.push_back(row(""));
        lines.push_back(box_bottom(W, "[ENTER] Add + save  [ESC] Back", border_bottom));
        return lines;
    }
    if (qtp_->stage == 0) {
        auto centred = [&](const std::string& plain) {
            const int fill = std::max(0, inner - display_width(plain));
            return bar + " " + std::string(static_cast<size_t>(fill / 2), ' ') + plain + std::string(static_cast<size_t>(fill - fill / 2), ' ') + " " + bar;
        };
        const std::string a = "  New playlist  ", b = "  Existing playlist  ", gap = "    ";
        const int btn_w = display_width(a) + display_width(gap) + display_width(b);
        const int fill = std::max(0, inner - btn_w);
        lines.push_back(box_top(qtp_->track ? "Track to playlist" : "Queue to playlist", W, border));
        lines.push_back(centred(""));
        lines.push_back(centred(qtp_->track ? truncate_str("Add \"" + atp_track_.title + "\" to:", inner)
                                           : "Add the " + std::to_string(n) + " track" + (n == 1 ? "" : "s") + " of the queue to:"));
        lines.push_back(centred(""));
        lines.push_back(bar + " " + std::string(static_cast<size_t>(fill / 2), ' ') + (qtp_->choice == 0 ? HI + a + R : a) + gap
                        + (qtp_->choice == 1 ? HI + b + R : b) + std::string(static_cast<size_t>(fill - fill / 2), ' ') + " " + bar);
        lines.push_back(centred(""));
        lines.push_back(box_bottom(W, "[n / e]  [ENTER] choose  [ESC] Close", border_bottom));
        return lines;
    }
    lines.push_back(box_top("Existing playlist", W, border));
    {
        const std::string prefix = "Search: ";
        const int field_w = std::max(1, inner - static_cast<int>(prefix.size()));
        const bool typed = !qtp_->query.empty();
        EditPaint p = paint_edit_field(qtp_->query, edit_caret_, edit_anchor_, field_w, typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
        lines.push_back(box_line_field(prefix, search_run(settings_, p.s, typed), p.cols, W, border, search_idle_sgr(settings_)));
    }
    const int total = static_cast<int>(qtp_->view.size());
    const int scroll = std::clamp(qtp_->selected - kQtpListRows / 2, 0, std::max(0, total - kQtpListRows));
    for (int r = 0; r < kQtpListRows; ++r) {
        const int idx = scroll + r;
        if (idx >= total) { lines.push_back(row(total == 0 && r == 0 ? "(no saved playlists match)" : "")); continue; }
        const std::string cnt = std::to_string(qtp_->view[static_cast<size_t>(idx)].track_count) + " tracks";
        const int name_w = std::max(1, inner - display_width(cnt) - 2);
        std::string content = pad_right(truncate_str(qtp_->view[static_cast<size_t>(idx)].name, name_w), name_w) + "  " + cnt;
        content = pad_right(truncate_str(content, inner), inner);
        if (idx == qtp_->selected) lines.push_back(bar + " " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + content + R + " " + bar);
        else lines.push_back(bar + " " + content + " " + bar);
    }
    lines.push_back(box_bottom(W, total > kQtpListRows ? std::to_string(total) + " playlists  [\u2191\u2193] [ENTER] Add  [ESC] Back" : "[\u2191\u2193] [ENTER] Add  [ESC] Back", border_bottom));
    return lines;
}

// "A newer version is out": version, the version in use and the address of the release page; [ESC] closes, [o] opens it.
std::vector<std::string> App::build_update_panel() const {
    const int W = kUpdatePanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border + settings_.box_vertical + "\x1b[0m";
    auto row = [&](const std::string& plain) {
        const std::string t = truncate_str(plain, inner);
        return bar + " " + t + std::string(static_cast<size_t>(std::max(0, inner - display_width(t))), ' ') + " " + bar;
    };
    std::string ver = update_latest_;
    while (!ver.empty() && (ver[0] == 'v' || ver[0] == 'V')) ver.erase(0, 1);
    std::string shown_url = update_url_;
    if (shown_url.compare(0, 8, "https://") == 0) shown_url.erase(0, 8);
    std::vector<std::string> lines;
    lines.push_back(box_top("New version", W, border));
    lines.push_back(row(""));
    lines.push_back(row("Mousiki " + ver + " is out."));
    lines.push_back(row("You are using " + update_current_ + "."));
    lines.push_back(row(""));
    lines.push_back(row(shown_url));
    lines.push_back(row(""));
    for (const std::string& l : wrap_lines("Your configuration stays the same when you update by installing the newer version. New commands get their default key; if you already use that key for something else, yours is kept and the new command is left unbound (set it under Settings > REFERENCE).", inner, 6))
        lines.push_back(row(l));
    lines.push_back(row(""));
    for (const std::string& l : wrap_lines("Note for those using portable versions: unzip the new version over the old folder, or copy your data folder into the new one, and keep portable.txt next to the launcher. Without portable.txt your configs are not found.", inner, 6))
        lines.push_back(row(l));
    lines.push_back(row(""));
    lines.push_back(box_bottom(W, "[ESC] Close  [o] Go to release", border_bottom));
    return lines;
}

std::vector<std::string> App::build_retry_lyrics_panel() const {
    const int W = kRetryLyricsPanelWidth; // 62, matches the reference design
    const int label_w = 14;               // left label column, blank on box top/bottom rows
    const int box_w = W - 2 - label_w;    // 46 -- nested input box width
    std::string border = ansi_for(settings_.border_color, false);
    std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    auto wrap = [&](const std::string& left, const std::string& right) { return obar + left + right + obar; };
    auto label = [&](const std::string& text) { return pad_left(text, label_w - 2) + " :"; };
    auto blank_label = [&] { return std::string(label_w, ' '); };

    // A text field: label appears only on the middle (content) row, the
    // nested box's own top/bottom rows get a blank label column.
    // `placeholder` shows only when the field is both empty AND not
    // currently focused -- e.g. REMIX/OTHER's "NOT SELECTED". While
    // actively editing an empty field, show just the cursor rather than
    // the placeholder text glued to it (which would otherwise look like
    // "NOT SELECTED" was real, already-typed content).
    auto text_field = [&](RLField f, const std::string& lbl, const std::string& value,
                           const std::string& placeholder, std::vector<std::string>& out) {
        out.push_back(wrap(blank_label(), box_top("", box_w, border)));
        bool focused = (rl_->focus == f);
        if (focused) {   // caret, marked range and block cursor from the shared single-line editor
            const bool mine = edit_owner_ == rl_owner(f);
            const size_t cc = mine ? std::min(edit_caret_, value.size()) : value.size();
            const size_t aa = mine ? std::min(edit_anchor_, value.size()) : value.size();
            const EditPaint p = paint_edit_field(value, cc, aa, std::max(4, box_w - 4), "", true);
            out.push_back(wrap(label(lbl), box_line_field("", p.s, p.cols, box_w, border)));
        } else {
            out.push_back(wrap(label(lbl), box_line((value.empty() ? placeholder : value), box_w, border)));
        }
        out.push_back(wrap(blank_label(), box_bottom(box_w, "", border)));
    };

    std::vector<std::string> lines;
    lines.push_back(box_top("LYRICS REFETCH AND VERSION CONTROL", W, border));

    {
        const int inner = W - 2;
        lines.push_back(obar + border + pad_right(" RETRY WITH ANOTHER TITLE / ARTIST", inner) + "\x1b[0m" + obar);
    }

    text_field(RLField::Title, "SONG TITLE", rl_->title, "", lines);
    text_field(RLField::Artist, "ARTIST NAME", rl_->artist, "", lines);
    text_field(RLField::Ft, "FT  ( opt )", rl_->ft, "", lines);

    // TYPE: two rows of plain (non-boxed) checkbox-style toggles.
    // Slowed/Ultra Slowed/Spedup are a radio group (only one can show
    // "[o]" at a time); Reverb/Remix/Other are independent.
    //
    // Built and measured as plain ASCII first, then the focus highlight
    // is spliced in by byte offset *after* pad_right/truncate_str has
    // already run -- same reasoning as the bulk-add hover fix above:
    // those two don't skip ANSI escapes when counting width, so
    // wrapping an option in "\x1b[7m" before truncating/padding the row
    // risks slicing through the reset code and leaking reverse-video
    // onto the rest of the panel. Every word here is plain ASCII, so a
    // byte offset is also a column offset -- no UTF-8 width subtleties
    // to worry about.
    auto opt_plain = [&](bool checked, const std::string& word) { return (checked ? "[o] " : " o  ") + word; };
    auto type_row = [&](std::initializer_list<std::tuple<RLField, bool, std::string>> opts) {
        std::string plain = " ";
        int focus_start = -1, focus_len = 0;
        for (const auto& [f, checked, word] : opts) {
            std::string s = opt_plain(checked, word);
            if (rl_->focus == f) { focus_start = static_cast<int>(plain.size()); focus_len = static_cast<int>(s.size()); }
            plain += s;
            plain += " ";
        }
        std::string padded = pad_right(truncate_str(plain, box_w), box_w);
        if (focus_start >= 0 && focus_start + focus_len <= static_cast<int>(padded.size())) {
            padded.insert(static_cast<size_t>(focus_start + focus_len), "\x1b[0m");
            padded.insert(static_cast<size_t>(focus_start), "\x1b[7m");
        }
        return padded;
    };
    lines.push_back(wrap(label("TYPE"),
        type_row({{RLField::TypeReverb, rl_->reverb, "reverb"},
                  {RLField::TypeSlowed, rl_->slowed, "slowed"},
                  {RLField::TypeUltraSlowed, rl_->ultra_slowed, "ultra slowed"}})));
    lines.push_back(wrap(blank_label(),
        type_row({{RLField::TypeSpedup, rl_->spedup, "spedup"},
                  {RLField::TypeRemix, rl_->remix, "remix"},
                  {RLField::TypeOther, rl_->other, "other /pls specify"}})));

    // REMIX / OTHER: only present while their TYPE toggle is on (the panel grows and shrinks with them).
    if (rl_->remix) text_field(RLField::RemixText, "REMIX", rl_->remix_text, "NOT SELECTED", lines);
    if (rl_->other) text_field(RLField::OtherText, "OTHER", rl_->other_text, "NOT SELECTED", lines);

    // One empty line, then the note (two columns of air on each side). While [d] waits for its y / n the same two rows
    // carry the warning instead.
    {
        const int inner = W - 2;
        auto note_row = [&](const std::string& sgr, const std::string& text) {
            return obar + std::string(2, ' ') + sgr + pad_right(truncate_str(text, inner - 4), inner - 4) + "\x1b[0m" + std::string(2, ' ') + obar;
        };
        lines.push_back(obar + std::string(inner, ' ') + obar);
        if (rl_->confirm_delete && rl_->src_sel >= 0 && rl_->src_sel < static_cast<int>(rl_->versions.size())) {
            const LyricsVersion& v = rl_->versions[static_cast<size_t>(rl_->src_sel)];
            lines.push_back(note_row("\x1b[1;31m", "Delete \"" + v.label + "\" for good? This cannot be undone."));
            lines.push_back(note_row("\x1b[1;31m", "[y] yes, delete it    any other key: keep it"));
        } else {
            const std::string leg = legend_sgr(settings_);
            lines.push_back(note_row(leg, "[ENTER] fetches again; the lyrics in use are kept as an"));
            lines.push_back(note_row(leg, settings_.lyrics_on() ? "older version, so saved timing is never overwritten."
                                                               : "older version. Lyrics engine is off: see karaoke / READ LYRICS."));
        }
    }

    lines.push_back(obar + std::string(W - 2, ' ') + obar);   // the one empty line between the note and the list
    // --- the versions (below the note): lyrics written into the file, the fetched copy in use and the older ones a refetch put aside.
    // Three rows are shown; with more versions the list scrolls (arrows at the right edge say which way). ---
    {
        const int inner = W - 2;
        auto heading = [&](const std::string& text) {
            return obar + border + pad_right(" " + text, inner) + "\x1b[0m" + obar;
        };
        lines.push_back(heading("LYRICS IN USE"));
        constexpr int kShown = 3;
        const int n = static_cast<int>(rl_->versions.size());
        const int first = std::clamp(rl_->src_scroll, 0, std::max(0, n - kShown));
        for (int r = 0; r < kShown; ++r) {
            const int i = first + r;
            std::string plain;
            if (n == 0 && r == 0) plain = "   none yet -- [ENTER] fetches them";
            else if (i < n) {
                const LyricsVersion& v = rl_->versions[static_cast<size_t>(i)];
                plain = std::string(v.active ? " (o) " : " ( ) ") + v.label + (v.detail.empty() ? "" : "  " + v.detail);
            }
            std::string row = pad_right(truncate_str(plain, inner - 2), inner - 2);
            if (rl_->focus == RLField::Source && i == rl_->src_sel && i < n) row = "\x1b[7m" + row + "\x1b[0m";
            const bool more_up = (r == 0 && first > 0), more_down = (r == kShown - 1 && first + kShown < n);
            const std::string mark = more_up ? "\u25b2" : more_down ? "\u25bc" : " ";
            lines.push_back(obar + row + " " + (more_up || more_down ? border + mark + "\x1b[0m" : mark) + obar);
        }
    }

    lines.push_back(box_bottom(W, rl_->confirm_delete ? "[y] delete  [n] keep" : "[SPACE] choose  [d] delete  [CTRL+n] Add manually  [ENTER] Refetch  [ESC] cancel", border));
    return lines;
}

} // namespace muisc
