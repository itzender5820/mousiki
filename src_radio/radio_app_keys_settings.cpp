#include "radio_app.h"
#include "path_utf8.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

bool RadioApp::key_settings_theme(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    SettingsModel& sm = ui.model.settings;
    if (sm.theme_ui != 0) {                                        // the THEME picker / CREATE THEME overlay
        const int nt = static_cast<int>(sm.theme_list.size());
        if (sm.editing) {
            if (k == 3 || k == kKeyCtrlC) { sm.editing = false; set_text_entry(false); running = false; return true; }
            if (k == 27) { sm.editing = false; sm.status.clear(); set_text_entry(false); return true; }
            if ((k == 13 || k == 10) && (sm.theme_ui == 4 || sm.theme_ui == 5)) {
                if (sm.theme_ui == 4 && sm.theme_io_row == 0) sm.theme_io_name = themes::sanitize_name(sm.buffer);
                else {
                    sm.theme_io_dir = sm.buffer;
                    if (sm.theme_ui == 5) { theme_io_rescan(); sm.theme_io_sel = 0; sm.theme_io_row = sm.theme_io_files.empty() ? 0 : 1; }
                }
                sm.editing = false; set_text_entry(false);
                return true;
            }
            if (k == 13 || k == 10) {
                if (sm.theme_row < 0) sm.theme_name = themes::sanitize_name(sm.buffer);
                else {
                    std::string* f = color_field(sm.theme_draft, sm.theme_row, sm.theme_col);
                    if (f && valid_color_value(sm.buffer)) { *f = sm.buffer == "0" ? std::string() : sm.buffer; sm.status.clear(); }
                    else sm.status = "Not a colour: use a palette number 0-255 (empty or 0 = terminal colour)";
                }
                sm.editing = false; set_text_entry(false);
                return true;
            }
            if (sm.theme_ui == 4 || sm.theme_ui == 5) {
                edit_text_key(sm.buffer, sm.edit, k, (sm.theme_ui == 4 && sm.theme_io_row == 0) ? static_cast<size_t>(themes::kMaxNameLen) : 240, &sm.status);
                return true;
            }
            if (sm.theme_row >= 0 && textedit_detail::is_text_key(k) && !arrow && !(k >= '0' && k <= '9')) return true;
            edit_text_key(sm.buffer, sm.edit, k, sm.theme_row < 0 ? static_cast<size_t>(themes::kMaxNameLen) : 3, &sm.status);
            return true;
        }
        if (k == 3 || k == kKeyCtrlC) { settings_close(false); running = false; return true; }
        if (sm.theme_ui == 4) {                                    // export the current colours as a theme file
            if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) { sm.theme_ui = 0; sm.status.clear(); return true; }
            if (arrow && k == 'A') { sm.theme_io_row = 0; return true; }
            if (arrow && k == 'B') { sm.theme_io_row = 1; return true; }
            if (k == 13 || k == 10) {
                sm.buffer = sm.theme_io_row == 0 ? sm.theme_io_name : sm.theme_io_dir;
                sm.edit.to_end(sm.buffer); sm.editing = true; sm.status.clear(); set_text_entry(true);
                return true;
            }
            if (!arrow && (k == 's' || k == 'S')) {
                const std::string name = themes::sanitize_name(sm.theme_io_name);
                if (name.empty()) { sm.status = "give the theme a name first"; return true; }
                const int cur = theme_index(cfg, sm.theme_list);
                themes::Theme t = theme_capture(cfg, sm.theme_list[static_cast<size_t>(cur >= 0 ? cur : 0)]);
                t.name = name;
                const std::filesystem::path file = theme_dir_path(sm.theme_io_dir) / themes::file_name_for(name);
                std::string err;
                if (!themes::write_theme_file(file, t, err)) { sm.status = err; return true; }
                sm.theme_ui = 0; sm.status = "EXPORTED " + muisc::path_utf8(file);
            }
            return true;
        }
        if (sm.theme_ui == 5) {                                    // import theme files from a folder
            const int nf = static_cast<int>(sm.theme_io_files.size());
            auto import_files = [&](const std::vector<std::filesystem::path>& files) {
                int n = 0; std::string last, err;
                for (const auto& f : files)
                    for (themes::Theme& t : themes::read_theme_file(f)) {
                        const std::string name = themes::import_theme(t, err);
                        if (!name.empty()) { ++n; last = name; }
                    }
                sm.theme_list = themes::load_all();
                sm.theme_ui = 0;
                sm.status = n == 0 ? (err.empty() ? std::string("no theme found in that file") : err)
                                   : "IMPORTED " + std::to_string(n) + " THEME" + (n == 1 ? "" : "S") + (n == 1 ? ": " + last : std::string()) + " -- pick it in the THEME row";
            };
            if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) { sm.theme_ui = 0; sm.status.clear(); return true; }
            if (arrow && k == 'A') { if (sm.theme_io_row == 1 && sm.theme_io_sel > 0) --sm.theme_io_sel; else sm.theme_io_row = 0; return true; }
            if (arrow && k == 'B') {
                if (sm.theme_io_row == 0) { if (nf > 0) sm.theme_io_row = 1; }
                else if (sm.theme_io_sel < nf - 1) ++sm.theme_io_sel;
                return true;
            }
            if (k == 13 || k == 10) {
                if (sm.theme_io_row == 0) { sm.buffer = sm.theme_io_dir; sm.edit.to_end(sm.buffer); sm.editing = true; sm.status.clear(); set_text_entry(true); }
                else if (sm.theme_io_sel >= 0 && sm.theme_io_sel < nf) import_files({sm.theme_io_files[static_cast<size_t>(sm.theme_io_sel)]});
                return true;
            }
            if (!arrow && k == 'a') { if (nf > 0) import_files(sm.theme_io_files); return true; }
            return true;
        }
        if (sm.theme_ui == 1 || sm.theme_ui == 3) {
            if (sm.theme_peek) { sm.theme_peek = false; return true; }   // any key closes the preview
            if (!arrow && k == 'P') { sm.theme_peek = true; return true; }
            if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) { sm.theme_ui = sm.theme_ui == 3 ? 2 : 0; return true; }
            if (arrow && (k == 'A' || k == 'B') && last_key_was_shifted()) {   // SHIFT+up / down: a page
                sm.theme_sel = std::clamp(sm.theme_sel + (k == 'A' ? -10 : 10), 0, std::max(0, nt - 1));
                return true;
            }
            if ((arrow && k == 'A') || (!arrow && k == 'k')) { sm.theme_sel = std::max(0, sm.theme_sel - 1); return true; }
            if ((arrow && k == 'B') || (!arrow && k == 'j')) { sm.theme_sel = std::min(std::max(0, nt - 1), sm.theme_sel + 1); return true; }
            if (k == kKeyHome) { sm.theme_sel = 0; return true; }
            if (k == kKeyEnd) { sm.theme_sel = std::max(0, nt - 1); return true; }
            if (sm.theme_ui == 1 && !arrow && k == 'D' && sm.theme_sel >= 0 && sm.theme_sel < nt) {   // SHIFT+d: delete a theme of your own
                const themes::Theme& t = sm.theme_list[static_cast<size_t>(sm.theme_sel)];
                if (t.builtin) sm.status = "\"" + t.name + "\" is a built-in theme and cannot be deleted";
                else sm.leave_prompt = 4;   // "Do you want to delete theme?"
                return true;
            }
            if ((k == 13 || k == 10) && sm.theme_sel >= 0 && sm.theme_sel < nt) {
                const themes::Theme& t = sm.theme_list[static_cast<size_t>(sm.theme_sel)];
                if (sm.theme_ui == 1) {
                    theme_apply(cfg, t);
                    sm.dirty = true; sm.theme_ui = 0; sm.status = "THEME: " + t.name + " (s saves the settings)";
                } else {
                    theme_apply(sm.theme_draft, t);
                    sm.theme_base = t; sm.theme_ui = 2; sm.status = "template: " + t.name;
                }
            }
            return true;
        }
        // --- CREATE THEME
        if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) {
            if (theme_changed()) { sm.leave_prompt = 2; return true; }   // anything made here is lost on ESC: ask first
            sm.theme_ui = 0; sm.status.clear(); return true;
        }
        if (!arrow && (k == 's' || k == 'S')) { theme_try_save(); return true; }
        const bool up = (arrow && k == 'A') || (!arrow && k == 'k');
        const bool down = (arrow && k == 'B') || (!arrow && k == 'j');
        const bool left = (arrow && k == 'D') || (!arrow && k == 'h');
        const bool right = (arrow && k == 'C') || (!arrow && k == 'l');
        if (up) { if (sm.theme_row >= 0) { --sm.theme_row; if (sm.theme_row < 0) sm.theme_col = 0; } return true; }
        if (down) {
            if (sm.theme_row < kColorRowCount - 1) {
                ++sm.theme_row;
                if (sm.theme_col == 1 && !color_field(sm.theme_draft, sm.theme_row, 1)) sm.theme_col = 0;
            }
            return true;
        }
        if (left) { sm.theme_col = 0; return true; }
        if (right) { if (sm.theme_row < 0 || color_field(sm.theme_draft, sm.theme_row, 1)) sm.theme_col = 1; return true; }
        if (k == 13 || k == 10) {
            if (sm.theme_row < 0 && sm.theme_col == 1) {
                sm.theme_ui = 3; sm.theme_sel = 0;
                for (int i = 0; i < nt; ++i) if (sm.theme_list[static_cast<size_t>(i)].name == sm.theme_base.name) sm.theme_sel = i;
                return true;
            }
            if (sm.theme_row < 0) sm.buffer = sm.theme_name;
            else { const std::string* f = color_field(sm.theme_draft, sm.theme_row, sm.theme_col); sm.buffer = f ? *f : std::string(); }
            sm.edit.to_end(sm.buffer); sm.editing = true; sm.status.clear(); set_text_entry(true);
        }
        return true;
    }
    return false;
}

