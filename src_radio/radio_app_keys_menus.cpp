#include "radio_app.h"
#include "cli_core.h"
namespace cli = muisc::cli;
#if defined(_WIN32)
#include "win_compat.h"
#endif

bool RadioApp::key_browse(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ RADIO BROWSER menu (Shift+S)
    if (ui.model.browse.open) {
        BrowseModel& bm = ui.model.browse;
        const int nres = bm.results ? static_cast<int>(bm.results->size()) : 0;
        const bool in_field = bm.focus != BrowseFocus::Results;
        const int fi = static_cast<int>(bm.focus);
        // In a pane Ctrl+C is COPY (text-entry mode delivers it as kKeyCtrlC); in RESULTS it quits.
        if ((k == 3 || k == kKeyCtrlC) && !in_field) { running = false; return true; }
        if (k == 9) {                                              // TAB: NAME ... BITRATE -> RESULTS -> NAME
            browse_focus(bm.focus == BrowseFocus::Results ? BrowseFocus::Name : static_cast<BrowseFocus>(fi + 1));
            return true;
        }
        if (k == 27) {                                             // ESC: clear the pane, else close
            if (in_field && !bm.text[fi].empty()) { bm.text[fi].clear(); bm.edit[fi].reset(); }
            else browse_close();
            return true;
        }
        if (k == 13 || k == 10) {                                  // ENTER: search (pane) / tune (RESULTS)
            if (in_field) browse_search();
            else if (bm.cursor >= 0 && bm.cursor < nres) browse_tune((*bm.results)[static_cast<size_t>(bm.cursor)]);
            return true;
        }
        if (arrow && (k == 'A' || k == 'B')) {
            if (!in_field) {                                       // RESULTS: move the cursor
                if (k == 'A') bm.cursor = std::max(0, bm.cursor - (last_key_was_shifted() ? 10 : 1));
                else bm.cursor = std::min(std::max(0, nres - 1), bm.cursor + (last_key_was_shifted() ? 10 : 1));
            } else if (k == 'A') { if (fi >= 2) browse_focus(static_cast<BrowseFocus>(fi - 2)); }   // up one pane row
            else browse_focus(fi + 2 >= kBrowseFields ? BrowseFocus::Results : static_cast<BrowseFocus>(fi + 2));
            return true;
        }
        if (in_field) {
            std::string status;
            edit_text_key(bm.text[fi], bm.edit[fi], k, kTextMax, &status);   // typing, caret, marking, Home/End, Del, Ctrl+C/X/V
            if (!status.empty()) bm.flash = status;
            return true;
        }
        // --- RESULTS focused
        if (k == '/') { browse_focus(BrowseFocus::Name); return true; }
        if (k == '?') { ui.model.cheat_open = true; ui.model.cheat_scroll = 0; return true; }
        if (k == 'a' && bm.cursor >= 0 && bm.cursor < nres) { browse_add((*bm.results)[static_cast<size_t>(bm.cursor)]); return true; }
        return true;
    }
    return false;
}

