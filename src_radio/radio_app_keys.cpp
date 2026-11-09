#include "radio_app.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

bool RadioApp::key_wizard(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ HELP WIZARD (SHIFT+w)
    if (ui.model.wizard_open) {
        // The page / part / contents state and what a key does live in src/wizard.cpp, the same for both modes.
        wizard::Key wk = wizard::Key::None;
        if (ui.model.wizard.searching) {      // SHIFT+s: the query has the keyboard (CTRL+C copies there)
            wizard::search_key(ui.model.wizard, k, arrow, arrow && last_key_was_shifted());
            set_text_entry(ui.model.wizard.searching);
            return true;
        }
        if (k == 3 || k == kKeyCtrlC) running = false;
        else if (arrow) {
            const bool shifted = last_key_was_shifted();
            if (k == 'C') wk = wizard::Key::Right;
            else if (k == 'D') wk = wizard::Key::Left;
            else if (k == 'A') wk = shifted ? wizard::Key::PageUp : wizard::Key::Up;
            else if (k == 'B') wk = shifted ? wizard::Key::PageDown : wizard::Key::Down;
        } else if (k == 27 || k == 'q' || k == 'Q' || key_translate(cfg, k) == 'W') wk = wizard::Key::Close;
        else if (k == 13 || k == 10) wk = wizard::Key::Enter;
        else if (k == 8 || k == 127) wk = wizard::Key::Back;                 // BACKSPACE and [t]: the table of content
        else if (k == 't' || k == 'T') wk = wizard::Key::Contents;
        else if (k == 'S') wk = wizard::Key::Search;                         // SHIFT+s: search the whole text
        if (wk == wizard::Key::Search) set_text_entry(true);
        if (wizard::handle_key(ui.model.wizard, wk)) {
            set_text_entry(false);
            ui.model.wizard_open = false;
            out_clear_next = true;
            wizard_seen_save();
        }
        return true;
    }

    // SHIFT+w opens the wizard from every menu and overlay too -- unless a text field has the keyboard (then it is a letter).
    if (wizard::kEnabled && !arrow && !text_entry_active() && key_translate(cfg, k) == 'W') {
        wizard_open_now();
        return true;
    }
    return false;
}

bool RadioApp::key_cheat(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ CHEATSHEET (?)
    if (ui.model.cheat_open) {
        UiModel& um = ui.model;
        if (um.cheat_searching) {   // SHIFT+s: the search field has the keyboard (CTRL+C copies there)
            const int n = static_cast<int>(um.cheat_result_rows.size());
            if (!arrow && k == 27) {
                if (!um.cheat_query.empty()) { um.cheat_query.clear(); um.cheat_qcaret = um.cheat_qanchor = 0; um.cheat_sel = um.cheat_first = 0; }
                else um.cheat_searching = false;
            } else if (arrow && (k == 'A' || k == 'B')) {
                const int step = last_key_was_shifted() ? 5 : 1;
                um.cheat_sel = std::clamp(um.cheat_sel + (k == 'B' ? step : -step), 0, std::max(0, n - 1));
            } else if (!arrow && (k == 13 || k == 10)) {
                if (n > 0) um.cheat_jump_row = um.cheat_result_rows[static_cast<size_t>(std::clamp(um.cheat_sel, 0, n - 1))];
                um.cheat_searching = false;
            } else if (muisc::edit_text_key(um.cheat_query, um.cheat_qcaret, um.cheat_qanchor, k, 80, nullptr)) {
                um.cheat_sel = um.cheat_first = 0;
            }
            set_text_entry(um.cheat_searching);
            return true;
        }
        if (!arrow && k == 'S') { set_text_entry(true); um.cheat_searching = true; um.cheat_query.clear(); um.cheat_qcaret = um.cheat_qanchor = 0; um.cheat_sel = um.cheat_first = 0; return true; }
        if (k == 3 || k == kKeyCtrlC) running = false;
        else if (arrow) {
            if (k == 'A') ui.model.cheat_scroll -= (last_key_was_shifted() ? 10 : 1);        // up (SHIFT / PgUp: a page); the renderer clamps both ends
            else if (k == 'B') ui.model.cheat_scroll += (last_key_was_shifted() ? 10 : 1);   // down
        } else if (k == '?' || k == 27) ui.model.cheat_open = false;
        else if (k == 'j') ++ui.model.cheat_scroll;
        else if (k == 'k') --ui.model.cheat_scroll;
        return true;
    }
    return false;
}

