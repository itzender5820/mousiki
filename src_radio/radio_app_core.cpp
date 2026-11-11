#include "radio_app.h"
#include "console_log.h"
#include "path_utf8.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif

void RadioApp::load_storage() {
    ui.stations = load_stations(&source);
    ui.model.stations = &ui.stations;
    PresetStore store = load_presets(ui.stations);
    ui.model.banks = std::move(store.banks);
    ui.model.bank_active = std::clamp(store.active, 0, static_cast<int>(ui.model.banks.size()) - 1);
    ui.model.presets = ui.model.banks[static_cast<size_t>(ui.model.bank_active)].slots;
    ui.model.lists = load_station_lists(ui.stations);
}

// Settings that live in the audio path (mono fold, loudness normalisation): pushed to the engine at start and after every change.
void RadioApp::apply_audio() {
    engine.set_stereo(cfg.stereo);
    engine.set_tune_noise(cfg.tune_noise);
    engine.set_normalization(cfg.normalize, static_cast<float>(cfg.normalize_target_lufs), static_cast<float>(cfg.normalize_max_boost_db));
    engine.set_equalizer(cfg.eq_enabled, cfg.eq_gains);
}

void RadioApp::apply_timeshift() {
    if (timeshift_minutes(cfg) == timeshift_applied) return;
    timeshift_applied = timeshift_minutes(cfg);
    engine.set_timeshift(timeshift_applied, timeshift_file);
}

// Starts a recording at stream position `from_frame` (frames at 48 kHz; from the record overlay). The file is
// named after the wall-clock time the recorded audio was on air.
void RadioApp::start_record(long long from_frame, double seconds_before_now) {
    const RadioStatus rs = engine.status();
    char stamp[32];
    const std::time_t tt = std::time(nullptr) - static_cast<std::time_t>(seconds_before_now);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tmv);
    std::string err;
    const std::string title = !rs.info.title.empty() ? (rs.info.artist.empty() ? rs.info.title : rs.info.artist + " - " + rs.info.title)
                                                      : (rs.info.station.empty() ? rs.tuned_name : rs.info.station);
    if (engine.start_recording(storage.download_dir.string(), stamp, title, from_frame, &err)) {
        ui.model.notice = "recording to " + muisc::path_utf8(storage.download_dir / (std::string(stamp) + ".mp3")) + "   [y] stop";
        notice_until = ui.model.t_sec + 8;
    } else { ui.model.notice = "cannot record: " + err; notice_until = ui.model.t_sec + 6; }
}

void RadioApp::sleep_cancel() {
    sleep_minutes = 0; engine.set_fade(1.0f); ui.model.sleep_running = 0;
}

void RadioApp::sleep_start(int minutes) {
    sleep_minutes = minutes;
    sleep_deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(minutes * sleep_unit));
    sleep_fade_len = std::clamp(minutes * 6.0 * sleep_unit / 60.0, 30.0 * sleep_unit / 60.0, 600.0 * sleep_unit / 60.0);
    engine.set_fade(1.0f);
    ui.model.notice = "sleep timer: the stream stops in " + std::to_string(minutes) + " min" + (cfg.sleep_fade ? " (fading out over the last " + std::to_string(static_cast<int>(sleep_fade_len)) + " s)" : "");
    notice_until = ui.model.t_sec + 6;
}

void RadioApp::tune_to(int idx, bool record) {
    if (idx < 0 || idx >= static_cast<int>(ui.stations.size())) return;
    const int cur = engine.status().tuned_index;
    if (record && cur >= 0 && cur != idx) {
        history.push_back(cur);
        if (history.size() > 100) history.erase(history.begin());
    }
    muisc::ConsoleLog::instance().log_basic("radio: tuning " + ui.stations[static_cast<size_t>(idx)].name);
    engine.tune(ui.stations[static_cast<size_t>(idx)], idx);
}

void RadioApp::tune_visible(int vis_pos) {
    if (vis_pos < 0 || vis_pos >= static_cast<int>(ui.model.visible.size())) return;
    tune_to(ui.model.visible[static_cast<size_t>(vis_pos)]);
}

