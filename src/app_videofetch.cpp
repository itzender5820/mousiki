// The FETCH VIDEOS overlay (Mode::VideoFetch, HKeyVideoFetch = ALT+v), part of class App (src/app.h). A full-screen overlay built like the
// cover fetch / meta data menus: on the left the songs (the playing one first under the cursor; the songs that have no video yet), on the
// right the videos found on YouTube (yt-dlp) for the song under the cursor -- searched by itself when the cursor rests on a song, with the
// file name and the tags -- and a preview of the video under the cursor. When there are several videos to choose from, the user has to
// choose one (ENTER); a single hit is the choice already. Only songs with an active choice are downloaded (into <download folder>/video_down)
// and paired with their song.
#include "app.h"
#include "app_internal.h"
#include "console_log.h"
#include "metadata_probe.h"
#include "online_source.h"
#include "open_url.h"
#include "overlay_common.h"
#include "path_utf8.h"
#include "process_util.h"
#include "string_util.h"
#include "terminal_ui.h"
#include "text_edit.h"
#include "tiny_json.h"
#include "utf8_util.h"
#include "video_links.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace muisc {

namespace {

std::string vf_safe_name(std::string s) {
    for (char& c : s) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) c = '_';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    if (s.size() > 150) s.resize(150);
    return s;
}

std::string vf_without_track_number(const std::string& stem) {
    size_t i = 0;
    while (i < stem.size() && (std::isdigit(static_cast<unsigned char>(stem[i])) || stem[i] == ' ' || stem[i] == '.' || stem[i] == '-' || stem[i] == '_')) ++i;
    return i < stem.size() ? stem.substr(i) : stem;
}