bool RadioApp::key_adev(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ CHOOSE AUDIO DEVICE (over the settings or the main screen)
    if (ui.model.settings.adev_open) {
        SettingsModel& sm = ui.model.settings;
        if (k == 3 || k == kKeyCtrlC) { adev_close(); if (sm.open) settings_close(false); running = false; return true; }
        const int rows = 1 + static_cast<int>(sm.adev_list.size());
        if (arrow && k == 'A') { sm.adev_sel = (sm.adev_sel + rows - 1) % rows; return true; }
        if (arrow && k == 'B') { sm.adev_sel = (sm.adev_sel + 1) % rows; return true; }
        if (!arrow && (k == 'r' || k == 'R')) {   // look again (a headset that was just plugged in)
            const std::string keep = sm.adev_sel > 0 && sm.adev_sel <= static_cast<int>(sm.adev_list.size()) ? sm.adev_list[static_cast<size_t>(sm.adev_sel - 1)].name : std::string();
            sm.adev_err.clear();
            sm.adev_list = muisc::list_output_devices(&sm.adev_err);
            sm.adev_sel = 0;
            for (size_t i = 0; i < sm.adev_list.size(); ++i) if (!keep.empty() && sm.adev_list[i].name == keep) sm.adev_sel = static_cast<int>(i) + 1;
            return true;
        }
        if (!arrow && (k == 13 || k == 10)) { adev_pick(); adev_close(); return true; }
        if (!arrow && (k == 27 || key_translate(cfg, k) == kKeyDegree)) adev_close();
        return true;
    }
    return false;
}

bool RadioApp::key_history(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ LISTENING HISTORY (SHIFT+h)
    if (ui.model.hmenu.open) {
        HistoryModel& hm = ui.model.hmenu;
        const int nh = static_cast<int>(rhist.size());
        const int nt = static_cast<int>(rhist.top_channels(true).size());
        if (hm.yt_open) {                                              // the YouTube overlay owns every key
            if (k == 3) { hm.yt_open = false; set_text_entry(false); running = false; return true; }
            if (k == 27 && !arrow) { hm.yt_open = false; yt_jump = false; set_text_entry(false); return true; }
            const int nres = static_cast<int>(hm.yt.items.size());
            if (hm.yt_in_field) {
                if (k == 13 || k == 10) {
                    if (!hm.yt_text.empty()) { yt.search(hm.yt_text); yt_jump = true; hm.yt_cursor = 0; }
                } else if (k == 9 || (arrow && k == 'B')) {
                    if (nres > 0) { hm.yt_in_field = false; set_text_entry(false); }
                } else edit_text_key(hm.yt_text, hm.yt_edit, k, 200, nullptr);
                return true;
            }
            if (arrow && k == 'A') hm.yt_cursor = std::max(0, hm.yt_cursor - 1);
            else if (arrow && k == 'B') hm.yt_cursor = std::min(std::max(0, nres - 1), hm.yt_cursor + 1);
            else if (!arrow && k == 'k') hm.yt_cursor = std::max(0, hm.yt_cursor - 1);
            else if (!arrow && k == 'j') hm.yt_cursor = std::min(std::max(0, nres - 1), hm.yt_cursor + 1);
            else if (k == 13 || k == 10) { if (nres > 0) yt.download(static_cast<size_t>(hm.yt_cursor)); }
            else if (k == 9 || k == '/') { hm.yt_in_field = true; hm.yt_edit.to_end(hm.yt_text); set_text_entry(true); }
            return true;
        }
        if (k == 3 || k == kKeyCtrlC) { running = false; return true; }
        if (!arrow && (k == 27 || k == 'q' || k == 'Q' || k == key_of(cfg, "History"))) { hm.open = false; hm.flash.clear(); return true; }
        if (!arrow && (k == '1' || k == '2' || k == '3')) { hm.tab = k - '1'; hm.flash.clear(); return true; }
        if (k == 9 || (arrow && k == 'C')) { hm.tab = (hm.tab + 1) % 3; hm.flash.clear(); return true; }
        if (arrow && k == 'D') { hm.tab = (hm.tab + 2) % 3; hm.flash.clear(); return true; }
        int& cur = hm.tab == 1 ? hm.top_cursor : hm.tab == 0 ? hm.cursor : hm.scroll;
        const int last = hm.tab == 0 ? nh - 1 : hm.tab == 1 ? nt - 1 : 1000000;
        auto move = [&](int d) { cur = std::clamp(cur + d, 0, std::max(0, last)); };
        if ((arrow && k == 'A') || (!arrow && k == 'k')) { move(arrow && last_key_was_shifted() ? -10 : -1); return true; }
        if ((arrow && k == 'B') || (!arrow && k == 'j')) { move(arrow && last_key_was_shifted() ? +10 : +1); return true; }
        if (k == kKeyHome || (!arrow && k == 'g')) { cur = 0; return true; }
        if (k == kKeyEnd || (!arrow && k == 'G')) { cur = std::max(0, last); return true; }
        if (!arrow && k == 'r' && hm.tab == 1) { hm.most_first = !hm.most_first; hm.top_cursor = 0; return true; }
        if (!arrow && k == 'y' && hm.tab == 0) {
            if (nh == 0) { hm.flash = "nothing to search for yet"; return true; }
            const HistoryEntry& e = rhist.newest(static_cast<size_t>(std::clamp(hm.cursor, 0, nh - 1)));
            std::string q = e.artist.empty() ? e.title : e.title.empty() ? e.artist : e.artist + " " + e.title;
            if (q.empty()) { hm.flash = "this line has no artist or title to search for"; return true; }
            hm.yt_open = true; hm.yt_in_field = true; hm.yt_text = q; hm.yt_edit.to_end(hm.yt_text);
            hm.yt_cursor = 0; yt.clear(); yt_jump = false; hm.flash.clear();
            yt.set_download_dir(storage.download_dir);
            hm.yt_dir = storage.download_dir.string();
            set_text_entry(true);
            yt.search(q); yt_jump = true;                              // the query is already right most of the time
            return true;
        }
        return true;
    }
    return false;
}

