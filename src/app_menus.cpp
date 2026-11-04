// app_menus.cpp
// Part of class App (src/app.h): Small menus and floating panels: retry lyrics, oscilloscope / normalization options, lyrics timing, sleep timer, MENUS index, ...
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


// ---------------------------------------------------------------------
// Retry Lyrics (HKeyRetryLyrics, 'l') -- manual title/artist override
// ---------------------------------------------------------------------

std::vector<App::RLField> App::rl_visible_fields() const {
    std::vector<RLField> f;
    f.insert(f.end(), {
        RLField::Title, RLField::Artist, RLField::Ft,
        RLField::TypeReverb, RLField::TypeSlowed, RLField::TypeUltraSlowed,
        RLField::TypeSpedup, RLField::TypeRemix, RLField::TypeOther,
    });
    if (rl_remix_) f.push_back(RLField::RemixText);
    if (rl_other_) f.push_back(RLField::OtherText);
    if (!rl_versions_.empty()) f.push_back(RLField::Source);   // the list sits at the bottom of the overlay
    return f;
}

bool* App::rl_bool_ptr(RLField f) {
    switch (f) {
        case RLField::TypeReverb: return &rl_reverb_;
        case RLField::TypeSlowed: return &rl_slowed_;
        case RLField::TypeUltraSlowed: return &rl_ultra_slowed_;
        case RLField::TypeSpedup: return &rl_spedup_;
        case RLField::TypeRemix: return &rl_remix_;
        case RLField::TypeOther: return &rl_other_;
        default: return nullptr;
    }
}

std::string* App::rl_text_ptr(RLField f) {
    switch (f) {
        case RLField::Title: return &rl_title_;
        case RLField::Artist: return &rl_artist_;
        case RLField::Ft: return &rl_ft_;
        case RLField::RemixText: return &rl_remix_text_;
        case RLField::OtherText: return &rl_other_text_;
        default: return nullptr;
    }
}

// Pre-fills from the current track's metadata_ rather than opening
// blank, and resets every other bit of state -- so reopening after a
// previous override (or a previous cancel) never leaks stale values
// from last time.
void App::rl_open_from_current_track() {
    edit_owner_.clear();   // fresh fields: the caret starts at the end of the one in focus
    rl_title_ = metadata_.name;
    rl_artist_ = (metadata_.artist == "-") ? "" : metadata_.artist;
    rl_ft_.clear();

    // Best-effort ft./feat. split: if the title itself carries a
    // "feat."/"ft." tag, pull it out into its own field rather than
    // leaving it embedded (so it doesn't get double-appended once the
    // TYPE tags get tacked on after the title at submit time).
    static const std::vector<std::string> markers = {" feat. ", " feat ", " ft. ", " ft "};
    std::string lower_title = ascii_lower_str(metadata_.name);
    for (const auto& marker : markers) {
        size_t pos = lower_title.find(marker);
        if (pos != std::string::npos) {
            rl_title_ = metadata_.name.substr(0, pos);
            rl_ft_ = metadata_.name.substr(pos + marker.size());
            // trim a trailing ')' if the split landed inside "(feat. X)"
            if (!rl_ft_.empty() && rl_ft_.back() == ')') rl_ft_.pop_back();
            while (!rl_title_.empty() && (rl_title_.back() == ' ' || rl_title_.back() == '(')) rl_title_.pop_back();
            break;
        }
    }

    rl_remix_text_.clear();
    rl_other_text_.clear();
    rl_reverb_ = rl_slowed_ = rl_ultra_slowed_ = rl_spedup_ = rl_remix_ = rl_other_ = false;
    rl_focus_ = RLField::Title;
    rl_versions_ = list_lyrics_versions(current_path_);
    rl_src_sel_ = 0;
    for (size_t i = 0; i < rl_versions_.size(); ++i) if (rl_versions_[i].active) { rl_src_sel_ = static_cast<int>(i); break; }
    rl_confirm_delete_ = false;
    rl_src_scroll_ = 0;
    rl_fix_scroll();
}

// Over READ LYRICS and over the karaoke screen: the same overlay, for the track that plays (the lyrics engine belongs to it).
// Closing returns to the screen it came from; the reader loads the new lyrics when the fetch is done (readlyrics_poll()).
void App::rl_open_over_reader() {
    if (!readlyrics_track_plays()) { rd_status_ = "re-fetch: only for the track that plays -- start it with [p]"; rd_status_warn_ = true; return; }
    rl_open_from_current_track();
    rl_from_reader_ = true;
    rl_from_karaoke_ = false;
    rd_watch_epoch_ = lyrics_epoch_.load();
    mode_ = Mode::RetryLyrics;
}

void App::rl_open_over_karaoke() {
    if (!has_track_) return;
    rl_open_from_current_track();
    rl_from_reader_ = false;
    rl_from_karaoke_ = true;
    mode_ = Mode::RetryLyrics;
}

