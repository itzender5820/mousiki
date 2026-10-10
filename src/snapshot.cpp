#include "snapshot.h"
#include "tiny_json.h"
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace muisc {

using namespace tinyjson;

fs::path snapshot_path() {
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home) home = std::getenv("USERPROFILE");
#endif
    fs::path base = home ? fs::path(home) : fs::path(".");
    return base / ".cache" / "mousiki" / "snapshot" / "snapshot.json";
}

static Value track_to_json(const SnapshotTrack& t) {
    Value v = Value::make_obj();
    v.set("is_local", Value::make_bool(t.is_local));
    v.set("path", Value::make_str(t.path));
    v.set("video_id", Value::make_str(t.video_id));
    v.set("title", Value::make_str(t.title));
    v.set("artist", Value::make_str(t.artist));
    return v;
}

static SnapshotTrack track_from_json(const Value& v) {
    SnapshotTrack t;
    if (auto* p = v.find("is_local")) t.is_local = p->as_bool(true);
    if (auto* p = v.find("path")) t.path = p->as_string();
    if (auto* p = v.find("video_id")) t.video_id = p->as_string();
    if (auto* p = v.find("title")) t.title = p->as_string();
    if (auto* p = v.find("artist")) t.artist = p->as_string();
    return t;
}

bool load_snapshot(SnapshotData& out) {
    fs::path p = snapshot_path();
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;

    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
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
    std::error_code ec;
    fs::remove(snapshot_path(), ec);
}

} // namespace muisc