bool RadioApp::key_stations_dialogs(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ SHIFT+o / SHIFT+v overlays (main screen)
    if (ui.model.stov.open && ui.model.stov.del_idx >= 0 && !ui.model.cheat_open) {   // the warning before a deletion
        StationsOverlay& so = ui.model.stov;
        if (k == 3) { running = false; return true; }
        if (!arrow && (k == 'y' || k == 'Y')) { const int d = so.del_idx; so.del_idx = -1; stov_delete(d); }
        else if ((!arrow && (k == 'n' || k == 'N')) || (k == 27 && !arrow)) { so.del_idx = -1; so.flash = "kept"; }
        return true;
    }

    if (ui.model.stov.open && (ui.model.stov.add_open || ui.model.stov.edit_open) && !ui.model.cheat_open) {
        StationsOverlay& so = ui.model.stov;
        if (k == 3) { set_text_entry(false); running = false; return true; }
        if (k == 27 && !arrow) { so.add_open = false; so.edit_open = false; so.add_error.clear(); so.edit_error.clear(); set_text_entry(false); return true; }
        if (so.edit_open) {
            constexpr int nf = StationsOverlay::kEditFields;
            auto go = [&](int f) { so.edit_field = (f % nf + nf) % nf; so.edit_ed[so.edit_field].to_end(so.edit_text[so.edit_field]); };
            if (k == 13 || k == 10) stov_edit_commit();
            else if (k == 9 || (arrow && k == 'B')) go(so.edit_field + 1);
            else if (arrow && k == 'A') go(so.edit_field - 1);
            else if (edit_text_key(so.edit_text[so.edit_field], so.edit_ed[so.edit_field], k, 500, nullptr)) so.edit_error.clear();   // (Left / Right / Home / End move the caret)
            return true;
        }
        if (k == 13 || k == 10) { stov_add_commit(); return true; }
        if (k == 9 || (arrow && (k == 'A' || k == 'B'))) { so.add_field ^= 1; (so.add_field ? so.add_edit[1] : so.add_edit[0]).to_end(so.add_field ? so.add_name : so.add_url); return true; }
        std::string& buf = so.add_field ? so.add_name : so.add_url;
        if (edit_text_key(buf, so.add_edit[so.add_field], k, 500, nullptr)) so.add_error.clear();
        return true;
    }
    return false;
}