// A file name as a YouTube search text: yt-dlp writes look-alike characters into file names for the ones a file name
// cannot hold ("/" -> U+29F8, "|" -> U+FF5C, ":" -> U+FF1A ...) and appends " [videoid]": none of that helps a search.
std::string vf_search_text(const std::string& name) {
    static const char* const kLookAlikes[] = {"\xE2\xA7\xB8", "\xE2\x88\x95", "\xEF\xBC\x8F", "\xEF\xBD\x9C", "\xEF\xBC\x9A", "\xEF\xBC\x9F", "\xEF\xBC\x8A", "\xEF\xBC\x9C", "\xEF\xBC\x9E", "\xE2\x80\x98", "\xE2\x80\x99"};
    std::string t = name;
    for (const char* la : kLookAlikes) {
        const std::string from = la;
        size_t at = 0;
        while ((at = t.find(from, at)) != std::string::npos) { t.replace(at, from.size(), " "); at += 1; }
    }
    const size_t br = t.rfind(" [");   // " [dQw4w9WgXcQ]"
    if (br != std::string::npos && t.size() >= 2 && t.back() == ']' && t.size() - br - 3 == 11) t.erase(br);
    std::string out;
    bool space = false;
    for (char c : t) {
        if (c == ' ' || c == '\t') { space = true; continue; }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string vf_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string vf_clock(double sec) {
    if (sec < 0) return "";
    const int s = static_cast<int>(sec + 0.5);
    char b[24];
    if (s >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

// the index of the hit whose length is closest to the song's (within a fifth, at least 20 s), -1 = none
int vf_closest(const std::vector<OnlineResult>& hits, double song_seconds) {
    if (song_seconds <= 0) return -1;
    const double tol = std::max(20.0, song_seconds * 0.2);
    int best = -1;
    double best_diff = 0;
    for (size_t i = 0; i < hits.size(); ++i) {
        if (hits[i].duration_sec < 0) continue;
        const double d = std::fabs(hits[i].duration_sec - song_seconds);
        if (d <= tol && (best < 0 || d < best_diff)) { best = static_cast<int>(i); best_diff = d; }
    }
    return best;
}

constexpr int kCandRows = 6;

}  // namespace

// The hit to download when nobody chose: with a known length of the song the one whose length is closest, else the first.
const OnlineResult* video_fetch_pick(const std::vector<OnlineResult>& hits, double song_seconds) {
    if (hits.empty()) return nullptr;
    const int c = vf_closest(hits, song_seconds);
    return c >= 0 ? &hits[static_cast<size_t>(c)] : &hits[0];
}

// ---- the list ----------------------------------------------------------------------------------------------------------------------

void App::video_fetch_rebuild_list() {
    // the songs without a video (no connection to a file that is there, no video of the same name beside them) -- and the playing song
    std::vector<LocalTrack> scanned;
    const std::vector<LocalTrack>* src = &all_local_tracks_;
    if (!settings_.video_mode) {   // the library list holds no videos while the Video mode is off: look at the folders once for them
        LocalSource ls;
        ls.set_include_video(true);
        std::vector<std::string> diag;
        scanned = ls.scan(library_roots(), &diag);
        src = &scanned;
    }
    std::unordered_set<std::string> have;   // folder + file name of the videos
    for (const LocalTrack& t : *src)
        if (LocalSource::is_video_file(t.path)) have.insert(vf_lower(path_utf8(t.path.parent_path())) + "/" + vf_lower(path_utf8(t.path.stem())));
    std::unordered_map<std::string, VFEntry> old;   // what was found and chosen stays when the list is built again
    if (!vf_saved_loaded_) {   // what the last time left (video_fetch_state.json): the searches and the choices of the songs that were not downloaded
        vf_saved_loaded_ = true;
        std::ifstream in(config_path().parent_path() / "video_fetch_state.json", std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        tinyjson::Value root;
        if (in.good() || !ss.str().empty()) {
            if (tinyjson::parse(ss.str(), root) && root.type == tinyjson::Type::Object) {
                if (const tinyjson::Value* arr = root.find("songs")) {
                    for (const tinyjson::Value& sv : arr->arr) {
                        VFEntry e;
                        const std::string path = tinyjson::string_field(sv, "path");
                        if (path.empty()) continue;
                        e.path = path_from_utf8(path);
                        e.seconds = tinyjson::number_field(sv, "seconds", -1.0);
                        if (const tinyjson::Value* cv = sv.find("cands")) {
                            for (const tinyjson::Value& c : cv->arr) {
                                OnlineResult r;
                                r.video_id = tinyjson::string_field(c, "id");
                                r.title = tinyjson::string_field(c, "title");
                                r.uploader = tinyjson::string_field(c, "uploader");
                                r.duration_sec = tinyjson::number_field(c, "dur", -1.0);
                                if (!r.video_id.empty()) e.cands.push_back(std::move(r));
                            }
                        }
                        if (e.cands.empty()) continue;
                        e.searched = true;
                        const int ch = static_cast<int>(tinyjson::number_field(sv, "choice", -1.0));
                        e.choice = ch >= 0 && ch < static_cast<int>(e.cands.size()) ? ch : -1;
                        vf_saved_[path] = std::move(e);
                    }
                }
            }
        }
    }
    for (VFEntry& e : vf_all_) old[path_utf8(e.path)] = std::move(e);
    vf_all_.clear();
    const std::string playing = (has_track_ && !current_path_.empty()) ? path_utf8(current_path_) : std::string();
    for (const LocalTrack& t : *src) {
        if (LocalSource::is_video_file(t.path)) continue;
        const std::string key = path_utf8(t.path);
        const std::string linked = VideoLinks::get().video_for(key);
        std::error_code ec;
        bool has = (!linked.empty() && fs::exists(path_from_utf8(linked), ec)) ||
                   have.count(vf_lower(path_utf8(t.path.parent_path())) + "/" + vf_lower(path_utf8(t.path.stem()))) > 0;
        if (vf_filter_ == 0 && has && key != playing) continue;   // missing video only (the playing song stays: it can be re-fetched)
        if (vf_filter_ == 2 && !has) continue;                    // songs with a video only
        VFEntry e;
        const auto it = old.find(key);
        if (it != old.end()) e = std::move(it->second);
        else if (!has) { const auto sv = vf_saved_.find(key); if (sv != vf_saved_.end()) e = sv->second; }   // (searched and chosen the last time)
        e.path = t.path;
        e.has_video = has;
        e.label = (t.folder_artist.empty() || t.folder_artist == "-") ? t.title : t.folder_artist + " - " + t.title;
        e.folder = path_utf8(t.path.parent_path().filename());
        e.hay = vf_lower(with_latin_spelling(e.label) + " " + path_utf8(t.path.filename()) + " " + e.folder);
        e.searching = false; e.queued = false;
        vf_all_.push_back(std::move(e));
    }
    std::sort(vf_all_.begin(), vf_all_.end(), [](const VFEntry& a, const VFEntry& b) { return vf_lower(a.label) < vf_lower(b.label); });
    vf_queue_.clear();
    video_fetch_refilter();
}

// The found videos and the choices are kept in video_fetch_state.json (next to config.txt) at every change: leaving the overlay without
// downloading loses nothing. Songs that are not listed at the moment keep what they had; a song that has its video now, or whose file is
// gone, is dropped.
void App::video_fetch_save_state() {
    for (const VFEntry& e : vf_all_) {
        const std::string key = path_utf8(e.path);
        if (e.cands.empty() || (e.has_video && !e.refetch)) vf_saved_.erase(key);
        else vf_saved_[key] = e;
    }
    tinyjson::Value songs = tinyjson::Value::make_arr();
    for (auto it = vf_saved_.begin(); it != vf_saved_.end();) {
        std::error_code ec;
        if (!fs::exists(it->second.path, ec)) { it = vf_saved_.erase(it); continue; }
        const VFEntry& e = it->second;
        tinyjson::Value sv = tinyjson::Value::make_obj();
        sv.set("path", tinyjson::Value::make_str(it->first));
        sv.set("choice", tinyjson::Value::make_num(e.choice));
        sv.set("seconds", tinyjson::Value::make_num(e.seconds));
        tinyjson::Value cv = tinyjson::Value::make_arr();
        for (const OnlineResult& r : e.cands) {
            tinyjson::Value c = tinyjson::Value::make_obj();
            c.set("id", tinyjson::Value::make_str(r.video_id));
            c.set("title", tinyjson::Value::make_str(r.title));
            c.set("uploader", tinyjson::Value::make_str(r.uploader));
            c.set("dur", tinyjson::Value::make_num(r.duration_sec));
            cv.arr.push_back(std::move(c));
        }
        sv.set("cands", std::move(cv));
        songs.arr.push_back(std::move(sv));
        ++it;
    }
    tinyjson::Value root = tinyjson::Value::make_obj();
    root.set("songs", std::move(songs));
    const fs::path p = config_path().parent_path() / "video_fetch_state.json";
    std::error_code ec;
    if (vf_saved_.empty()) { fs::remove(p, ec); return; }
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out.is_open()) return;
        out << tinyjson::write(root);
    }
    fs::rename(tmp, p, ec);
}

void App::video_fetch_refilter_keep() {
    std::string keep;
    if (const VFEntry* h = video_fetch_hovered()) keep = path_utf8(h->path);
    video_fetch_refilter();
    for (size_t k = 0; k < vf_view_.size() && !keep.empty(); ++k)
        if (path_utf8(vf_all_[static_cast<size_t>(vf_view_[k])].path) == keep) { vf_sel_ = static_cast<int>(k); break; }
}

void App::video_fetch_refilter() {
    std::vector<std::string> words;
    std::string cur;
    for (char c : vf_lower(vf_query_)) {
        if (c == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); } else cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
    vf_view_.clear();
    for (size_t i = 0; i < vf_all_.size(); ++i) {
        bool ok = true;
        for (const std::string& w : words) if (vf_all_[i].hay.find(w) == std::string::npos) { ok = false; break; }
        if (ok && vf_filter_ == 3 && vf_all_[i].choice < 0) ok = false;   // only the songs with a chosen video
        if (ok) vf_view_.push_back(static_cast<int>(i));
    }
    vf_sel_ = std::clamp(vf_sel_, 0, std::max(0, static_cast<int>(vf_view_.size()) - 1));
}

const App::VFEntry* App::video_fetch_hovered() const {
    if (vf_view_.empty()) return nullptr;
    return &vf_all_[static_cast<size_t>(vf_view_[static_cast<size_t>(std::clamp(vf_sel_, 0, static_cast<int>(vf_view_.size()) - 1))])];
}

void App::video_fetch_open(const fs::path& focus) {
    const fs::path target = focus.empty() ? (has_track_ ? current_path_ : fs::path()) : focus;   // (the song the cursor starts on)
    vf_focus_ = 1;
    vf_tab_ = 0;
    vm_confirm_.clear();
    vf_cand_sel_ = 0;
    vf_warn_ = false;
    vf_filter_ = (!target.empty() && !LocalSource::is_video_file(target) && !video_source_for(target).empty()) ? 1 : 0;   // (a song with a video is listed too, the cursor stands on it)
    vf_confirm_.clear();
    vf_msg_.clear();
    vf_hover_.clear();
    vf_pic_key_.clear();
    vf_query_.clear();     // (a filter from the last time would hide the playing song)
    edit_owner_.clear();
    video_fetch_rebuild_list();
    vf_sel_ = 0;
    // the cursor stands on the playing song
    if (!target.empty() && !LocalSource::is_video_file(target)) {
        auto same = [&](const fs::path& a) {
            if (a == target || vf_lower(path_utf8(a)) == vf_lower(path_utf8(target))) return true;
            std::error_code ec;
            return fs::equivalent(a, target, ec);
        };
        int at = -1;
        for (size_t k = 0; k < vf_view_.size() && at < 0; ++k)
            if (same(vf_all_[static_cast<size_t>(vf_view_[k])].path)) at = static_cast<int>(k);
        if (at < 0) {   // not in the scanned library (opened from elsewhere): it is listed all the same
            VFEntry e;
            e.path = target;
            const bool playing = has_track_ && target == current_path_;
            const TrackMetadata md = playing ? display_metadata() : TrackMetadata();
            e.label = (md.artist.empty() || md.artist == "-") ? (md.name.empty() || md.name == "-" ? path_utf8(target.stem()) : md.name) : md.artist + " - " + md.name;
            e.folder = path_utf8(target.parent_path().filename());
            e.hay = vf_lower(with_latin_spelling(e.label) + " " + path_utf8(target.filename()) + " " + e.folder);
            e.has_video = !video_source_for(target).empty();
            vf_all_.insert(vf_all_.begin(), std::move(e));
            video_fetch_refilter();
            at = 0;
        }
        vf_sel_ = at;
    }
    mode_ = Mode::VideoFetch;
    force_redraw_ = true;
}

void App::video_fetch_close() {
    video_fetch_save_state();
    text_disengage();
    gfx_.active = false;
    mode_ = Mode::Browse;   // (a running download goes on; its end is reported in the status line)
    force_redraw_ = true;
}

void App::video_fetch_cancel_and_join() {
    vf_.cancel = true;
    {
        std::lock_guard<std::mutex> lk(vf_.child_m);
        if (vf_.child) vf_.child->kill();
    }
    if (vf_.thread.joinable()) vf_.thread.join();
    vf_.running = false;
}

void App::video_fetch_enqueue(const std::string& path, bool front) {
    for (VFEntry& e : vf_all_) {
        if (path_utf8(e.path) != path || e.searched || e.searching || e.queued || (e.has_video && !e.refetch)) continue;
        e.queued = true;
        if (front) vf_queue_.push_front(path); else vf_queue_.push_back(path);
        return;
    }
}

int App::video_fetch_nav(int selected, int total, bool up) const {
    if (total <= 0) return 0;
    int to;
    if (last_key_was_ctrl()) to = up ? 0 : total - 1;
    else if (last_key_was_shifted()) to = selected + (up ? -1 : 1) * std::max(1, vf_page_rows_);
    else to = selected + (up ? -1 : 1);
    return std::clamp(to, 0, total - 1);
}

// ---- searching and downloading --------------------------------------------------------------------------------------------------------

void App::video_fetch_poll() {
    if (vf_.linked.exchange(false)) {
        ++video_rev_;
        video_for_track_.clear();
        force_redraw_ = true;
    }
    if (mode_ == Mode::VideoFetch) {
        // a finished search
        if (!vf_searching_.empty() && vf_search_.valid() && vf_search_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            std::vector<OnlineResult> hits = vf_search_.get();
            for (VFEntry& e : vf_all_) {
                if (path_utf8(e.path) != vf_searching_) continue;
                e.searching = false;
                e.searched = true;
                e.cands = std::move(hits);
                e.choice = e.cands.size() == 1 ? 0 : -1;   // one hit is the choice; several ones have to be chosen
                if (e.cands.empty()) {   // (the exact texts searched are named: a search that finds nothing can be compared with /sv:)
                    vf_msg_ = "nothing found for: " + vf_last_queries_;
                    vf_warn_ = true;
                    log_event("video fetch: nothing found for: " + vf_last_queries_);
                }
                break;
            }
            bool auto_go = false;
            for (VFEntry& e : vf_all_) {   // "Refetch this Video": the closest hit is chosen and downloaded by itself
                if (path_utf8(e.path) != vf_searching_ || vf_auto_ != vf_searching_) continue;
                if (!e.cands.empty()) { const int c = vf_closest(e.cands, e.seconds); e.choice = c >= 0 ? c : 0; auto_go = true; }
                vf_auto_.clear();
            }
            vf_searching_.clear();
            video_fetch_save_state();
            if (vf_filter_ == 3) video_fetch_refilter_keep();
            if (auto_go) video_fetch_start();
            force_redraw_ = true;
        }
        // the search of the song under the cursor starts by itself when the cursor has rested there for a moment
        if (const VFEntry* h = vf_tab_ == 0 ? video_fetch_hovered() : nullptr) {
            const std::string hp = path_utf8(h->path);
            const auto now = std::chrono::steady_clock::now();
            if (hp != vf_hover_) {
                vf_hover_ = hp; vf_hover_since_ = now;
                const RowMeta rm = meta_row_meta(h->path);   // (the cheap native tag reader; cached)
                std::string tg;
                for (const std::string& part : {rm.title, rm.artist, rm.album}) if (!part.empty() && part != "-") tg += (tg.empty() ? "" : "  /  ") + part;
                for (VFEntry& x : vf_all_) if (x.path == h->path) x.tags = tg;
            }
            else if (!h->searched && !h->searching && !h->queued && now - vf_hover_since_ >= std::chrono::milliseconds(250)) video_fetch_enqueue(hp, true);
        }
        // the next search of the queue
        if (vf_searching_.empty() && !vf_queue_.empty()) {
            const std::string path = vf_queue_.front();
            vf_queue_.pop_front();
            for (VFEntry& e : vf_all_) {
                if (path_utf8(e.path) != path) continue;
                e.queued = false;
                if (e.searched) break;
                const RowMeta rm = meta_row_meta(e.path);
                e.seconds = rm.duration_sec;
                std::vector<std::string> queries;
                const std::string by_name = vf_search_text(vf_without_track_number(path_utf8(e.path.stem())));
                const std::string by_tags = !rm.title.empty() ? vf_search_text(rm.artist.empty() ? rm.title : rm.artist + " " + rm.title) : std::string();
                if (vf_src_ != 2 || by_tags.empty()) queries.push_back(by_name);
                if (vf_src_ != 1 && !by_tags.empty() && vf_lower(by_tags) != vf_lower(by_name)) queries.push_back(by_tags);
                e.searching = true;
                vf_searching_ = path;
                vf_last_queries_.clear();
                for (const std::string& q : queries) vf_last_queries_ += (vf_last_queries_.empty() ? "\"" : " | \"") + q + "\"";
                vf_search_ = std::async(std::launch::async, [queries]() {
                    std::vector<std::vector<OnlineResult>> lists;
                    for (const std::string& q : queries) { OnlineSource src; auto hits = src.search_videos(q, 10);
                        if (hits.empty()) hits = src.search_videos(q, 10);   // (one more try: a single failed yt-dlp call must not empty the list)
                        lists.push_back(std::move(hits));
                    }
                    std::vector<OnlineResult> merged;   // alternating, without doubles
                    std::unordered_set<std::string> seen;
                    for (size_t i = 0; i < 10; ++i)
                        for (const auto& l : lists)
                            if (i < l.size() && seen.insert(l[i].video_id).second) merged.push_back(l[i]);
                    if (merged.size() > 8) merged.resize(8);
                    return merged;
                });
                break;
            }
            force_redraw_ = true;
        }
    }
    if (!vf_.running) return;
    bool finished;
    int ok, failed, total;
    std::string err;
    std::vector<std::string> done_paths;
    {
        std::lock_guard<std::mutex> lk(vf_.m);
        finished = vf_.finished;
        ok = vf_.ok; failed = vf_.failed; total = vf_.total;
        err = vf_.last_error;
        done_paths = vf_.done_paths;
    }
    if (mode_ == Mode::VideoFetch) force_redraw_ = true;   // (the progress line moves)
    if (!finished) return;
    if (vf_.thread.joinable()) vf_.thread.join();
    vf_.running = false;
    if (vf_.linked.exchange(false)) { ++video_rev_; video_for_track_.clear(); }
    std::string replaced_note;
    for (const std::string& dp : done_paths) {   // "Refetch this Video": the old downloaded file goes once the new one is connected
        const auto it = vf_replace_.find(dp);
        if (it == vf_replace_.end()) continue;
        const std::string old = it->second;
        vf_replace_.erase(it);
        const std::string now = VideoLinks::get().video_for(dp);
        if (old.empty() || old == now) continue;
        bool other_users = false;
        for (const std::string& t : VideoLinks::get().tracks_for(old)) if (t != dp) other_users = true;
        if (other_users) { replaced_note += " (the old video is still used by another song: not deleted)"; continue; }
        std::string dmsg;
        if (video_delete_file(old, dmsg, false)) replaced_note += " (the old video was deleted)";
        else replaced_note += " (" + dmsg + ")";
    }
    if (settings_.video_mode && ok > 0) rescan_now();   // the new files are listed
    std::string msg = vf_.cancel.load() ? "video fetch cancelled: " : "video fetch done: ";
    msg += std::to_string(ok) + " of " + std::to_string(total) + " chosen videos downloaded and paired" + (failed > 0 ? ", " + std::to_string(failed) + " failed" + (err.empty() ? "" : " (" + err + ")") : "") + replaced_note;
    status_line_ = msg;
    log_event(msg);
    if (mode_ == Mode::VideoFetch) {
        vf_msg_ = msg;
        vf_warn_ = failed > 0;
        // the songs that have their video now leave the list
        vf_all_.erase(std::remove_if(vf_all_.begin(), vf_all_.end(), [&](const VFEntry& e) {
            return std::find(done_paths.begin(), done_paths.end(), path_utf8(e.path)) != done_paths.end() && path_utf8(e.path) != (has_track_ ? path_utf8(current_path_) : std::string());
        }), vf_all_.end());
        for (VFEntry& e : vf_all_)
            if (std::find(done_paths.begin(), done_paths.end(), path_utf8(e.path)) != done_paths.end()) { e.has_video = true; e.refetch = false; e.choice = -1; e.cands.clear(); e.searched = false; }
        video_fetch_refilter();
        video_fetch_save_state();
    }
    force_redraw_ = true;
}

void App::video_fetch_start(bool only_current) {
    if (vf_.running) { vf_msg_ = "a download is running already"; vf_warn_ = true; return; }
    std::vector<VFJob> jobs;
    int undecided = 0;
    std::string only;   // only_current: the song under the cursor
    if (only_current) {
        const VFEntry* h = video_fetch_hovered();
        if (!h) { vf_msg_ = "no song selected"; vf_warn_ = true; return; }
        only = path_utf8(h->path);
    }
    for (const VFEntry& e : vf_all_) {
        if (only_current && path_utf8(e.path) != only) continue;
        if (e.choice < 0 || static_cast<size_t>(e.choice) >= e.cands.size()) { if (e.cands.size() > 1) ++undecided; continue; }
        VFJob j;
        j.track = e.path;
        j.label = e.label;
        j.video_id = e.cands[static_cast<size_t>(e.choice)].video_id;
        j.title = e.cands[static_cast<size_t>(e.choice)].title;
        j.base = vf_safe_name(path_utf8(e.path.stem()));
        if (j.base.empty()) j.base = "video";
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) {
        vf_msg_ = only_current ? (undecided > 0 ? "this song needs a choice" : "nothing chosen for this song yet")
                : undecided > 0 ? std::to_string(undecided) + (undecided == 1 ? " song needs" : " songs need") + " a choice"
                                : "nothing chosen yet";
        vf_warn_ = true;
        return;
    }
    const fs::path dir = video_down_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) { vf_msg_ = "cannot create " + path_utf8(dir) + ": " + ec.message(); vf_warn_ = true; return; }
    if (vf_.thread.joinable()) vf_.thread.join();
    {
        std::lock_guard<std::mutex> lk(vf_.m);
        vf_.total = static_cast<int>(jobs.size());
        vf_.done = vf_.ok = vf_.failed = 0;
        vf_.status = "starting ...";
        vf_.last_error.clear();
        vf_.done_paths.clear();
        vf_.finished = false;
    }
    vf_.cancel = false;
    vf_.running = true;
    const int height = settings_.video_down_height;
    vf_.thread = std::thread([this, jobs, dir, height]() {
        for (size_t i = 0; i < jobs.size() && !vf_.cancel.load(); ++i) {
            const VFJob& j = jobs[i];
            auto say = [&](const std::string& what) {
                std::lock_guard<std::mutex> lk(vf_.m);
                vf_.status = "[" + std::to_string(i + 1) + "/" + std::to_string(jobs.size()) + "] " + j.label + ": " + what;
            };
            auto fail = [&](const std::string& why) {
                std::lock_guard<std::mutex> lk(vf_.m);
                ++vf_.failed;
                ++vf_.done;
                vf_.last_error = j.label + ": " + why;
            };
            std::string file;
            std::error_code ec;
            const fs::path existing = dir / path_from_utf8(j.base + ".mp4");
            const fs::path known = cache_.find_video_download(j.video_id);   // the same video fetched before (Mousiki or down_vid)
            if (!known.empty()) {
                file = path_utf8(known);
            } else if (fs::exists(existing, ec)) {   // downloaded before: only the pairing is missing
                file = path_utf8(existing);
            } else {
                say("downloading \"" + j.title + "\" ...");
                const std::vector<std::string> args = video_download_args(j.video_id, dir, j.base, height);
                ConsoleLog::instance().log_basic("video fetch: " + command_text(args));
                std::unique_ptr<ChildProcess> child = spawn_capture(args, true);
                if (!child) { fail("yt-dlp could not be started (is it installed and on the PATH?)"); break; }
                {
                    std::lock_guard<std::mutex> lk(vf_.child_m);
                    vf_.child = child.get();
                }
                std::string buf, last_error, printed;
                char chunk[4096];
                for (;;) {
                    const std::ptrdiff_t n = child->read(chunk, sizeof chunk);
                    if (n <= 0) break;
                    buf.append(chunk, static_cast<size_t>(n));
                    size_t nl;
                    while ((nl = buf.find_first_of("\r\n")) != std::string::npos) {
                        std::string line = buf.substr(0, nl);
                        buf.erase(0, nl + 1);
                        if (line.empty()) continue;
                        if (line.rfind("download:PCT ", 0) == 0 || line.rfind("PCT ", 0) == 0) say("downloading " + trim_ws(line.substr(line.find("PCT ") + 4)));
                        else if (line.rfind("FILE:", 0) == 0) printed = trim_ws(line.substr(5));
                        else if (line.rfind("ERROR", 0) == 0) last_error = line;
                        else if (line.find("[Merger]") != std::string::npos) say("joining picture and sound ...");
                        else ConsoleLog::instance().log_basic("video fetch: " + line);
                    }
                }
                const int code = child->wait();
                {
                    std::lock_guard<std::mutex> lk(vf_.child_m);
                    vf_.child = nullptr;
                }
                if (vf_.cancel.load()) break;
                file = printed;
                if (file.empty() || !fs::exists(path_from_utf8(file), ec)) {   // the printed path can be garbled by a console code page: look the file up by the name it was given
                    file.clear();
                    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                        const std::string ext = path_utf8(it->path().extension());
                        if (path_utf8(it->path().stem()) == j.base && ext != ".part" && ext != ".ytdl" && ext != ".temp") { file = path_utf8(it->path()); break; }
                    }
                }
                if (file.empty()) { fail(!last_error.empty() ? last_error.substr(0, 120) : "yt-dlp ended with exit code " + std::to_string(code)); continue; }
            }
            cache_.record_video_download(j.video_id, file);
            VideoLinks::get().set(path_utf8(j.track), file);
            vf_.linked.store(true);
            std::lock_guard<std::mutex> lk(vf_.m);
            ++vf_.ok;
            ++vf_.done;
            vf_.done_paths.push_back(path_utf8(j.track));
        }
        std::lock_guard<std::mutex> lk(vf_.m);
        vf_.finished = true;
    });
    vf_msg_ = "downloading " + std::to_string(jobs.size()) + (jobs.size() == 1 ? " chosen video" : " chosen videos") + " ...";
    vf_warn_ = false;
}

