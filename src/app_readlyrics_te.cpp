// READ LYRICS overlay (stored lyrics versions, translations, search). Split out of app.cpp.
// Member functions of App (declared in app.h); the helpers shared with app.cpp are in app_internal.h.
#include "app.h"
#include "readlyrics_state.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
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
#include <regex>
#include <sstream>
#include <thread>
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

#include "app_internal.h"

namespace muisc {

// Part of app_readlyrics.cpp, moved to its own file so that the two compile in parallel (the same App class, nothing else changed).
void App::build_readlyrics_screen(std::ostringstream& frame, int W, int target_height) {
    if (W < 60) W = 60;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const int inner = W - 4;
    auto boxed = [&](const std::string& painted) { return bar + " " + painted + " " + bar; };

    // One line of choices that scrolls sideways: the lyrics files / the translations. `sel` is highlighted.
    auto chooser = [&](const std::vector<std::string>& items, int& sel, int& first, int pane_inner) {
        const int n = static_cast<int>(items.size());
        const int avail = std::max(6, pane_inner - 4);   // the two 2-cell scroll marks
        const std::string sep_txt = " " + (settings_.list_separator.empty() ? std::string("|") : settings_.list_separator) + " ";
        const int sw = display_width(sep_txt);
        auto iw = [&](int i) { return std::min(avail, display_width(items[static_cast<size_t>(i)])); };
        sel = std::clamp(sel, 0, n - 1);
        first = std::clamp(first, 0, n - 1);
        if (sel < first) first = sel;
        auto span_to_sel = [&](int f) { int w = 0; for (int i = f; i <= sel; ++i) w += iw(i) + (i > f ? sw : 0); return w; };
        while (first < sel && span_to_sel(first) > avail) ++first;
        std::vector<int> shown;
        int used = 0;
        for (int i = first; i < n; ++i) {
            const int add = iw(i) + (i > first ? sw : 0);
            if (used + add > avail) break;
            used += add;
            shown.push_back(i);
        }
        std::string mid;
        for (size_t k = 0; k < shown.size(); ++k) {
            const int i = shown[k];
            if (k > 0) mid += border + sep_txt + R;
            const std::string txt = truncate_str(items[static_cast<size_t>(i)], avail);
            mid += (i == sel) ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + txt + R : txt;
        }
        mid += std::string(static_cast<size_t>(std::max(0, avail - used)), ' ');
        const bool more_left = first > 0;
        const bool more_right = !shown.empty() && shown.back() < n - 1;
        return border + (more_left ? "◀ " : "  ") + R + mid + border + (more_right ? " ▶" : "  ") + R;
    };

    // The two top panes sit side by side: the lyrics files left, the translations right (each one line, scrolling sideways).
    const int wL = W / 2, wR = W - wL;
    const int inL = wL - 4, inR = wR - 4;
    auto boxed_w = [&](const std::string& painted) { return bar + " " + painted + " " + bar; };
    std::string top_l, top_r, mid_l, mid_r, bot_l, bot_r;

    // --- top pane 1: ONE line with the lyrics files of the track ---
    top_l = box_top("READ LYRICS" + (rd_->pane == 0 ? focus_indicator(settings_, border) : std::string()), wL, border);
    {
        std::string line;
        if (rd_->versions.empty()) {
            line = pad_right(truncate_str("no lyrics saved yet (fetched while the track plays)", inL), inL);
        } else {
            std::vector<std::string> items;
            for (const auto& v : rd_->versions) {
                std::string t = (v.active ? "● " : "") + v.label;
                if (!v.detail.empty()) t += " (" + v.detail + ")";
                items.push_back(t);
            }
            line = chooser(items, rd_->sel, rd_->first, inL);
        }
        mid_l = boxed_w(line);
    }
    bot_l = box_bottom(wL, n_files_label(rd_->versions.size()), border_bottom);

    // --- top pane 2: ONE line with the translations saved for the chosen file ---
    const bool show = rd_->show && !rd_->tlist.empty() && rd_->trans.size() == rd_->lines.size();
    top_r = box_top("TRANSLATION" + (rd_->pane == 1 ? focus_indicator(settings_, border) : std::string()), wR, border);
    {
        std::string line;
        if (rd_->tlist.empty()) {
            line = pad_right(truncate_str(rd_->lines.empty() ? "nothing to translate" : "no translation yet -- [t] translates, [SHIFT+c] creates one", inR), inR);
        } else {
            std::vector<std::string> items;
            for (size_t i = 0; i < rd_->tlist.size(); ++i) {
                const TranslationInfo& t = rd_->tlist[i];
                items.push_back(std::string(show && static_cast<int>(i) == rd_->tsel ? "● " : "") +
                                (translate_service_is_manual(t.service) ? translate_service_name(t.service)
                                 : language_name(t.to) + (t.service.empty() ? std::string() : " (" + translate_service_name(t.service) + ")")));
            }
            line = chooser(items, rd_->tsel, rd_->tfirst, inR);
        }
        mid_r = boxed_w(line);
    }
    {
        std::string label;
        if (rd_->busy) label = "translating into " + language_name(rd_->job_to) + " with " + translate_service_name(rd_->job_svc) + " ...";
        else if (!rd_->tlist.empty()) label = std::to_string(rd_->tlist.size()) + (rd_->tlist.size() == 1 ? " translation" : " translations") + (show ? "" : " (hidden)");
        bot_r = box_bottom(wR, label, border_bottom);
    }
    frame << top_l << top_r << "\n" << mid_l << mid_r << "\n" << bot_l << bot_r << "\n";

    // --- footer first (the lyrics pane takes the rows it leaves) ---
    // The legend is a small tree table (Move / Translate / Session), three rows whatever the width; the same in both panes
    // (TAB only moves the focus indicator): the keys that act on the focused pane say so in one entry.
    std::vector<std::string> hint_lines;
    if (rd_->confirm_del && rd_->tsel >= 0 && rd_->tsel < static_cast<int>(rd_->tlist.size())) {
        hint_lines.push_back(legend_sgr(settings_) + truncate_str(" Delete the " + language_name(rd_->tlist[static_cast<size_t>(rd_->tsel)].to) + " translation (" + translate_service_name(rd_->tlist[static_cast<size_t>(rd_->tsel)].service) + ")?  [y] Yes | [any other key] No", W) + "\x1b[0m");
        hint_lines.push_back(std::string());
        hint_lines.push_back(std::string());
    } else {
        std::vector<LegendGroup> groups = {
            {"Move", {"[\u2190\u2192] Choose file/Translation\001Choose", "[TAB] Focus", "[\u2191\u2193] Scroll", "[SHIFT+\u2191\u2193] Page up/down\001Page", "[HOME/END] Top/Bottom"}},
            {"Play", {"[p] Play/Pause", "[SHIFT+\u2190\u2192] Seek 5 s\001Seek", "[f] Follow the song\001Follow", "[" + hotkey_text("HKeyLyricsTiming", "ALT+L") + "] Lyrics timing\001Timing"}},
            {"Translate", {"[t] Translate", "[SHIFT+t] Show/Hide translations\001Show/Hide", "[d/DEL] Delete translation\001Delete", "[SHIFT+c] Create","[SHIFT+e] Edit/Add translation\001Edit/Add", "[e] Export"}},
            {"Session", {"[" + hotkey_text("HKeySearch", "/") + "] Search", "[" + hotkey_text("HKeyRetryLyrics", "l") + "] Re-fetch lyrics\001Re-fetch", "[a] Add / Assign lyrics manually\001Add / Assign", "[ESC] Close"}},
        };
        const auto lrows = layout_legend_tree(groups, W - 1);
        for (size_t i = 0; i < lrows.size(); ++i) hint_lines.push_back(" " + paint_legend(settings_, lrows, i, border, 0, true));
    }
    const int legend_rows = static_cast<int>(hint_lines.size());
    const int status_rows = 0;   // no status line here: the status message and the log overlay show rd_->status (it used to change the size of the panes)
    const int budget = std::min(target_height, term_rows_ - 1);
    const int body = std::max(3, budget - 3 /*the two top panes, side by side*/ - 2 /*lyrics pane borders*/ - legend_rows - status_rows);
    // two blank rows above and below the lyrics (none on a terminal too short for them)
    const int margin = body >= 10 ? 2 : 0;
    const int text_rows = body - 2 * margin;
    const int wrap_w = std::max(10, inner - 8);   // a little air left and right: the lines are centred

    // --- lyrics pane ---
    // The track of the reader is the loaded one: its lyrics are drawn the way the lyrics engine draws them (the active line, the words that
    // are sung, the animation of Settings), the view follows the song, and the translation of the active line is lit with it.
    struct Row { std::string text; int style; bool pre = false; };   // 0 original, 1 translation, 2 blank, 3 translation of the active line; pre: painted already (wrap_w wide)
    const bool plays = readlyrics_track_plays() && rd_->timed.size() == rd_->lines.size() && !rd_->lines.empty();
    const double elapsed = plays ? player_.poll_elapsed() : 0.0;
    bool synced = false;
    for (const LyricLine& l : rd_->timed) if (l.start_time >= 0.0) { synced = true; break; }
    double offset = rd_->delay;
    if (mode_ == Mode::LyricsEdit && lyrics_.edit_from_reader) { std::lock_guard<std::mutex> lk(lyrics_.mutex); offset = lyrics_.result.delay; }   // the timing overlay is open: live
    const double lt = elapsed - offset;
    int active = -1;
    if (plays) {
        if (synced) {
            for (size_t i = 0; i < rd_->timed.size(); ++i) if (rd_->timed[i].start_time >= 0.0 && rd_->timed[i].start_time <= lt) active = static_cast<int>(i);
        } else if (total_sec_ > 0) {   // plain lyrics: spread over the track
            active = std::clamp(static_cast<int>(elapsed / static_cast<double>(total_sec_) * static_cast<double>(rd_->lines.size())), 0, static_cast<int>(rd_->lines.size()) - 1);
        }
    }
    Settings ks = settings_;   // centred whole-line look, as in the karaoke view
    ks.lyrics_alignment = 0;
    std::vector<Row> rows;
    int active_row = -1;
    for (size_t i = 0; i < rd_->lines.size(); ++i) {
        if (rd_->lines[i].empty()) { rows.push_back({std::string(), 2}); continue; }
        const bool is_active = plays && static_cast<int>(i) == active;
        if (is_active) active_row = static_cast<int>(rows.size());
        if (plays) {
            LyricLine ll = rd_->timed[i];
            if (translit_enabled()) { ll.full_text = transliterate(ll.full_text); for (auto& w : ll.words) w.second = transliterate(w.second); }
            const bool timed = synced && ll.start_time >= 0.0;
            for (auto& w : render_lyric_line_wrapped(ll, timed ? lt : 1e12, wrap_w, is_active, ks)) rows.push_back({w, 0, true});
        } else {
            for (auto& w : wrap_lines(shown_text(rd_->lines[i]), wrap_w, 8)) rows.push_back({w, 0});
        }
        if (show) {
            const std::string& t = rd_->trans[i];
            if (!t.empty() && ascii_lower_str(t) != ascii_lower_str(rd_->lines[i]))
                for (auto& w : wrap_lines("(" + shown_text(t) + ")", wrap_w, 8)) rows.push_back({w, is_active ? 3 : 1});
        }
    }
    if (rd_->lines.empty()) {
        rows.push_back({rd_->versions.empty() ? "no lyrics saved for this track" : "this file has no readable lyrics", 2});
    }
    if (plays && rd_->follow && active_row >= 0) rd_->scroll = std::max(0, active_row - text_rows / 2 + 1);   // (clamped below)
    const int max_scroll = std::max(0, static_cast<int>(rows.size()) - text_rows);
    rd_->scroll = std::clamp(rd_->scroll, 0, max_scroll);

    std::string info;
    if (!rows.empty() && !rd_->lines.empty())
        info = "row " + std::to_string(std::min<int>(rd_->scroll + text_rows, static_cast<int>(rows.size()))) + "/" + std::to_string(rows.size());
    if (plays) info += std::string(info.empty() ? "" : "  ") + (player_.is_paused() ? "paused " : "playing ") + fmt_mmss(elapsed) + " / " + fmt_mmss(static_cast<double>(total_sec_)) +
                       (rd_->follow ? "" : "  (follow off: [f])");
    const std::string sep2 = info.empty() ? "" : "  ";
    if (show && translate_service_is_manual(rd_->svc)) info += sep2 + "translation: " + translate_service_name(rd_->svc) + " (" + language_name(rd_->to) + ")";
    else if (show) info += sep2 + "translation: " + language_name(rd_->from) + " > " + language_name(rd_->to) +
                      (rd_->svc.empty() ? std::string() : " (" + translate_service_name(rd_->svc) + ")");
    else if (rd_->same && rd_->tlist.empty()) info += sep2 + "already " + language_name(settings_.translate_to);
    else if (!rd_->tlist.empty()) info += sep2 + "translation hidden -- [SHIFT+t] shows it";

    frame << box_top(truncate_str(rd_->title, std::max(10, W - 8)), W, border) << "\n";
    const std::string orig_sgr = ansi_for(settings_.active_line_color, false);
    const std::string trans_sgr = ansi_for(settings_.inactive_line_color, false);
    for (int k = 0; k < body; ++k) {
        const int idx = rd_->scroll + k - margin;
        std::string painted;
        if (k >= margin && k < body - margin && idx >= 0 && idx < static_cast<int>(rows.size())) {
            const Row& r = rows[static_cast<size_t>(idx)];
            if (r.pre) {   // painted by the lyrics engine, wrap_w columns wide
                const int lead = std::max(0, (inner - wrap_w) / 2);
                painted = std::string(static_cast<size_t>(lead), ' ') + r.text + R + std::string(static_cast<size_t>(std::max(0, inner - lead - wrap_w)), ' ');
            } else {
                const std::string cut = truncate_str(r.text, inner);
                const int lead = std::max(0, (inner - display_width(cut)) / 2);   // centred
                const std::string t = pad_right(std::string(static_cast<size_t>(lead), ' ') + cut, inner);
                painted = r.style == 0 ? orig_sgr + t + R : r.style == 1 ? trans_sgr + t + R : r.style == 3 ? orig_sgr + t + R : t;
            }
        } else {
            painted = std::string(static_cast<size_t>(inner), ' ');
        }
        frame << boxed(painted) << "\n";
    }
    frame << box_bottom(W, truncate_str(info, std::max(4, W - 6)), border_bottom) << "\n";

    for (const auto& l : hint_lines) frame << l << "\n";
}


// --- SHIFT+e: edit a translation by hand / write a new one from scratch -------------------------------------------------
// One translation line per lyrics line, side by side with the original. A machine translation that is edited is saved as a NEW
// "Manual Translation" (the original of the service stays as it was); a manual translation that is edited is changed in place.
// The language can be chosen (TAB). The lines that are empty in the lyrics (the gaps between stanzas) are not edited.
int App::readlyrics_te_body_rows() const {
    return std::max(3, std::max(20, term_rows_ - 1) - kTeLegendRows - 5);   // top border, header, column titles, status, bottom border
}

void App::readlyrics_te_step(int dir) {
    int i = rd_->te_row;
    const int n = static_cast<int>(rd_->te_lines.size());
    for (i += dir; i >= 0 && i < n; i += dir) if (!readlyrics_te_blank(static_cast<size_t>(i))) { rd_->te_row = i; return; }
}

// SHIFT+c: creates an EMPTY translation file for the chosen lyrics file (a "Manual Translation", into the Translate To language) and
// selects it; [SHIFT+e] writes it.
void App::readlyrics_create_translation() {
    rd_->status_warn = true;
    bool any = false;
    for (size_t i = 0; i < rd_->lines.size(); ++i) if (!readlyrics_te_blank(i)) any = true;
    if (!any) { rd_->status = "there are no lyrics to translate"; return; }
    if (rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) { rd_->status = "no lyrics file is chosen"; return; }
    const LyricsVersion& v = rd_->versions[static_cast<size_t>(rd_->sel)];
    LyricsTranslation t;
    t.ok = true;
    t.service = translate_next_manual_service(rd_->tlist);
    t.lines.assign(rd_->lines.size(), std::string());
    const std::string to = settings_.translate_to;
    if (!save_lyrics_translation(rd_->track, v, to, rd_->lines, t)) { rd_->status = "could not create the translation (is the lyrics folder writable?)"; return; }
    readlyrics_refresh_translations(to, t.service);
    rd_->show = !rd_->tlist.empty();
    rd_->pane = 1;
    rd_->status_warn = false;
    rd_->status = "created an empty " + translate_service_name(t.service) + " (" + language_name(to) + ") -- [SHIFT+e] writes it";
    log_event("empty translation created: " + translate_service_name(t.service) + " " + to);
    force_redraw_ = true;
}

void App::readlyrics_te_open() {
    rd_->status_warn = true;
    bool any = false;
    for (size_t i = 0; i < rd_->lines.size(); ++i) if (!readlyrics_te_blank(i)) any = true;
    if (!any) { rd_->status = "there are no lyrics to translate"; return; }
    rd_->status_warn = false;
    rd_->te_lines.assign(rd_->lines.size(), std::string());
    rd_->te_editing = false;
    rd_->te_to = settings_.translate_to;
    rd_->te_orig_to.clear();
    rd_->te_msg.clear();
    rd_->te_warn = false;
    const bool have = rd_->tsel >= 0 && rd_->tsel < static_cast<int>(rd_->tlist.size()) && rd_->trans.size() == rd_->lines.size();
    if (have) {
        const TranslationInfo& t = rd_->tlist[static_cast<size_t>(rd_->tsel)];
        rd_->te_lines = rd_->trans;
        rd_->te_to = t.to;
        if (translate_service_is_manual(t.service)) {
            rd_->te_editing = true;
            rd_->te_svc = t.service;
            rd_->te_orig_to = t.to;
            rd_->te_msg = "changing " + translate_service_name(t.service);
        } else {
            rd_->te_svc = translate_next_manual_service(rd_->tlist);
            rd_->te_msg = "a copy of the " + translate_service_name(t.service) + " text: it is saved as a new " + translate_service_name(rd_->te_svc);
        }
    } else {
        rd_->te_svc = translate_next_manual_service(rd_->tlist);
        rd_->te_msg = "a new translation from scratch: it is saved as " + translate_service_name(rd_->te_svc);
    }
    for (size_t i = 0; i < rd_->te_lines.size(); ++i) if (readlyrics_te_blank(i)) rd_->te_lines[i].clear();
    rd_->te_row = -1;
    readlyrics_te_step(+1);
    rd_->te_mark = -1;
    rd_->te_hist.clear(); rd_->te_redo.clear(); rd_->te_kind = 0;
    rd_->te_first = 0;
    rd_->te_focus = 0;
    rd_->te_dirty = rd_->te_ask = rd_->te_new_ask = false;
    edit_owner_.clear();
    rd_->te_open = true;
    force_redraw_ = true;
}

bool App::readlyrics_te_save() {
    rd_->te_warn = true;
    bool filled = false;
    for (const std::string& l : rd_->te_lines) if (l.find_first_not_of(" \t\r") != std::string::npos) filled = true;
    if (!filled) { rd_->te_msg = "nothing is written yet -- nothing to save"; return false; }
    if (rd_->sel < 0 || rd_->sel >= static_cast<int>(rd_->versions.size())) { rd_->te_msg = "no lyrics file is chosen"; return false; }
    const LyricsVersion& v = rd_->versions[static_cast<size_t>(rd_->sel)];
    LyricsTranslation t;
    t.ok = true;
    t.service = rd_->te_svc;
    t.lines = rd_->te_lines;
    for (size_t i = 0; i < t.lines.size(); ++i) if (readlyrics_te_blank(i)) t.lines[i].clear();
    if (!save_lyrics_translation(rd_->track, v, rd_->te_to, rd_->lines, t)) { rd_->te_msg = "could not save the translation (is the lyrics folder writable?)"; return false; }
    if (rd_->te_editing && !rd_->te_orig_to.empty() && rd_->te_orig_to != rd_->te_to) delete_lyrics_translation(rd_->track, v, rd_->te_orig_to, rd_->te_svc);   // the language changed: the file moves
    const std::string svc = rd_->te_svc, to = rd_->te_to;
    rd_->te_open = false;
    rd_->te_dirty = false;
    text_disengage();
    readlyrics_refresh_translations(to, svc);
    rd_->show = !rd_->tlist.empty();
    rd_->pane = 1;
    rd_->status_warn = false;
    rd_->status = "saved: " + translate_service_name(svc) + " (" + language_name(to) + ") -- in the lyrics folder (translations)";
    log_event("manual translation saved: " + translate_service_name(svc) + " " + to);
    force_redraw_ = true;
    return true;
}

void App::readlyrics_te_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto close_editor = [&]() { rd_->te_open = false; rd_->te_ask = rd_->te_new_ask = false; text_disengage(); force_redraw_ = true; };
    if (rd_->te_ask) {   // "leave without saving?"
        if (!arrow && (key == 'y' || key == 'Y')) close_editor();
        else rd_->te_ask = false;
        return;
    }
    if (rd_->te_new_ask) {   // "start over with an empty translation?"
        if (!arrow && (key == 'y' || key == 'Y')) {
            std::fill(rd_->te_lines.begin(), rd_->te_lines.end(), std::string());
            rd_->te_svc = translate_next_manual_service(rd_->tlist);
            rd_->te_editing = false;
            rd_->te_orig_to.clear();
            rd_->te_dirty = false;
            rd_->te_row = -1;
            readlyrics_te_step(+1);
            rd_->te_mark = -1;
            rd_->te_hist.clear(); rd_->te_redo.clear(); rd_->te_kind = 0;
            edit_owner_.clear();
            rd_->te_warn = false;
            rd_->te_msg = "empty translation: it is saved as " + translate_service_name(rd_->te_svc);
        }
        rd_->te_new_ask = false;
        return;
    }
    if (key == 27) {
        if (rd_->te_mark >= 0) { rd_->te_mark = -1; rd_->te_msg.clear(); return; }   // first ESC: the marking goes
        if (rd_->te_focus == 1) { rd_->te_focus = 0; return; }
        if (rd_->te_dirty) { rd_->te_ask = true; return; }
        close_editor();
        return;
    }
    if (key == kKeyCtrlS) { if (readlyrics_te_save()) text_disengage(); return; }
    if (key == kKeyCtrlN) {
        bool filled = false;
        for (const std::string& l : rd_->te_lines) if (l.find_first_not_of(" \t\r") != std::string::npos) filled = true;
        if (!filled && !rd_->te_editing) { rd_->te_msg = "this translation is empty already"; rd_->te_warn = false; return; }
        rd_->te_new_ask = true;
        return;
    }
    // Latin letters on / off (the original column shows kana / Hangul / Chinese in Latin letters). CTRL+T works everywhere (T for transliteration; CTRL+2 is the same, but many terminals and keyboard layouts send nothing for it); the
    // " sign (the main key of it) only while the language is chosen, because in a line it is a character that can be typed.
    if (key == kKeyCtrlT || key == kKeyCtrl2 || (!arrow && key == '"' && rd_->te_focus == 1)) {
        toggle_latin_letters();
        rd_->te_msg = translit_enabled() ? "Latin letters: on -- the original is shown in Latin letters (your text is not changed)" : "Latin letters: off";
        rd_->te_warn = false;
        return;
    }
    if (!arrow && key == '\t') { rd_->te_focus = rd_->te_focus == 0 ? 1 : 0; return; }
    if (rd_->te_focus == 1) {   // the language: LEFT / RIGHT (or the first letter) choose, ENTER goes back to the lines
        if (key == '\r' || key == '\n') { rd_->te_focus = 0; return; }
        const auto& langs = translate_languages();
        int idx = 0;
        for (size_t i = 0; i < langs.size(); ++i) if (langs[i].first == rd_->te_to) idx = static_cast<int>(i);
        const int n = static_cast<int>(langs.size());
        int next = idx;
        if (arrow && (key == 'C' || key == 'B')) next = (idx + 1) % n;
        else if (arrow && (key == 'D' || key == 'A')) next = (idx + n - 1) % n;
        else if (!arrow && key < 128 && std::isalpha(key)) {
            const char want = static_cast<char>(std::tolower(key));
            for (int k = 1; k <= n; ++k) {
                const int i = (idx + k) % n;
                if (!langs[static_cast<size_t>(i)].second.empty() && std::tolower(static_cast<unsigned char>(langs[static_cast<size_t>(i)].second[0])) == want) { next = i; break; }
            }
        }
        if (next != idx) { rd_->te_to = langs[static_cast<size_t>(next)].first; rd_->te_dirty = true; rd_->te_msg.clear(); }
        return;
    }
    const int last = static_cast<int>(rd_->te_lines.size()) - 1;
    rd_->te_row = std::clamp(rd_->te_row, 0, last);
    if (arrow && last_key_was_page() && (key == 'A' || key == 'B')) {
        for (int i = 0; i < 10; ++i) readlyrics_te_step(key == 'A' ? -1 : +1);
        return;
    }
    // ---- whole lines: SHIFT+up/down marks, CTRL+c / x / v copy, cut and paste them, DEL clears them, CTRL+z undoes ----
    auto marked_from = [&]() { return rd_->te_mark < 0 ? rd_->te_row : std::min(rd_->te_mark, rd_->te_row); };
    auto marked_to = [&]() { return rd_->te_mark < 0 ? rd_->te_row : std::max(rd_->te_mark, rd_->te_row); };
    // The undo history: up to 10 states. A typed run in one line is one step; a deletion, a paste, a cut, a clear is one step each.
    auto push_snap = [&](const std::vector<std::string>& lines, int row) {
        rd_->te_hist.push_back({lines, row});
        if (rd_->te_hist.size() > 10) rd_->te_hist.erase(rd_->te_hist.begin());
        rd_->te_redo.clear();
    };
    auto remember = [&]() { push_snap(rd_->te_lines, rd_->te_row); rd_->te_kind = 0; };
    auto lines_text = [&](int a, int b) {   // one clipboard line per row, so the rows keep their place (gaps between stanzas stay empty lines)
        std::string t;
        for (int i = a; i <= b; ++i) { if (i > a) t += '\n'; if (!readlyrics_te_blank(static_cast<size_t>(i))) t += rd_->te_lines[static_cast<size_t>(i)]; }
        return t;
    };
    auto count_word = [](int n) { return std::to_string(n) + (n == 1 ? " line" : " lines"); };
    // text with several lines (clipboard) goes into the rows one after the other from `from` on; the rows that are empty in the original
    // take their line of the text but stay empty (so a text copied with its stanza gaps lines up)
    auto paste_lines = [&](int from, const std::string& clip) {
        std::vector<std::string> in;
        size_t pos = 0;
        while (true) {
            size_t nl = clip.find('\n', pos);
            std::string l = clip.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            while (!l.empty() && l.back() == '\r') l.pop_back();
            in.push_back(l);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        while (in.size() > 1 && in.back().empty()) in.pop_back();   // the line break at the very end of a copied text
        remember();
        int put = 0;
        for (size_t k = 0; k < in.size() && from + static_cast<int>(k) <= last; ++k) {
            const size_t i = static_cast<size_t>(from) + k;
            if (readlyrics_te_blank(i)) continue;
            std::string l = in[k];
            if (l.size() > 1000) l.resize(1000);
            rd_->te_lines[i] = l;
            ++put;
        }
        rd_->te_dirty = true;
        rd_->te_warn = false;
        rd_->te_msg = "pasted " + count_word(put) + " from line " + std::to_string(from + 1);
    };
    if (key == kKeyCtrlZ || key == 25) {   // undo (CTRL+z; CTRL+y is the same key on a QWERTZ keyboard)
        if (rd_->te_hist.empty()) { rd_->te_msg = "nothing to undo"; rd_->te_warn = false; return; }
        rd_->te_redo.push_back({rd_->te_lines, rd_->te_row});
        rd_->te_lines = rd_->te_hist.back().lines;
        rd_->te_row = rd_->te_hist.back().row;
        rd_->te_hist.pop_back();
        rd_->te_mark = -1;
        rd_->te_kind = 0;
        rd_->te_dirty = true;
        edit_owner_.clear();
        rd_->te_msg = "undone (" + std::to_string(rd_->te_hist.size()) + " more step" + (rd_->te_hist.size() == 1 ? "" : "s") + " can be undone; CTRL+r brings the change back)";
        rd_->te_warn = false;
        return;
    }
    if (key == 18 || key == kKeyCtrlShiftZ) {   // redo (CTRL+r, or CTRL+SHIFT+z in a terminal that reports it)
        if (rd_->te_redo.empty()) { rd_->te_msg = "nothing to redo"; rd_->te_warn = false; return; }
        rd_->te_hist.push_back({rd_->te_lines, rd_->te_row});
        rd_->te_lines = rd_->te_redo.back().lines;
        rd_->te_row = rd_->te_redo.back().row;
        rd_->te_redo.pop_back();
        rd_->te_mark = -1;
        rd_->te_kind = 0;
        rd_->te_dirty = true;
        edit_owner_.clear();
        rd_->te_msg = "redone";
        rd_->te_warn = false;
        return;
    }
    if (arrow && (key == 'A' || key == 'B') && last_key_was_shifted() && !last_key_was_page()) {   // SHIFT+up / down: mark lines
        if (rd_->te_mark < 0) rd_->te_mark = rd_->te_row;
        readlyrics_te_step(key == 'A' ? -1 : +1);
        const int n = marked_to() - marked_from() + 1;
        rd_->te_msg = count_word(n) + " marked (CTRL+c copy, CTRL+x cut, CTRL+v paste, DEL clear)";
        rd_->te_warn = false;
        edit_owner_.clear();
        return;
    }
    if (rd_->te_mark >= 0) {
        const int a = marked_from(), b = marked_to();
        if (key == kKeyCtrlC || key == kKeyCtrlX) {
            clipboard_set(lines_text(a, b));
            if (key == kKeyCtrlX) {
                remember();
                for (int i = a; i <= b; ++i) rd_->te_lines[static_cast<size_t>(i)].clear();
                rd_->te_dirty = true;
            }
            rd_->te_msg = std::string(key == kKeyCtrlX ? "cut " : "copied ") + count_word(b - a + 1);
            rd_->te_warn = false;
            rd_->te_mark = -1;
            return;
        }
        if (key == kKeyCtrlV) { paste_lines(a, clipboard_get()); rd_->te_mark = -1; rd_->te_row = a; edit_owner_.clear(); return; }
        if (key == kKeyDelete || key == 127) {
            remember();
            for (int i = a; i <= b; ++i) rd_->te_lines[static_cast<size_t>(i)].clear();
            rd_->te_dirty = true;
            rd_->te_msg = "cleared " + count_word(b - a + 1);
            rd_->te_warn = false;
            rd_->te_mark = -1;
            edit_owner_.clear();
            return;
        }
        if (key == kKeyCtrlO) {
            remember();
            int put = 0;
            for (int i = a; i <= b; ++i) if (!readlyrics_te_blank(static_cast<size_t>(i))) { rd_->te_lines[static_cast<size_t>(i)] = rd_->lines[static_cast<size_t>(i)]; ++put; }
            rd_->te_dirty = true;
            rd_->te_msg = "the original copied into " + count_word(put);
            rd_->te_warn = false;
            rd_->te_mark = -1;
            edit_owner_.clear();
            return;
        }
        rd_->te_mark = -1;   // anything else ends the marking and does what it normally does
        edit_owner_.clear();
    }
    if (arrow && key == 'A') { readlyrics_te_step(-1); return; }
    if (arrow && key == 'B') { readlyrics_te_step(+1); return; }
    if (!arrow && (key == '\r' || key == '\n')) { readlyrics_te_step(+1); return; }
    if (key == kKeyCtrlV) {   // text with line breaks (a whole translation copied from somewhere): it goes into the lines from here on
        const std::string clip = clipboard_get();
        // (text marked inside the line: the paste replaces just that text, so the line-by-line fill is for an unmarked caret only)
        const bool marked_here = edit_owner_ == "rd-te-" + std::to_string(rd_->te_row) && edit_caret_ != edit_anchor_;
        if (!marked_here && clip.find('\n') != std::string::npos) { paste_lines(rd_->te_row, clip); edit_owner_.clear(); return; }
    }
    std::string& cur = rd_->te_lines[static_cast<size_t>(rd_->te_row)];
    const std::string owner = "rd-te-" + std::to_string(rd_->te_row);
    edit_focus(owner, cur);
    if (key == kKeyCtrlO) {   // the original line as the translation (names, "oh oh oh" ...)
        remember();
        cur = rd_->lines[static_cast<size_t>(rd_->te_row)];
        edit_caret_ = edit_anchor_ = cur.size();
        rd_->te_dirty = true;
        rd_->te_msg = "the original line copied";
        rd_->te_warn = false;
        return;
    }
    std::string st;
    const std::vector<std::string> before_lines = rd_->te_lines;
    if (edit_text_key(cur, edit_caret_, edit_anchor_, key, 1000, &st)) {
        const bool typed = is_text_key(key) && !arrow;
        if (!(typed && rd_->te_kind == 1 && rd_->te_kind_row == rd_->te_row)) push_snap(before_lines, rd_->te_row);
        rd_->te_redo.clear();
        rd_->te_kind = typed ? 1 : 2;
        rd_->te_kind_row = rd_->te_row;
        rd_->te_dirty = true; rd_->te_msg.clear();
    } else {
        rd_->te_kind = 0;
        if (!st.empty()) { rd_->te_msg = st; rd_->te_warn = false; }
    }
}

std::vector<std::string> App::build_readlyrics_te_panel(int W) const {
    const int PW = std::max(70, W), inner = PW - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const std::string leg = legend_sgr(settings_);
    const std::string ink = ansi_for(settings_.list_color, false);
    std::vector<std::string> out;
    out.push_back(box_top("EDIT / ADD TRANSLATION", PW, border));
    auto row = [&](const std::string& painted, int used) { out.push_back(bar + " " + painted + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + " " + bar); };
    // header: the language (TAB chooses it) and the name the translation is saved under
    {
        const std::string lang = language_name(rd_->te_to);
        const std::string saved = "Saved as: " + translate_service_name(rd_->te_svc);
        const std::string lab = "Language: ";
        const std::string lang_txt = truncate_str(lang, std::max(4, inner / 2 - display_width(lab)));
        const std::string sh = truncate_str(saved, std::max(0, inner - display_width(lab) - display_width(lang_txt) - 3));
        const std::string lpart = rd_->te_focus == 1 ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + lang_txt + R : ink + lang_txt + R;
        row(hdr + lab + R + lpart + "   " + leg + sh + R, display_width(lab) + display_width(lang_txt) + 3 + display_width(sh));
    }
    const int half = (inner - 3) / 2;                 // the original's column; the translation gets the rest
    const int rw = inner - half - 3;
    {
        const std::string a = truncate_str("ORIGINAL", half), b = truncate_str("TRANSLATION", rw);
        row(hdr + pad_right(a, half) + R + "   " + hdr + b + R, half + 3 + display_width(b));
    }
    const int body = readlyrics_te_body_rows();
    const int n = static_cast<int>(rd_->te_lines.size());
    const int cur = std::clamp(rd_->te_row, 0, std::max(0, n - 1));
    // A line that does not fit its column is broken into several rows -- in both columns at once, so a translation always stays beside
    // its original line. (One column is kept free in the translation for the caret.)
    const int tw = std::max(1, rw - 1);
    auto wrap_at = [](const std::string& t, int w) {   // byte ranges of the rows: at a space when there is one, else in the word
        std::vector<std::pair<size_t, size_t>> r;
        const std::vector<size_t> cuts = utf8_cuts(t);
        size_t start = 0;
        while (true) {
            int cw = 0;
            size_t end = t.size(), lastsp = std::string::npos;
            bool broke = false;
            for (size_t k = 0; k + 1 < cuts.size(); ++k) {
                if (cuts[k] < start) continue;
                const size_t from = cuts[k], to = cuts[k + 1];
                const int gw = display_width(t.substr(from, to - from));
                if (cw + gw > w) {
                    end = (lastsp != std::string::npos && lastsp > start) ? lastsp : (from > start ? from : to);
                    broke = true;
                    break;
                }
                cw += gw;
                if (t[from] == ' ') lastsp = to;
            }
            r.push_back({start, end});
            if (!broke || end >= t.size()) break;
            start = end;
        }
        return r;
    };
    std::vector<std::vector<std::pair<size_t, size_t>>> orow(static_cast<size_t>(n)), trow(static_cast<size_t>(n));
    std::vector<std::string> oshown(static_cast<size_t>(n));
    std::vector<int> hgt(static_cast<size_t>(n), 1);
    for (int i = 0; i < n; ++i) {
        oshown[static_cast<size_t>(i)] = shown_text(rd_->lines[static_cast<size_t>(i)]);
        orow[static_cast<size_t>(i)] = wrap_at(oshown[static_cast<size_t>(i)], half);
        trow[static_cast<size_t>(i)] = wrap_at(rd_->te_lines[static_cast<size_t>(i)], tw);
        hgt[static_cast<size_t>(i)] = static_cast<int>(std::max(orow[static_cast<size_t>(i)].size(), trow[static_cast<size_t>(i)].size()));
    }
    int first = std::clamp(rd_->te_first, 0, std::max(0, n - 1));
    if (cur < first) first = cur;
    for (;;) {   // the caret's line (all its rows) has to be inside the window
        int used = 0;
        for (int i = first; i <= cur; ++i) used += hgt[static_cast<size_t>(i)];
        if (used <= body || first >= cur) break;
        ++first;
    }
    rd_->te_first = first;
    const bool typing = rd_->te_focus == 0 && !rd_->te_ask && !rd_->te_new_ask;
    const int mlo = rd_->te_mark < 0 ? -1 : std::min(rd_->te_mark, cur), mhi = rd_->te_mark < 0 ? -1 : std::max(rd_->te_mark, cur);
    const std::string sel = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
    int shown_rows = 0;
    for (int i = first; i < n && shown_rows < body; ++i) {
        const bool here = i == cur;
        const bool marked = i >= mlo && i <= mhi && mlo >= 0;
        const std::string& orig = oshown[static_cast<size_t>(i)];
        const std::string& tr = rd_->te_lines[static_cast<size_t>(i)];
        const bool edit_here = here && typing && !marked;
        const bool mine = edit_owner_ == "rd-te-" + std::to_string(i);
        size_t caret = mine ? std::min(edit_caret_, tr.size()) : tr.size();
        size_t anchor = mine ? std::min(edit_anchor_, tr.size()) : tr.size();
        const size_t slo = std::min(caret, anchor), shi = std::max(caret, anchor);
        for (int k = 0; k < hgt[static_cast<size_t>(i)] && shown_rows < body; ++k, ++shown_rows) {
            std::string left, right;
            int lused = 0, rused = 0;
            const std::string weight = (here || marked) ? "\x1b[1m" : std::string();
            if (k < static_cast<int>(orow[static_cast<size_t>(i)].size())) {
                const auto& rg = orow[static_cast<size_t>(i)][static_cast<size_t>(k)];
                std::string t = orig.substr(rg.first, rg.second - rg.first);
                while (!t.empty() && t.back() == ' ') t.pop_back();
                lused = display_width(t);
                left = (marked ? sel : weight + ink) + t + R;
            }
            left += std::string(static_cast<size_t>(std::max(0, half - lused)), ' ');
            if (k < static_cast<int>(trow[static_cast<size_t>(i)].size())) {
                const auto& rg = trow[static_cast<size_t>(i)][static_cast<size_t>(k)];
                if (edit_here) {
                    // the caret goes where it belongs; at the end of a row that has a successor it belongs to the start of the successor
                    const bool last_row = k + 1 == static_cast<int>(trow[static_cast<size_t>(i)].size());
                    std::string o = change_sgr(settings_);
                    int cols = 0;
                    const std::vector<size_t> cuts = utf8_cuts(tr);
                    auto in_row = [&](size_t off) { return off >= rg.first && (off < rg.second || (off == rg.second && last_row)); };
                    for (size_t c = 0; c + 1 < cuts.size() || (c < cuts.size() && cuts[c] == tr.size()); ++c) {
                        const size_t from = cuts[c];
                        if (from < rg.first) continue;
                        if (from > rg.second || (from == rg.second && !last_row)) break;
                        const bool is_caret = from == caret && in_row(from);
                        const bool is_sel = from >= slo && from < shi;
                        if (from == tr.size() || from >= rg.second) {   // the caret behind the last character
                            if (is_caret) { o += "\xe2\x96\x88" + R + change_sgr(settings_); cols += 1; }
                            break;
                        }
                        const std::string g = tr.substr(from, cuts[c + 1] - from);
                        if (is_caret) { o += "\xe2\x96\x88"; cols += 1; if (!is_sel) { o += g; cols += display_width(g); } else { o += "\x1b[7m" + g + "\x1b[0m" + change_sgr(settings_); cols += display_width(g); } }
                        else if (is_sel) { o += "\x1b[7m" + g + "\x1b[0m" + change_sgr(settings_); cols += display_width(g); }
                        else { o += g; cols += display_width(g); }
                    }
                    right = o + R;
                    rused = cols;
                } else {
                    std::string t = tr.substr(rg.first, rg.second - rg.first);
                    while (!t.empty() && t.back() == ' ') t.pop_back();
                    rused = display_width(t);
                    right = (marked ? sel : weight + ink) + t + R;
                }
            } else if (edit_here && k == 0) {   // an empty line that has the caret
                right = change_sgr(settings_) + "\xe2\x96\x88" + R;
                rused = 1;
            }
            row(left + border + " " + settings_.box_vertical + " " + R + right, half + 3 + rused);
        }
    }
    for (; shown_rows < body; ++shown_rows) row("", 0);
    // status: what happened / what is asked / where the caret is
    {
        std::string s;
        std::string sgr = leg;
        if (rd_->te_ask) s = "Leave without saving?  [y] Yes | [any other key] No";
        else if (rd_->te_new_ask) s = "Start over with an empty translation? What is written here is dropped.  [y] Yes | [any other key] No";
        else if (!rd_->te_msg.empty()) { s = rd_->te_msg; if (rd_->te_warn) sgr = std::string("\x1b[31m"); }
        else {
            int filled = 0, total = 0;
            for (int i = 0; i < n; ++i) if (!readlyrics_te_blank(static_cast<size_t>(i))) { ++total; if (rd_->te_lines[static_cast<size_t>(i)].find_first_not_of(" \t\r") != std::string::npos) ++filled; }
            s = "line " + std::to_string(cur + 1) + "/" + std::to_string(n) + "   " + std::to_string(filled) + " of " + std::to_string(total) + " lines written" + (rd_->te_dirty ? "   (not saved)" : "") +
                (translit_enabled() ? "   [Latin letters on]" : "");
        }
        s = truncate_str(s, inner);
        row(sgr + s + R, display_width(s));
    }
    out.push_back(box_bottom(PW, "[CTRL+s] Save  [ESC] Close", border_bottom));
    return out;
}

std::vector<std::string> App::build_readlyrics_te_legend(int W) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::vector<LegendGroup> groups = {
        {"Move", {"[↑↓/ENTER] Line", "[PgUp/PgDn] Page up/down\001Page", "[TAB] Language / Lines\001Language"}},
        {"Edit", {"[CTRL+o] Copy original line\001Copy line", "[CTRL+n] Start from scratch\001Scratch", "[CTRL+c/x/v] Copy/Cut/Paste\001Clipboard"}},
        {"Lines", {"[SHIFT+↑↓] Mark lines\001Mark", "[CTRL+c/x/v] Copy/Cut/Paste lines\001Clipboard", "[DEL] Clear", "[CTRL+z/y] Undo", "[CTRL+r] Redo"}},
        {"Session", {"[CTRL+t] Latin letters\001Latin", "[CTRL+s] Save", "[ESC] Close"}},
    };
    std::vector<std::string> out;
    const auto lrows = layout_legend_tree(groups, std::max(20, W - 1));
    for (size_t i = 0; i < lrows.size(); ++i) out.push_back(" " + paint_legend(settings_, lrows, i, border, 0, true));
    return out;
}


bool App::readlyrics_te_blank(size_t i) const { return i >= rd_->lines.size() || rd_->lines[i].find_first_not_of(" \t\r") == std::string::npos; }


} // namespace muisc