bool RadioApp::key_lists_menu(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ STATION LISTS menu (Shift+K)
    if (ui.model.lmenu.open) {
        ListMenuModel& lm = ui.model.lmenu;
        const int nstat = static_cast<int>(lm.visible.size());
        const int nitems = static_cast<int>(lm.items.size());
        const int nman = static_cast<int>(lm.mvisible.size());
        // prompts swallow every key but the three they care about (same as the player)
        if (lm.confirm_exit) {
            if (k == 'y' || k == 'Y') { lm.confirm_exit = false; lm_save(true); lm_sync_entry(); }
            else if (k == 'n' || k == 'N') lm_close();
            else if (k == 27) { lm.confirm_exit = false; lm_sync_entry(); }
            return true;
        }
        if (lm.confirm_delete) {
            if (k == 'y' || k == 'Y') { lm.confirm_delete = false; lm_delete(); lm_sync_entry(); }
            else if (k == 'n' || k == 'N' || k == 27) { lm.confirm_delete = false; lm_sync_entry(); }
            return true;
        }
        const bool in_text = lm_in_text();
        if ((k == 3 || k == kKeyCtrlC) && !in_text) { running = false; return true; }   // in a text field Ctrl+C copies
        if (k == 27) {                                    // ESC: leave a field typed in, clear a search with text, else leave (asking if unsaved)
            if (in_text && lm.typing) { lm.typing = false; return true; }   // Left/Right switch tabs again; the text stays
            if (lm.tab == 0 && lm.focus == ListFocus::Search && !lm.search.empty()) {
                lm.search.clear(); lm.search_edit.reset(); lm.cursor = 0; ui.refilter_lists_stations();
            } else if (lm.tab == 1 && lm.mfocus == ListManageFocus::Search && !lm.msearch.empty()) {
                lm.msearch.clear(); lm.msearch_edit.reset(); lm.mcursor = 0; ui.refilter_saved_lists();
            } else if (lm.tab == 0 && lm.dirty) { lm.confirm_exit = true; set_text_entry(false); }
            else lm_close();
            return true;
        }
        if (k == kKeyCtrlS) {                             // CTRL+s saves from every pane of tab 1
            if (lm.tab == 0) lm_save(false);
            else lm.flash = "switch to CREATE / EDIT (\u2190\u2192) to save a list";
            return true;
        }
        // Tab switch: plain Left/Right -- except in a text box that is in use (typed / edited in), where they move
        // the caret until ESC, TAB or ENTER. ALT+Left/Right switches from everywhere.
        if (k == kKeyAltLeft || k == kKeyAltRight || (arrow && (k == 'C' || k == 'D') && !(in_text && lm.typing))) {
            lm.tab = 1 - lm.tab;
            lm.typing = false;
            if (lm.tab == 1) ui.refilter_saved_lists();
            lm_sync_entry();
            return true;
        }
        // Any other key that reaches a text box below (typing, BACKSPACE / DEL, HOME / END, marking,
        // the clipboard) puts it in use. Up/Down walk the list below the box, ENTER / TAB leave it.
        if (in_text && !(arrow && (k == 'A' || k == 'B')) && k != 13 && k != 10 && k != 9) lm.typing = true;
        if (lm.tab == 1) {
            // --- tab 2: SAVED STATION LISTS
            if (k == 9) { lm_mfocus(lm.mfocus == ListManageFocus::Search ? ListManageFocus::List : ListManageFocus::Search); return true; }
            if (arrow && (k == 'A' || k == 'B')) {
                lm.mcursor = k == 'A' ? std::max(0, lm.mcursor - (last_key_was_shifted() ? 10 : 1)) : std::min(std::max(0, nman - 1), lm.mcursor + (last_key_was_shifted() ? 10 : 1));
                return true;
            }
            if (lm.mfocus == ListManageFocus::Search) {
                if (k == 13 || k == 10) { lm_mfocus(ListManageFocus::List); return true; }
                std::string status;
                const bool changed = edit_text_key(lm.msearch, lm.msearch_edit, k, kTextMax, &status);
                lm.flash = status;
                if (changed) { lm.mcursor = 0; ui.refilter_saved_lists(); }
                return true;
            }
            if (k == 13 || k == 10) { lm_load(); return true; }
            if (k == kKeyDelete && nman > 0) { lm.confirm_delete = true; return true; }
            if (k == 'j') { lm.mcursor = std::min(std::max(0, nman - 1), lm.mcursor + 1); return true; }
            if (k == 'k') { lm.mcursor = std::max(0, lm.mcursor - 1); return true; }
            if (k == '/') { lm_mfocus(ListManageFocus::Search); return true; }
            if (k == '?') { ui.model.cheat_open = true; ui.model.cheat_scroll = 0; return true; }
            return true;
        }
        // --- tab 1: CREATE / EDIT
        if (k == 9) {                                    // TAB: name -> search -> STATIONS -> LIST CONTENTS -> name
            lm_focus(lm.focus == ListFocus::Name ? ListFocus::Search
                     : lm.focus == ListFocus::Search ? ListFocus::Stations
                     : lm.focus == ListFocus::Stations ? ListFocus::Contents : ListFocus::Name);
            return true;
        }
        if (lm.focus == ListFocus::Name) {
            if (k == 13 || k == 10) { lm_focus(ListFocus::Search); return true; }    // confirm the name, go and pick stations
            std::string status;
            if (edit_limited(lm.name, lm.name_edit, k, static_cast<size_t>(kListNameMax), &status)) lm.dirty = true;
            lm.flash = status;
            return true;
        }
        if (lm.focus == ListFocus::Search) {
            if (arrow && (k == 'A' || k == 'B')) {      // Up/Down keep walking the results while the box has the keyboard
                lm.cursor = k == 'A' ? std::max(0, lm.cursor - (last_key_was_shifted() ? 10 : 1)) : std::min(std::max(0, nstat - 1), lm.cursor + (last_key_was_shifted() ? 10 : 1));
                return true;
            }
            if (k == 13 || k == 10) { lm_add(); return true; }
            std::string status;
            const bool changed = edit_text_key(lm.search, lm.search_edit, k, kTextMax, &status);
            lm.flash = status;
            if (changed) { lm.cursor = 0; ui.refilter_lists_stations(); }
            return true;
        }
        if (lm.focus == ListFocus::Stations) {
            if (arrow && (k == 'A' || k == 'B')) {
                lm.cursor = k == 'A' ? std::max(0, lm.cursor - (last_key_was_shifted() ? 10 : 1)) : std::min(std::max(0, nstat - 1), lm.cursor + (last_key_was_shifted() ? 10 : 1));
                return true;
            }
            if (k == 'j') { lm.cursor = std::min(std::max(0, nstat - 1), lm.cursor + 1); return true; }
            if (k == 'k') { lm.cursor = std::max(0, lm.cursor - 1); return true; }
            if (k == 13 || k == 10) { lm_add(); return true; }
            if (k == 'T') {                              // SHIFT+t: list order <-> name A-Z (also for search results)
                ui.toggle_lists_sort();
                lm.flash = lm.sort_az ? "STATIONS sorted by name A-Z" : "STATIONS in list order";
                return true;
            }
        } else {   // Contents
            if (arrow && (k == 'A' || k == 'B')) {
                lm.item_cursor = k == 'A' ? std::max(0, lm.item_cursor - 1) : std::min(std::max(0, nitems - 1), lm.item_cursor + 1);
                return true;
            }
            if (k == 'j') { lm.item_cursor = std::min(std::max(0, nitems - 1), lm.item_cursor + 1); return true; }
            if (k == 'k') { lm.item_cursor = std::max(0, lm.item_cursor - 1); return true; }
            if (k == 13 || k == 10) {                    // tune the hovered station (audition the list)
                if (lm.item_cursor >= 0 && lm.item_cursor < nitems) tune_to(lm.items[static_cast<size_t>(lm.item_cursor)]);
                return true;
            }
            if (k == kKeyDelete || k == 127 || k == 8 || k == 'd') { lm_remove(); return true; }
            if (k == '4') { lm_move(-1); return true; }
            if (k == '5') { lm_move(+1); return true; }
        }
        // keys shared by STATIONS and LIST CONTENTS (never reached from a text field)
        if (k == '/') { lm_focus(ListFocus::Search); return true; }
        if (k == '?') { ui.model.cheat_open = true; ui.model.cheat_scroll = 0; return true; }
        return true;
    }
    return false;
}