bool RadioApp::key_overlays_a(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    if (ui.model.overlay == 8 && !ui.model.cheat_open) {                 // "a newer version is out"
        if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; running = false; return true; }
        if (!arrow && (k == 'o' || k == 'O')) {
            if (!muisc::open_url(ui.model.update_url)) { ui.model.notice = "could not open the browser: " + ui.model.update_url; notice_until = ui.model.t_sec + 6; }
            ui.model.overlay = 0;
        } else if (!arrow && k == 27) {
            ui.model.overlay = 0;
        }
        return true;
    }

    if (ui.model.overlay == 5 && !ui.model.cheat_open) {                 // the record overlay (y)
        int& orow = ui.model.overlay_row;
        constexpr int kRows = static_cast<int>(sizeof(kRecordBack) / sizeof(kRecordBack[0]));
        if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; running = false; return true; }
        if (arrow) {
            if (k == 'A') orow = (orow + kRows - 1) % kRows;
            else if (k == 'B') orow = (orow + 1) % kRows;
            return true;
        }
        if (k == 13 || k == 10 || (k == 'y' && !arrow)) {
            // seconds back from the moment the overlay opened (-1 = everything that is buffered)
            const double want = kRecordBack[orow] < 0 ? ui.model.rec_avail_sec : static_cast<double>(kRecordBack[orow]);
            const double back = std::min(want, ui.model.rec_avail_sec);
            const long long from = std::max(0LL, ui.model.rec_anchor - static_cast<long long>(back * 48000.0));
            const RadioStatus rs = engine.status();
            ui.model.overlay = 0;
            start_record(from, back + rs.behind_sec);
            return true;
        }
        if (k == 27) { ui.model.overlay = 0; return true; }
        return true;
    }

    if (ui.model.overlay == 7 && !ui.model.cheat_open) {                 // the MENUS index (<)
        int& orow = ui.model.overlay_row;
        if (cfg.menu_classic) orow = menus_classic_sel(orow);
        if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; running = false; return true; }
        if (arrow) {
            if (k == 'A' || k == 'B' || k == 'C' || k == 'D') orow = menus_move(orow, k == 'A' ? 0 : k == 'B' ? 1 : k == 'D' ? 2 : 3, cfg.menu_classic);
            return true;
        }
        if (k == 'T') { cfg.menu_classic = !cfg.menu_classic; if (cfg.menu_classic) orow = menus_classic_sel(orow); settings_save(); return true; }   // SHIFT+t: the tree or the classic boxes
        if (k == 13 || k == 10) {   // open it: close the index, then handle the entry's key
            const auto& node = menus_node(orow);
            if (!node.action) { orow = menus_move(orow, 1); return true; }   // a group: go in
            const int chosen = key_of(cfg, node.action);
            ui.model.overlay = 0;
            if (chosen > 0) menus_inject = chosen;
            return true;
        }
        if (k == 27 || k == key_of(cfg, "Menus")) ui.model.overlay = 0;
        return true;
    }

    if (ui.model.overlay == 3 && !ui.model.cheat_open) {                 // the sleep timer overlay
        int& orow = ui.model.overlay_row;
        if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; settings_save(); running = false; return true; }
        if (arrow) {
            if (k == 'A') orow = (orow + 6) % 7;
            else if (k == 'B') orow = (orow + 1) % 7;
            return true;
        }
        if (k == 13 || k == 10) {
            if (orow < 5) { sleep_start(kSleepChoice[orow]); ui.model.overlay = 0; }
            else if (orow == 5) { cfg.sleep_fade = !cfg.sleep_fade; ui.model.settings.dirty = true; }
            else { sleep_cancel(); ui.model.notice = "sleep timer: off"; notice_until = ui.model.t_sec + 4; ui.model.overlay = 0; }
            return true;
        }
        if (k == 27 || k == key_of(cfg, "SleepTimer")) { ui.model.overlay = 0; settings_save(); }
        return true;
    }

    if (ui.model.overlay == 4 && !ui.model.cheat_open) {                 // the equaliser overlay (SHIFT+e)
        EqUi& eu = ui.model.eq;
        const bool earrow = last_key_was_arrow();
        auto eq_close = [&]() { ui.model.overlay = 0; eu.naming = false; set_text_entry(false); settings_save(); };
        auto eq_changed = [&]() { apply_audio(); ui.model.settings.dirty = true; };
        if (eu.naming) {                                                    // the "Save as:" prompt owns every key
            if (!earrow && k == 27) { eu.naming = false; eu.name.clear(); eu.name_edit.reset(); eu.status = "Save cancelled"; set_text_entry(false); return true; }
            if (!earrow && (k == 13 || k == 10)) { if (eq_commit_name(cfg, eu)) { ui.model.settings.dirty = true; settings_save(); } set_text_entry(false); return true; }
            edit_text_key(eu.name, eu.name_edit, k, kEqNameMaxBytes, nullptr);
            return true;
        }
        if (k == 3 || k == kKeyCtrlC) { eq_close(); running = false; return true; }
        const bool armed = eu.delete_armed;
        eu.delete_armed = false;
        eu.status.clear();
        if (earrow && k == 'C') { eu.band = (eu.band + 1) % kEqBands; return true; }
        if (earrow && k == 'D') { eu.band = (eu.band + kEqBands - 1) % kEqBands; return true; }
        if (earrow && k == 'A') { eq_set_gain(cfg, eu.band, cfg.eq_gains[static_cast<size_t>(eu.band)] + 1.0f); eq_changed(); return true; }
        if (earrow && k == 'B') { eq_set_gain(cfg, eu.band, cfg.eq_gains[static_cast<size_t>(eu.band)] - 1.0f); eq_changed(); return true; }
        if (earrow) return true;
        if (k == ',' || k == '<') { eq_select_preset(cfg, eu, -1); eq_changed(); return true; }
        if (k == '.' || k == '>' || k == 9) { eq_select_preset(cfg, eu, +1); eq_changed(); return true; }
        if (k == '0') { eq_set_gain(cfg, eu.band, 0.0f); eq_changed(); return true; }
        if (k == ' ') { cfg.eq_enabled = !cfg.eq_enabled; eq_changed(); return true; }
        if (k == 'r' || k == 'R') { cfg.eq_gains = kEqPresets[0].gains; eu.last_preset = 0; eq_changed(); return true; }
        if (k == 27 || k == key_of(cfg, "EqMenu")) { eq_close(); return true; }
        if (k == 's' || k == 'S') { eq_begin_naming(cfg, eu); set_text_entry(true); return true; }
        if (k == kKeyDelete || k == 'x' || k == 'X') { if (eq_delete_custom(cfg, eu, armed)) { ui.model.settings.dirty = true; settings_save(); } return true; }
        return true;
    }
    return false;
}