bool RadioApp::key_settings_edit(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    SettingsModel& sm = ui.model.settings;
    if (sm.editing && sm.tab == 3) {                              // a folder field: full line editor
        if (k == 3) { sm.editing = false; set_text_entry(false); running = false; return true; }
        if (k == 27 && !last_key_was_arrow()) { sm.editing = false; sm.status.clear(); set_text_entry(false); return true; }
        if (k == 13 || k == 10) {
            std::string* f = path_row_text(cfg, sm.row);
            if (f) {
                std::string v = sm.buffer;
                while (!v.empty() && v.back() == ' ') v.pop_back();
                if (!v.empty() && v[0] == '~' && (v.size() == 1 || v[1] == '/' || v[1] == '\\')) {
                    const char* home = std::getenv("HOME");
                    if (!home || !*home) home = std::getenv("USERPROFILE");
                    if (home) v = std::string(home) + v.substr(1);
                }
                if (sm.row == 5 && muisc::book_path_conflicts(muisc::load_settings().local_music_paths, v)) {
                    sm.status = "CONFLICT: that folder is also a LOCAL MUSIC PATH of the player. Pick a sub folder of it or another folder";
                    sm.editing = false; set_text_entry(false);
                    return true;
                }
                if (*f != v) { *f = v; sm.dirty = true; storage_changed(); }
            }
            sm.editing = false; set_text_entry(false);
            return true;
        }
        edit_text_key(sm.buffer, sm.edit, k, 400, &sm.status);
        return true;
    }
    if (sm.editing && sm.tab == 4) {                              // a key: one character or a name
        if (k == 3) { sm.editing = false; set_text_entry(false); running = false; return true; }
        if (k == 27 && !last_key_was_arrow()) { sm.editing = false; sm.status.clear(); set_text_entry(false); return true; }
        if (k == 13 || k == 10) {
            std::string v = sm.buffer;
            while (!v.empty() && v.back() == ' ') v.pop_back();
            if (v.empty() && sm.buffer.find(' ') != std::string::npos) v = "SPACE";
            std::string up = v;
            for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (up == "SPACE" || up == "TAB" || up == "BACKSPACE") v = up;
            if (key_code(v) == 0) { sm.status = "Not a key: type one character, or SPACE / TAB / BACKSPACE"; sm.buffer.clear(); sm.edit.reset(); return true; }
            const std::string conflict = key_conflict(cfg, v, (sm.row - 1));
            if (!conflict.empty()) {
                sm.status = "KEY \"" + v + "\" ALREADY USED BY " + conflict + " -- try another key";
                sm.buffer.clear(); sm.edit.reset();
                return true;                                           // stays in the field, like the player
            }
            key_change_begin((sm.row - 1));
            const auto before = cfg.keys;
            if (v == kKeyActions[(sm.row - 1)].def) cfg.keys.erase(kKeyActions[(sm.row - 1)].id); else cfg.keys[kKeyActions[(sm.row - 1)].id] = v;
            key_change_end(cfg.keys != before);
            sm.dirty = true; sm.status = std::string("UPDATED ") + kKeyActions[(sm.row - 1)].label;
            sm.editing = false; set_text_entry(false);
            return true;
        }
        edit_text_key(sm.buffer, sm.edit, k, 12, &sm.status);
        return true;
    }
    if (sm.editing) {
        if (k == 3 || k == kKeyCtrlC) { sm.editing = false; set_text_entry(false); running = false; return true; }
        if (k == 27) { sm.editing = false; sm.status.clear(); set_text_entry(false); return true; }
        if (k == 13 || k == 10) {
            std::string* f = color_field(cfg, color_row_of_tab(sm.row), sm.col);
            if (f && valid_color_value(sm.buffer)) {
                *f = sm.buffer == "0" ? std::string() : sm.buffer;
                sm.dirty = true; sm.status.clear();
            } else sm.status = "Not a colour: use a palette number 0-255 (empty or 0 = terminal colour)";
            sm.editing = false; set_text_entry(false);
            return true;
        }
        // Same caret keys as every other field (LEFT/RIGHT move it, SHIFT+LEFT/RIGHT mark, HOME/END,
        // DEL / BACKSPACE, CTRL+C/X/V). TAB and the arrows never leave the field: only ENTER or ESC
        // do. Typing is digits only (a palette number); a pasted non-number is refused by ENTER.
        if (textedit_detail::is_text_key(k) && !arrow && !(k >= '0' && k <= '9')) return true;
        edit_text_key(sm.buffer, sm.edit, k, 3, &sm.status);
        return true;
    }
    return false;
}