// The channels `n` / `b` / `#` surf through: the stations of the station list shown in the STATIONS pane
// (in the list's own order), or all stations when no list is shown.
std::vector<int> RadioApp::surf_pool() {
    std::vector<int> pool;
    const int total = static_cast<int>(ui.stations.size());
    if (const StationList* al = ui.active_list_ptr()) {
        for (int i : al->items) if (i >= 0 && i < total) pool.push_back(i);
    } else {
        for (int i = 0; i < total; ++i) pool.push_back(i);
    }
    return pool;
}

void RadioApp::tune_relative(int delta) {
    const std::vector<int> pool = surf_pool();
    const int n = static_cast<int>(pool.size());
    if (n == 0) return;
    const int tuned = engine.status().tuned_index;
    int pos = -1;
    for (int i = 0; i < n; ++i) if (pool[static_cast<size_t>(i)] == tuned) { pos = i; break; }
    if (pos < 0) pos = delta > 0 ? -1 : 0;     // not in the pool: n starts at its first, b at its last channel
    tune_to(pool[static_cast<size_t>(((pos + delta) % n + n) % n)]);
}

void RadioApp::tune_shuffle() {
    const std::vector<int> pool = surf_pool();
    const int n = static_cast<int>(pool.size());
    if (n == 0) return;
    if (n == 1) { tune_to(pool[0]); return; }
    const int cur = engine.status().tuned_index;
    std::vector<int> cand;
    for (int i : pool) if (i != cur) cand.push_back(i);
    if (cand.empty()) return;
    tune_to(cand[static_cast<size_t>(std::uniform_int_distribution<int>(0, static_cast<int>(cand.size()) - 1)(rng))]);
}

// 'b': the only key that follows the S/L mode. S: the channel that was played before (history, repeatable).
// L: the channel before the current one in the list.
void RadioApp::tune_back() {
    if (!ui.model.shuffle) { tune_relative(-1); return; }
    const int cur = engine.status().tuned_index;
    while (!history.empty()) {
        const int prev = history.back();
        history.pop_back();
        if (prev != cur) { tune_to(prev, false); return; }
    }
}

// Remote control (`mousiki --cmd next` ... see src/remote_control.h): the same actions as the keys, without touching the UI state.
void RadioApp::remote_apply(const muisc::remote::Command& c) {
    const RadioStatus rs = engine.status();
    const bool tuned = rs.state != StreamState::Idle && rs.state != StreamState::Failed;
    const std::string& n = c.name;
    if (n == "cli") { cli_run(c.arg, c.from_net ? muisc::cli::Origin::Network : muisc::cli::Origin::Remote, c.id); return; }   // a command line (text) from outside
    if (n == "next") tune_relative(+1);
    else if (n == "prev") tune_back();
    else if (n == "shuffle") tune_shuffle();
    else if (n == "play-pause") {
        if (!tuned) tune_visible(ui.model.cursor);
        else engine.toggle_pause();
    }
    else if (n == "play") {
        if (!tuned) tune_visible(ui.model.cursor);
        else if (rs.timeshift_paused) engine.toggle_pause();
        else if (rs.muted) engine.set_muted(false);
    }
    else if (n == "pause") { if (tuned && !rs.timeshift_paused) engine.toggle_pause(); }
    else if (n == "stop") engine.stop();
    else if (n == "volume-up") engine.set_volume(rs.volume + static_cast<int>(muisc::remote::number(c.arg, 5)));
    else if (n == "volume-down") engine.set_volume(rs.volume - static_cast<int>(muisc::remote::number(c.arg, 5)));
    else if (n == "volume") {
        const int v = static_cast<int>(muisc::remote::number(c.arg, rs.volume));
        engine.set_volume((!c.arg.empty() && (c.arg[0] == '+' || c.arg[0] == '-')) ? rs.volume + v : v);
    }
    else if (n == "mute") engine.set_muted(true);
    else if (n == "unmute") engine.set_muted(false);
    else if (n == "toggle-mute") engine.set_muted(!rs.muted);
    else if (n == "seek") { if (tuned) engine.jump(muisc::remote::number(c.arg, 0)); }
    else if (n == "live") { if (tuned) engine.go_live(); }
    else if (n == "playback-mode") { ui.model.shuffle = !ui.model.shuffle; cfg.playback_shuffle = ui.model.shuffle; ui.model.settings.dirty = true; }
    else if (n == "switch-mode") {
        const int dir = muisc::remote::number(c.arg, 1) < 0 ? -1 : 1;   // (-1: the previous mode)
        if (muisc::next_mode_in(muisc::read_mode_config(), 1, dir) != 1) { g_mode_switch_dir = dir; switch_mode = true; running = false; }
    }
    else if (n == "quit") running = false;
    // the phone's STATIONS list: open / close it, page through it, tune the tapped station
    else if (n == "remote-view") { ui.model.phone_view = c.arg == "1" ? 1 : 0; ui.model.phone_page = 0; ui.model.phone_note.clear(); }
    else if (n == "remote-scroll") {
        int pc = 0, pr = 0;
        muisc::remote::net::screen_wanted(pc, pr);
        const int per = muisc::remote::net::phone_list_rows(pr > 0 ? pr : 30);
        const int count = ui.stations.empty() ? 0 : static_cast<int>(ui.stations.size());
        const int pages = std::max(1, (count + per - 1) / per);
        ui.model.phone_page = std::clamp(ui.model.phone_page + static_cast<int>(muisc::remote::number(c.arg, 1)), 0, pages - 1);
    }
    else if (n == "tune") {
        const int k = static_cast<int>(muisc::remote::number(c.arg, 0)) - 1;
        if (k >= 0 && k < static_cast<int>(ui.stations.size())) {
            tune_to(k);
            ui.model.phone_note = "tuned " + ui.stations[static_cast<size_t>(k)].name;
        } else ui.model.phone_note = "no station " + c.arg;
    }
}

