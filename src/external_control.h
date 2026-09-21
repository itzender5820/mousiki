#pragma once
// Control of music that is NOT playing inside mousiki: a YouTube tab in
// Chromium/Firefox, the Spotify desktop app, a video in mpv, anything
// else that speaks MPRIS. The point is that mousiki's transport keys can
// stay useful even when the thing making noise isn't ours.
//
// On Linux every desktop media player of note exports the MPRIS2 spec
// over the session bus: a bus name under `org.mpris.MediaPlayer2.*`,
// one object at `/org/mpris/MediaPlayer2`, and the properties and
// methods of `org.mpris.MediaPlayer2.Player`. That is the whole
// mechanism -- there is no browser-specific API to special-case.
//
// mousiki has zero external C++ library dependencies on purpose (see
// online_source.cpp / metadata_probe.cpp: yt-dlp, ffmpeg and ffprobe are
// all spoken to as subprocesses), so this module does NOT link libdbus
// or glib. It shells out to whichever D-Bus CLI is already installed --
// playerctl, busctl or gdbus -- through the same run_capture() helper
// everything else uses. Linking libdbus would drag in a build-time
// dependency and a mainloop, to do something `busctl call` does in one
// line.
//
// THREADING: a single ExternalControl instance's methods are safe to
// call from one background thread at a time; they are NOT internally
// locked, so do not share one instance across two threads without your
// own mutex (separate instances are fine, the object holds no state).
// Every method BLOCKS on a subprocess round trip -- typically 10-60ms,
// and bounded by an explicit D-Bus timeout -- so none of them belong in
// the render loop. The backend probe behind available()/backend_name()
// is the one piece of shared state and is guarded, so concurrent
// first-use is safe.
#include <string>
#include <vector>

namespace muisc {

enum class ExtStatus { Unknown, Playing, Paused, Stopped };

// One discovered external media player.
struct ExternalPlayer {
    std::string bus_name;      // full MPRIS bus name, e.g. "org.mpris.MediaPlayer2.spotify"
    std::string id;            // short id after the prefix, e.g. "spotify", "chromium.instance10741"
    std::string display_name;  // human label: MPRIS "Identity" property if readable, else a prettified id
    std::string kind;          // "spotify" | "browser" | "other"  (classify by id substring)
};

// A snapshot of what an external player is currently doing.
struct ExtTrackState {
    bool valid = false;               // false = the poll failed / player vanished
    ExtStatus status = ExtStatus::Unknown;
    std::string title;
    std::string artist;               // MPRIS xesam:artist is an array — join with ", "
    std::string album;
    std::string art_url;              // mpris:artUrl, may be empty or a file:// URL
    std::string track_id;             // mpris:trackid, an object path — needed by set_position()
    double position_sec = -1.0;       // -1 = unknown
    double length_sec = -1.0;         // -1 = unknown (mpris:length is microseconds)
    int volume_pct = -1;              // -1 = unknown/unsupported (MPRIS Volume is a double 0.0-1.0)
    bool can_go_next = false;
    bool can_go_previous = false;
    bool can_pause = false;
    bool can_seek = false;
    bool can_control = false;
};

class ExternalControl {
public:
    // True if any usable backend CLI was found on this system. Result is
    // cached after the first call (a PATH probe per frame would be silly).
    static bool available();
    // "playerctl" | "busctl" | "gdbus" | "osascript" | "none"
    static const char* backend_name();

    // Discovery. Returns every live MPRIS player, sorted so Spotify comes
    // first, then browsers, then everything else.
    std::vector<ExternalPlayer> list_players();

    // One full property read for a player. Blocking (a subprocess round
    // trip, typically 10-60ms) — the caller polls this from a background
    // thread, never from the render loop.
    ExtTrackState poll(const std::string& bus_name);

    // Transport commands. Each returns true if the command was dispatched
    // without error. All are blocking subprocess calls like poll().
    bool play_pause(const std::string& bus_name);
    bool play(const std::string& bus_name);
    bool pause(const std::string& bus_name);
    bool stop(const std::string& bus_name);
    bool next(const std::string& bus_name);
    bool previous(const std::string& bus_name);
    bool seek_relative(const std::string& bus_name, double delta_sec);  // MPRIS Seek takes signed microseconds
    bool set_position(const std::string& bus_name, const std::string& track_id, double pos_sec);
    bool set_volume(const std::string& bus_name, int volume_pct);       // 0-100 -> double 0.0-1.0
};

// Human-readable label for a status, for the UI status line ("Playing", "Paused", ...).
const char* ext_status_name(ExtStatus s);

} // namespace muisc