// ---- keys ------------------------------------------------------------------------------------------------------------------------------

void App::video_fetch_key(int key) {
    const bool arrow = last_key_was_arrow();
    const bool in_text = (vf_focus_ == 0 && text_engaged("vf-search")) || (vf_focus_ == 0 && text_engaged("vm-search"));
    if (key_is("HKeyVideoFetch", key) && !(in_text && key >= 32 && key < 127)) { video_fetch_close(); return; }
    if (key == kKeyAltLeft || key == kKeyAltRight || (arrow && (key == 'C' || key == 'D') && !in_text && vf_focus_ != 2 && vf_confirm_.empty() && vm_confirm_.empty())) {   // the two tabs
        text_disengage();
        vf_tab_ = 1 - vf_tab_;
        vf_focus_ = 1;
        vf_confirm_.clear();
        vm_confirm_.clear();
        vf_pic_key_.clear();
        vm_pic_key_.clear();
        vf_warn_ = false;
        if (vf_tab_ == 1) { video_manage_rebuild(); vf_msg_.clear(); }
        else vf_msg_.clear();
        force_redraw_ = true;
        return;
    }
    if (vf_tab_ == 1) { video_manage_key(key); return; }
    if (key == 27) {
        if (!vf_confirm_.empty()) { vf_confirm_.clear(); vf_msg_ = "re-fetch cancelled"; vf_warn_ = false; return; }
        if (vf_focus_ == 2) { vf_focus_ = 1; return; }
        if (vf_focus_ == 0 && !vf_query_.empty()) { vf_query_.clear(); edit_caret_ = edit_anchor_ = 0; vf_sel_ = 0; video_fetch_refilter(); return; }
        video_fetch_close();
        return;
    }
    if (!arrow && key == '\t') {
        text_disengage();
        vf_focus_ = vf_focus_ == 0 ? 1 : vf_focus_ == 1 ? (video_fetch_hovered() && !video_fetch_hovered()->cands.empty() ? 2 : 0) : 0;
        return;
    }
    if (key == kKeyCtrlC && vf_.running && vf_focus_ != 0) {
        video_fetch_cancel_and_join();
        vf_msg_ = "download cancelled";
        vf_warn_ = true;
        return;
    }
    if (key == kKeyCtrlF) {   // which songs are listed: missing video only / all songs / songs with a video / chosen
        text_disengage();
        std::string keep;
        if (const VFEntry* h = video_fetch_hovered()) keep = path_utf8(h->path);
        vf_filter_ = (vf_filter_ + 1) % 4;
        video_fetch_rebuild_list();
        vf_sel_ = 0;
        for (size_t k = 0; k < vf_view_.size(); ++k)
            if (path_utf8(vf_all_[static_cast<size_t>(vf_view_[k])].path) == keep) { vf_sel_ = static_cast<int>(k); break; }
        vf_cand_sel_ = 0;
        if (vf_focus_ == 2) vf_focus_ = 1;
        vf_hover_.clear();
        vf_msg_ = vf_filter_ == 0 ? "listed: songs with a missing video (and the playing song)" : vf_filter_ == 1 ? "listed: all songs" : vf_filter_ == 2 ? "listed: songs that have a video" : "listed: the songs with a chosen video";
        vf_warn_ = false;
        return;
    }
    if (vf_focus_ != 0 && !arrow && key_is("HKeySearch", key)) {   // "/" (SHIFT+7 on a German keyboard): to the filter field
        vf_focus_ = 0;
        edit_focus("vf-search", vf_query_);
        text_engage("vf-search");
        edit_caret_ = edit_anchor_ = vf_query_.size();
        return;
    }
    if (key == kKeyCtrlT) {   // searched by: file name + tags / file name / tags
        vf_src_ = (vf_src_ + 1) % 3;
        for (VFEntry& e : vf_all_) { e.cands.clear(); e.searched = false; e.choice = -1; }
        vf_queue_.clear();
        video_fetch_save_state();
        if (vf_filter_ == 3) video_fetch_refilter_keep();
        vf_msg_ = vf_src_ == 0 ? "search text: the file name and the tags" : vf_src_ == 1 ? "search text: the file name" : "search text: the tags (artist + title)";
        vf_warn_ = false;
        vf_hover_.clear();
        return;
    }
    if (vf_focus_ == 0) {   // --- the filter field of the song list ---
        if (key == '\r' || key == '\n' || (arrow && key == 'B')) { text_disengage(); vf_focus_ = 1; return; }
        if (arrow) return;
        edit_focus("vf-search", vf_query_);
        text_engage("vf-search");
        std::string note;
        if (edit_text_key(vf_query_, edit_caret_, edit_anchor_, key, 120, &note)) { vf_sel_ = 0; video_fetch_refilter(); }
        if (!note.empty()) { vf_msg_ = note; vf_warn_ = false; }
        return;
    }
    if (vf_focus_ == 1 && !vf_confirm_.empty()) {   // the warning of a re-fetch waits for its answer
        const std::string p = vf_confirm_;
        vf_confirm_.clear();
        if (!arrow && (key == 'r' || key == 'R' || key == '\r' || key == '\n')) {
            VideoLinks::get().mark_none(p);   // the former video is unassigned (also its pairing by file name)
            ++video_rev_;
            video_for_track_.clear();
            for (VFEntry& e : vf_all_) {
                if (path_utf8(e.path) != p) continue;
                e.refetch = true;
                e.has_video = !video_source_for(e.path).empty();
                e.searched = false; e.searching = false; e.cands.clear(); e.choice = -1;
            }
            video_fetch_enqueue(p, true);
            vf_msg_ = "the former video is unassigned -- searching other videos ...";
            vf_warn_ = false;
            force_redraw_ = true;
            return;
        }
        vf_msg_ = "re-fetch cancelled";
        vf_warn_ = false;
        if (key == 27) return;
    }
    if (vf_focus_ == 1) {   // --- the songs ---
        const int n = static_cast<int>(vf_view_.size());
        if (arrow && (key == 'A' || key == 'B')) {
            if (key == 'A' && vf_sel_ == 0 && !last_key_was_ctrl()) { vf_focus_ = 0; return; }
            vf_sel_ = video_fetch_nav(vf_sel_, n, key == 'A');
            vf_cand_sel_ = 0;
            return;
        }
        const VFEntry* h = video_fetch_hovered();
        if (!h) return;
        const bool held = h->has_video && !h->refetch;   // a song with a video: nothing is searched unless a re-fetch is confirmed
        if (!arrow && (key == 'r' || key == 'R') && held) {
            vf_confirm_ = path_utf8(h->path);
            const bool by_link = !VideoLinks::get().video_for(vf_confirm_).empty();
            vf_msg_ = by_link ? "WARNING: the video of this song is unassigned first (the file stays). [r] / [ENTER] confirms"
                              : "WARNING: this song's video is paired by its file name; the download takes its place. [r] / [ENTER] confirms";
            vf_warn_ = true;
            return;
        }
        if (held && (key == '\r' || key == '\n')) {
            vf_msg_ = "this song has a video already";
            vf_warn_ = false;
            return;
        }
        if (key == '\r' || key == '\n') {
            if (h->cands.empty()) {   // nothing found before: [ENTER] searches again
                const std::string hp = path_utf8(h->path);
                for (VFEntry& e : vf_all_) if (path_utf8(e.path) == hp && e.searched && !e.searching) { e.searched = false; e.queued = false; }
                video_fetch_enqueue(hp, true);
                vf_msg_ = "searching ...";
                vf_warn_ = false;
                return;
            }
            vf_focus_ = 2;
            vf_cand_sel_ = std::max(0, h->choice);
            return;
        }
        if (!arrow && key == 's' && held) { vf_msg_ = "this song has a video already"; vf_warn_ = false; return; }
        if (!arrow && key == 's') {   // search this song (again)
            for (VFEntry& e : vf_all_) if (e.path == h->path) { e.searched = false; e.cands.clear(); e.choice = -1; }
            video_fetch_save_state();
            video_fetch_enqueue(path_utf8(h->path), true);
            vf_msg_ = "searching ...";
            vf_warn_ = false;
            return;
        }
        if (!arrow && key == 'S') {   // search every song shown
            int added = 0;
            for (int i : vf_view_) { const VFEntry& e = vf_all_[static_cast<size_t>(i)]; if (!e.searched && !e.searching && !e.queued && !(e.has_video && !e.refetch)) { video_fetch_enqueue(path_utf8(e.path), false); ++added; } }
            vf_msg_ = added > 0 ? "searching the videos of " + std::to_string(added) + " songs, one after the other" : "all shown songs are searched already";
            vf_warn_ = false;
            return;
        }
        if (!arrow && key == 'c') {   // clear the choice of this song
            for (VFEntry& e : vf_all_) if (e.path == h->path) e.choice = -1;
            video_fetch_save_state();
            if (vf_filter_ == 3) video_fetch_refilter_keep();
            return;
        }
        if (!arrow && key == 'F') { video_fetch_start(); return; }
        if (!arrow && key == 'A') { video_fetch_start(true); return; }
        return;
    }
    // --- the videos of the song ---
    const VFEntry* h = video_fetch_hovered();
    if (!h || h->cands.empty()) { vf_focus_ = 1; return; }
    const int n = static_cast<int>(h->cands.size());
    if (arrow && (key == 'A' || key == 'B')) { vf_cand_sel_ = std::clamp(vf_cand_sel_ + (key == 'A' ? -1 : 1), 0, n - 1); return; }
    if (key == '\r' || key == '\n' || key == ' ') {   // the choice of this song; the cursor stays on the song
        for (VFEntry& e : vf_all_) if (e.path == h->path) e.choice = vf_cand_sel_;
        video_fetch_save_state();
        vf_focus_ = 1;
        vf_cand_sel_ = std::max(0, vf_cand_sel_);
        vf_msg_ = "chosen";
        vf_warn_ = false;
        return;
    }
    if (!arrow && key == 'c') { for (VFEntry& e : vf_all_) if (e.path == h->path) e.choice = -1; video_fetch_save_state(); if (vf_filter_ == 3) { video_fetch_refilter_keep(); vf_focus_ = 1; } return; }
    if (!arrow && (key == 'o' || key == 'O')) { open_url("https://www.youtube.com/watch?v=" + h->cands[static_cast<size_t>(vf_cand_sel_)].video_id); return; }
    if (!arrow && key == 'F') { video_fetch_start(); return; }
    if (!arrow && key == 'A') { video_fetch_start(true); return; }
}