bool RadioApp::key_settings(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ RADIO SETTINGS (s)
    if (ui.model.settings.open) {
        SettingsModel& sm = ui.model.settings;
        if (sm.remote_ov.open) {   // the REMOTE APP overlay has the keys
            if (k == 3 || k == kKeyCtrlC) { settings_close(false); running = false; return true; }
            muisc::remote_ui::Values v{cfg.remote_app, cfg.remote_port, cfg.remote_pin};
            bool close = false;
            if (muisc::remote_ui::key(sm.remote_ov, v, k, arrow, close)) {
                cfg.remote_app = v.enabled; cfg.remote_port = v.port; cfg.remote_pin = v.pin;
                muisc::remote::net::configure({cfg.remote_app, cfg.remote_port, cfg.remote_pin, cfg.remote_hosts, cfg.remote_cli});
                sm.dirty = true;
            }
            if (close) { sm.remote_ov = muisc::remote_ui::State{}; sm.status = cfg.remote_app ? "remote app on (s saves)" : "remote app off (s saves)"; }
            return true;
        }
        if (sm.leave_prompt != 0 && !sm.editing) {
            // the question on top: [s] / ENTER save, [d] discard, anything else (ESC) goes back to editing; "overwrite?" wants [y]
            const int kind = sm.leave_prompt;
            sm.leave_prompt = 0;
            if (k == 3 || k == kKeyCtrlC) { settings_close(false); running = false; return true; }
            const bool save = !arrow && (k == 's' || k == 'S' || k == 13 || k == 10);
            const bool discard = !arrow && (k == 'd' || k == 'D');
            if (kind == 3) { if (!arrow && (k == 'y' || k == 'Y')) theme_save_now(); return true; }
            if (kind == 4) {   // "delete the theme?": only y says yes
                if (!arrow && (k == 'y' || k == 'Y') && sm.theme_sel >= 0 && sm.theme_sel < static_cast<int>(sm.theme_list.size())) {
                    const std::string name = sm.theme_list[static_cast<size_t>(sm.theme_sel)].name;
                    std::string err;
                    if (themes::delete_theme(name, err)) {
                        sm.theme_list = themes::load_all();
                        sm.theme_sel = std::clamp(sm.theme_sel, 0, std::max(0, static_cast<int>(sm.theme_list.size()) - 1));
                        sm.status = "THEME \"" + name + "\" DELETED";
                    } else sm.status = err;
                }
                return true;
            }
            if (kind == 1) { if (save) settings_close(true); else if (discard) settings_close(false); }
            else if (save) theme_try_save();     // may ask once more (overwriting a theme of your own)
            else if (discard) { sm.theme_ui = 0; sm.status.clear(); }
            return true;
        }
        if (key_settings_theme(kc)) return true;   // (the THEME picker / CREATE THEME overlay)
        if (key_settings_edit(kc)) return true;    // (a field is being edited)
        if (k == 3 || k == kKeyCtrlC) { settings_close(false); running = false; return true; }
        if (k == kKeyCtrlShiftU) {                                     // undo the last key change (up to 5, newest first)
            if (key_undo.empty()) { sm.status = "nothing to undo"; return true; }
            const KeyUndo u = key_undo.back();
            key_undo.pop_back();
            int where = u.row;
            if (where < 0) {                                           // a full reset: go to the first key that comes back
                for (int i = 0; i < kKeyActionCount && where < 0; ++i) {
                    RadioSettings old_s; old_s.keys = u.keys;                  // the binding as it was before the reset
                    if (key_binding(old_s, i) != key_binding(cfg, i)) where = i;
                }
            }
            cfg.keys = u.keys;
            sm.tab = 4; sm.col = 0; sm.row = std::clamp(where, 0, kKeyActionCount - 1) + 1;   // row 0 is the reset line
            sm.dirty = true;
            sm.status = u.row < 0 ? "UNDONE: reset of all keys" : std::string("UNDONE: ") + kKeyActions[u.row].label + " is now \"" + key_binding(cfg, u.row) + "\"";
            return true;
        }
        if (k == 9) {                                                  // TAB: COLORS <-> GENERAL
            sm.tab = (sm.tab + 1) % 6; sm.row = 0; sm.col = 0; sm.status.clear();
            return true;
        }
        const int nrows = sm.tab == 1 ? kOnOffRowCount : sm.tab == 2 ? (rs_set_hifi_rows(cfg.bars_style == 1), rs_row_count(2)) : sm.tab == 3 ? kPathRowCount : sm.tab == 4 ? kKeyActionCount + 1 : sm.tab == 5 ? static_cast<int>(about_app_text().size()) : color_tab_row_count();   // + the THEME and SMOOTHNESS rows
        // the Volume indicator row has two values (the look | gradient or not): the only row of the other tabs that keeps its column
        const bool vol_row = (sm.tab == 1 || sm.tab == 2) && std::string(rs_row_label(sm.tab, sm.row)) == "Volume indicator";
        auto fix_col = [&]() {
            const bool vol_now = (sm.tab == 1 || sm.tab == 2) && std::string(rs_row_label(sm.tab, sm.row)) == "Volume indicator";
            if ((sm.tab >= 1 && !vol_now) || (sm.tab == 0 && sm.row != 0 && !color_field(cfg, color_row_of_tab(sm.row), sm.col))) sm.col = 0;
        };
        auto toggle = [&](int dir) {   // GENERAL and ANIMA/VIZ rows: left / right / ENTER
            if (sm.tab == 1 && std::string(rs_row_label(sm.tab, sm.row)) == "Choose Audio Device") {   // the CHOOSE AUDIO DEVICE overlay
                if (dir == 0) adev_open(true);
                else sm.status = "[ENTER] lists the audio devices";
                return;
            }
            if (sm.tab == 1 && std::string(rs_row_label(sm.tab, sm.row)) == "CLI Scripts") {   // the script overlay (ESC comes back here)
                if (dir == 0) cliov_open("", "", true);
                else sm.status = "[ENTER] opens the script overlay: the scripts of the command line (CLI)";
                return;
            }
            if (sm.tab == 1 && std::string(rs_row_label(sm.tab, sm.row)) == "Remote App") {   // opens the REMOTE APP overlay
                sm.remote_ov = muisc::remote_ui::State{};
                sm.remote_ov.open = true;
                return;
            }
            if (sm.tab == 3) {   // PATHS: only the bool row toggles; the text rows are edited with ENTER
                if (!path_row_is_bool(sm.row)) {
                    if (dir == 0) {
                        std::string* f = path_row_text(cfg, sm.row);
                        sm.editing = true; sm.buffer = f ? *f : std::string(); sm.edit.to_end(sm.buffer); sm.status.clear(); set_text_entry(true);
                    }
                    return;
                }
                cfg.download_same_as_player = !cfg.download_same_as_player;
                sm.dirty = true; sm.status.clear(); storage_changed();
                return;
            }
            if (vol_row) {   // LEFT / RIGHT (and h / l) switch between the look and the gradient, ENTER changes the one that is lit
                if (dir != 0) { sm.col = dir > 0 ? 1 : 0; sm.status = "[ENTER] changes the value that is lit; [<-] [->] switch between the look and the gradient"; return; }
                if (sm.col == 1) cfg.volume_gradient = !cfg.volume_gradient;
                else rs_row_change(cfg, sm.tab, sm.row, 0);
                sm.dirty = true;
                return;
            }
            if (sm.tab == 1 && std::string(rs_row_label(sm.tab, sm.row)) == "Terminal default font") {   // applied and saved at once, for all modes
                rs_row_change(cfg, sm.tab, sm.row, dir);
                muisc::set_config_value("TerminalDefaultFont", cfg.terminal_default_font ? "true" : "false");
                sm.status = muisc::terminal_font::apply(cfg.terminal_default_font ? std::string(muisc::terminal_font::kTerminalDefault) : cfg.terminal_font);
                return;
            }
            rs_row_change(cfg, sm.tab, sm.row, dir);
            ui.model.shuffle = cfg.playback_shuffle;
            sm.dirty = true; sm.status.clear();
            apply_audio();
        };
        if (arrow) {
            if (k == 'A') sm.row = std::max(0, sm.row - 1);
            else if (k == 'B') sm.row = std::min(nrows - 1, sm.row + 1);
            else if (sm.tab >= 1 && sm.tab <= 3) toggle(k == 'C' ? +1 : -1);
            else if (sm.row == 2) { cfg.border_smooth = !cfg.border_smooth; sm.dirty = true; sm.status.clear(); }   // SMOOTHNESS
            else if (k == 'C') sm.col = 1;
            else if (k == 'D') sm.col = 0;
            fix_col();
            return true;
        }
        if (k == 27 || k == 'q' || k == 'Q') {
            if (settings_changed()) { sm.leave_prompt = 1; return true; }   // unsaved changes: ask first
            settings_close(false);
            return true;
        }
        if (k == 's' || k == 'S') { settings_close(true); return true; }
        if (sm.tab == 0 && !arrow && k == 'e') { theme_open_creator(true); return true; }   // edit the theme in use
        if (k == 'j') { sm.row = std::min(nrows - 1, sm.row + 1); fix_col(); return true; }
        if (k == 'k') { sm.row = std::max(0, sm.row - 1); fix_col(); return true; }
        if (sm.tab == 0 && !arrow && (k == 'E' || k == 'I')) {        // SHIFT+e / SHIFT+i: export / import a theme
            sm.theme_list = themes::load_all();
            if (sm.theme_io_dir.empty()) sm.theme_io_dir = themes::exchange_dir().string();
            if (k == 'E') {
                const int cur = theme_index(cfg, sm.theme_list);
                sm.theme_io_name = cur >= 0 ? sm.theme_list[static_cast<size_t>(cur)].name : std::string("My Theme");
                sm.theme_io_row = 0; sm.theme_ui = 4;
            } else {
                sm.theme_io_sel = 0; theme_io_rescan();
                sm.theme_io_row = sm.theme_io_files.empty() ? 0 : 1; sm.theme_ui = 5;
            }
            sm.status.clear();
            return true;
        }
        if (sm.tab == 5) return true;                                     // ABOUT APP: only scrolls
        if (sm.tab == 4) {                                             // REFERENCE
            if (sm.row == 0) {                                          // the RESET line (right under the note)
                if (k == 13 || k == 10) {
                    if (cfg.keys.empty()) sm.status = "all keys already have their default";
                    else { key_change_begin(-1); cfg.keys.clear(); sm.dirty = true; sm.status = "ALL KEYS RESET TO DEFAULT (Ctrl+Shift+U undoes it)"; }
                }
                return true;
            }
            if (k == 13 || k == 10) { sm.editing = true; sm.buffer.clear(); sm.edit.reset(); sm.status.clear(); set_text_entry(true); }
            else if (k == kKeyDelete || k == 127) {
                key_change_begin(sm.row - 1);
                const auto before = cfg.keys;
                cfg.keys.erase(kKeyActions[sm.row - 1].id);
                key_change_end(cfg.keys != before);
                sm.dirty = true; sm.status = std::string("default key restored for ") + kKeyActions[sm.row - 1].label;
            }
            return true;
        }
        if (sm.tab >= 1) {
            if (k == 'h') toggle(-1);
            else if (k == 'l') toggle(+1);
            else if (k == 13 || k == 10 || k == ' ') toggle(0);
            return true;
        }
        if (sm.row == 2) {                                              // SMOOTHNESS: rough <-> smooth
            if (k == 'h' || k == 'l' || k == 13 || k == 10 || k == ' ') { cfg.border_smooth = !cfg.border_smooth; sm.dirty = true; sm.status.clear(); }
            return true;
        }
        if (k == 'h') { sm.col = 0; return true; }
        if (k == 'l') { if (sm.row == 0 || color_field(cfg, color_row_of_tab(sm.row), 1)) sm.col = 1; return true; }
        if ((k == 13 || k == 10) && sm.row == 0) {          // THEME | CREATE THEME
            sm.theme_list = themes::load_all();
            const int cur = theme_index(cfg, sm.theme_list);
            if (sm.col == 0) { sm.theme_sel = std::max(0, cur); sm.theme_ui = 1; sm.status.clear(); }
            else theme_open_creator(false);
            return true;
        }
        if (k == 13 || k == 10) {
            const std::string* f = color_field(cfg, color_row_of_tab(sm.row), sm.col);
            if (f) { sm.editing = true; sm.buffer = *f; sm.edit.to_end(sm.buffer); sm.status.clear(); set_text_entry(true); }
            return true;
        }
        return true;
    }
    return false;
}
