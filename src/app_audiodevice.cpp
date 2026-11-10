// app_audiodevice.cpp
// Part of class App (src/app.h): the CHOOSE AUDIO DEVICE overlay (Settings -> GENERAL -> AUDIO "Choose Audio Device", [ENTER];
// HKeyAudioDevice on the main screen, "°" / "&" by default) and handing the chosen output to the player.
//
// The choice is one value for all modes (config.txt AudioDevice=<name>, "" = the system's default; the radio reads the same line).
// It is applied once per frame by audio_device_apply(): a changed name goes to the player, and a track that plays moves to the new
// device at once (same position, pause state kept). Opened from Settings (drawn over the settings screen)
// and from the main screen alike: the choice is written to config.txt at once (that one line), so the settings screen's s / ESC
// neither save nor undo it.
#include "app.h"
#include "app_state_adev.h"
#include "string_util.h"
#include "utf8_util.h"
#include "console_log.h"
#include "overlay_common.h"
#include "app_internal.h"
#include <algorithm>
#include <mutex>

namespace muisc {

std::string App::audio_device_label() const {
    if (settings_.audio_device.empty()) return "System Default";
    return settings_.audio_device;
}

void App::audio_device_open(bool from_settings) {
    adev_->from_settings = from_settings;
    adev_->err.clear();
    adev_->list = list_output_devices(&adev_->err);
    // the row that is in use now: 0 = the system default, 1.. = the devices in the system's order
    adev_->sel = 0;
    for (size_t i = 0; i < adev_->list.size(); ++i)
        if (!settings_.audio_device.empty() && adev_->list[i].name == settings_.audio_device) adev_->sel = static_cast<int>(i) + 1;
    mode_ = Mode::AudioDevice;
    force_redraw_ = true;
}

void App::audio_device_close() {
    mode_ = adev_->from_settings ? Mode::Settings : Mode::Browse;
    force_redraw_ = true;
}

void App::audio_device_key(int key) {
    const bool arrow = last_key_was_arrow();
    const int rows = 1 + static_cast<int>(adev_->list.size());
    if (arrow && key == 'A') { adev_->sel = (adev_->sel + rows - 1) % rows; return; }
    if (arrow && key == 'B') { adev_->sel = (adev_->sel + 1) % rows; return; }
    if (!arrow && (key == 'r' || key == 'R')) {   // look again (a headset that was just plugged in)
        const std::string keep = adev_->sel > 0 && adev_->sel <= static_cast<int>(adev_->list.size()) ? adev_->list[adev_->sel - 1].name : std::string();
        adev_->err.clear();
        adev_->list = list_output_devices(&adev_->err);
        adev_->sel = 0;
        for (size_t i = 0; i < adev_->list.size(); ++i) if (!keep.empty() && adev_->list[i].name == keep) adev_->sel = static_cast<int>(i) + 1;
        status_line_ = "audio devices: " + std::to_string(adev_->list.size()) + " found";
        return;
    }
    if (!arrow && (key == '\r' || key == '\n')) {
        const std::string pick = adev_->sel > 0 && adev_->sel <= static_cast<int>(adev_->list.size()) ? adev_->list[adev_->sel - 1].name : std::string();
        settings_.audio_device = pick;
        audio_device_apply();   // moves a playing track there now
        const std::string now = player_.active_device();
        status_line_ = "audio device: " + audio_device_label();
        if (!pick.empty() && !now.empty() && now != pick) status_line_ += " -- could not be opened, the default device plays";
        set_config_value("AudioDevice", pick);    // saved at once, also from Settings (s / ESC there leave it as it is)
        if (adev_->from_settings) {                // what ESC in Settings goes back to, and no "unsaved changes" for it
            settings_snapshot_.audio_device = pick;
            settings_snapshot_text_ = settings_to_text(settings_snapshot_);
        }
        audio_device_close();
        return;
    }
    std::string act = resolve_hotkey_action(key);
    if (!arrow && (key == 27 || act == "HKeyAudioDevice")) audio_device_close();
}

void App::audio_device_apply() {
    if (settings_.audio_device == adev_->applied) return;
    adev_->applied = settings_.audio_device;
    player_.set_device(adev_->applied);
    if (!has_track_) return;   // nothing open: the next track opens the new device
    device_run([this] {   // on the device worker, like every opening and closing of the device (WASAPI wants one thread)
        if (!player_.reopen_device()) ConsoleLog::instance().log_basic("audio: no audio device could be opened");
    });
}

std::vector<std::string> App::build_audio_device_panel() const {
    const int W = kAudioDevicePanelWidth;
    const int inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string body = pad_right(truncate_str(plain, inner), inner);
        return bar + " " + (hi ? HI + body + R : body) + " " + bar;
    };
    // "> name ..... in use": the cursor, the device and its marks within `inner` columns
    auto entry = [&](bool cursor, const std::string& name, const std::string& right) {
        const std::string l = std::string(cursor ? "> " : "  ");
        const int room = std::max(4, inner - display_width(l) - display_width(right) - 1);
        const std::string n = truncate_str(name, room);
        const int gap = std::max(1, inner - display_width(l) - display_width(n) - display_width(right));
        return l + n + std::string(static_cast<size_t>(gap), ' ') + right;
    };
    const std::string active = player_.active_device();
    std::string def_name;
    for (const AudioOutputDevice& d : adev_->list) if (d.is_default) def_name = d.name;

    std::vector<std::string> lines;
    lines.push_back(box_top("Choose Audio Device", W, border));
    lines.push_back(row("", false));
    {
        const bool chosen = settings_.audio_device.empty();
        lines.push_back(row(entry(adev_->sel == 0, "System Default" + (def_name.empty() ? std::string() : " (" + def_name + ")"), chosen ? "in use" : ""), adev_->sel == 0));
    }
    for (size_t i = 0; i < adev_->list.size(); ++i) {
        const int r = static_cast<int>(i) + 1;
        const bool chosen = !settings_.audio_device.empty() && settings_.audio_device == adev_->list[i].name;
        std::string mark = chosen ? "in use" : "";
        if (chosen && !active.empty() && active != adev_->list[i].name) mark = "not opened";
        lines.push_back(row(entry(adev_->sel == r, adev_->list[i].name, mark), adev_->sel == r));
    }
    if (!settings_.audio_device.empty()) {   // the chosen device is not there (unplugged): say so instead of hiding it
        bool there = false;
        for (const AudioOutputDevice& d : adev_->list) if (d.name == settings_.audio_device) there = true;
        if (!there) lines.push_back(row("  " + truncate_str(settings_.audio_device, inner - 18) + " (not found)", false));
    }
    if (!adev_->err.empty()) lines.push_back(row("  " + adev_->err, false));
    lines.push_back(row("", false));
    for (const std::string& l : wrap_lines("The choice is the same for the music player, the radio and the audio books. A playing track moves to the new device at once.", inner, 3))
        lines.push_back(row(l, false));
    lines.push_back(row("", false));
    lines.push_back(centred_bottom(settings_, W, "[UP/DOWN] [ENTER] use  [r] look again  [ESC] close", border_bottom));
    return lines;
}

} // namespace muisc
