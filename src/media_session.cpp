// System media keys and now-playing info: the platform part is MPRIS on Linux and the macOS media
// session; elsewhere a stub does nothing. Key presses from the system become player commands.
#include "media_session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "settings.h"
#include "utf8_util.h"
#include "string_util.h"

namespace muisc::media {

namespace detail {
// The platform part: media_session_mpris.cpp (Linux), media_session_mac.mm (macOS); elsewhere the stubs below.
bool backend_start();
void backend_stop();
void backend_update(const Info& info);
std::vector<remote::Command> backend_poll(const Info& now);
} // namespace detail

namespace {
bool g_on = false;
Info g_last;
} // namespace

Info info_from_status(const std::vector<std::pair<std::string, std::string>>& kv) {
    Info i;
    std::string station;
    auto get = [&](const char* k) -> const std::string* {
        for (const auto& p : kv) if (p.first == k) return &p.second;
        return nullptr;
    };
    if (const std::string* v = get("state")) i.state = (*v == "playing" || *v == "muted") ? "playing" : *v == "paused" ? "paused" : "stopped";
    if (const std::string* v = get("title")) i.title = *v;
    if (const std::string* v = get("artist")) i.artist = *v;
    if (const std::string* v = get("album")) i.album = *v;
    if (const std::string* v = get("station")) station = *v;
    if (const std::string* v = get("position_sec")) i.position = std::max(0.0, std::atof(v->c_str()));
    if (const std::string* v = get("duration_sec")) i.duration = std::max(0.0, std::atof(v->c_str()));
    if (const std::string* v = get("volume")) i.volume = std::clamp(muisc::atoi_safe(v->c_str()), 0, 100);
    i.title = clean_tag_text(i.title);   // well-formed UTF-8, no control characters: D-Bus and macOS reject anything else
    i.artist = clean_tag_text(i.artist);
    i.album = clean_tag_text(i.album);
    station = clean_tag_text(station);
    if (!station.empty()) {   // the radio: the song is the title, the station the album; no song info: the station is the title
        if (i.title.empty()) i.title = station;
        else if (i.album.empty()) i.album = station;
    }
    if (i.state == "stopped") { i.title.clear(); i.artist.clear(); i.album.clear(); i.position = i.duration = 0.0; }
    return i;
}

remote::Command command_for(const std::string& method, long long value, const Info& now) {
    remote::Command c;
    char buf[32];
    if (method == "Play") c.name = "play";
    else if (method == "Pause") c.name = "pause";
    else if (method == "PlayPause") c.name = "play-pause";
    else if (method == "Stop") c.name = "stop";
    else if (method == "Next") c.name = "next";
    else if (method == "Previous") c.name = "prev";
    else if (method == "Seek") {
        c.name = "seek";
        std::snprintf(buf, sizeof buf, "%+.1f", std::clamp(static_cast<double>(value) / 1e6, -86400.0, 86400.0));
        c.arg = buf;
    } else if (method == "SetPosition") {
        c.name = "seek";
        std::snprintf(buf, sizeof buf, "%+.1f", std::clamp(static_cast<double>(value) / 1e6 - now.position, -86400.0, 86400.0));
        c.arg = buf;
    } else if (method == "SetVolume") {
        c.name = "volume";
        std::snprintf(buf, sizeof buf, "%d", std::clamp(static_cast<int>(std::lround(static_cast<double>(value) / 1e4)), 0, 100));
        c.arg = buf;
    }
    return c;
}

bool start() {
    if (g_on) return true;
    if (const char* off = std::getenv("MOUSIKI_NO_MEDIA")) if (*off) return false;   // (the tests set it: no media session on the developer's desktop)
    if (!read_media_keys()) return false;
    g_on = detail::backend_start();
    return g_on;
}

void shutdown() {
    if (!g_on) return;
    g_on = false;
    detail::backend_stop();
}

bool active() { return g_on; }

void update(const Info& info) {
    if (!g_on) return;
    g_last = info;
    detail::backend_update(info);
}

std::vector<remote::Command> poll() {
    if (!g_on) return {};
    return detail::backend_poll(g_last);
}

} // namespace muisc::media