bool RadioApp::key_presets_menu(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    // ------------------------------------------------------------------ PRESETS menu (P)
    if (ui.model.menu.open) {
        MenuModel& mn = ui.model.menu;
        const int mvis = static_cast<int>(mn.visible.size());
        const int pvis = static_cast<int>(mn.pvisible.size());
        auto set_focus = [&](MenuFocus f) {
            mn.focus = f;
            if (f == MenuFocus::Search) { mn.search_edit.to_end(mn.search); mn.psearch_edit.to_end(mn.psearch); }
            set_text_entry(f == MenuFocus::Search);
        };
        // SHIFT+d (twice) deletes the hovered preset; any other key withdraws the first press.
        const int del_pending = mn.del_confirm;
        if (!(k == 'D' && !arrow && mn.focus == MenuFocus::Presets)) mn.del_confirm = -1;
        auto tune_hovered = [&]() {
            if (mn.cursor < 0 || mn.cursor >= mvis) return;
            tune_to(mn.visible[static_cast<size_t>(mn.cursor)]);
        };
        auto open_hovered_preset = [&]() {
            if (mn.pcursor < 0 || mn.pcursor >= pvis) return;
            const int bi = mn.pvisible[static_cast<size_t>(mn.pcursor)];
            ui.use_bank(bi);
            mn.flash = "Preset \"" + ui.model.banks[static_cast<size_t>(bi)].name + "\" opened";
            for (int sl = 0; sl < static_cast<int>(ui.model.presets.size()); ++sl)      // ENTER also tunes in: the first filled slot
                if (ui.model.presets[static_cast<size_t>(sl)] >= 0) { tune_to(ui.model.presets[static_cast<size_t>(sl)]); mn.flash += "  -- tuned slot " + std::string(1, kPresetKeys[sl]); break; }
            if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
        };
        auto close_menu = [&]() { mn.open = false; mn.name.open = false; set_focus(MenuFocus::Stations); set_text_entry(false); mn.flash.clear(); };
        // moves in the SELECT PRESET grid (4 columns)
        auto move_preset = [&](int d) { mn.pcursor = std::clamp(mn.pcursor + d, 0, std::max(0, pvis - 1)); };
        auto open_name_overlay = [&](bool rename) {
            NameOverlay& ov = mn.name;
            ov = NameOverlay{};
            ov.open = true; ov.rename = rename;
            if (rename) {
                ov.target = mn.pvisible[static_cast<size_t>(mn.pcursor)];
                ov.text = ui.model.banks[static_cast<size_t>(ov.target)].name;
                ov.edit.to_end(ov.text);
            }
            set_text_entry(true);
        };

        // --- name overlay (Shift+N / Shift+C): owns every key while it is open
        if (mn.name.open) {
            NameOverlay& ov = mn.name;
            auto leave = [&]() { ov.open = false; set_text_entry(mn.focus == MenuFocus::Search); };
            if (k == 27) { leave(); return true; }
            if (k == 13 || k == 10) {
                const std::string nm = clean_preset_name(ov.text);
                if (nm.empty()) { ov.error = "type a name first"; return true; }
                if (ui.bank_name_taken(nm, ov.rename ? ov.target : -1)) { ov.error = "a preset with that name exists"; return true; }
                if (ov.rename) {
                    ui.model.banks[static_cast<size_t>(ov.target)].name = nm;
                    mn.flash = "Preset renamed to \"" + nm + "\"";
                } else {
                    PresetBank b; b.name = nm;
                    ui.sync_active();
                    ui.model.banks.push_back(std::move(b));
                    ui.use_bank(static_cast<int>(ui.model.banks.size()) - 1);   // the new preset opens, ready to be filled
                    mn.psearch.clear(); mn.psearch_edit.reset();
                    mn.pcursor = 0;
                    mn.flash = "Preset \"" + nm + "\" created";
                }
                ui.refilter_presets();
                ui.cursor_to_active_preset();
                if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
                leave();
                return true;
            }
            // caret, marking, Home/End, Delete, Ctrl+C/X/V and typing -- the player's own text editor.
            // The name is limited in CHARACTERS (kPresetNameMax), the editor in bytes, so measure after.
            const std::string before = ov.text;
            const EditState before_edit = ov.edit;
            if (edit_text_key(ov.text, ov.edit, k, static_cast<size_t>(kPresetNameMax) * 4)) {
                if (count_chars(ov.text) > static_cast<size_t>(kPresetNameMax)) {
                    if (k == kKeyCtrlV) {
                        // too long a paste: keep what fits, cut the surplus off the end of the pasted part
                        const size_t start = std::min(before_edit.caret, before_edit.anchor);
                        while (count_chars(ov.text) > static_cast<size_t>(kPresetNameMax) && ov.edit.caret > start) {
                            size_t i = ov.edit.caret - 1;
                            while (i > start && (static_cast<unsigned char>(ov.text[i]) & 0xC0) == 0x80) --i;
                            ov.text.erase(i, ov.edit.caret - i);
                            ov.edit.caret = i;
                        }
                        ov.edit.anchor = ov.edit.caret;
                    } else {                                   // a typed character into a full box: refused
                        ov.text = before; ov.edit = before_edit;
                    }
                }
                ov.error.clear();
            }
            return true;
        }

        // In the search box Ctrl+C is COPY (text-entry mode delivers it as kKeyCtrlC) and Shift+Left/Right
        // mark text; everywhere else they quit / switch the preset.
        if ((k == 3 || k == kKeyCtrlC) && mn.focus != MenuFocus::Search) { running = false; return true; }
        if ((k == kKeyShiftLeft || k == kKeyShiftRight) && mn.focus != MenuFocus::Search) {   // previous / next preset
            if (ui.model.banks.size() > 1) {
                ui.step_bank(k == kKeyShiftRight ? +1 : -1);
                ui.cursor_to_active_preset();
                mn.flash.clear();
            }
            return true;
        }
        if (arrow && !(mn.focus == MenuFocus::Search && (k == 'C' || k == 'D'))) {   // (search box: Left/Right = caret)
            const bool grid = mn.focus == MenuFocus::Presets || (mn.focus == MenuFocus::Search && mn.target == SearchTarget::Presets);
            if (grid) {
                if (k == 'A') move_preset(-kPresetPaneCols);
                else if (k == 'B') { const int last = std::max(0, pvis - 1);
                                     mn.pcursor = (mn.pcursor + kPresetPaneCols <= last) ? mn.pcursor + kPresetPaneCols
                                                : (mn.pcursor / kPresetPaneCols < last / kPresetPaneCols ? last : mn.pcursor); }
                else if (k == 'D') move_preset(-1);
                else if (k == 'C') move_preset(+1);
            } else {
                if (k == 'A') mn.cursor = std::max(0, mn.cursor - 1);
                else if (k == 'B') mn.cursor = std::min(std::max(0, mvis - 1), mn.cursor + 1);
            }
            return true;
        }
        if (k == 9) {                                                            // TAB: SEARCH -> SELECT PRESET -> STATIONS -> ...
            set_focus(mn.focus == MenuFocus::Search ? MenuFocus::Presets
                      : mn.focus == MenuFocus::Presets ? MenuFocus::Stations : MenuFocus::Search);
            return true;
        }
        if (k == 27) {                                                           // ESC
            std::string& q = mn.target == SearchTarget::Presets ? mn.psearch : mn.search;
            if (mn.focus == MenuFocus::Search && !q.empty()) {
                q.clear(); (mn.target == SearchTarget::Presets ? mn.psearch_edit : mn.search_edit).reset();
                mn.cursor = 0; mn.pcursor = 0; ui.refilter_menu(); ui.refilter_presets();
            } else close_menu();
            return true;
        }
        if (k == 13 || k == 10) {                                                // ENTER: tune a station / open a preset
            if (mn.focus == MenuFocus::Presets) open_hovered_preset();
            else if (mn.focus == MenuFocus::Search && mn.target == SearchTarget::Presets) { open_hovered_preset(); set_focus(MenuFocus::Presets); }
            else { tune_hovered(); if (mn.focus == MenuFocus::Search) set_focus(MenuFocus::Stations); }
            return true;
        }
        if (mn.focus == MenuFocus::Search) {
            const bool presets = mn.target == SearchTarget::Presets;
            std::string& q = presets ? mn.psearch : mn.search;
            EditState& es = presets ? mn.psearch_edit : mn.search_edit;
            std::string status;   // "COPIED" / "CUT" / "PASTED" -> the status line under the hints
            const bool changed = edit_text_key(q, es, k, kTextMax, &status);
            mn.flash = status;
            if (!changed) return true;   // caret / marking / copy: the results stay exactly where they are
            // "p:" typed into an empty box points the search at the presets, "s:" back at the stations
            // (the older "/p:" and "/s:" still work)
            const std::string lq = ascii_lower_str(q);
            if (lq == "p:" || lq == "s:" || lq == "/p:" || lq == "/s:") {
                q.clear(); es.reset();
                mn.target = lq.find('p') != std::string::npos ? SearchTarget::Presets : SearchTarget::Stations;
            }
            mn.cursor = 0; mn.pcursor = 0;
            ui.refilter_menu(); ui.refilter_presets();
            return true;
        }
        if (k == '?') { ui.model.cheat_open = true; ui.model.cheat_scroll = 0; return true; }
        if (k == '/') { mn.target = mn.focus == MenuFocus::Presets ? SearchTarget::Presets : SearchTarget::Stations; set_focus(MenuFocus::Search); return true; }
        // --- SELECT PRESET focused
        if (mn.focus == MenuFocus::Presets) {
            if (k == 'j') { move_preset(+kPresetPaneCols); return true; }
            if (k == 'k') { move_preset(-kPresetPaneCols); return true; }
            if (k == 'l') { move_preset(+1); return true; }
            if (k == 'h') { move_preset(-1); return true; }
            if (k == 'N') { open_name_overlay(false); return true; }                       // SHIFT+n: new preset
            if (k == 'C' && pvis > 0) { open_name_overlay(true); return true; }            // SHIFT+c: rename the hovered preset
            if (k == 'D' && !arrow && pvis > 0) {                                        // SHIFT+d: delete the hovered preset (twice)
                const int bi = mn.pvisible[static_cast<size_t>(std::clamp(mn.pcursor, 0, pvis - 1))];
                const std::string bname = ui.model.banks[static_cast<size_t>(bi)].name;
                if (ui.model.banks.size() < 2) { mn.flash = "The last preset cannot be deleted"; return true; }
                if (del_pending != bi) {
                    mn.del_confirm = bi;
                    mn.flash = "Delete preset \"" + bname + "\"? Press SHIFT+d again to confirm, any other key cancels";
                    return true;
                }
                ui.sync_active();
                ui.model.banks.erase(ui.model.banks.begin() + bi);
                if (bi < ui.model.bank_active) --ui.model.bank_active;
                else if (bi == ui.model.bank_active) ui.model.bank_active = std::min(bi, static_cast<int>(ui.model.banks.size()) - 1);
                ui.model.presets = ui.model.banks[static_cast<size_t>(ui.model.bank_active)].slots;
                ui.refilter_presets();
                mn.flash = "Preset \"" + bname + "\" deleted";
                if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
                return true;
            }
            return true;
        }
        // --- STATIONS focused
        if (k == 'j') { mn.cursor = std::min(std::max(0, mvis - 1), mn.cursor + 1); return true; }
        if (k == 'k') { mn.cursor = std::max(0, mn.cursor - 1); return true; }
        if (mn.cursor < 0 || mn.cursor >= mvis) return true;
        const int idx = mn.visible[static_cast<size_t>(mn.cursor)];
        const std::string& nm = ui.stations[static_cast<size_t>(idx)].name;
        const int slot = preset_slot_for_key(k);
        if (slot >= 0) {
            auto& p = ui.model.presets;
            if (p[static_cast<size_t>(slot)] == idx) {             // same key again: clear
                p[static_cast<size_t>(slot)] = -1;
                mn.flash = std::string("Preset ") + kPresetKeys[slot] + " cleared";
            } else {
                const int old = preset_slot_of(p, idx);
                if (old >= 0) p[static_cast<size_t>(old)] = -1;      // a station sits in one slot only: it moves
                p[static_cast<size_t>(slot)] = idx;
                mn.flash = std::string("Preset ") + kPresetKeys[slot] + " = " + nm
                         + (old >= 0 ? std::string("  (moved from ") + kPresetKeys[old] + ")" : "");
            }
            if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
        } else if (k == kKeyDelete || k == 127 || k == 8) {
            if (clear_station_from_presets(ui.model.presets, idx)) {
                mn.flash = "Removed " + nm + " from the presets";
                if (!ui.persist()) mn.flash += "  -- could not write " + presets_path();
            }
        }
        return true;
    }
    return false;
}