void App::rl_close() {
    mode_ = rl_from_reader_ ? Mode::ReadLyrics : rl_from_karaoke_ ? Mode::Karaoke : Mode::Browse;
    rl_from_reader_ = false;
    rl_from_karaoke_ = false;
    force_redraw_ = true;
}

void App::rl_fix_scroll() {
    const int n = static_cast<int>(rl_versions_.size());
    rl_src_sel_ = std::clamp(rl_src_sel_, 0, std::max(0, n - 1));
    rl_src_scroll_ = std::clamp(rl_src_scroll_, 0, std::max(0, n - 3));
    if (rl_src_sel_ < rl_src_scroll_) rl_src_scroll_ = rl_src_sel_;
    if (rl_src_sel_ > rl_src_scroll_ + 2) rl_src_scroll_ = rl_src_sel_ - 2;
}

// [d] + y on the list: the highlighted saved copy is deleted. When it was the copy in use, the lyrics are reloaded
// without the network (the lyrics embedded in the file if there are any, otherwise none) -- a deliberate delete must
// not bring the same lyrics straight back.
void App::rl_delete_version() {
    if (rl_src_sel_ < 0 || rl_src_sel_ >= static_cast<int>(rl_versions_.size())) return;
    const LyricsVersion v = rl_versions_[static_cast<size_t>(rl_src_sel_)];
    if (!delete_lyrics_version(v)) { status_line_ = "could not delete those lyrics"; return; }
    rl_versions_ = list_lyrics_versions(current_path_);
    rl_fix_scroll();
    if (v.active) {
        bool embedded = false;
        for (const auto& x : rl_versions_) if (x.kind == LyricsVersion::Kind::Embedded) embedded = true;
        if (embedded) {
            std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
            launch_lyrics_fetch(metadata_.name, artist, current_path_);
        } else {
            ++lyrics_epoch_;
            std::lock_guard<std::mutex> lock(lyrics_mutex_);
            lyrics_result_ = LyricsResult();
            lyrics_result_.status = LyricsStatus::NotFound;
            lyrics_result_.message = "no lyrics";
            lyrics_path_ = current_path_;
            lyrics_ready_ = true;
        }
    }
    status_line_ = "lyrics deleted";
    log_event("lyrics: deleted " + v.label);
}

// SPACE on the version list: the highlighted version becomes the one that loads for this track. The copy that was in use
// is put aside as an older version, so every switch can be undone.
void App::rl_choose_version() {
    if (rl_src_sel_ < 0 || rl_src_sel_ >= static_cast<int>(rl_versions_.size())) return;
    const LyricsVersion v = rl_versions_[static_cast<size_t>(rl_src_sel_)];
    if (v.active) { status_line_ = "those lyrics are in use already"; return; }
    if (!activate_lyrics_version(current_path_, v)) { status_line_ = "could not switch the lyrics (is the lyrics folder writable?)"; return; }
    rl_versions_ = list_lyrics_versions(current_path_);
    rl_src_sel_ = 0;
    for (size_t i = 0; i < rl_versions_.size(); ++i) if (rl_versions_[i].active) { rl_src_sel_ = static_cast<int>(i); break; }
    rl_fix_scroll();
    std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
    launch_lyrics_fetch(metadata_.name, artist, current_path_);   // loads whatever is in use now (no network)
    log_event("lyrics: using " + v.label);
}

// Builds "title [ft. X] [tags...]" and launches the override fetch --
// this is the actual point of the whole form: feeding a corrected
// query into fetch_synced_lyrics() instead of the track's real
// metadata, for tracks whose auto-fetched lyrics are wrong/missing.
void App::rl_submit() {
    std::string query = rl_title_;
    if (!rl_ft_.empty()) query += " ft. " + rl_ft_;

    // Tags go after the title (per your note: "you usually need to place
    // them after title in url"). Slowed/Ultra Slowed/Spedup are mutually
    // exclusive so at most one of these three contributes; Reverb/Remix/
    // Other are independent and can stack with it and each other.
    if (rl_slowed_) query += " slowed";
    else if (rl_ultra_slowed_) query += " ultra slowed";
    else if (rl_spedup_) query += " sped up";
    if (rl_reverb_) query += " reverb";
    if (rl_remix_) query += rl_remix_text_.empty() ? " remix" : (" " + rl_remix_text_ + " remix");
    if (rl_other_ && !rl_other_text_.empty()) query += " " + rl_other_text_;

    launch_lyrics_fetch(query, rl_artist_, current_path_, /*force_network=*/true);
    log_event("retrying lyrics: \"" + query + "\"");

    rl_close();
}

