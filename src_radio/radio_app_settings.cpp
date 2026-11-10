#include "radio_app.h"
#include "path_utf8.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

void RadioApp::settings_save() {
    SettingsModel& sm = ui.model.settings;
    std::string err;
    if (save_radio_settings(cfg, &err)) { sm.status = "Saved " + muisc::path_utf8(radio_config_path()); sm.dirty = false; }
    else sm.status = "Could not save: " + err;
}

// The first time the help wizard is closed it stops opening by itself.
// Written right away rather than at exit, so a crash cannot bring the
// welcome back on the next start.
void RadioApp::wizard_seen_save() {
    if (cfg.wizard_seen) return;
    cfg.wizard_seen = true;
    save_radio_settings(cfg, nullptr);
}

void RadioApp::wizard_open_now() {
    if (!wizard::kEnabled) return;
    wizard::open(ui.model.wizard, !cfg.wizard_seen);
    ui.model.wizard_open = true;
}

// A PATHS value changed: switch the storage files (copying what is missing there), reload what lives in them, move the
// history store and tell the downloader about the new folder.
void RadioApp::storage_changed() {
    SettingsModel& sm = ui.model.settings;
    const StoragePaths np = resolve_storage_paths(cfg);
    std::string msg;
    if (np.stations_file != storage.stations_file || np.lists_file != storage.lists_file || np.presets_file != storage.presets_file) {
        engine.stop();   // the station numbers change with the files
        set_storage_files(np.stations_file.string(), np.lists_file.string(), np.presets_file.string());
        load_storage();
        ui.model.search_lists = false; ui.model.active_list.clear();
        ui.refilter(); ui.refilter_menu(); ui.refilter_presets(); ui.refilter_lists_stations(); ui.refilter_saved_lists(); ui.refilter_main_lists();
        msg = "Stations, presets and lists now come from the new folders (stopped playback). ";
    }
    if (np.history_dir != storage.history_dir) {
        rhist.close_live();
        rhist.open(np.history_dir);
        msg += "History moved. ";
    }
    yt.set_download_dir(np.download_dir);
    storage = np;
    sm.status = msg.empty() ? "" : msg;
}

std::filesystem::path RadioApp::theme_dir_path(std::string dir) {
    if (!dir.empty() && dir[0] == '~' && (dir.size() == 1 || dir[1] == '/' || dir[1] == '\\')) {
        const char* home = std::getenv("HOME");
        if (!home || !*home) home = std::getenv("USERPROFILE");
        if (home) dir = std::string(home) + dir.substr(1);
    }
    return std::filesystem::path(dir);
}

void RadioApp::theme_io_rescan() {
    SettingsModel& sm = ui.model.settings;
    sm.theme_io_files = themes::list_theme_files(theme_dir_path(sm.theme_io_dir));
    sm.theme_io_sel = std::clamp(sm.theme_io_sel, 0, std::max(0, static_cast<int>(sm.theme_io_files.size()) - 1));
}

void RadioApp::settings_open() {
    SettingsModel& sm = ui.model.settings;
    settings_snapshot = cfg; settings_snapshot_valid = true;
    sm.open = true; sm.editing = false; sm.status.clear();
    sm.tab = 0; sm.row = 0; sm.col = 0;   // like the music player: always start on the first tab
    sm.row = std::clamp(sm.row, 0, kColorRowCount - 1);
    sm.theme_ui = 0;
    sm.theme_list = themes::load_all();
}

// [S] saves and leaves the menu; ESC / Q leave WITHOUT saving: what was changed meanwhile is taken back
// (the settings and the key undo stack as they were when the menu opened).
void RadioApp::settings_close(bool save) {
    SettingsModel& sm = ui.model.settings;
    if (save) { settings_save(); }
    else if (sm.dirty && settings_snapshot_valid) {
        cfg = settings_snapshot;
        apply_audio();
        storage_changed();
        sm.dirty = false;
    }
    settings_snapshot_valid = false;
    key_undo.clear();
    sm.open = false; sm.editing = false; sm.theme_ui = 0; sm.remote_ov = muisc::remote_ui::State{};
    set_text_entry(false);
}

// CHOOSE AUDIO DEVICE (GENERAL -> AUDIO [ENTER], or its key on the main screen: overlay 9). The list is read when it opens.
void RadioApp::adev_open(bool from_settings) {
    SettingsModel& sm = ui.model.settings;
    sm.adev_err.clear();
    sm.adev_list = muisc::list_output_devices(&sm.adev_err);
    sm.adev_active = engine.active_device();
    sm.adev_sel = 0;
    for (size_t i = 0; i < sm.adev_list.size(); ++i)
        if (!cfg.audio_device.empty() && sm.adev_list[i].name == cfg.audio_device) sm.adev_sel = static_cast<int>(i) + 1;
    sm.adev_open = true;
    sm.adev_from_settings = from_settings;
    if (!from_settings) { ui.model.overlay = 9; ui.model.overlay_row = 0; }
}