bool RadioApp::key_overlays_b(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    if (ui.model.overlay == 10 && !ui.model.cheat_open) {  // spectrum bars options (SHIFT+b)
        int& orow = ui.model.overlay_row;
        const std::vector<int> brow = muisc::bars_rows(cfg);
        const int bn = static_cast<int>(brow.size());
        if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; settings_save(); running = false; return true; }
        if (arrow) {
            if (k == 'A') orow = orow <= 0 ? bn - 1 : orow - 1;
            else if (k == 'B') orow = orow >= bn - 1 ? 0 : orow + 1;
            else if (k == 'C' || k == 'D') { muisc::bars_change(cfg, brow[static_cast<size_t>(std::clamp(orow, 0, bn - 1))], k == 'C' ? +1 : -1); ui.model.settings.dirty = true; }
            orow = std::clamp(orow, 0, std::max(0, static_cast<int>(muisc::bars_rows(cfg).size()) - 1));
            return true;
        }
        if (k == 27 || k == key_of(cfg, "BarsMenu")) { ui.model.overlay = 0; settings_save(); return true; }
        if (k == 'r' || k == 'R') { muisc::bars_reset(cfg); ui.model.settings.dirty = true; orow = std::clamp(orow, 0, bn - 1); return true; }
        return true;
    }

    if (ui.model.overlay == 6 && !ui.model.cheat_open) {   // spectrogram options (SHIFT+i)
        int& orow = ui.model.overlay_row;
        if (k == 3 || k == kKeyCtrlC) { ui.model.overlay = 0; settings_save(); running = false; return true; }
        if (arrow) {
            if (k == 'A' || k == 'B') {   // only the rows of the view in use (the waterfall has others than the spectrogram); no "Display" row (the . key's job)
                const std::vector<int> vis = spectro_visible_rows(cfg.spectro);
                int at = 0;
                for (size_t i = 0; i < vis.size(); ++i) if (vis[i] <= orow) at = static_cast<int>(i);
                const int n = static_cast<int>(vis.size());
                at = k == 'A' ? (at + n - 1) % n : (at + 1) % n;
                orow = vis[static_cast<size_t>(at)];
            }
            else if (k == 'D' || k == 'C') {
                if (orow == kSrDisplay) osci_adjust(cfg, kOrDisplay, k == 'C' ? +1 : -1);   // osci -> sphere -> spectro
                else { spectro_adjust(cfg.spectro, orow, k == 'C' ? +1 : -1); cfg.spectro_sync(); }
                ui.model.settings.dirty = true;
            }
            return true;
        }
        if (k == key_of(cfg, "ScopeWindow")) { toggle_scope_window(); return true; }      // the windows: always
        if (k == key_of(cfg, "SpectroWindow")) { toggle_spectro_window(); return true; }
        if (k == 27 || k == key_of(cfg, "SpectroMenu")) { ui.model.overlay = 0; settings_save(); return true; }
        if (k == key_of(cfg, "SpectroFull")) {
            ui.model.spectro_full = !ui.model.spectro_full; if (ui.model.spectro_full) ui.model.scope_big = false; out_clear_next = true;
            return true;
        }
        if (k == 'r' || k == 'R') { spectro_reset(cfg.spectro); cfg.spectro_sync(); ui.model.settings.dirty = true; return true; }
        return true;
    }

    if (ui.model.scope_big && ui.model.overlay == 0 && !ui.model.cheat_open) {   // the big oscilloscope overlay (SHIFT+0)
        UiModel& um = ui.model;
        if (um.scope_big_color_edit) {                    // the grid colour is being typed: every key belongs to the prompt
            if (k == 3 || k == kKeyCtrlC) { running = false; return true; }
            if (arrow) return true;
            if (k == 27) { um.scope_big_color_edit = false; }
            else if (k == 13 || k == 10) {
                um.scope_big_color_edit = false;
                if (um.scope_big_color_buf.empty()) cfg.scope_grid_color.clear();   // empty = the colour of inactive list elements
                else cfg.scope_grid_color = std::to_string(std::clamp(std::atoi(um.scope_big_color_buf.c_str()), 0, 255));
                settings_save();
            } else if ((k == 127 || k == 8) && !um.scope_big_color_buf.empty()) um.scope_big_color_buf.pop_back();
            else if (k >= '0' && k <= '9' && um.scope_big_color_buf.size() < 3) um.scope_big_color_buf.push_back(static_cast<char>(k));
            return true;
        }
        if (um.scope_metrics_open) {                      // the metrics window: UP / DOWN pick a row, LEFT / RIGHT change it
            if (k == 3 || k == kKeyCtrlC) { running = false; return true; }
            if (arrow) {
                if (k == 'A') um.scope_metrics_row = (um.scope_metrics_row + kScopeMetricRows - 1) % kScopeMetricRows;
                else if (k == 'B') um.scope_metrics_row = (um.scope_metrics_row + 1) % kScopeMetricRows;
                else if (k == 'C' || k == 'D') { scope_metric_step(cfg.scope_metrics, um.scope_metrics_row, k == 'C' ? 1 : -1); settings_save(); }
            } else if (k == 27 || k == 'P') um.scope_metrics_open = false;
            else if (k == 'r' || k == 'R') { cfg.scope_metrics = ScopeMetrics(); settings_save(); }
            return true;
        }
        if (arrow && (k == 'C' || k == 'D')) { osci_adjust(cfg, kOrStyle, k == 'C' ? +1 : -1); settings_save(); return true; }   // image <-> braille
        if (arrow && (k == 'A' || k == 'B')) { osci_adjust(cfg, kOrMusic, k == 'B' ? +1 : -1); settings_save(); return true; }   // oscilloscope music mode
        if (!arrow && k == 27) { um.scope_big = false; out_clear_next = true; return true; }
        if (!arrow && k == 'P') { um.scope_metrics_open = true; return true; }                                    // SHIFT+p: grid metrics
        if (!arrow && k == 'A') { cfg.scope_grid = !cfg.scope_grid; settings_save(); return true; }               // SHIFT+a: axes / grid
        if (!arrow && k == 'V') { cfg.scope_values = !cfg.scope_values; settings_save(); return true; }               // SHIFT+v: axis values
        if (!arrow && k == 'C') { um.scope_big_color_edit = true; um.scope_big_color_buf.clear(); return true; }   // SHIFT+c: grid colour
    }

    if (ui.model.spectro_full && ui.model.overlay == 0 && !ui.model.cheat_open && arrow && (k == 'A' || k == 'B' || k == 'C' || k == 'D')) {
        spectro_adjust(cfg.spectro, (k == 'C' || k == 'D') ? kSrStyle : kSrView, (k == 'C' || k == 'B') ? +1 : -1);   // image <-> braille, spectrogram <-> waterfall
        cfg.spectro_sync();
        settings_save();
        out_clear_next = true;
        return true;
    }

    if (ui.model.spectro_full && ui.model.overlay == 0 && !ui.model.cheat_open && k == 27 && !arrow) {   // ESC leaves the full screen
        ui.model.spectro_full = false;
        out_clear_next = true;
        return true;
    }

    if (ui.model.overlay != 0 && !ui.model.cheat_open) {
        const bool osci = ui.model.overlay == 1;
        int& orow = ui.model.overlay_row;
        auto close_overlay = [&]() { ui.model.overlay = 0; settings_save(); };   // like the player: closing saves
        if (k == 3 || k == kKeyCtrlC) { close_overlay(); running = false; return true; }
        if (arrow) {
            const std::vector<int> oids = osci_visible_rows(cfg);
            const int nov = osci ? static_cast<int>(oids.size()) : 3;
            orow = std::clamp(orow, 0, nov - 1);
            if (k == 'A') orow = (orow + nov - 1) % nov;
            else if (k == 'B') orow = (orow + 1) % nov;
            else if (k == 'D' || k == 'C') {
                if (osci) {
                    const int id = oids[static_cast<size_t>(orow)];
                    const std::string proto_before = cfg.gfx_protocol;
                    osci_adjust(cfg, id, k == 'C' ? +1 : -1);
                    if (cfg.gfx_protocol != proto_before) {   // another protocol was picked: ask / look again
                        if (gfx_shown) { std::string off = gfx_clear(gfx); write_frame(off); gfx_shown = false; }
                        if (gfx2_shown) { std::string off = gfx_clear(gfx, 1); write_frame(off); gfx2_shown = false; }
                        gfx = gfx_probe(cfg.gfx_protocol);
                        ui.model.gfx_proto = gfx;
                    }
                    ui.model.shuffle = cfg.playback_shuffle;
                } else { norm_adjust(cfg, orow, k == 'C' ? +1 : -1); apply_audio(); }
                ui.model.settings.dirty = true;
            }
            return true;
        }
        if (osci && k == key_of(cfg, "ScopeBig")) {   // the big overlay can be opened / closed from the options too
            ui.model.scope_big = !ui.model.scope_big; ui.model.spectro_full = false; ui.model.scope_big_color_edit = false; ui.model.scope_metrics_open = false; out_clear_next = true;
            return true;
        }
        if (k == 27 || (osci && k == key_of(cfg, "OsciMenu")) || (!osci && k == key_of(cfg, "NormMenu"))) { close_overlay(); return true; }
        if (k == 'r' || k == 'R') {
            if (osci) osci_reset(cfg); else { norm_reset(cfg); apply_audio(); }
            ui.model.settings.dirty = true;
            return true;
        }
        if (!osci && (k == ' ' || k == 'v')) { cfg.normalize = !cfg.normalize; apply_audio(); ui.model.settings.dirty = true; return true; }
        return true;
    }
    return false;
}