// ---- drawing -----------------------------------------------------------------------------------------------------------------------------

std::vector<std::string> App::build_vf_songs_panel(int w, int h) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const int inner = w - 4;
    std::vector<std::string> out;
    int chosen = 0;
    for (const VFEntry& e : vf_all_) if (e.choice >= 0) ++chosen;
    out.push_back(box_top(std::string("SONGS, ") + (vf_filter_ == 0 ? "missing video" : vf_filter_ == 1 ? "all" : vf_filter_ == 2 ? "with video" : "chosen") + " (" + std::to_string(vf_view_.size()) + "), " + std::to_string(chosen) + " chosen" + (vf_focus_ == 1 ? focus_indicator(settings_, border) : std::string()), w, border));
    const int rows = std::max(1, h - 2);
    const int n = static_cast<int>(vf_view_.size());
    const int sel = std::clamp(vf_sel_, 0, std::max(0, n - 1));
    const int first = std::clamp(sel - rows / 2, 0, std::max(0, n - rows));
    for (int r = 0; r < rows; ++r) {
        const int i = first + r;
        if (i >= n) {
            std::string c = (r == 0 && n == 0) ? truncate_str(vf_filter_ == 3 && vf_query_.empty() ? "no video is chosen yet" : vf_all_.empty() ? (vf_filter_ == 0 ? "every song has a video already" : vf_filter_ == 2 ? "no song has a video yet" : "no songs") : "no song matches the filter", inner) : std::string();
            out.push_back(bar + " " + pad_right(c, inner) + " " + bar);
            continue;
        }
        const VFEntry& e = vf_all_[static_cast<size_t>(vf_view_[static_cast<size_t>(i)])];
        // [x] chosen   [?] several videos, a choice is needed   [ ] not searched yet   [~] searching   [-] nothing found
        const char* mark = e.choice >= 0 ? "[x]" : e.searching ? "[~]" : !e.searched ? "[ ]" : e.cands.empty() ? "[-]" : "[?]";
        std::string text = std::string(e.has_video && !e.refetch ? "[v]" : mark) + " " + apply_font_map(e.label, settings_.font_map);   // [v] = has a video (shown in the videos pane)
        // the song under the cursor scrolls as a marquee when it does not fit (like the library list); the other rows are cut
        const std::string padded = i == sel ? marquee_or_truncate(text, inner, i, vf_marq_, vf_marq_since_) : pad_right(truncate_str(text, inner), inner);
        const bool hi = i == sel && vf_focus_ == 1;
        out.push_back(bar + " " + (hi ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + R : (i == sel ? hdr + padded + R : padded)) + " " + bar);
    }
    const int remaining = n - (first + rows);
    out.push_back(box_bottom(w, remaining > 0 ? "(" + std::to_string(remaining) + " more)" : std::string(), border_bottom));
    return out;
}

