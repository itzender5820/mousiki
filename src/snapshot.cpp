// Session snapshot: saves the queue and the playing position so they can be restored at the next
// start, and keeps a previous snapshot.
#include "app_dirs.h"
#include "snapshot.h"
#include "path_utf8.h"
#include "tiny_json.h"
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace muisc {

using namespace tinyjson;

fs::path snapshot_path() {
    const char* home = std::getenv("HOME");
    // HOME is stored as UTF-8 by win_bootstrap_env() (it comes out of
    // GetEnvironmentVariableW, which is UTF-16). fs::path(std::string)
    // would re-read those bytes as ANSI, so a user profile with a
    // non-ASCII name produced a garbled base directory.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return muisc::app_dirs::cache() / "snapshot" / "snapshot.json";
}

static Value track_to_json(const SnapshotTrack& t) {
    Value v = Value::make_obj();
    v.set("is_local", Value::make_bool(t.is_local));
    v.set("path", Value::make_str(t.path));
    v.set("video_id", Value::make_str(t.video_id));
    v.set("title", Value::make_str(t.title));
    v.set("artist", Value::make_str(t.artist));
    if (!t.playlist.empty()) v.set("playlist", Value::make_str(t.playlist));
    return v;
}

static SnapshotTrack track_from_json(const Value& v) {
    SnapshotTrack t;
    if (auto* p = v.find("is_local")) t.is_local = p->as_bool(true);
    if (auto* p = v.find("path")) t.path = p->as_string();
    if (auto* p = v.find("video_id")) t.video_id = p->as_string();
    if (auto* p = v.find("title")) t.title = p->as_string();
    if (auto* p = v.find("artist")) t.artist = p->as_string();
    if (auto* p = v.find("playlist")) t.playlist = p->as_string();
    return t;
}

static fs::path snapshot_prev_path() {
    fs::path p = snapshot_path();
    p += ".prev";
    return p;
}

bool load_snapshot(SnapshotData& out) {
    fs::path p = snapshot_path();
    std::error_code ec;
    bool from_prev = false;
    if (!fs::exists(p, ec)) {
        // The last start restored the snapshot (and set it aside) but the app ended before it saved a new one -- a crash: the
        // session is given back once more (a second crash right away starts fresh, so a snapshot that crashes can not loop).
        p = snapshot_prev_path();
        if (!fs::exists(p, ec)) return false;
        from_prev = true;
    }

    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    in.close();
    if (from_prev) fs::remove(p, ec);   // given back this once
    if (text.empty()) return false;

    Value root;
    if (!parse(text, root) || root.type != Type::Object) return false;

    SnapshotData d;
    if (auto* np = root.find("now_playing")) {
        if (np->type == Type::Object) {
            d.has_now_playing = true;
            d.now_playing = track_from_json(*np);
        }
    }
    if (auto* p2 = root.find("position_sec")) d.position_sec = p2->as_number(0.0);
    if (auto* p2 = root.find("play_mode")) d.play_mode = static_cast<int>(p2->as_number(0));
    if (auto* p2 = root.find("muted")) d.muted = p2->as_bool(false);
    if (auto* p2 = root.find("volume")) d.volume = static_cast<int>(p2->as_number(70));
    if (auto* p2 = root.find("queue_locked")) d.queue_locked = p2->as_bool(true);
    if (auto* qp = root.find("queue")) {
        if (qp->type == Type::Array) {
            for (const auto& item : qp->arr) d.queue.push_back(track_from_json(item));
        }
    }

    // A snapshot with neither a current track nor a queue isn't worth
    // restoring -- treat it the same as "no snapshot".
    if (!d.has_now_playing && d.queue.empty()) return false;

    out = std::move(d);
    return true;
}

void save_snapshot(const SnapshotData& data) {
    fs::path p = snapshot_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::remove(snapshot_prev_path(), ec);   // a new session is saved: the set-aside copy of the restored one is no longer needed

    Value root = Value::make_obj();
    root.set("play_mode", Value::make_num(data.play_mode));
    root.set("muted", Value::make_bool(data.muted));
    root.set("volume", Value::make_num(data.volume));
    root.set("position_sec", Value::make_num(data.position_sec));
    if (data.has_now_playing) {
        root.set("now_playing", track_to_json(data.now_playing));
    }
    Value qarr = Value::make_arr();
    for (const auto& t : data.queue) qarr.arr.push_back(track_to_json(t));
    root.set("queue", qarr);
    root.set("queue_locked", Value::make_bool(data.queue_locked));

    // Single canonical file, always overwritten (trunc) -- never
    // appended/accumulated. Written to a temp file first and renamed
    // into place so a crash or kill mid-write can't leave a
    // half-written, unparseable snapshot.json behind (fs::rename on the
    // same filesystem is atomic).
    fs::path tmp = p;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out.is_open()) return;
    out << write(root);
    out.close();
    fs::rename(tmp, p, ec);
    if (ec) {
        // Cross-device or other rename failure -- fall back to a direct
        // write rather than leaving nothing behind at all.
        std::ofstream direct(p, std::ios::trunc | std::ios::binary);
        if (direct.is_open()) direct << write(root);
    }
}

void delete_snapshot() {
    // Set aside, not deleted: if the app crashes before it saves a session of its own, the next start can still restore it (once).
    std::error_code ec;
    fs::remove(snapshot_prev_path(), ec);
    fs::rename(snapshot_path(), snapshot_prev_path(), ec);
    if (ec) fs::remove(snapshot_path(), ec);
}

} // namespace muisc
