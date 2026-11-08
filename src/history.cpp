// Listening history: stores each play, aggregates it into the top lists and the habit statistics (by
// day, weekday and hour), and writes the history archive as JSON.
#include "history.h"

#include "path_utf8.h"
#include "tiny_json.h"
#if defined(_WIN32)
#include "win_compat.h" // win_localtime -- MSVC's localtime_s has swapped arguments
#endif
#include "date_util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>

namespace muisc {

using namespace tinyjson;

namespace {

// Two limits that keep this file honest over months of use: how many INDIVIDUAL
// plays are kept (the History tab shows only the newest 100; the rest of the
// window is what keeps history.json small, as it is rewritten after every
// track) and how long a silence has to be before it starts a new session.
// Nothing counted is lost when a record leaves the window: it is folded into
// the HistoryArchive (per-title / per-day / session totals) first, so the
// Habits and Top Tracks numbers are lifetime figures with no cap at all.
constexpr size_t kMaxPlays = 1000;
constexpr long long kSessionGapSec = 30 * 60;

fs::path default_history_dir() {
    const char* home = std::getenv("HOME");
    // HOME is UTF-8 on every platform this app runs on (win_bootstrap_env()
    // builds it from GetEnvironmentVariableW) -- path_from_utf8() is what
    // keeps a non-ASCII profile name from turning into ANSI garbage here,
    // exactly as in snapshot.cpp.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return base / ".cache" / "mousiki" / "history";
}

Value play_to_json(const HistoryPlay& p) {
    Value v = Value::make_obj();
    v.set("id", Value::make_str(p.id));
    v.set("t", Value::make_str(p.title));
    v.set("a", Value::make_str(p.artist));
    v.set("len", Value::make_num(p.len_sec));
    v.set("heard", Value::make_num(p.listened_sec));
    v.set("at", Value::make_num(static_cast<double>(p.started_at)));
    v.set("done", Value::make_bool(p.finished));
    if (!p.playlist.empty()) v.set("pl", Value::make_str(p.playlist));
    return v;
}

HistoryPlay play_from_json(const Value& v) {
    HistoryPlay p;
    if (auto* x = v.find("id")) p.id = x->as_string();
    if (auto* x = v.find("t")) p.title = x->as_string();
    if (auto* x = v.find("a")) p.artist = x->as_string();
    if (auto* x = v.find("len")) p.len_sec = x->as_number(0.0);
    if (auto* x = v.find("heard")) p.listened_sec = x->as_number(0.0);
    if (auto* x = v.find("at")) p.started_at = static_cast<long long>(x->as_number(0.0));
    if (auto* x = v.find("done")) p.finished = x->as_bool(false);
    if (auto* x = v.find("pl")) p.playlist = x->as_string();
    return p;
}

Value archive_to_json(const HistoryArchive& a) {
    Value v = Value::make_obj();
    v.set("plays", Value::make_num(static_cast<double>(a.plays)));
    v.set("done", Value::make_num(static_cast<double>(a.finished)));
    v.set("heard", Value::make_num(a.listened_sec));
    Value titles = Value::make_arr();
    for (const auto& kv : a.titles) {
        Value t = Value::make_obj();
        t.set("id", Value::make_str(kv.first));
        t.set("t", Value::make_str(kv.second.title));
        t.set("a", Value::make_str(kv.second.artist));
        t.set("len", Value::make_num(kv.second.len_sec));
        t.set("n", Value::make_num(kv.second.plays));
        t.set("heard", Value::make_num(kv.second.listened_sec));
        if (!kv.second.day_plays.empty()) {
            Value d = Value::make_obj();
            for (const auto& dk : kv.second.day_plays) d.set(dk.first, Value::make_num(dk.second));
            t.set("days", d);
        }
        const auto& tod = kv.second.tod;
        if (tod[0] || tod[1] || tod[2] || tod[3]) {
            Value a4 = Value::make_arr();
            for (int x : tod) a4.arr.push_back(Value::make_num(x));
            t.set("tod", a4);
        }
        titles.arr.push_back(t);
    }
    v.set("titles", titles);
    if (!a.playlists.empty()) {
        Value pls = Value::make_arr();
        for (const auto& kv : a.playlists) {
            Value t = Value::make_obj();
            t.set("name", Value::make_str(kv.first));
            t.set("n", Value::make_num(kv.second.plays));
            t.set("heard", Value::make_num(kv.second.listened_sec));
            if (!kv.second.day_plays.empty()) {
                Value d = Value::make_obj();
                for (const auto& dk : kv.second.day_plays) d.set(dk.first, Value::make_num(dk.second));
                t.set("days", d);
            }
            pls.arr.push_back(t);
        }
        v.set("playlists", pls);
    }
    Value days = Value::make_obj();
    for (const auto& kv : a.per_day) days.set(kv.first, Value::make_num(kv.second));
    v.set("days", days);
    Value hrs = Value::make_arr();
    for (double x : a.hours) hrs.arr.push_back(Value::make_num(x));
    v.set("hours", hrs);
    Value wds = Value::make_arr();
    for (double x : a.weekdays) wds.arr.push_back(Value::make_num(x));
    v.set("weekdays", wds);
    v.set("sess_closed", Value::make_num(a.sessions_closed));
    v.set("sess_sum", Value::make_num(a.session_sum_sec));
    v.set("sess_open", Value::make_bool(a.session_open));
    v.set("sess_start", Value::make_num(static_cast<double>(a.sess_start)));
    v.set("sess_end", Value::make_num(static_cast<double>(a.sess_end)));
    return v;
}

void archive_from_json(const Value& v, HistoryArchive& a) {
    a = HistoryArchive();
    if (v.type != Type::Object) return;
    if (auto* x = v.find("plays")) a.plays = static_cast<long long>(x->as_number(0.0));
    if (auto* x = v.find("done")) a.finished = static_cast<long long>(x->as_number(0.0));
    if (auto* x = v.find("heard")) a.listened_sec = x->as_number(0.0);
    if (auto* x = v.find("titles")) {
        if (x->type == Type::Array) {
            for (const auto& item : x->arr) {
                const Value* id = item.find("id");
                if (!id) continue;
                HistoryArchiveTitle t;
                if (auto* y = item.find("t")) t.title = y->as_string();
                if (auto* y = item.find("a")) t.artist = y->as_string();
                if (auto* y = item.find("len")) t.len_sec = y->as_number(0.0);
                if (auto* y = item.find("n")) t.plays = static_cast<int>(y->as_number(0.0));
                if (auto* y = item.find("heard")) t.listened_sec = y->as_number(0.0);
                if (auto* y = item.find("days"))
                    if (y->type == Type::Object)
                        for (const auto& dk : y->obj) t.day_plays[dk.first] = static_cast<int>(dk.second.as_number(0.0));
                if (auto* y = item.find("tod"))
                    if (y->type == Type::Array)
                        for (size_t i = 0; i < 4 && i < y->arr.size(); ++i) t.tod[i] = static_cast<int>(y->arr[i].as_number(0.0));
                a.titles[id->as_string()] = t;
            }
        }
    }
    if (auto* x = v.find("playlists")) {
        if (x->type == Type::Array) {
            for (const auto& item : x->arr) {
                const Value* name = item.find("name");
                if (!name || name->as_string().empty()) continue;
                HistoryArchivePlaylist t;
                if (auto* y = item.find("n")) t.plays = static_cast<int>(y->as_number(0.0));
                if (auto* y = item.find("heard")) t.listened_sec = y->as_number(0.0);
                if (auto* y = item.find("days"))
                    if (y->type == Type::Object)
                        for (const auto& dk : y->obj) t.day_plays[dk.first] = static_cast<int>(dk.second.as_number(0.0));
                a.playlists[name->as_string()] = t;
            }
        }
    }
    if (auto* x = v.find("days")) {
        if (x->type == Type::Object) for (const auto& kv : x->obj) a.per_day[kv.first] = kv.second.as_number(0.0);
    }
    if (auto* x = v.find("hours"))
        if (x->type == Type::Array) for (size_t i = 0; i < 24 && i < x->arr.size(); ++i) a.hours[i] = x->arr[i].as_number(0.0);
    if (auto* x = v.find("weekdays"))
        if (x->type == Type::Array) for (size_t i = 0; i < 7 && i < x->arr.size(); ++i) a.weekdays[i] = x->arr[i].as_number(0.0);
    if (auto* x = v.find("sess_closed")) a.sessions_closed = static_cast<int>(x->as_number(0.0));
    if (auto* x = v.find("sess_sum")) a.session_sum_sec = x->as_number(0.0);
    if (auto* x = v.find("sess_open")) a.session_open = x->as_bool(false);
    if (auto* x = v.find("sess_start")) a.sess_start = static_cast<long long>(x->as_number(0.0));
    if (auto* x = v.find("sess_end")) a.sess_end = static_cast<long long>(x->as_number(0.0));
}

} // namespace

// The identity this whole feature aggregates by: a local path as-is, a stream
// as its video id -- so the same YouTube track played from the search list
// and from a playlist counts as one title rather than two.
std::string history_track_id(bool is_local, const std::string& path, const std::string& video_id) {
    if (is_local) return path;
    if (video_id.empty()) return std::string();
    return "yt:" + video_id;
}


// Same bookkeeping history_stats() does for the window, applied to one record
// that is about to leave it. Oldest -> newest, so the session walk below is
// the forward direction too.
// The time-of-day / weekday buckets: a play counts, with the seconds heard, for the hour and the weekday it started in.
static void add_time_buckets(const HistoryPlay& p, std::array<double, 24>& hours, std::array<double, 7>& weekdays) {
    if (p.started_at <= 0) return;
    const std::tm tm = local_tm(p.started_at);
    hours[static_cast<size_t>(std::clamp(tm.tm_hour, 0, 23))] += p.listened_sec;
    weekdays[static_cast<size_t>(((tm.tm_wday % 7) + 6) % 7)] += p.listened_sec;   // Monday first
}

void HistoryArchive::fold(const HistoryPlay& p) {
    ++plays;
    if (p.finished) ++finished;
    listened_sec += p.listened_sec;
    if (!p.id.empty()) {
        HistoryArchiveTitle& t = titles[p.id];
        if (!p.title.empty()) t.title = p.title;   // newer folds overwrite: the latest name wins
        if (!p.artist.empty()) t.artist = p.artist;
        if (p.len_sec > 0) t.len_sec = p.len_sec;
        ++t.plays;
        t.listened_sec += p.listened_sec;
        if (p.started_at > 0) {
            ++t.day_plays[day_key(p.started_at)];
            const int b = history_tod_bucket(p.started_at);
            if (b >= 0) ++t.tod[static_cast<size_t>(b)];
        }
    }
    if (!p.playlist.empty()) {
        HistoryArchivePlaylist& pl = playlists[p.playlist];
        ++pl.plays;
        pl.listened_sec += p.listened_sec;
        if (p.started_at > 0) ++pl.day_plays[day_key(p.started_at)];
    }
    add_time_buckets(p, hours, weekdays);
    if (p.started_at > 0) {
        per_day[day_key(p.started_at)] += p.listened_sec;
        const long long p_end = p.started_at + static_cast<long long>(p.listened_sec + 0.5);
        if (!session_open || p.started_at - sess_end > kSessionGapSec) {
            if (session_open) {
                ++sessions_closed;
                session_sum_sec += static_cast<double>(std::max<long long>(0, sess_end - sess_start));
            }
            session_open = true;
            sess_start = p.started_at;
            sess_end = p_end;
        } else {
            sess_start = std::min(sess_start, p.started_at);
            sess_end = std::max(sess_end, p_end);
        }
    }
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

std::string HistoryStore::file_utf8() const {
    const fs::path dir = dir_.empty() ? default_history_dir() : path_from_utf8(dir_);
    return path_utf8(dir / path_from_utf8(file_name_));
}

std::string HistoryStore::effective_dir() const {
    return dir_.empty() ? path_utf8(default_history_dir()) : dir_;
}

bool HistoryStore::file_exists() const {
    std::error_code ec;
    return fs::exists(path_from_utf8(file_utf8()), ec);
}

void HistoryStore::load() {
    plays_.clear();
    archive_ = HistoryArchive();
    live_index_ = -1;

    fs::path p = path_from_utf8(file_utf8());
    std::error_code ec;
    if (!fs::exists(p, ec)) return;

    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    // A missing or corrupt file is not an error: history is a convenience
    // and must never be able to stop the app from starting.
    Value root;
    if (!parse(text, root) || root.type != Type::Object) return;
    if (auto* arch = root.find("archive")) archive_from_json(*arch, archive_);
    if (auto* arr = root.find("plays")) {
        if (arr->type == Type::Array) {
            for (const auto& item : arr->arr) plays_.push_back(play_from_json(item));
        }
    }
    // A play that was still in progress when the app died last time simply
    // stays historical -- it does not get its live record back, which is why
    // it remains a skip rather than being silently re-counted next run.
    // Anything beyond the window (an older, longer file) is folded into the
    // archive, not thrown away.
    trim_overflow();
}

void HistoryStore::trim_overflow() {
    while (plays_.size() > kMaxPlays) {
        archive_.fold(plays_.back()); // oldest record; the live one sits at the front and is never reached
        plays_.pop_back();
    }
}

void HistoryStore::save() const {
    fs::path p = path_from_utf8(file_utf8());
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    Value root = Value::make_obj();
    root.set("version", Value::make_num(2));
    if (!archive_.empty()) root.set("archive", archive_to_json(archive_));
    Value arr = Value::make_arr();
    for (const auto& pl : plays_) arr.arr.push_back(play_to_json(pl));
    root.set("plays", arr);

    // Temp file + rename, exactly like save_snapshot(): this is written after
    // every single track, so a crash mid-write must not be able to leave an
    // unparseable history.json behind.
    fs::path tmp = p;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out.is_open()) return;
    out << write(root);
    out.close();
    fs::rename(tmp, p, ec);
    if (ec) {
        std::ofstream direct(p, std::ios::trunc | std::ios::binary);
        if (direct.is_open()) direct << write(root);
    }
}

void HistoryStore::begin_play(const HistoryPlay& p) {
    plays_.insert(plays_.begin(), p);
    live_index_ = 0;
    trim_overflow(); // oldest records are folded into the archive; the live one is at the front
}

void HistoryStore::add_listened(double sec) {
    if (live_index_ < 0 || live_index_ >= static_cast<int>(plays_.size())) return;
    if (!(sec > 0)) return; // NaN/negative guards: this comes straight from a frame delta
    HistoryPlay& p = plays_[live_index_];
    p.listened_sec += sec;
    if (p.len_sec > 0 && p.listened_sec > p.len_sec) p.listened_sec = p.len_sec;
}

void HistoryStore::finish_live(bool completed) {
    if (live_index_ < 0 || live_index_ >= static_cast<int>(plays_.size())) return;
    HistoryPlay& p = plays_[live_index_];
    if (p.len_sec > 0 && p.listened_sec > p.len_sec) p.listened_sec = p.len_sec;
    p.finished = completed;
    live_index_ = -1;
}

double HistoryStore::live_listened() const {
    const HistoryPlay* p = live_play();
    return p ? p->listened_sec : 0.0;
}

double HistoryStore::live_len() const {
    const HistoryPlay* p = live_play();
    return p ? p->len_sec : 0.0;
}

const HistoryPlay* HistoryStore::live_play() const {
    if (live_index_ < 0 || live_index_ >= static_cast<int>(plays_.size())) return nullptr;
    return &plays_[live_index_];
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------

std::vector<HistoryTopRow> history_top(const std::vector<HistoryPlay>& plays, bool most_first,
                                       const HistoryArchive* archive) {
    std::vector<HistoryTopRow> rows;
    std::map<std::string, size_t> by_id;
    // Newest-first walk: a title's most recent play supplies the display
    // name, so a file renamed since earlier plays shows its current name.
    for (const HistoryPlay& p : plays) {
        if (p.id.empty()) continue;
        auto it = by_id.find(p.id);
        if (it == by_id.end()) {
            by_id[p.id] = rows.size();
            HistoryTopRow r;
            r.id = p.id;
            r.title = p.title;
            r.artist = p.artist;
            r.len_sec = p.len_sec;
            r.plays = 1;
            r.listened_sec = p.listened_sec;
            rows.push_back(r);
        } else {
            HistoryTopRow& r = rows[it->second];
            ++r.plays;
            r.listened_sec += p.listened_sec;
            if (r.title.empty() && !p.title.empty()) r.title = p.title;
            if (r.artist.empty() && !p.artist.empty()) r.artist = p.artist;
            if (r.len_sec <= 0 && p.len_sec > 0) r.len_sec = p.len_sec;
        }
    }
    // Plays that already left the window of individual records. The window is
    // newer, so its name/artist/length win and the archive only fills gaps.
    if (archive) {
        for (const auto& kv : archive->titles) {
            const HistoryArchiveTitle& t = kv.second;
            auto it = by_id.find(kv.first);
            if (it == by_id.end()) {
                HistoryTopRow r;
                r.id = kv.first;
                r.title = t.title;
                r.artist = t.artist;
                r.len_sec = t.len_sec;
                r.plays = t.plays;
                r.listened_sec = t.listened_sec;
                by_id[kv.first] = rows.size();
                rows.push_back(r);
            } else {
                HistoryTopRow& r = rows[it->second];
                r.plays += t.plays;
                r.listened_sec += t.listened_sec;
                if (r.title.empty()) r.title = t.title;
                if (r.artist.empty()) r.artist = t.artist;
                if (r.len_sec <= 0) r.len_sec = t.len_sec;
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [most_first](const HistoryTopRow& a, const HistoryTopRow& b) {
        if (a.plays != b.plays) return most_first ? a.plays > b.plays : a.plays < b.plays;
        if (a.listened_sec != b.listened_sec) return a.listened_sec > b.listened_sec;
        return a.title < b.title;
    });
    return rows;
}

// ---------------------------------------------------------------------------
// Smart lists
// ---------------------------------------------------------------------------

int history_tod_bucket(long long unix_sec) {
    if (unix_sec <= 0) return -1;
    const int h = local_tm(unix_sec).tm_hour;
    if (h >= 7 && h < 11) return 0;    // morning
    if (h >= 11 && h < 18) return 1;   // day
    if (h >= 18 && h < 22) return 2;   // evening
    return 3;                          // night, 22-7
}

namespace {

// history_top() with a filter: `keep` decides for each play of the window,
// `archived` returns how many of a title's archived plays count (0 = none).
template <class KeepPlay, class ArchivedCount>
std::vector<HistoryTopRow> top_filtered(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                                        bool most_first, KeepPlay keep, ArchivedCount archived) {
    std::vector<HistoryTopRow> rows;
    std::map<std::string, size_t> by_id;
    auto row_for = [&](const std::string& id) -> HistoryTopRow& {
        auto it = by_id.find(id);
        if (it != by_id.end()) return rows[it->second];
        by_id[id] = rows.size();
        rows.push_back(HistoryTopRow{});
        rows.back().id = id;
        return rows.back();
    };
    for (const HistoryPlay& p : plays) {   // newest first: the newest name / artist wins
        if (p.id.empty() || !keep(p)) continue;
        HistoryTopRow& r = row_for(p.id);
        ++r.plays;
        r.listened_sec += p.listened_sec;
        if (r.title.empty()) r.title = p.title;
        if (r.artist.empty()) r.artist = p.artist;
        if (r.len_sec <= 0) r.len_sec = p.len_sec;
    }
    if (archive) {
        for (const auto& kv : archive->titles) {
            const int n = archived(kv.second);
            if (n <= 0) continue;
            HistoryTopRow& r = row_for(kv.first);
            r.plays += n;
            if (kv.second.plays > 0) r.listened_sec += kv.second.listened_sec * n / kv.second.plays;
            if (r.title.empty()) r.title = kv.second.title;
            if (r.artist.empty()) r.artist = kv.second.artist;
            if (r.len_sec <= 0) r.len_sec = kv.second.len_sec;
        }
    }
    std::sort(rows.begin(), rows.end(), [most_first](const HistoryTopRow& a, const HistoryTopRow& b) {
        if (a.plays != b.plays) return most_first ? a.plays > b.plays : a.plays < b.plays;
        if (a.listened_sec != b.listened_sec) return most_first ? a.listened_sec > b.listened_sec
                                                                : a.listened_sec < b.listened_sec;
        return a.title < b.title;
    });
    return rows;
}

} // namespace

std::vector<HistoryTopRow> history_top_since(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                                             long long since, bool most_first) {
    const std::string first_day = day_key(since);
    return top_filtered(plays, archive, most_first,
        [since](const HistoryPlay& p) { return p.started_at >= since; },
        [&first_day](const HistoryArchiveTitle& t) {
            int n = 0;
            for (auto it = t.day_plays.lower_bound(first_day); it != t.day_plays.end(); ++it) n += it->second;
            return n;
        });
}

std::vector<HistoryTopRow> history_top_tod(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                                           int tod, bool most_first) {
    if (tod < 0 || tod > 3) return {};
    return top_filtered(plays, archive, most_first,
        [tod](const HistoryPlay& p) { return history_tod_bucket(p.started_at) == tod; },
        [tod](const HistoryArchiveTitle& t) { return t.tod[static_cast<size_t>(tod)]; });
}

std::vector<HistoryPlaylistRow> history_top_playlists(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                                                      long long since, size_t n) {
    std::map<std::string, HistoryPlaylistRow> by_name;
    for (const HistoryPlay& p : plays) {
        if (p.playlist.empty() || (since > 0 && p.started_at < since)) continue;
        HistoryPlaylistRow& r = by_name[p.playlist];
        r.name = p.playlist;
        ++r.plays;
        r.listened_sec += p.listened_sec;
    }
    if (archive) {
        const std::string first_day = since > 0 ? day_key(since) : std::string();
        for (const auto& kv : archive->playlists) {
            int cnt = 0;
            double sec = 0.0;
            if (since <= 0) { cnt = kv.second.plays; sec = kv.second.listened_sec; }
            else {
                for (auto it = kv.second.day_plays.lower_bound(first_day); it != kv.second.day_plays.end(); ++it) cnt += it->second;
                if (kv.second.plays > 0) sec = kv.second.listened_sec * cnt / kv.second.plays;
            }
            if (cnt <= 0) continue;
            HistoryPlaylistRow& r = by_name[kv.first];
            r.name = kv.first;
            r.plays += cnt;
            r.listened_sec += sec;
        }
    }
    std::vector<HistoryPlaylistRow> rows;
    rows.reserve(by_name.size());
    for (auto& kv : by_name) rows.push_back(std::move(kv.second));
    std::sort(rows.begin(), rows.end(), [](const HistoryPlaylistRow& a, const HistoryPlaylistRow& b) {
        if (a.plays != b.plays) return a.plays > b.plays;
        if (a.listened_sec != b.listened_sec) return a.listened_sec > b.listened_sec;
        return a.name < b.name;
    });
    if (rows.size() > n) rows.resize(n);
    return rows;
}

void history_playlist_totals(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive,
                             int& playlists, long long& playlist_plays) {
    const auto rows = history_top_playlists(plays, archive, 0, static_cast<size_t>(-1));
    playlists = static_cast<int>(rows.size());
    playlist_plays = 0;
    for (const auto& r : rows) playlist_plays += r.plays;
}

HistoryStats history_stats(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive) {
    HistoryStats s;
    // Lifetime totals = the archive (records that left the window) + the window.
    long long total_plays = static_cast<long long>(plays.size());
    long long total_finished = 0;
    std::map<std::string, int> per_title;
    std::map<std::string, double> per_day;
    if (archive) {
        total_plays += archive->plays;
        total_finished += archive->finished;
        s.total_sec += archive->listened_sec;
        for (const auto& kv : archive->titles) per_title[kv.first] += kv.second.plays;
        per_day = archive->per_day;
        s.hours = archive->hours;
        s.weekdays = archive->weekdays;
    }
    for (const HistoryPlay& p : plays) {
        if (p.finished) ++total_finished;
        if (!p.id.empty()) ++per_title[p.id];
        s.total_sec += p.listened_sec;
        if (p.started_at > 0) per_day[day_key(p.started_at)] += p.listened_sec;
        add_time_buckets(p, s.hours, s.weekdays);
    }
    {
        double best = 0;
        for (int h = 0; h < 24; ++h) if (s.hours[static_cast<size_t>(h)] > best) { best = s.hours[static_cast<size_t>(h)]; s.busiest_hour = h; }
    }
    s.plays = static_cast<int>(total_plays);
    s.finished = static_cast<int>(total_finished);
    s.skipped = s.plays - s.finished;
    s.completion = s.plays > 0 ? static_cast<double>(s.finished) / s.plays : 0.0;
    for (const auto& kv : per_title) {
        if (kv.second > 1) s.replays += kv.second - 1;
    }

    // Sessions: walk oldest -> newest (plays is newest-first, so the loop
    // runs backwards) and cut wherever the silence between the END of the
    // session so far and the start of the next play exceeds 30 minutes.
    // Measuring from the previous end (not from its start) is what stops one
    // long album or podcast run from being counted as many short sessions.
    // The walk starts from the archive's state, so sessions that began before
    // the window are neither lost nor split.
    int sessions = archive ? archive->sessions_closed : 0;
    double session_sum = archive ? archive->session_sum_sec : 0.0;
    long long sess_start = archive ? archive->sess_start : 0;
    long long sess_end = archive ? archive->sess_end : 0; // earliest start / latest end of the session being grown
    bool in_session = archive ? archive->session_open : false;
    for (size_t i = plays.size(); i-- > 0;) {
        const HistoryPlay& p = plays[i];
        if (p.started_at <= 0) continue;
        long long p_end = p.started_at + static_cast<long long>(p.listened_sec + 0.5);
        // The gap is measured from the latest end seen so far to this (newer)
        // play's start. The subtraction must use this order: the gap is only positive when walking
        // newest -> oldest; walking the other way it is always hugely negative, so no gap would be
        // detected and the whole history would come out as one session.
        if (!in_session || p.started_at - sess_end > kSessionGapSec) {
            if (in_session) {
                ++sessions;
                session_sum += static_cast<double>(std::max<long long>(0, sess_end - sess_start));
            }
            in_session = true;
            sess_start = p.started_at;
            sess_end = p_end;
        } else {
            sess_start = std::min(sess_start, p.started_at);
            sess_end = std::max(sess_end, p_end);
        }
    }
    if (in_session) {
        ++sessions;
        session_sum += static_cast<double>(std::max<long long>(0, sess_end - sess_start));
    }
    s.sessions = sessions;
    s.avg_session_sec = sessions > 0 ? session_sum / sessions : 0.0;
    s.tracks_per_session = sessions > 0 ? static_cast<double>(s.plays) / sessions : 0.0;

    s.days = static_cast<int>(per_day.size());
    s.avg_day_sec = s.days > 0 ? s.total_sec / s.days : 0.0;
    std::string today = day_key(static_cast<long long>(std::time(nullptr)));

    // Average per weekday: every weekday of the calendar from the first day with music up to today counts, so a
    // Monday without music pulls the Monday average down (it is "an average Monday", not "an average Monday with music").
    if (!per_day.empty()) {
        long long first = 0, last = 0;
        const std::string& first_key = per_day.begin()->first;   // "YYYY-MM-DD" sorts chronologically
        const std::string& last_key = std::max(today, per_day.rbegin()->first);
        if (civil_day_number(first_key, first) && civil_day_number(last_key, last) && last >= first) {
            const long long n = last - first + 1;
            const int first_wd = monday_first_weekday(first);
            for (int k = 0; k < 7; ++k) {
                // the k-th weekday after the first day occurs ceil((n - k) / 7) times in the range
                const long long cnt = n > k ? (n - k + 6) / 7 : 0;
                s.weekday_count[static_cast<size_t>((first_wd + k) % 7)] = static_cast<int>(cnt);
            }
        }
    }
    for (size_t d = 0; d < 7; ++d)
        s.weekday_avg[d] = s.weekday_count[d] > 0 ? s.weekdays[d] / s.weekday_count[d] : 0.0;
    auto it = per_day.find(today);
    if (it != per_day.end()) s.today_sec = it->second;
    return s;
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

std::string format_mmss(double sec) {
    if (sec < 0) sec = 0;
    long long t = static_cast<long long>(sec + 0.5);
    long long h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", h, m, s);
    else std::snprintf(buf, sizeof(buf), "%lld:%02lld", m, s);
    return buf;
}

std::string format_len(double sec) {
    if (sec < 0) sec = 0;
    long long t = static_cast<long long>(sec + 0.5);
    char buf[32];
    if (t < 60) {
        std::snprintf(buf, sizeof(buf), "%llds", t);
        return buf;
    }
    long long h = t / 3600, m = (t % 3600) / 60;
    if (h == 0) {
        std::snprintf(buf, sizeof(buf), "%lldm", m);
        return buf;
    }
    std::snprintf(buf, sizeof(buf), "%lldh %02lldm", h, m);
    return buf;
}

std::string format_when(long long unix_sec) {
    std::tm tm = local_tm(unix_sec);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    return buf;
}

} // namespace muisc