std::vector<std::string> App::build_vf_choice_panel(int w, int h) const {
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const std::string hdr = header_sgr(settings_);
    const std::string leg = legend_sgr(settings_);
    const int inner = w - 4;
    auto line = [&](const std::string& content, int used) { return bar + " " + content + std::string(static_cast<size_t>(std::max(0, inner - used)), ' ') + " " + bar; };
    std::vector<std::string> out;
    const VFEntry* e = video_fetch_hovered();
    out.push_back(box_top(std::string("VIDEOS FOR THE SONG") + (vf_focus_ == 2 ? focus_indicator(settings_, border) : std::string()), w, border));
    const std::string src_name = vf_src_ == 0 ? "file name + tags" : vf_src_ == 1 ? "file name" : "tags";
    auto text_line = [&](const std::string& t, const std::string& sgr) {
        const std::string c = truncate_str(t, inner);
        return line(sgr + c + R, display_width(c));
    };
    out.push_back(text_line(e ? "Song:    " + apply_font_map(e->label, settings_.font_map) + (e->seconds > 0 ? "  (" + fmt_mmss(e->seconds) + ")" : std::string()) : std::string("(no song)"), hdr));   // (with its length)
    const bool has_tags = e && !e->tags.empty();
    if (has_tags) out.push_back(text_line("Tags:    " + apply_font_map(e->tags, settings_.font_map), leg));   // title / artist / album, when the song has them
    out.push_back(text_line("Search:  " + src_name + "   [CTRL+t] changes it", leg));
    const int hl = has_tags ? 3 : 2;   // the header lines above the list
    const int cand_rows = std::clamp((h - 4 - hl) / 2, 2, kCandRows);
    const int n = e ? static_cast<int>(e->cands.size()) : 0;
    const int csel = std::clamp(vf_cand_sel_, 0, std::max(0, n - 1));
    const int closest = e ? vf_closest(e->cands, e->seconds) : -1;
    const int first = std::clamp(csel - cand_rows / 2, 0, std::max(0, n - cand_rows));
    for (int r = 0; r < cand_rows; ++r) {
        const int i = first + r;
        if (!e || i >= n) {
            std::string msg;
            if (e && n == 0 && e->has_video && !e->refetch) {
                const fs::path cur = path_from_utf8(video_source_for(e->path));
                const std::string lines[] = {"This song has a video already:", "  " + apply_font_map(path_utf8(cur.filename()), settings_.font_map), "", "[r] Re-fetch: search other videos. The current video",
                                             "is unassigned from the song first (asked before)."};
                if (r < 5) msg = lines[r];
            } else if (r == 0 && e && n == 0) msg = e->searching ? "searching the videos ..." : e->queued ? "waiting for the search ..." : e->searched ? "no videos found (is yt-dlp installed, and the computer online?)" : "the search starts in a moment ([s] searches now)";
            const std::string c = truncate_str(msg, inner);
            out.push_back(line(leg + c + R, display_width(c)));
            continue;
        }
        const OnlineResult& c = e->cands[static_cast<size_t>(i)];
        const std::string right = " " + vf_clock(c.duration_sec) + (i == closest ? " \xe2\x89\x88" : "");   // ≈ = the length is closest to the song's
        const std::string left = std::string(i == e->choice ? "\xe2\x97\x8f " : "\xe2\x97\x8b ") + apply_font_map(c.title, settings_.font_map) + "  - " + apply_font_map(c.uploader, settings_.font_map);   // ● chosen  ○ not chosen
        const int rw = display_width(right);
        const std::string padded = pad_right(truncate_str(left, inner - rw), inner - rw) + right;
        const bool hi = i == csel && vf_focus_ == 2;
        out.push_back(bar + " " + (hi ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + R : (i == e->choice ? hdr + padded + R : padded)) + " " + bar);
    }
    {
        std::string state;
        if (e && e->choice >= 0 && static_cast<size_t>(e->choice) < e->cands.size()) state = "chosen: " + apply_font_map(e->cands[static_cast<size_t>(e->choice)].title, settings_.font_map);
        else if (e && e->has_video && !e->refetch) state = "has a video: nothing is searched by itself";
        else if (e && e->cands.size() > 1) state = "several videos: a choice is needed ([ENTER] on the song, then on the video)";
        else state = "no choice yet";
        out.push_back(text_line(state, e && e->choice >= 0 ? hdr : leg));
    }
    // the preview of the video under the cursor: a real picture (Kitty / Sixel), braille dots when the terminal has neither
    const int pic_rows = std::max(2, h - 2 - hl - cand_rows - 1);
    if (e && n > 0) {
        const OnlineResult& c = e->cands[static_cast<size_t>(csel)];
        const int scr_col = vf_art_col_ + 2, scr_row = 3 + 1 + hl + cand_rows + 1;
        bool as_picture = false;
        if (gfx_proto_ != GfxProto::None)
            as_picture = cover_full_picture("https://i.ytimg.com/vi/" + c.video_id + "/mqdefault.jpg", false, inner, pic_rows, scr_col, scr_row);
        const std::vector<std::string> pv = as_picture ? std::vector<std::string>() : video_search_preview(c, inner, pic_rows);
        for (int r = 0; r < pic_rows; ++r) {
            if (as_picture) out.push_back(bar + " " + gfx_hole(inner) + " " + bar);
            else out.push_back(bar + " " + (r < static_cast<int>(pv.size()) ? pv[static_cast<size_t>(r)] : std::string(static_cast<size_t>(inner), ' ')) + R + " " + bar);
        }
    } else {
        for (int r = 0; r < pic_rows; ++r) out.push_back(line("", 0));
    }
    out.push_back(box_bottom(w, "", border_bottom));
    return out;
}