void RadioApp::set_search_focus(bool on) {
    ui.model.search_focus = on;
    if (on) ui.model.search_edit.to_end(ui.model.search);   // caret behind the text, nothing marked
    set_text_entry(on);
}

// SHIFT+9 / SHIFT+8: the oscilloscope / the spectrogram in their own windows (graphics card, needs SDL2). They work
// whatever is switched off on the GENERAL tab, and also while an overlay is open.
std::string RadioApp::window_key_label(const char* id) {
    for (int i = 0; i < kKeyActionCount; ++i)
        if (std::string(kKeyActions[i].id) == id) return muisc::symbol_key_label(key_binding(cfg, i));
    return std::string();
}

void RadioApp::toggle_scope_window() {
    if (scope_window_running()) {
        scope_window_close();
        scope_win_was = false;
        ui.model.notice = "scope window closed"; notice_until = ui.model.t_sec + 3;
        return;
    }
    ScopeWinConfig c;
    fill_scope_window_config(cfg, engine.status(), c);
    scope_window_set_config(c);   // the settings are there before the first sample
    std::string err;
    if (scope_window_open(&err)) {
        scope_win_was = true;
        ui.model.notice = "scope window opened: F / double-click fullscreen, T always on top, ESC or " +
                          window_key_label("ScopeWindow") + " closes";
        notice_until = ui.model.t_sec + 6;
    } else { ui.model.notice = err; notice_until = ui.model.t_sec + 10; }
}

void RadioApp::toggle_spectro_window() {
    if (spectro_window_running()) {
        spectro_window_close();
        spectro_win_was = false;
        ui.model.notice = "spectrogram window closed"; notice_until = ui.model.t_sec + 3;
        return;
    }
    SpectroWinConfig c;
    fill_spectro_window_config(cfg, engine.status(), c);
    spectro_window_set_config(c);
    std::string err;
    if (spectro_window_open(&err)) {
        spectro_win_was = true;
        ui.model.notice = "spectrogram window opened: F / double-click fullscreen, T always on top, ESC or " +
                          window_key_label("SpectroWindow") + " closes";
        notice_until = ui.model.t_sec + 6;
    } else { ui.model.notice = err; notice_until = ui.model.t_sec + 10; }
}