// Oscilloscope tuning overlay (SHIFT+o). The rows come from osci_visible_rows() (osci_settings.cpp): which ones are
// shown depends on the style (braille / image). The ranges match what settings.cpp clamps on load.
void App::osci_menu_adjust(int dir) {
    const std::vector<int> rows = osci_visible_rows(settings_);
    if (rows.empty()) return;
    osci_menu_row_ = std::clamp(osci_menu_row_, 0, static_cast<int>(rows.size()) - 1);
    const int id = rows[osci_menu_row_];
    osci_adjust(settings_, id, dir);
    // the style row changes which rows exist: keep the cursor on the same one
    const std::vector<int> now = osci_visible_rows(settings_);
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
    const std::vector<int> rows = osci_visible_rows(settings_);
    std::vector<std::string> lines;
    lines.push_back(box_top("Oscilloscope", W, border));
    for (size_t i = 0; i < rows.size(); ++i) {
        const bool sel = static_cast<int>(i) == osci_menu_row_;
        const std::string plain = std::string(sel ? "> " : "  ") + pad_right(osci_row_label(rows[i]), 22) +
                                  pad_left(osci_row_value(settings_, rows[i]), inner - 24);
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
    std::lock_guard<std::mutex> lk(lyrics_mutex_);
    if (!settings_.lyrics_on() || !has_track_ || !lyrics_ready_ || lyrics_result_.lines.empty()) {
        status_line_ = "lyrics timing: no synced lyrics are loaded for this track";
        return;
    }
    lyrics_edit_orig_delay_ = lyrics_result_.delay;
    lyrics_edit_from_karaoke_ = false;
    lyrics_edit_from_reader_ = false;
    mode_ = Mode::LyricsEdit;
}

// Over READ LYRICS: the same overlay, for the lyrics of the track that plays (the reader's lines light up with the new offset while it is
// open). The lyrics engine is started for the track like the karaoke screen does, so the main screen's lyrics field may be off.
void App::lyrics_edit_open_over_reader() {
    if (!readlyrics_track_plays()) { rd_status_ = "lyrics timing: only for the track that plays -- start it with [p]"; rd_status_warn_ = true; return; }
    karaoke_ensure_lyrics();
    std::lock_guard<std::mutex> lk(lyrics_mutex_);
    if (!lyrics_ready_ || lyrics_path_ != current_path_ || lyrics_result_.lines.empty()) {
        rd_status_ = "lyrics timing: no synced lyrics are loaded for this track (yet)";
        rd_status_warn_ = true;
        return;
    }
    lyrics_edit_orig_delay_ = lyrics_result_.delay;
    lyrics_edit_from_karaoke_ = false;
    lyrics_edit_from_reader_ = true;
    mode_ = Mode::LyricsEdit;
}

// Over the karaoke screen. The karaoke overlay runs the lyrics engine itself, so the lyrics field of the main screen may be off.
void App::lyrics_edit_open_over_karaoke() {
    std::lock_guard<std::mutex> lk(lyrics_mutex_);
    if (!has_track_ || !lyrics_ready_ || lyrics_path_ != current_path_ || lyrics_result_.lines.empty()) {
        status_line_ = "lyrics timing: no synced lyrics are loaded for this track";
        return;
    }
    lyrics_edit_orig_delay_ = lyrics_result_.delay;
    lyrics_edit_from_karaoke_ = true;
    lyrics_edit_from_reader_ = false;
    mode_ = Mode::LyricsEdit;
}

// Back to the screen it was opened from: the main screen, or the karaoke screen.
void App::lyrics_edit_close() {
    mode_ = lyrics_edit_from_reader_ ? Mode::ReadLyrics : lyrics_edit_from_karaoke_ ? Mode::Karaoke : Mode::Browse;
    lyrics_edit_from_karaoke_ = false;
    lyrics_edit_from_reader_ = false;
    force_redraw_ = true;
}

void App::lyrics_edit_adjust(double step) {
    std::lock_guard<std::mutex> lk(lyrics_mutex_);
    // Round to hundredths so repeated presses never accumulate float drift.
    const double next = std::round((lyrics_result_.delay + step) * 100.0) / 100.0;
    lyrics_result_.delay = std::clamp(next, -120.0, 120.0);
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
        std::lock_guard<std::mutex> lk(lyrics_mutex_);
        lines = lyrics_result_.lines;
        delay = lyrics_result_.delay;
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
    sleep_timer_active_ = false;
    sleep_timer_minutes_ = 0;
    sleep_stop_after_track_ = false;
}

void App::sleep_timer_apply(int row) {
    // The two kinds of timer are mutually exclusive: whatever was armed is
    // replaced (picking the running minute entry again simply restarts it).
    sleep_timer_cancel();
    if (row >= 0 && row < 5) {
        sleep_timer_minutes_ = kSleepMinutes[row];
        sleep_timer_deadline_ = std::chrono::steady_clock::now() + std::chrono::minutes(sleep_timer_minutes_);
        sleep_timer_active_ = true;
        status_line_ = "sleep timer: playback pauses in " + std::to_string(sleep_timer_minutes_) + " min";
    } else if (row == 5) {
        if (!has_track_) {
            status_line_ = "sleep timer: nothing is playing, so there is no current song to stop after";
            return;
        }
        sleep_stop_after_track_ = true;
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
    if (sleep_stop_after_track_ && !has_track_ && !advancing_ && !load_in_progress_.load())
        sleep_stop_after_track_ = false;
    if (!sleep_timer_active_) return;
    if (std::chrono::steady_clock::now() < sleep_timer_deadline_) {
        const double left = std::chrono::duration<double>(sleep_timer_deadline_ - std::chrono::steady_clock::now()).count();
        const double len = sleep_fade_seconds(sleep_timer_minutes_);
        if (settings_.sleep_fade && left < len) { const double t = left / len; player_.set_fade(static_cast<float>(t * t)); }
        else player_.set_fade(1.0f);
        return;
    }
    if (has_track_ && !advancing_ && device_play_pending_gen_.load() == 0) {
        // Pause, not stop: the position survives and one press of play resumes.
        if (!player_.is_paused()) player_.pause();
        player_.set_fade(1.0f);   // the next press of play comes back at the normal volume
        sleep_timer_active_ = false;
        sleep_timer_minutes_ = 0;
        status_line_ = "sleep timer: playback paused";
    } else if (!has_track_ && !advancing_ && !load_in_progress_.load()) {
        // Nothing is playing any more (Stop mode, end of the list, ...).
        sleep_timer_active_ = false;
        sleep_timer_minutes_ = 0;
        status_line_ = "sleep timer: finished";
    }
    // else: a track is loading / being handed over -- try again next frame, so
    // the pause lands on the new track instead of racing its start.
}

std::string App::sleep_timer_label() const {
    if (sleep_timer_active_) {
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(
            sleep_timer_deadline_ - std::chrono::steady_clock::now()).count();
        return "SLEEP " + fmt_countdown(left);
    }
    if (sleep_stop_after_track_) return "SLEEP after song";
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
    if (sleep_timer_active_)
        left_secs = std::chrono::duration_cast<std::chrono::seconds>(sleep_timer_deadline_ - std::chrono::steady_clock::now()).count();

    std::vector<std::string> lines;
    lines.push_back(box_top("Sleep Timer", W, border));
    for (int i = 0; i < 5; ++i) {
        const bool running = sleep_timer_active_ && sleep_timer_minutes_ == kSleepMinutes[i];
        lines.push_back(row(two(i == sleep_menu_row_, std::to_string(kSleepMinutes[i]) + " minutes",
                                running ? fmt_countdown(left_secs) + " left" : ""), i == sleep_menu_row_));
    }
    lines.push_back(row(two(sleep_menu_row_ == 5, "Stop after current song", sleep_stop_after_track_ ? "on" : ""), sleep_menu_row_ == 5));
    lines.push_back(row(two(sleep_menu_row_ == 6, "Fade out", settings_.sleep_fade ? "on" : "off"), sleep_menu_row_ == 6));
    lines.push_back(row(two(sleep_menu_row_ == 7, "Off", (!sleep_timer_active_ && !sleep_stop_after_track_) ? "(no timer)" : ""), sleep_menu_row_ == 7));
    lines.push_back(row(settings_.sleep_fade ? "Volume glides down over the last 10 % of" : "Playback is paused when the time is up.", false));
    lines.push_back(row(settings_.sleep_fade ? "the time (30 s - 10 min), then it pauses." : "Minute timers pause, they do not stop.", false));
    lines.push_back(centred_bottom(settings_, W, "[UP/DOWN] [ENTER] set  [ESC] close", border_bottom));
    return lines;
}

// ---------------------------------------------------------------------
// MENUS index (HKeyMenus, "<"): every menu and overlay of the player by category, with its key. A floating panel in two
// columns; ENTER closes it and presses the key of the highlighted entry (as bound now), so a rebinding is followed.
// ---------------------------------------------------------------------
namespace {
#if defined(__APPLE__)
constexpr const char* kLyricsTimingKey = "J";       // the default on a Mac (settings.cpp)
constexpr const char* kCoverFetchKey = "Q";
constexpr const char* kVideoFetchKey = "D";
#else
constexpr const char* kLyricsTimingKey = "ALT+L";
constexpr const char* kCoverFetchKey = "ALT+C";
constexpr const char* kVideoFetchKey = "ALT+V";
#endif
struct MenuIdxEntry { const char* cat; const char* title; const char* action; const char* fallback; };
// (SYSTEM first: it is the top left corner of the index)
const MenuIdxEntry kMenuIdxPlayer[] = {
    {"SYSTEM",           "Settings",                  "HKeySetting",        "s"},
    {"SYSTEM",           "Console / logs / status",   "HKeyConsole",        "t"},
    {"SYSTEM",           "Help wizard",               "HKeyWizard",         "W"},
    {"SYSTEM",           "Cheat sheet",               "HKeyCheatsheet",     "?"},
    {"LYRICS",           "Karaoke",                   "HKeyKaraoke",        "k"},
    {"LYRICS",           "Video overlay",            "HKeyVideo",          "&"},
    {"LYRICS",           "Fetch videos",             "HKeyVideoFetch",     kVideoFetchKey},
    {"LYRICS",           "Read lyrics",               "HKeyReadLyrics",     "CTRL+L"},
    {"LYRICS",           "Lyrics timing",             "HKeyLyricsTiming",   kLyricsTimingKey},
    {"LYRICS",           "Retry given lyrics",              "HKeyRetryLyrics",    "l"},
    {"LISTS & LIBRARY",  "Big list overlay",          "HKeyListOverlay",    "L"},
    {"LISTS & LIBRARY",  "Big queue overlay",         "HKeyQueueOverlay",   "K"},
    {"LISTS & LIBRARY",  "Big folder overlay",        "HKeyFolderOverlay",  "F"},
    {"LISTS & LIBRARY",  "File conversion",           "HKeyConvertOverlay", "C"},
    {"LISTS & LIBRARY",  "Playlist editor",           "HKeyPlaylist",       "P"},
    {"LISTS & LIBRARY",  "Meta data editor",          "HKeyMetaEditor",     "M"},
    {"LISTS & LIBRARY",  "Cover fetch",               "HKeyCoverFetch",     kCoverFetchKey},
    {"LISTS & LIBRARY",  "Style options (cover look)", "HKeyStyleOptions",   "S"},
    {"LISTS & LIBRARY",  "Listening history",         "HKeyHistory",        "H"},
    {"SOUND",            "Equalizer",                 "HKeyEqualizer",      "E"},
    {"SOUND",            "Normalization",             "HKeyNormMenu",       "V"},
    {"SOUND",            "Sleep timer",               "HKeySleepTimer",     "Z"},
    {"SOUND",            "Audio device",              "HKeyAudioDevice",    "°"},
    {"VISUALS",          "Oscilloscope options",      "HKeyOscMenu",        "O"},
    {"VISUALS",          "Oscilloscope full screen",  "HKeyScopeBig",       "="},
    {"VISUALS",          "Spectrogram options",       "HKeySpectroMenu",    "I"},
    {"VISUALS",          "Spectrum bars options",     "HKeyBarsMenu",       "B"},
    {"VISUALS",          "Spectrogram full screen",   "HKeySpectroFull",    "U"},
    {"VISUALS",          "Scope window",              "HKeyScopeWindow",    ")"},
    {"VISUALS",          "Spectrogram window",        "HKeySpectroWindow",  "("},
    {"CREATE",           "Creator",                   "HKeyCreator",        "^"},
};
// The audio book mode: only what it can do (the lyrics, playlist and creator entries are not there). An action starting with '#'
// is a fixed key (the fallback), not a bound one.
const MenuIdxEntry kMenuIdxBook[] = {
    {"BOOKS & NOTES",    "Library overlay",           "#",                  "S"},
    {"BOOKS & NOTES",    "Notes overlay",             "HKeyListOverlay",    "L"},
    {"BOOKS & NOTES",    "Bookmark list",             "HKeyQueueOverlay",   "K"},
    {"BOOKS & NOTES",    "New note",                  "HKeyToggleMetaOnly", "N"},
    {"BOOKS & NOTES",    "Set bookmark",              "HKeyBookmark",       "B"},
    {"BOOKS & NOTES",    "Chapter marks",             "HKeyLyricsTiming",   kLyricsTimingKey},
    {"BOOKS & NOTES",    "Tag editor",                "HKeyMetaEditor",     "M"},
    {"BOOKS & NOTES",    "Reading history",           "HKeyHistory",        "H"},
    {"BOOKS & NOTES",    "File conversion",           "HKeyConvertOverlay", "C"},
    {"SOUND",            "Equalizer",                 "HKeyEqualizer",      "E"},
    {"SOUND",            "Normalization",             "HKeyNormMenu",       "V"},
    {"SOUND",            "Sleep timer",               "HKeySleepTimer",     "Z"},
    {"SOUND",            "Audio device",              "HKeyAudioDevice",    "°"},
    {"VISUALS",          "Oscilloscope options",      "HKeyOscMenu",        "O"},
    {"VISUALS",          "Oscilloscope full screen",  "HKeyScopeBig",       "="},
    {"VISUALS",          "Spectrogram options",       "HKeySpectroMenu",    "I"},
    {"VISUALS",          "Spectrum bars options",     "HKeyBarsMenu",       "B"},
    {"VISUALS",          "Spectrogram full screen",   "HKeySpectroFull",    "U"},
    {"SYSTEM",           "Settings",                  "HKeySetting",        "s"},
    {"SYSTEM",           "Console / logs / status",   "HKeyConsole",        "t"},
    {"SYSTEM",           "Help wizard",               "HKeyWizard",         "W"},
    {"SYSTEM",           "Cheat sheet",               "HKeyCheatsheet",     "?"},
};
struct MenuIdxTable { const MenuIdxEntry* e; int n; };
MenuIdxTable menu_idx_table(bool book) {
    return book ? MenuIdxTable{kMenuIdxBook, static_cast<int>(sizeof(kMenuIdxBook) / sizeof(kMenuIdxBook[0]))}
                : MenuIdxTable{kMenuIdxPlayer, static_cast<int>(sizeof(kMenuIdxPlayer) / sizeof(kMenuIdxPlayer[0]))};
}

// Two columns of cells (entry index, or -1 for a header / blank line), split at the category boundary that balances them.
struct MenuIdxCell { int entry = -1; const char* header = nullptr; };
std::vector<std::vector<MenuIdxCell>> menu_idx_layout(bool book) {
    const MenuIdxTable tab = menu_idx_table(book);
    const MenuIdxEntry* kMenuIdx = tab.e;
    const int kMenuIdxCount = tab.n;
    std::vector<std::vector<MenuIdxCell>> cats;   // one cell list per category
    for (int i = 0; i < kMenuIdxCount; ++i) {
        if (i == 0 || std::string(kMenuIdx[i].cat) != kMenuIdx[i - 1].cat) cats.push_back({MenuIdxCell{-1, kMenuIdx[i].cat}});
        cats.back().push_back(MenuIdxCell{i, nullptr});
    }
    int total = 0;
    for (const auto& c : cats) total += static_cast<int>(c.size()) + 1;
    int best = 1, best_diff = 1 << 20, acc = 0;
    for (size_t k = 0; k + 1 < cats.size(); ++k) {
        acc += static_cast<int>(cats[k].size()) + 1;
        const int diff = std::abs(acc - (total - acc));
        if (diff < best_diff) { best_diff = diff; best = static_cast<int>(k) + 1; }
    }
    std::vector<std::vector<MenuIdxCell>> cols(2);
    for (size_t k = 0; k < cats.size(); ++k) {
        auto& col = cols[static_cast<int>(k) < best ? 0 : 1];
        if (!col.empty()) col.push_back(MenuIdxCell{});   // blank line between categories
        col.insert(col.end(), cats[k].begin(), cats[k].end());
    }
    return cols;
}
} // namespace

std::vector<std::string> App::build_menu_index_panel(int window_w) const {
    const int W = std::min(kMenuIndexPanelWidth, std::max(40, window_w));   // (a window narrower than 100 columns gets a narrower index)
    const int inner = W - 4;
    const int cw = (inner - 3) / 2, cw2 = inner - 3 - cw;   // the right column takes the odd column
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    const MenuIdxTable tab = menu_idx_table(audiobook_);
    const MenuIdxEntry* kMenuIdx = tab.e;
    const int kMenuIdxCount = tab.n;
    const auto cols = menu_idx_layout(audiobook_);
    const size_t rows = std::max(cols[0].size(), cols[1].size());
    const int sel = std::clamp(menu_index_sel_, 0, kMenuIdxCount - 1);

    // every cell keeps two empty columns on its left and right (the highlight covers them too)
    auto cell = [&](const std::vector<MenuIdxCell>& col, size_t r, int cw_full) {
        const int cw = cw_full - 4;
        if (r >= col.size()) return std::string(static_cast<size_t>(cw_full), ' ');
        const MenuIdxCell& c = col[r];
        if (c.header) return "  " + header_sgr(settings_) + pad_right(c.header, cw) + R + "  ";
        if (c.entry < 0) return std::string(static_cast<size_t>(cw_full), ' ');
        const MenuIdxEntry& e = kMenuIdx[c.entry];
        const std::string key = "[" + (e.action[0] == '#' ? std::string(e.fallback) : hotkey_text(e.action, e.fallback)) + "]";
        const std::string left = std::string(c.entry == sel ? "> " : "  ") + e.title;
        const int gap = std::max(1, cw - display_width(left) - display_width(key));
        const std::string plain = truncate_str(left + std::string(static_cast<size_t>(gap), ' ') + key, cw);
        if (c.entry == sel) return HI + "  " + pad_right(plain, cw) + "  " + R;
        const std::string lead = pad_right(left, cw - display_width(key));
        return "  " + lead + legend_sgr(settings_) + key + R + "  ";
    };

    std::vector<std::string> lines;
    lines.push_back(box_top("MENUS & OVERLAYS", W, border));
    lines.push_back(bar + " " + pad_right("", inner) + " " + bar);
    // A window with fewer rows than the index shows the part around the selected entry.
    const size_t room = static_cast<size_t>(std::max(5, term_rows_ - 2 - 4));
    size_t first = 0, last = rows;
    if (rows > room) {
        size_t sel_row = 0;
        for (int c = 0; c < 2; ++c)
            for (size_t r = 0; r < cols[c].size(); ++r) if (cols[c][r].entry == sel) sel_row = r;
        first = sel_row > room / 2 ? sel_row - room / 2 : 0;
        if (first + room > rows) first = rows - room;
        last = first + room;
    }
    for (size_t r = first; r < last; ++r)
        lines.push_back(bar + " " + cell(cols[0], r, cw) + " " + border + settings_.box_vertical + R + " " + cell(cols[1], r, cw2) + " " + bar);
    lines.push_back(bar + " " + pad_right("", inner) + " " + bar);
    lines.push_back(box_bottom(W, "[↑↓←→] Select  [ENTER] Open  [ESC / " + hotkey_text("HKeyMenus", "<") + "] Close", border_bottom));
    return lines;
}

void App::menu_index_key(int key) {
    const bool arrow = last_key_was_arrow();
    if (!arrow && (key == 27 || key_is("HKeyMenus", key))) { mode_ = Mode::Browse; return; }
    const MenuIdxTable tab = menu_idx_table(audiobook_);
    const MenuIdxEntry* kMenuIdx = tab.e;
    const int kMenuIdxCount = tab.n;
    const auto cols = menu_idx_layout(audiobook_);
    menu_index_sel_ = std::clamp(menu_index_sel_, 0, kMenuIdxCount - 1);
    int col = 0; size_t row = 0;
    for (int c = 0; c < 2; ++c)
        for (size_t r = 0; r < cols[c].size(); ++r) if (cols[c][r].entry == menu_index_sel_) { col = c; row = r; }
    auto entry_at = [&](int c, size_t r) { return r < cols[c].size() ? cols[c][r].entry : -1; };
    if (arrow && (key == 'A' || key == 'B')) {   // up / down: the previous / next entry, wrapping
        menu_index_sel_ = (menu_index_sel_ + (key == 'A' ? kMenuIdxCount - 1 : 1)) % kMenuIdxCount;
        return;
    }
    if (arrow && (key == 'C' || key == 'D')) {   // right / left: the other column, the entry nearest to this row
        const int oc = col == 0 ? 1 : 0;
        for (size_t d = 0; d < std::max(cols[0].size(), cols[1].size()); ++d) {
            if (row >= d && entry_at(oc, row - d) >= 0) { menu_index_sel_ = entry_at(oc, row - d); return; }
            if (entry_at(oc, row + d) >= 0) { menu_index_sel_ = entry_at(oc, row + d); return; }
        }
        return;
    }
    if (!arrow && (key == '\r' || key == '\n')) {   // open it: close the index, then press the key the entry is bound to
        const MenuIdxEntry& e = kMenuIdx[std::clamp(menu_index_sel_, 0, kMenuIdxCount - 1)];
        const auto it = settings_.hotkeys.find(e.action);
        mode_ = Mode::Browse;
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
    const int n = qtp_track_ ? 1 : static_cast<int>(queue_local_tracks(skipped).size());
    std::vector<std::string> lines;
    if (qtp_stage_ == 2) {   // SHIFT+a, a new playlist: its name
        lines.push_back(box_top("New playlist", W, border));
        lines.push_back(row(""));
        lines.push_back(row("Add \"" + atp_track_.title + "\" to a new playlist."));
        lines.push_back(row(""));
        {
            const std::string prefix = "Name: ";
            const int field_w = std::max(1, inner - static_cast<int>(prefix.size()));
            const bool typed = !qtp_new_name_.empty();
            EditPaint p = paint_edit_field(qtp_new_name_, edit_caret_, edit_anchor_, field_w, typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
            lines.push_back(box_line_field(prefix, search_run(settings_, p.s, typed), p.cols, W, border, search_idle_sgr(settings_)));
        }
        lines.push_back(row(""));
        lines.push_back(box_bottom(W, "[ENTER] Add + save  [ESC] Back", border_bottom));
        return lines;
    }
    if (qtp_stage_ == 0) {
        auto centred = [&](const std::string& plain) {
            const int fill = std::max(0, inner - display_width(plain));
            return bar + " " + std::string(static_cast<size_t>(fill / 2), ' ') + plain + std::string(static_cast<size_t>(fill - fill / 2), ' ') + " " + bar;
        };
        const std::string a = "  New playlist  ", b = "  Existing playlist  ", gap = "    ";
        const int btn_w = display_width(a) + display_width(gap) + display_width(b);
        const int fill = std::max(0, inner - btn_w);
        lines.push_back(box_top(qtp_track_ ? "Track to playlist" : "Queue to playlist", W, border));
        lines.push_back(centred(""));
        lines.push_back(centred(qtp_track_ ? truncate_str("Add \"" + atp_track_.title + "\" to:", inner)
                                           : "Add the " + std::to_string(n) + " track" + (n == 1 ? "" : "s") + " of the queue to:"));
        lines.push_back(centred(""));
        lines.push_back(bar + " " + std::string(static_cast<size_t>(fill / 2), ' ') + (qtp_choice_ == 0 ? HI + a + R : a) + gap
                        + (qtp_choice_ == 1 ? HI + b + R : b) + std::string(static_cast<size_t>(fill - fill / 2), ' ') + " " + bar);
        lines.push_back(centred(""));
        lines.push_back(box_bottom(W, "[n / e]  [ENTER] choose  [ESC] Close", border_bottom));
        return lines;
    }
    lines.push_back(box_top("Existing playlist", W, border));
    {
        const std::string prefix = "Search: ";
        const int field_w = std::max(1, inner - static_cast<int>(prefix.size()));
        const bool typed = !qtp_query_.empty();
        EditPaint p = paint_edit_field(qtp_query_, edit_caret_, edit_anchor_, field_w, typed ? search_sgr(settings_) : search_idle_sgr(settings_), true);
        lines.push_back(box_line_field(prefix, search_run(settings_, p.s, typed), p.cols, W, border, search_idle_sgr(settings_)));
    }
    const int total = static_cast<int>(qtp_view_.size());
    const int scroll = std::clamp(qtp_selected_ - kQtpListRows / 2, 0, std::max(0, total - kQtpListRows));
    for (int r = 0; r < kQtpListRows; ++r) {
        const int idx = scroll + r;
        if (idx >= total) { lines.push_back(row(total == 0 && r == 0 ? "(no saved playlists match)" : "")); continue; }
        const std::string cnt = std::to_string(qtp_view_[static_cast<size_t>(idx)].track_count) + " tracks";
        const int name_w = std::max(1, inner - display_width(cnt) - 2);
        std::string content = pad_right(truncate_str(qtp_view_[static_cast<size_t>(idx)].name, name_w), name_w) + "  " + cnt;
        content = pad_right(truncate_str(content, inner), inner);
        if (idx == qtp_selected_) lines.push_back(bar + " " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + content + R + " " + bar);
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
        bool focused = (rl_focus_ == f);
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

    text_field(RLField::Title, "SONG TITLE", rl_title_, "", lines);
    text_field(RLField::Artist, "ARTIST NAME", rl_artist_, "", lines);
    text_field(RLField::Ft, "FT  ( opt )", rl_ft_, "", lines);

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
            if (rl_focus_ == f) { focus_start = static_cast<int>(plain.size()); focus_len = static_cast<int>(s.size()); }
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
        type_row({{RLField::TypeReverb, rl_reverb_, "reverb"},
                  {RLField::TypeSlowed, rl_slowed_, "slowed"},
                  {RLField::TypeUltraSlowed, rl_ultra_slowed_, "ultra slowed"}})));
    lines.push_back(wrap(blank_label(),
        type_row({{RLField::TypeSpedup, rl_spedup_, "spedup"},
                  {RLField::TypeRemix, rl_remix_, "remix"},
                  {RLField::TypeOther, rl_other_, "other /pls specify"}})));

    // REMIX / OTHER: only present while their TYPE toggle is on (the panel grows and shrinks with them).
    if (rl_remix_) text_field(RLField::RemixText, "REMIX", rl_remix_text_, "NOT SELECTED", lines);
    if (rl_other_) text_field(RLField::OtherText, "OTHER", rl_other_text_, "NOT SELECTED", lines);

    // One empty line, then the note (two columns of air on each side). While [d] waits for its y / n the same two rows
    // carry the warning instead.
    {
        const int inner = W - 2;
        auto note_row = [&](const std::string& sgr, const std::string& text) {
            return obar + std::string(2, ' ') + sgr + pad_right(truncate_str(text, inner - 4), inner - 4) + "\x1b[0m" + std::string(2, ' ') + obar;
        };
        lines.push_back(obar + std::string(inner, ' ') + obar);
        if (rl_confirm_delete_ && rl_src_sel_ >= 0 && rl_src_sel_ < static_cast<int>(rl_versions_.size())) {
            const LyricsVersion& v = rl_versions_[static_cast<size_t>(rl_src_sel_)];
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
        const int n = static_cast<int>(rl_versions_.size());
        const int first = std::clamp(rl_src_scroll_, 0, std::max(0, n - kShown));
        for (int r = 0; r < kShown; ++r) {
            const int i = first + r;
            std::string plain;
            if (n == 0 && r == 0) plain = "   none yet -- [ENTER] fetches them";
            else if (i < n) {
                const LyricsVersion& v = rl_versions_[static_cast<size_t>(i)];
                plain = std::string(v.active ? " (o) " : " ( ) ") + v.label + (v.detail.empty() ? "" : "  " + v.detail);
            }
            std::string row = pad_right(truncate_str(plain, inner - 2), inner - 2);
            if (rl_focus_ == RLField::Source && i == rl_src_sel_ && i < n) row = "\x1b[7m" + row + "\x1b[0m";
            const bool more_up = (r == 0 && first > 0), more_down = (r == kShown - 1 && first + kShown < n);
            const std::string mark = more_up ? "\u25b2" : more_down ? "\u25bc" : " ";
            lines.push_back(obar + row + " " + (more_up || more_down ? border + mark + "\x1b[0m" : mark) + obar);
        }
    }

    lines.push_back(box_bottom(W, rl_confirm_delete_ ? "[y] delete  [n] keep" : "[SPACE] choose  [d] delete  [CTRL+n] Add manually  [ENTER] Refetch  [ESC] cancel", border));
    return lines;
}

} // namespace muisc