void RadioApp::adev_close() {
    SettingsModel& sm = ui.model.settings;
    sm.adev_open = false;
    if (!sm.adev_from_settings) ui.model.overlay = 0;
}

// The pick is written into config.txt at once (one choice for all modes) and the stream moves to that device now.
void RadioApp::adev_pick() {
    SettingsModel& sm = ui.model.settings;
    const std::string pick = sm.adev_sel > 0 && sm.adev_sel <= static_cast<int>(sm.adev_list.size()) ? sm.adev_list[static_cast<size_t>(sm.adev_sel - 1)].name : std::string();
    cfg.audio_device = pick;
    if (settings_snapshot_valid) settings_snapshot.audio_device = pick;   // ESC in the settings does not take it back
    muisc::set_config_value("AudioDevice", pick);   // the player's config.txt (one choice for all modes)
    std::string err;
    engine.set_output_device(pick, &err);
    const std::string now = engine.active_device();
    std::string msg = "audio device: " + (pick.empty() ? std::string("System Default") : pick);
    if (!pick.empty() && !now.empty() && now != pick) msg += " -- could not be opened, the default device plays";
    if (!err.empty()) msg = "audio device: " + err;
    if (sm.adev_from_settings) sm.status = msg;
    else { ui.model.notice = msg; notice_until = ui.model.t_sec + 6; }
}

// Has anything been changed since the settings screen opened? (the same text that [s] would write)
bool RadioApp::settings_changed() {
    return settings_snapshot_valid && radio_settings_text(cfg) != radio_settings_text(settings_snapshot);
}

// The CREATE THEME overlay. edit_current ([e]): it opens on the colours in use; if they are a theme of your own its name is
// already in the NAME field and saving overwrites it (after a question), a built-in theme needs a new name.
void RadioApp::theme_open_creator(bool edit_current) {
    SettingsModel& sm = ui.model.settings;
    sm.theme_list = themes::load_all();
    const int cur = theme_index(cfg, sm.theme_list);
    sm.theme_base = sm.theme_list[static_cast<size_t>(cur >= 0 ? cur : 0)];
    sm.theme_draft = cfg; sm.theme_name.clear(); sm.theme_row = -1; sm.theme_col = 0; sm.theme_ui = 2;
    sm.status.clear();
    if (edit_current) {
        if (cur >= 0 && !sm.theme_list[static_cast<size_t>(cur)].builtin) sm.theme_name = sm.theme_list[static_cast<size_t>(cur)].name;
        else sm.status = cur >= 0 ? "\"" + sm.theme_list[static_cast<size_t>(cur)].name + "\" is built in and cannot be overwritten: give your edit a new name"
                                  : "these colors are not a saved theme: give them a name";
    }
    sm.theme_name_initial = sm.theme_name;
    sm.theme_initial_kv = theme_capture(sm.theme_draft, sm.theme_base).kv;
    sm.leave_prompt = 0;
}

bool RadioApp::theme_changed() {
    const SettingsModel& sm = ui.model.settings;
    return sm.theme_name != sm.theme_name_initial || theme_capture(sm.theme_draft, sm.theme_base).kv != sm.theme_initial_kv;
}

void RadioApp::theme_save_now() {
    SettingsModel& sm = ui.model.settings;
    themes::Theme t = theme_capture(sm.theme_draft, sm.theme_base);
    t.name = themes::sanitize_name(sm.theme_name);
    std::string err;
    if (!themes::save_theme(t, err)) { sm.status = err; return; }
    sm.theme_list = themes::load_all();
    theme_apply(cfg, t);
    sm.dirty = true; sm.theme_ui = 0;
    sm.status = "THEME \"" + t.name + "\" SAVED AND APPLIED (s saves the settings)";
}

// [s] in the creator: a name is needed, a built-in theme cannot be overwritten, overwriting a theme of your own is asked first.
void RadioApp::theme_try_save() {
    SettingsModel& sm = ui.model.settings;
    const std::string name = themes::sanitize_name(sm.theme_name);
    if (name.empty()) { sm.status = "give the theme a name first (NAME row), then [s]"; return; }
    const int at = themes::find_name(sm.theme_list, name);
    if (at >= 0) {
        if (sm.theme_list[static_cast<size_t>(at)].builtin) { sm.status = "\"" + sm.theme_list[static_cast<size_t>(at)].name + "\" is a built-in theme and cannot be overwritten -- pick another name"; return; }
        sm.leave_prompt = 3;
        return;
    }
    theme_save_now();
}

// REFERENCE tab: the last 5 key changes can be undone with Ctrl+Shift+U. An entry is the key map from before the
// change and the action it concerned (-1 = all keys were reset); undoing it puts the cursor back on that row.
void RadioApp::key_change_begin(int row) {
    key_undo.push_back({cfg.keys, row});
    if (key_undo.size() > 5) key_undo.erase(key_undo.begin());
}

void RadioApp::key_change_end(bool changed) {
    if (!changed && !key_undo.empty()) key_undo.pop_back();
}