bool RadioApp::key_search(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    if (ui.model.search_focus) {
        // Up/Down walk the results, everything else goes to the text editor (Left/Right = caret,
        // Shift+Left/Right = mark, Home/End, Del, Ctrl+C/X/V, typing). Ctrl+C is copy here, not quit.
        UiModel& um = ui.model;
        const int nl = static_cast<int>(um.lvisible.size());
        if (cli_search_key(kc)) return true;   // "c:" (command line) / "cs:" (script search)
        if (arrow && (k == 'A' || k == 'B')) {
            int& cur = um.search_lists ? um.lcursor : um.cursor;
            const int n = um.search_lists ? nl : nvis;
            cur = k == 'A' ? std::max(0, cur - 1) : std::min(std::max(0, n - 1), cur + 1);
        } else if (k == 27) {   // clear the box, back to searching stations, leave it
            um.search.clear(); um.search_edit.reset(); um.search_lists = false;
            ui.refilter(); ui.refilter_main_lists(); set_search_focus(false);
        } else if (k == 13 || k == 10) {
            if (um.search_lists) {                // ENTER on a list: its stations fill the STATIONS pane (ESC = all again)
                if (um.lcursor >= 0 && um.lcursor < nl) {
                    um.active_list = um.lists[static_cast<size_t>(um.lvisible[static_cast<size_t>(um.lcursor)])].name;
                    um.search.clear(); um.search_edit.reset(); um.search_lists = false; um.cursor = 0;
                    ui.refilter(); ui.refilter_main_lists(); set_search_focus(false);
                    tune_visible(0);                   // ENTER also tunes in: the first station of the list
                }                                  // no list matches: stay in the box, nothing to open
            } else if (nvis > 0) { set_search_focus(false); tune_visible(um.cursor); }
            // no hit: stay in the box, so the query can be edited or cleared with ESC
        } else if (edit_text_key(um.search, um.search_edit, k, kTextMax)) {
            // "p:" typed into the empty box points it at the station lists, "s:" back at the stations
            // (the older "/p:" and "/s:" spellings work too) -- same switch as the PRESETS menu
            const std::string lq = ascii_lower_str(um.search);
            if (lq == "p:" || lq == "s:" || lq == "/p:" || lq == "/s:") {
                um.search.clear(); um.search_edit.reset();
                um.search_lists = lq.find('p') != std::string::npos;
            }
            um.cursor = 0; um.lcursor = 0;
            ui.refilter(); ui.refilter_main_lists();
            if (search_is_cs(um.search)) { um.cliov.filter = cli::trim(um.search.substr(um.search.find(':') + 1)); cliov_refresh(); }   // the pane lists the matching scripts
        }
        return true;
    }
    return false;
}