void App::build_vf_screen(std::ostringstream& frame, int W, int target_height) const {
    if (W < 60) W = 60;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const int fixed_rows = 3;
    {
        const int field_w = 26;
        const std::string prefix = " Filter: ";
        const int inner_w = static_cast<int>(prefix.size()) + field_w + 2 + 1;
        std::string field;
        int field_cols = 0;
        const std::string& qtext = vf_tab_ == 0 ? vf_query_ : vm_query_;
        const char* owner = vf_tab_ == 0 ? "vf-search" : "vm-search";
        if (vf_focus_ == 0) {
            EditPaint p = paint_edit_field(qtext, edit_owner_ == owner ? edit_caret_ : qtext.size(),
                                           edit_owner_ == owner ? edit_anchor_ : qtext.size(), field_w,
                                           qtext.empty() ? search_idle_sgr(settings_) : search_sgr(settings_), true);
            field = search_run(settings_, p.s, !qtext.empty());
            field_cols = p.cols;
        } else {
            const std::string plain = truncate_str(qtext.empty() ? std::string(vf_tab_ == 0 ? "(type to filter the songs)" : "(type to filter the videos)") : qtext, field_w);
            field = search_run(settings_, plain, !qtext.empty());
            field_cols = display_width(plain);
        }
        std::string focus_mark;
        int focus_w = 0;
        if (vf_focus_ == 0) { focus_mark = " " + focus_sgr(settings_) + "\xe2\x97\x80" + "\x1b[0m"; focus_w = 2; }
        const std::string line2 = search_idle_sgr(settings_) + prefix + field + "\x1b[0m" + focus_mark;
        const auto strip = menu_tab_strip(W, "FETCH VIDEOS", {"SONGS AND VIDEOS", "VIDEO FILES"}, vf_tab_, border, border,
                                          settings_.box_upper_left, settings_.box_upper_right, settings_.box_vertical,
                                          inner_w, line2, static_cast<int>(prefix.size()) + field_cols + focus_w,
                                          ansi_for(settings_.tab_current_color, false), ansi_for(settings_.tab_other_color, false));
        frame << strip.first << "\n" << strip.second << "\n";
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    int chosen = 0;
    for (const VFEntry& e : vf_all_) if (e.choice >= 0) ++chosen;
    const bool in_text = vf_focus_ == 0 && text_engaged(vf_tab_ == 0 ? "vf-search" : "vm-search");
    std::vector<LegendGroup> groups;
    if (vf_tab_ == 1) {
        groups.push_back({"Move", {in_text ? "[\xe2\x86\x90\xe2\x86\x92] Cursor" : "[\xe2\x86\x90\xe2\x86\x92] Tab", "[TAB] Focus", "[\xe2\x86\x91\xe2\x86\x93] Navi.", "[SHIFT+\xe2\x86\x91\xe2\x86\x93] Page\001Page", "[CTRL+\xe2\x86\x91\xe2\x86\x93] Top/bottom\001Top/bot."}});
        groups.push_back({"Find", {"[/] Filter field", "[CTRL+f] All / coupled / video only / newest first\001Videos"}});
        groups.push_back({"Delete", {"[DEL] Delete this video file (asked first)\001Delete", "[u] Undo last deletion\001Undo", "[CTRL+3 / m] Make an mp3 from a video only file\001mp3", "[ESC] Back/Exit\001Exit"}});
    } else {
    groups.push_back({"Move", {in_text ? "[\xe2\x86\x90\xe2\x86\x92] Cursor" : "[\xe2\x86\x90\xe2\x86\x92] Tab", "[TAB] Focus", "[\xe2\x86\x91\xe2\x86\x93] Navi.", "[SHIFT+\xe2\x86\x91\xe2\x86\x93] Page\001Page", "[CTRL+\xe2\x86\x91\xe2\x86\x93] Top/bottom\001Top/bot.", "[ENTER] Videos / Choose\001Choose"}});
    groups.push_back({"Search", {"[s] This song\001This", "[r] Re-fetch (song with a video)\001Re-fetch", "[SHIFT+s] All shown songs\001All shown", "[CTRL+t] File name + tags / file name / tags\001Source", "[o] Open page"}});
    groups.push_back({"Find", {"[/] Filter field", "[CTRL+f] Missing video / all / with video / chosen\001Songs"}});
    groups.push_back({"Fetch", {"[SHIFT+a] Download only current selected\001Download current", "[SHIFT+f] Download all chosen (" + std::to_string(chosen) + ")\001Download all (" + std::to_string(chosen) + ")", "[c] Clear choice\001Clear"}});
    groups.push_back({"Session", {"[CTRL+c] Cancel download\001Cancel", "[ESC] Back/Exit\001Exit"}});
    }
    const auto lrows = layout_legend_tree(groups, W - 1);   // (known first: the panels take every row that the legend and the status line leave)

    const int budget = std::min(target_height, term_rows_ - 1);
    const int panel_h = std::max(10, budget - fixed_rows - static_cast<int>(lrows.size()) - 1);
    vf_page_rows_ = std::max(1, panel_h - 3);
    const int left_w = W * 2 / 5, right_w = W - left_w;
    vf_art_col_ = left_w;
    const auto left = vf_tab_ == 0 ? build_vf_songs_panel(left_w, panel_h) : build_vm_list_panel(left_w, panel_h);
    const auto right = vf_tab_ == 0 ? build_vf_choice_panel(right_w, panel_h) : build_vm_detail_panel(right_w, panel_h);
    const size_t rows = std::max(left.size(), right.size());
    for (size_t i = 0; i < rows; ++i)
        frame << (i < left.size() ? left[i] : std::string(static_cast<size_t>(left_w), ' '))
              << (i < right.size() ? right[i] : std::string(static_cast<size_t>(right_w), ' ')) << "\n";

    for (size_t i = 0; i < lrows.size(); ++i) frame << " " << paint_legend(settings_, lrows, i, border, 0, true) << "\n";
    std::string status = vf_msg_;
    const bool dl_running = vf_tab_ == 0 && vf_.running;
    if (dl_running) {
        std::lock_guard<std::mutex> lk(vf_.m);
        status = vf_.status;
    }
    if (!status.empty()) frame << (vf_warn_ && !dl_running ? "\x1b[31m" : "\x1b[38;5;246m") << truncate_str(status, W) << "\x1b[0m\n";
    else frame << "\n";
}

}  // namespace muisc