bool RadioApp::key_main(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    if (k == 27) {   // ESC in the main screen: first clear a search that is still there, then go back from a list to all stations
        UiModel& um = ui.model;
        if (!um.search.empty() || um.search_lists) { um.search.clear(); um.search_edit.reset(); um.search_lists = false; um.cursor = 0; }
        else if (!um.active_list.empty()) { um.active_list.clear(); um.cursor = 0; }
        ui.refilter(); ui.refilter_main_lists();
        return true;
    }

    if (k == kKeyShiftLeft || k == kKeyShiftRight) { ui.step_bank(k == kKeyShiftRight ? +1 : -1); return true; }   // main UI: previous / next preset

    if (arrow) {   // Left / Right do nothing in the main UI any more (channels: n / b / # / presets)
        if (k == 'A') ui.model.cursor = std::max(0, ui.model.cursor - (last_key_was_shifted() ? 10 : 1));   // SHIFT+arrow / PgUp / PgDn: a page
        else if (k == 'B') ui.model.cursor = std::min(std::max(0, nvis - 1), ui.model.cursor + (last_key_was_shifted() ? 10 : 1));
        return true;
    }

    if ((g_mode_key_next != 0 && k == g_mode_key_next) || (g_mode_key_prev != 0 && k == g_mode_key_prev)) {   // SHIFT and the + key ('*' on a German keyboard): the next mode; SHIFT and - ('_'): the previous one (keys: the player's REFERENCE tab)
        const int dir = (g_mode_key_next != 0 && k == g_mode_key_next) ? 1 : -1;
        if (muisc::next_mode_in(muisc::read_mode_config(), 1, dir) == 1) {
            ui.model.scope_notice = "the radio is the only mode switched on (Settings -> GENERAL -> MODES)";
            ui.model.scope_notice_until = ui.model.t_sec + 4;
            return true;
        }
        g_mode_switch_dir = dir;
        switch_mode = true; running = false; return true;
    }

    {   // Timeshift: pause / jump back and forth / back to live. Matched by the bound key itself (the key table's
        // translation maps a key to the first letter of its default, which "SPACE" would turn into 'S').
        auto mmss = [](double sec) {
            const long long t = std::max(0LL, static_cast<long long>(sec + 0.5));
            char b[24];
            if (t >= 3600) std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
            else std::snprintf(b, sizeof b, "%lld:%02lld", t / 60, t % 60);
            return std::string(b);
        };
        auto show = [&](const std::string& what) {
            const RadioStatus a = engine.status();
            std::string where = a.timeshift_paused ? "paused, " + mmss(a.behind_sec) + " behind live"
                              : a.timeshift_shifted && a.behind_sec >= 2.5 ? mmss(a.behind_sec) + " behind live" : "live";
            ui.model.scope_notice = what.empty() ? where : what + "  |  " + where;
            ui.model.scope_notice_until = ui.model.t_sec + 4;
        };
        const RadioStatus ts = engine.status();
        const bool tuned = ts.state != StreamState::Idle && ts.state != StreamState::Failed;
        auto jump_by = [&](double sec) {
            if (!tuned) { ui.model.notice = "timeshift: tune a station first"; notice_until = ui.model.t_sec + 4; return; }
            const double moved = engine.jump(sec);
            char b[48];
            if (std::fabs(moved) < 0.5) std::snprintf(b, sizeof b, sec < 0 ? "start of the buffer" : "already live");
            else std::snprintf(b, sizeof b, "%s%s", moved < 0 ? "-" : "+", mmss(std::fabs(moved)).c_str());
            show(b);
        };
        if (k == key_of(cfg, "Pause")) {
            if (!tuned) { ui.model.notice = "timeshift: tune a station first"; notice_until = ui.model.t_sec + 4; return true; }
            engine.toggle_pause();
            show(engine.status().timeshift_paused ? "PAUSE" : "PLAY");
            return true;
        }
        if (k == key_of(cfg, "Back30")) { jump_by(-30); return true; }
        if (k == key_of(cfg, "Fwd30")) { jump_by(30); return true; }
        if (k == key_of(cfg, "Back5m")) { jump_by(-300); return true; }
        if (k == key_of(cfg, "GoLive")) {
            if (tuned) { engine.go_live(); show("LIVE"); }
            return true;
        }
    }

    switch (key_translate(cfg, k)) {
        case 'q': case 3: case kKeyCtrlC: running = false; break;   // q is free now that the preset keys are 1234567890ertdfg
        case kKeyDegree: adev_open(false); break;   // CHOOSE AUDIO DEVICE ("°" / "&" by default; key_translate() hands its key over as this)
        case 13: case 10: tune_visible(ui.model.cursor); break;
        case 'n': tune_relative(+1); break;       // always the next channel in the list
        case 'b': tune_back(); break;              // previous channel; depends on the S/L mode
        case '#': tune_shuffle(); break;           // always a random next channel
        case 'j': case 'm': ui.model.shuffle = !ui.model.shuffle; cfg.playback_shuffle = ui.model.shuffle; ui.model.settings.dirty = true; break;      // S (shuffle) <-> L (list)
        case 'p':   // mute / unmute -- but with nothing loaded it tunes the hovered station, like ENTER
            if (engine.status().state == StreamState::Idle) tune_visible(ui.model.cursor);
            else engine.set_muted(!engine.status().muted);
            break;
        case '+': engine.set_volume(engine.status().volume + 5); break;
        case '=':                                  // SHIFT+0 on a German keyboard: the big oscilloscope overlay
            ui.model.scope_big = !ui.model.scope_big;
            ui.model.spectro_full = false;
            ui.model.scope_big_color_edit = false;
            ui.model.scope_metrics_open = false;
            out_clear_next = true;
            break;
        case '-': case '_': engine.set_volume(engine.status().volume - 5); break;
        case 'x': engine.stop(); break;
        case 's': settings_open(); break;      // the RADIO SETTINGS screen
        case 'L': ui.model.stov = StationsOverlay{}; ui.model.stov.open = true; break;   // the big STATIONS overlay
        case ')': toggle_scope_window(); break;     // SHIFT+9: the oscilloscope in its own window
        case '(': toggle_spectro_window(); break;   // SHIFT+8: the spectrogram in its own window
        case '.':                              // the scope block: oscilloscope -> sphere -> spectrogram -> oscilloscope
            // (what is switched off on the GENERAL tab is skipped; the sphere is always there)
            for (int n = 0; n < 3; ++n) {
                cfg.scope_mode = cfg.scope_mode == 0 ? 1 : cfg.scope_mode == 1 ? 3 : 0;
                if ((cfg.scope_mode == 0 && !cfg.use_osci) || (cfg.scope_mode == 3 && !cfg.use_spectro)) continue;
                break;
            }
            ui.model.settings.dirty = true;
            break;
        case 'B': ui.model.overlay = 10; ui.model.overlay_row = 0; ui.model.bars_scroll = 0; break;   // Shift+B: spectrum bars options
        case 'I': ui.model.overlay = 6; ui.model.overlay_row = kSrView; break;   // Shift+I: spectrogram options
        case 'U':                              // Shift+U: spectrogram full screen
            ui.model.spectro_full = !ui.model.spectro_full;
            if (ui.model.spectro_full) ui.model.scope_big = false;
            out_clear_next = true;
            break;
        case 'h': {                            // the LISTENING HISTORY
            HistoryModel& hm = ui.model.hmenu;
            hm.open = true; hm.flash.clear(); hm.cursor = 0; hm.top_cursor = 0; hm.scroll = 0;
            break;
        }
        case 'y': {                            // record the tuned stream: y while recording stops, else the record overlay
            const RadioStatus rs = engine.status();
            if (rs.recording) { engine.stop_recording(); ui.model.notice = "recording stopped: converting to MP3 ..."; notice_until = ui.model.t_sec + 6; break; }
            if (rs.state == StreamState::Idle || rs.state == StreamState::Failed) { ui.model.notice = "nothing to record: tune a station first"; notice_until = ui.model.t_sec + 4; break; }
            // "From now on" means the moment this overlay opened: that position is fixed here, so no time is lost
            // while choosing.
            ui.model.rec_anchor = rs.heard_frame;
            ui.model.rec_avail_sec = std::max(0.0, static_cast<double>(rs.heard_frame - rs.oldest_frame) / 48000.0);
            ui.model.overlay = 5;
            ui.model.overlay_row = 0;
            break;
        }
        case 'v': cfg.normalize = !cfg.normalize; apply_audio(); ui.model.settings.dirty = true; break;   // loudness normalisation on/off
        case 'V': ui.model.overlay = 2; ui.model.overlay_row = 0; break;   // Shift+V: loudness normalisation overlay
        case 'O': ui.model.overlay = 1; ui.model.overlay_row = 0; break;   // Shift+O: oscilloscope overlay
        case 'E': ui.model.overlay = 4; ui.model.overlay_row = 0; eq_open(cfg, ui.model.eq); break;   // Shift+E: equaliser (the preset key is the lowercase e)
        case 'Z': {                            // Shift+Z: sleep timer
            ui.model.overlay = 3;
            ui.model.overlay_row = 0;
            for (int i = 0; i < 5; ++i) if (sleep_minutes == kSleepChoice[i]) ui.model.overlay_row = i;
            break;
        }
        case 'R': engine.reconnect(); break;                          // also for a Radio Browser station that is not in the list
        case '/': set_search_focus(true); break;
        case '?': ui.model.cheat_open = true; ui.model.cheat_scroll = 0; break;
        case '<': ui.model.overlay = 7; ui.model.overlay_row = 1; break;   // the MENUS index: every menu and overlay with its key
        case 'W':                         // Shift+W: the help wizard, as in the music player
            wizard_open_now();
            break;
        case 'S': browse_open(); break;   // Shift+S: the RADIO BROWSER search menu
        case 'K': lm_open(); break;       // Shift+K: the STATION LISTS menu
        case '"': {                        // SHIFT+2 on a German keyboard: Latin letters on / off (the same switch as in the music player)
            const bool on = !muisc::translit_enabled();
            muisc::set_translit_enabled(on);
            muisc::write_latin_letters(on);   // config.txt: kept for the next start and for the player
            ui.model.notice = on ? "Latin letters: on (Japanese kana, Korean and Chinese text in Latin letters)" : "Latin letters: off";
            notice_until = ui.model.t_sec + 4;
            break;
        }
        case 'T': ui.toggle_sort(); break; // Shift+T: STATIONS sort, list order <-> name A-Z (search results too)
        case 'P': ui.model.menu.open = true; ui.model.menu.focus = MenuFocus::Search; ui.model.menu.target = SearchTarget::Stations;
                  ui.model.menu.flash.clear(); ui.model.menu.name = NameOverlay{};
                  ui.refilter_menu(); ui.refilter_presets(); ui.cursor_to_active_preset();
                  ui.model.menu.search_edit.to_end(ui.model.menu.search); ui.model.menu.psearch_edit.to_end(ui.model.menu.psearch);
                  set_text_entry(true); break;
        default:
            const int slot = preset_slot_for_key(k);
            if (slot >= 0) {
                const int idx = ui.model.presets[static_cast<size_t>(slot)];
                if (idx >= 0) tune_to(idx);
            }
            break;
    }
    return false;
}