bool RadioApp::key_stations_overlay(KeyCtx& kc) {
    [[maybe_unused]] int& k = kc.k;
    [[maybe_unused]] const bool arrow = kc.arrow;
    [[maybe_unused]] const int nvis = kc.nvis;
    [[maybe_unused]] int& menus_inject = kc.menus_inject;
    if (ui.model.stov.open) {                                        // the big STATIONS overlay (same state as the main pane)
        UiModel& um = ui.model;
        StationsOverlay& so = um.stov;
        const int nl = static_cast<int>(um.lvisible.size());
        if (k == 27 && !arrow) {
            if (!um.search.empty() || um.search_lists) { um.search.clear(); um.search_edit.reset(); um.search_lists = false; um.cursor = 0; }
            else if (!um.active_list.empty()) { um.active_list.clear(); um.cursor = 0; }
            else { so.open = false; so.flash.clear(); }
            ui.refilter(); ui.refilter_main_lists();
            return true;
        }
        if (!arrow && k == key_of(cfg, "Stations")) { so.open = false; so.flash.clear(); return true; }
        if (um.search_lists && arrow) {   // the pane lists the station lists
            if (k == 'A') um.lcursor = std::max(0, um.lcursor - (last_key_was_shifted() ? 10 : 1));
            else if (k == 'B') um.lcursor = std::min(std::max(0, nl - 1), um.lcursor + (last_key_was_shifted() ? 10 : 1));
            return true;
        }
        if (um.search_lists && (k == 13 || k == 10)) {
            if (um.lcursor >= 0 && um.lcursor < nl) {
                um.active_list = um.lists[static_cast<size_t>(um.lvisible[static_cast<size_t>(um.lcursor)])].name;
                um.search.clear(); um.search_edit.reset(); um.search_lists = false; um.cursor = 0;
                ui.refilter(); ui.refilter_main_lists();
                tune_visible(0);                       // ENTER also tunes in: the first station of the list
            }
            return true;
        }
        if (!arrow && k == 'a') {
            so.add_open = true; so.add_field = 0; so.add_url.clear(); so.add_name.clear(); so.add_error.clear();
            so.add_edit[0].reset(); so.add_edit[1].reset(); so.flash.clear(); set_text_entry(true);
            return true;
        }
        if (!arrow && k == 'C') {
            if (um.cursor >= 0 && um.cursor < nvis && !um.search_lists) stov_edit_open(um.visible[static_cast<size_t>(um.cursor)]);
            return true;
        }
        if (!arrow && k == 'U') { if (!um.search_lists) stov_undo_do(); return true; }
        if (k == kKeyDelete) {
            if (um.cursor >= 0 && um.cursor < nvis && !um.search_lists) { so.del_idx = um.visible[static_cast<size_t>(um.cursor)]; so.flash.clear(); }
            return true;
        }
        {   // keys that would open another menu are ignored here
            const int kt = key_translate(cfg, k);
            if (!arrow && (kt == 's' || kt == 'h' || kt == 'K' || kt == 'S' || kt == 'P' || kt == 'O' || kt == 'V' || kt == 'Z' || kt == 'E')) return true;
        }
    }
    return false;
}
