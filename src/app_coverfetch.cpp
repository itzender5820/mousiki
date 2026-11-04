// Cover fetch overlay (Mode::CoverFetch, HKeyCoverFetch = ALT+c): the albums of the library with their covers, the pictures an album
// can use, and a fetch list that sends album names to MusicBrainz / the Cover Art Archive (scripts/fetch_cover.py).
// Member functions of App (declared in app.h); the helpers shared with app.cpp are in app_internal.h.
//
// Why an overlay of its own and not a tab of the meta editor: MusicBrainz is poor at recognising album titles from loose files,
// so fetching covers is a deliberate step on a list the user has looked at -- not something that runs along while tags are edited.
#include "app.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "path_utf8.h"
#include "console_log.h"
#include "picture_formats.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include "app_internal.h"

namespace muisc {

namespace {

std::string folder_name_of(const fs::path& p) {
    std::string n = path_utf8(p.filename());
    return n.empty() ? path_utf8(p) : n;
}

std::string covers_key(const std::string& folder, const std::string& album) { return folder + "|" + cover_name_key(album); }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------
// open / close / index
// ---------------------------------------------------------------------------------------------------------------------------

void App::cf_open() {
    if (audiobook_) return;
    mode_ = Mode::CoverFetch;
    text_disengage();
    cf_tab_ = 0;
    cf_focus_ = 0;
    cf_filter_ = 0;
    cf_selected_ = cf_art_sel_ = cf_list_sel_ = 0;
    cf_prompt_ = CfPrompt::None;
    cf_status_.clear();
    cf_query_.clear();
    edit_caret_ = edit_anchor_ = 0;
    cf_folders_.clear();
    cf_rows_.clear();
    cf_index_ready_ = false;
    cf_refresh();
}

void App::cf_close() {
    cf_gen_.fetch_add(1);
    if (cf_index_thread_.joinable()) cf_index_thread_.join();
    cf_fetch_cancel_.request();
    if (cf_fetch_thread_.joinable()) cf_fetch_thread_.join();
    cf_fetch_running_ = false;
}

// Searches again: the tracks of the query are read on this thread (cheap, the library is in memory), grouped by folder and album name
// on a worker (tag reads), and the covers of every album are counted after that (the picture inside a file needs ffprobe).
void App::cf_refresh() {
    cf_gen_.fetch_add(1);
    if (cf_index_thread_.joinable()) cf_index_thread_.join();
    std::vector<LocalTrack> tracks = filter_local_or_folders(cf_query_);
    meta_ensure_session_loaded();
    cf_album_override_.clear();   // album tags changed in the meta editor and not saved yet count as what they will be
    for (const auto& e : meta_session_)
        if (e.edited[static_cast<size_t>(MetaField::Album)]) cf_album_override_[e.path] = e.value[static_cast<size_t>(MetaField::Album)];
    const unsigned gen = cf_gen_.load();
    cf_scanning_ = true;
    cf_pending_ready_ = false;
    cf_progress_ = 0;
    cf_total_ = static_cast<int>(tracks.size());
    cf_index_thread_ = std::thread([this, gen, tracks = std::move(tracks), overrides = cf_album_override_]() {
        auto alive = [&]() { return cf_gen_.load() == gen; };
        std::map<std::string, CfFolder> by_folder;
        std::map<std::string, std::map<std::string, int>> album_at;   // folder -> album key -> index
        std::map<std::string, std::map<std::string, int>> artists;   // "folder|album key" -> artist -> number of tracks
        std::map<std::string, int> untagged_in;
        int untagged = 0;
        for (const auto& t : tracks) {
            if (!alive()) return;
            ++cf_progress_;
            const std::string folder = path_utf8(t.path.parent_path());
            RowMeta rm = meta_row_meta(t.path);
            {
                const auto ov = overrides.find(path_utf8(t.path));
                if (ov != overrides.end()) rm.album = ov->second;
            }
            CfFolder& f = by_folder[folder];
            if (f.path.empty()) { f.path = folder; f.name = folder_name_of(t.path.parent_path()); }
            if (rm.album.empty()) { ++untagged; ++untagged_in[folder]; continue; }
            const std::string key = cover_name_key(rm.album);
            if (key.empty()) { ++untagged; ++untagged_in[folder]; continue; }
            auto& at = album_at[folder];
            auto it = at.find(key);
            if (it == at.end()) {
                it = at.emplace(key, static_cast<int>(f.albums.size())).first;
                CfAlbum a;
                a.name = rm.album;
                f.albums.push_back(std::move(a));
            }
            f.albums[static_cast<size_t>(it->second)].tracks.push_back(path_utf8(t.path));
            if (!rm.artist.empty()) ++artists[covers_key(folder, rm.album)][rm.artist];
        }
        std::vector<CfFolder> folders;
        for (auto& kv : by_folder) {
            CfFolder f = std::move(kv.second);
            if (f.albums.empty()) continue;
            for (auto& a : f.albums) {
                const auto& as = artists[covers_key(f.path, a.name)];
                int best = 0;   // the artist most of the tracks have (the tags are right already: a compilation is still asked for as the album it is tagged as)
                for (const auto& kv : as) if (kv.second > best) { best = kv.second; a.artist = kv.first; }
            }
            std::sort(f.albums.begin(), f.albums.end(), [](const CfAlbum& x, const CfAlbum& y) { return cover_name_key(x.name) < cover_name_key(y.name); });
            f.only_album = f.albums.size() == 1 && untagged_in[f.path] == 0;
            folders.push_back(std::move(f));
        }
        if (!alive()) return;
        {
            std::lock_guard<std::mutex> lk(cf_mutex_);
            cf_pending_folders_ = folders;
            cf_untagged_ = untagged;
            cf_covers_.clear();
        }
        cf_pending_ready_ = true;
        // phase B: how many covers every album has
        cf_progress_ = 0;
        int total = 0;
        for (const auto& f : folders) total += static_cast<int>(f.albums.size());
        cf_total_ = total;
        for (const auto& f : folders) {
            for (const auto& a : f.albums) {
                if (!alive()) return;
                AlbumCovers ac = album_covers(path_from_utf8(f.path), a.name, path_from_utf8(a.tracks.front()), f.only_album, true);
                {
                    std::lock_guard<std::mutex> lk(cf_mutex_);
                    cf_covers_[covers_key(f.path, a.name)] = ac.count();
                }
                ++cf_progress_;
            }
        }
        cf_scanning_ = false;
    });
}

int App::cf_covers_of(const CfFolder& f, const CfAlbum& a) const {
    std::lock_guard<std::mutex> lk(cf_mutex_);
    auto it = cf_covers_.find(covers_key(f.path, a.name));
    return it == cf_covers_.end() ? -1 : it->second;
}

bool App::cf_album_shown(const CfFolder& f, const CfAlbum& a) const {
    const bool failed = !cover_fetch_failure(a.name).empty();
    if (cf_filter_ == 4) return failed;
    if (cf_filter_ == 5) return !failed && cover_recently_fetched(a.name);
    if (failed) return false;   // asked for before without success: only the filter [p] lists it
    if (cf_filter_ == 0) return true;
    const int n = cf_covers_of(f, a);
    return n >= 0 && ((cf_filter_ == 1 && n == 0) || (cf_filter_ == 2 && n == 1) || (cf_filter_ == 3 && n >= 2));
}

// The rows hold indexes into cf_folders_, so the highlighted folder / album is remembered BY NAME before that vector is replaced
// (a new index after a search, a deleted picture ...); otherwise the old indexes would be read against the new folders.
void App::cf_remember_selection() {
    cf_keep_folder_.clear();
    cf_keep_album_.clear();
    if (cf_selected_ >= 0 && cf_selected_ < static_cast<int>(cf_rows_.size())) {
        const CfRow& r = cf_rows_[static_cast<size_t>(cf_selected_)];
        if (r.folder >= 0 && r.folder < static_cast<int>(cf_folders_.size())) {
            const CfFolder& f = cf_folders_[static_cast<size_t>(r.folder)];
            cf_keep_folder_ = f.path;
            if (r.album >= 0 && r.album < static_cast<int>(f.albums.size())) cf_keep_album_ = f.albums[static_cast<size_t>(r.album)].name;
        }
    }
}

void App::cf_rebuild_rows() {
    if (!cf_keep_ready_) cf_remember_selection();
    cf_keep_ready_ = false;
    const std::string keep_folder = cf_keep_folder_, keep_album = cf_keep_album_;
    cf_rows_.clear();
    const bool open_all = cf_filter_ != 0;
    for (size_t fi = 0; fi < cf_folders_.size(); ++fi) {
        const CfFolder& f = cf_folders_[fi];
        std::vector<int> shown;
        for (size_t ai = 0; ai < f.albums.size(); ++ai) {
            if (cf_album_shown(f, f.albums[ai])) shown.push_back(static_cast<int>(ai));
        }
        if (shown.empty()) continue;   // (a folder whose albums are all hidden is not listed)
        cf_rows_.push_back({static_cast<int>(fi), -1});
        if (open_all || cf_open_.count(f.path)) {
            for (int ai : shown) cf_rows_.push_back({static_cast<int>(fi), ai});
        }
    }
    cf_selected_ = 0;
    for (size_t i = 0; i < cf_rows_.size(); ++i) {
        const CfRow& r = cf_rows_[i];
        const CfFolder& f = cf_folders_[static_cast<size_t>(r.folder)];
        if (f.path == keep_folder && ((r.album < 0 && keep_album.empty()) || (r.album >= 0 && f.albums[static_cast<size_t>(r.album)].name == keep_album))) {
            cf_selected_ = static_cast<int>(i);
            break;
        }
    }
    cf_art_sel_ = 0;
    cf_cur_key_.clear();
}

// Takes what the index worker produced; ends a finished fetch.
void App::poll_cover_fetch() {
    if (cf_pending_ready_.exchange(false)) {
        {
            std::lock_guard<std::mutex> lk(cf_mutex_);
            cf_remember_selection();
            cf_keep_ready_ = true;
            cf_folders_ = cf_pending_folders_;
        }
        cf_index_ready_ = true;
        cf_rebuild_rows();
    }
    {   // the counts came in: a filter on counts is applied again once
        static bool was_scanning = false;
        const bool now = cf_scanning_.load();
        if (was_scanning && !now && cf_index_ready_ && cf_filter_ != 0) cf_rebuild_rows();
        was_scanning = now;
    }
    if (cf_fetch_thread_.joinable() && !cf_fetch_running_.load()) {
        cf_fetch_thread_.join();
        int ok = 0, bad = 0;
        {
            std::lock_guard<std::mutex> lk(cf_mutex_);
            for (const auto& it : cf_list_) { if (it.done && it.ok) ++ok; else if (it.done) ++bad; }
        }
        cf_status_ = "fetch finished: " + std::to_string(ok) + " cover" + (ok == 1 ? "" : "s") + " saved" + (bad ? ", " + std::to_string(bad) + " not found" : std::string());
        cover_store_touch();
        cf_cur_key_.clear();
        if (mode_ == Mode::CoverFetch) cf_refresh();
    }
}

const App::CfAlbum* App::cf_selected_album(const CfFolder** folder) const {
    if (cf_selected_ < 0 || cf_selected_ >= static_cast<int>(cf_rows_.size())) return nullptr;
    const CfRow& r = cf_rows_[static_cast<size_t>(cf_selected_)];
    if (r.album < 0) return nullptr;
    const CfFolder& f = cf_folders_[static_cast<size_t>(r.folder)];
    if (folder) *folder = &f;
    return &f.albums[static_cast<size_t>(r.album)];
}

// The cover the highlighted album has now and the pictures it can choose from.
void App::cf_update_current() const {
    const CfFolder* f = nullptr;
    const CfAlbum* a = cf_selected_album(&f);
    if (!a) { cf_cur_key_ = "-"; cf_cands_.clear(); cf_cur_src_ = CoverSource(); cf_cur_fallback_ = false; return; }
    const std::string key = f->path + "|" + a->name + "|" + std::to_string(cover_store_revision()) + "|" + std::to_string(settings_.cover_search);
    if (key == cf_cur_key_) return;
    cf_cur_key_ = key;
    CoverRequest rq;
    rq.path = a->tracks.front();
    rq.title = path_utf8(path_from_utf8(rq.path).stem());
    rq.album = a->name;
    rq.search = settings_.cover_search;
    rq.store_rev = cover_store_revision();
    cf_cur_src_ = resolve_cover_source(rq);
    cf_cands_.clear();
    AlbumCovers ac = album_covers(path_from_utf8(f->path), a->name, path_from_utf8(a->tracks.front()), f->only_album, true);
    if (ac.embedded) {
        CoverCandidate c;
        c.kind = CoverCandidate::Embedded;
        c.file = path_from_utf8(a->tracks.front());
        c.label = "inside the file";
        cf_cands_.push_back(c);
    }
    for (const auto& p : ac.files) {
        CoverCandidate c;
        c.file = p;
        const bool fetched = path_utf8(p.parent_path().filename()) == kFetchedCoversDir;
        c.kind = fetched ? CoverCandidate::Fetched : CoverCandidate::Folder;
        c.label = (fetched ? std::string(kFetchedCoversDir) + "/" : std::string()) + path_utf8(p.filename());
        cf_cands_.push_back(c);
    }
    // a picture the playback search takes from the folder by a loose name match ("Something Real.jpg" for the album "4REAL") or as a
    // last resort is not this album's cover: the library counts "no cover" and the pane says so (it names the picture the player would use)
    cf_cur_fallback_ = false;
    if (!cf_cur_src_.none && !cf_cur_src_.chosen && !cf_cur_src_.embedded && !cf_cur_src_.file.empty()) {
        bool own = false;
        for (const auto& p : ac.files) if (path_utf8(p) == cf_cur_src_.file) { own = true; break; }
        cf_cur_fallback_ = !own;
    }
    // the picture the album really has now (a chosen one) is always on the list
    if (!cf_cur_fallback_ && !cf_cur_src_.none && !cf_cur_src_.file.empty()) {
        bool listed = false;
        for (const auto& c : cf_cands_) {
            if (cf_cur_src_.embedded ? c.kind == CoverCandidate::Embedded : (c.kind != CoverCandidate::Embedded && path_utf8(c.file) == cf_cur_src_.file)) { listed = true; break; }
        }
        if (!listed) {
            CoverCandidate c;
            c.file = path_from_utf8(cf_cur_src_.file);
            if (cf_cur_src_.embedded) { c.kind = CoverCandidate::Embedded; c.label = "inside the file"; }
            else {
                const bool fetched = path_utf8(c.file.parent_path().filename()) == kFetchedCoversDir;
                c.kind = fetched ? CoverCandidate::Fetched : CoverCandidate::Folder;
                c.label = (fetched ? std::string(kFetchedCoversDir) + "/" : std::string()) + path_utf8(c.file.filename());
            }
            cf_cands_.push_back(c);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// the fetch list and the fetch itself
// ---------------------------------------------------------------------------------------------------------------------------

void App::cf_add_album(const CfFolder& f, const CfAlbum& a) {
    const std::string key = cover_name_key(a.name);
    std::lock_guard<std::mutex> lk(cf_mutex_);
    for (auto& it : cf_list_) {
        if (cover_name_key(it.album) != key) continue;
        if (std::find(it.folders.begin(), it.folders.end(), f.path) == it.folders.end()) {
            it.folders.push_back(f.path);
            it.done = false;
            it.state.clear();
        }
        return;
    }
    CfFetchItem it;
    it.album = a.name;
    it.artist = a.artist;
    it.folders.push_back(f.path);
    it.tracks = a.tracks;
    cf_list_.push_back(std::move(it));
}

void App::cf_add_selected() {
    if (cf_selected_ < 0 || cf_selected_ >= static_cast<int>(cf_rows_.size())) { cf_status_ = "nothing selected"; return; }
    const CfRow& r = cf_rows_[static_cast<size_t>(cf_selected_)];
    const CfFolder& f = cf_folders_[static_cast<size_t>(r.folder)];
    const size_t before = cf_list_.size();
    if (r.album >= 0) cf_add_album(f, f.albums[static_cast<size_t>(r.album)]);
    else for (const auto& a : f.albums) cf_add_album(f, a);
    cf_status_ = std::to_string(cf_list_.size() - before) + " album" + (cf_list_.size() - before == 1 ? "" : "s") + " added to the fetch list (" + std::to_string(cf_list_.size()) + " in all)";
}

void App::cf_add_view() {
    const size_t before = cf_list_.size();
    std::set<int> seen_folders;
    for (const auto& r : cf_rows_) {
        const CfFolder& f = cf_folders_[static_cast<size_t>(r.folder)];
        if (r.album >= 0) { cf_add_album(f, f.albums[static_cast<size_t>(r.album)]); seen_folders.insert(r.folder); }
    }
    // folders that are closed: all their albums (as far as the filter keeps them)
    for (size_t fi = 0; fi < cf_folders_.size(); ++fi) {
        if (seen_folders.count(static_cast<int>(fi))) continue;
        bool listed = false;
        for (const auto& r : cf_rows_) if (r.folder == static_cast<int>(fi)) { listed = true; break; }
        if (!listed) continue;
        for (const auto& a : cf_folders_[fi].albums) {
            if (cf_album_shown(cf_folders_[fi], a)) cf_add_album(cf_folders_[fi], a);
        }
    }
    cf_status_ = std::to_string(cf_list_.size() - before) + " album" + (cf_list_.size() - before == 1 ? "" : "s") + " added to the fetch list (" + std::to_string(cf_list_.size()) + " in all)";
}

void App::cf_start_fetch() {
    if (cf_fetch_running_.load()) { cf_status_ = "a fetch is already running"; return; }
    if (cf_fetch_thread_.joinable()) cf_fetch_thread_.join();
    if (cf_script_.empty() || !fs::exists(cf_script_)) { cf_status_ = "scripts/fetch_cover.py was not found"; return; }
    size_t todo = 0;
    for (const auto& it : cf_list_) if (!it.done || !it.ok) ++todo;
    if (!todo) { cf_status_ = "nothing to fetch -- the list is empty or everything on it is done"; return; }
    cf_fetch_cancel_.reset();
    cf_fetch_running_ = true;
    for (auto& it : cf_list_) if (!it.done || !it.ok) { it.done = false; it.state = "waiting"; }
    const fs::path script = cf_script_;
    cf_fetch_thread_ = std::thread([this, script]() {
        const size_t n = [&]() { std::lock_guard<std::mutex> lk(cf_mutex_); return cf_list_.size(); }();
        std::vector<char> retry(n, 0);   // albums that failed for a temporary reason (HTTP 500 / 503 ..., no connection): tried again later
        auto temporary = [](const std::string& st) {
            return st.find("HTTP") != std::string::npos || st.find("not reachable") != std::string::npos || st.find("unreadable") != std::string::npos;
        };
        constexpr int kRounds = 3;   // after the first pass up to three more, each after a longer pause
        for (int round = 0; round <= kRounds && !cf_fetch_cancel_.requested(); ++round) {
            for (size_t i = 0; i < n; ++i) {
                if (cf_fetch_cancel_.requested()) break;
                CfFetchItem it;
                {
                    std::lock_guard<std::mutex> lk(cf_mutex_);
                    if (i >= cf_list_.size()) break;
                    if (round == 0 ? cf_list_[i].done : !retry[i]) continue;
                    it = cf_list_[i];
                    cf_list_[i].state = round == 0 ? "fetching ..." : "fetching again ...";
                    cf_fetch_progress_ = "fetching covers " + std::to_string(i + 1) + "/" + std::to_string(n) + (round ? " (second try)" : "") + " ... (SHIFT+c cancels)";
                }
                retry[i] = 0;
                std::string state, matched;
                bool ok = false;
                // already fetched into one of the folders: copied to the others without asking MusicBrainz again
                fs::path have;
                for (const auto& f : it.folders) {
                    fs::path p = find_fetched_cover(path_from_utf8(f), it.album);
                    if (!p.empty()) { have = p; break; }
                }
                if (have.empty()) {
                    std::vector<std::string> titles;   // up to three song titles of the album, as the tags have them
                    for (const auto& tp : it.tracks) {
                        if (titles.size() >= 3) break;
                        const std::string t = meta_row_meta(path_from_utf8(tp)).title;
                        if (!t.empty()) titles.push_back(t);
                    }
                    CoverFetchResult res = fetch_album_cover(script, it.album, it.artist, fetched_cover_stem(path_from_utf8(it.folders.front()), it.album), &cf_fetch_cancel_, titles);
                    if (res.ok) {
                        have = path_from_utf8(res.file);
                        state = "saved (" + res.release + ")";
                        matched = res.release + (res.artist.empty() ? std::string() : " - " + res.artist) + (res.confirmed ? " [songs confirmed]" : "");
                    }
                    else if (cf_fetch_cancel_.requested()) state = "cancelled";
                    else state = res.python_missing ? "Python was not found" : res.script_missing ? "script not found" : res.error.empty() ? "not found" : res.error;
                } else {
                    state = "already fetched";
                }
                if (!have.empty()) {
                    ok = true;
                    int copies = 0;
                    for (const auto& f : it.folders) {
                        if (!find_fetched_cover(path_from_utf8(f), it.album).empty()) continue;
                        std::error_code ec;
                        const fs::path dir = path_from_utf8(f) / kFetchedCoversDir;
                        fs::create_directories(dir, ec);
                        fs::copy_file(have, dir / (cover_file_name(it.album) + have.extension().string()), fs::copy_options::overwrite_existing, ec);
                        if (!ec) ++copies;
                    }
                    if (copies) state += ", copied to " + std::to_string(copies) + " more folder" + (copies == 1 ? "" : "s");
                }
                const bool again = !ok && state != "cancelled" && temporary(state) && round < kRounds;
                {
                    std::lock_guard<std::mutex> lk(cf_mutex_);
                    if (i < cf_list_.size()) {
                        cf_list_[i].state = again ? state + " -- will try again" : state;
                        cf_list_[i].done = state != "cancelled" && !again;
                        cf_list_[i].ok = ok;
                    }
                }
                if (again) retry[i] = 1;
                // remembered: failures MusicBrainz really answered (not the temporary ones) stay out of the list; successes get the time and what was matched
                if (ok) { set_cover_fetch_failure(it.album, std::string()); if (!matched.empty()) mark_cover_fetched(it.album, matched); }
                else if (state != "cancelled" && !temporary(state) && state.find("Python") == std::string::npos && state.find("script") == std::string::npos)
                    set_cover_fetch_failure(it.album, state);
                cover_store_touch();
            }
            size_t waiting = 0;
            for (char r : retry) waiting += r ? 1 : 0;
            if (!waiting || round == kRounds) break;
            // the servers are busy (HTTP 500 / 503 mean "try again later"): wait a while, then ask for those albums once more
            const int pause = 45 * (round + 1);
            for (int sec = pause; sec > 0 && !cf_fetch_cancel_.requested(); --sec) {
                {
                    std::lock_guard<std::mutex> lk(cf_mutex_);
                    cf_fetch_progress_ = "the server was busy for " + std::to_string(waiting) + " album" + (waiting == 1 ? "" : "s") + ": trying again in " + std::to_string(sec) + " s ... (SHIFT+c cancels)";
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        cf_fetch_running_ = false;
    });
    cf_status_ = "fetching the front covers of " + std::to_string(todo) + " album" + (todo == 1 ? "" : "s") + " ...";
}

// SHIFT+d: the album tag of every track of the highlighted album is set to empty -- as pending edits of the meta editor (nothing is
// written to the files until CTRL+s there; CTRL+SHIFT+x discards). The album then leaves this list (it counts as untagged).
void App::cf_clear_album_title() {
    const CfFolder* f = nullptr;
    const CfAlbum* a = cf_selected_album(&f);
    if (!a) { cf_status_ = "select an album first"; return; }
    for (const auto& t : a->tracks) meta_set_field(t, static_cast<int>(MetaField::Album), std::string());
    cf_status_ = "album title removed from " + std::to_string(a->tracks.size()) + " track" + (a->tracks.size() == 1 ? "" : "s") + " (pending: save it in the meta editor with CTRL+s)";
    cf_cur_key_.clear();
    cf_refresh();
}

void App::cf_delete_candidate() {
    cf_update_current();
    if (cf_art_sel_ < 0 || cf_art_sel_ >= static_cast<int>(cf_cands_.size())) return;
    std::string why;
    if (!delete_cover_candidate(cf_cands_[static_cast<size_t>(cf_art_sel_)], &why)) { cf_status_ = why.empty() ? "cannot be deleted" : why; return; }
    cf_status_ = "picture deleted";
    cf_cur_key_.clear();
    cf_art_sel_ = 0;
    cf_refresh();
}

int App::cf_nav(int selected, int total, bool up) const {
    if (total <= 0) return 0;
    int to;
    if (last_key_was_ctrl()) to = up ? 0 : total - 1;
    else if (last_key_was_shifted()) to = selected + (up ? -1 : 1) * std::max(1, cf_page_rows_);
    else to = selected + (up ? -1 : 1);
    return std::clamp(to, 0, total - 1);
}

// ---------------------------------------------------------------------------------------------------------------------------
// keys
// ---------------------------------------------------------------------------------------------------------------------------

void App::handle_cf_key(int key) {
    if (cf_prompt_ != CfPrompt::None) {
        const bool yes = key == 'y' || key == 'Y';
        const bool no = key == 'n' || key == 'N' || key == 27;
        if (yes) {
            const CfPrompt p = cf_prompt_;
            cf_prompt_ = CfPrompt::None;
            if (p == CfPrompt::Fetch) cf_start_fetch();
            else if (p == CfPrompt::Delete) cf_delete_candidate();
            else if (p == CfPrompt::ClearAlbum) cf_clear_album_title();
            else if (p == CfPrompt::ClearList) {
                std::lock_guard<std::mutex> lk(cf_mutex_);
                cf_list_.clear();
                cf_list_sel_ = 0;
                cf_status_ = "fetch list emptied";
            }
        } else if (no) {
            cf_prompt_ = CfPrompt::None;
        }
        return;
    }

    const bool arrow = last_key_was_arrow();
    const bool in_text = cf_tab_ == 0 && cf_focus_ == 0 && text_engaged("cf-search");

    auto close_menu = [&]() {
        cf_gen_.fetch_add(1);                       // the index worker stops; a running fetch goes on in the background
        if (cf_index_thread_.joinable()) cf_index_thread_.join();
        cf_scanning_ = false;
        text_disengage();
        mode_ = Mode::Browse;
    };
    // The key opens the menu and closes it again (ALT+c also from the search field; a plain letter such as the macOS default
    // SHIFT+q is text there, ESC leaves the field first).
    if (key_is("HKeyCoverFetch", key) && !(in_text && key >= 32 && key < 127)) { close_menu(); return; }
    // SHIFT+r: read the folders again (new, removed or changed files / albums)
    if (key_is("HKeyRescan", key) && !in_text && (cf_tab_ == 1 || cf_focus_ != 0)) {
        if (cf_fetch_running_.load()) { cf_status_ = "a fetch is running: rescan after it"; return; }
        rescan_library();
        cf_refresh();
        cf_status_ = "library rescanned: " + std::to_string(all_local_tracks_.size()) + " tracks";
        return;
    }

    if (key == 27) {
        if (cf_tab_ == 0 && cf_focus_ == 2) { cf_focus_ = 1; return; }
        if (cf_tab_ == 0 && cf_focus_ == 1 && cf_filter_ != 0) { cf_filter_ = 0; cf_rebuild_rows(); return; }
        if (cf_tab_ == 0 && cf_focus_ == 1) { cf_focus_ = 0; return; }
        if (cf_tab_ == 0 && cf_focus_ == 0 && !cf_query_.empty()) {
            cf_query_.clear();
            edit_caret_ = edit_anchor_ = 0;
            text_disengage();
            cf_refresh();
            return;
        }
        if (in_text) { text_disengage(); return; }
        close_menu();
        return;
    }
    if (key == kKeyAltLeft || key == kKeyAltRight || (arrow && (key == 'C' || key == 'D') && !in_text && cf_focus_ != 2)) {
        cf_tab_ = 1 - cf_tab_;
        text_disengage();
        return;
    }
    if (key == 9) {
        if (cf_tab_ == 0) cf_focus_ = (cf_focus_ + 1) % 3;
        text_disengage();
        return;
    }
    if (key == 'C' && !arrow && cf_tab_ == 1) {
        if (cf_fetch_running_.load()) { cf_fetch_cancel_.request(); cf_status_ = "stopping the fetch ..."; }
        else cf_status_ = "no fetch is running";
        return;
    }

    if (cf_tab_ == 1) {   // --- fetch list ---
        int total = 0;
        { std::lock_guard<std::mutex> lk(cf_mutex_); total = static_cast<int>(cf_list_.size()); }
        if (arrow && (key == 'A' || key == 'B')) { cf_list_sel_ = cf_nav(cf_list_sel_, total, key == 'A'); return; }
        if (key == '\r' || key == '\n') {
            if (total == 0) { cf_status_ = "the fetch list is empty -- add albums with [a] on the ALBUM LIBRARY tab"; return; }
            cf_prompt_ = CfPrompt::Fetch;
            return;
        }
        if ((key == kKeyDelete && last_key_was_shifted()) || (key == 'D' && !arrow)) {
            if (total == 0) { cf_status_ = "the fetch list is empty"; return; }
            if (cf_fetch_running_.load()) { cf_status_ = "a fetch is running: stop it first with SHIFT+c"; return; }
            cf_prompt_ = CfPrompt::ClearList;
            return;
        }
        if (key == kKeyDelete || key == 127 || key == 'd') {
            if (cf_fetch_running_.load()) { cf_status_ = "a fetch is running: stop it first with SHIFT+c"; return; }
            std::lock_guard<std::mutex> lk(cf_mutex_);
            if (cf_list_sel_ >= 0 && cf_list_sel_ < static_cast<int>(cf_list_.size())) {
                cf_list_.erase(cf_list_.begin() + cf_list_sel_);
                cf_list_sel_ = std::clamp(cf_list_sel_, 0, std::max(0, static_cast<int>(cf_list_.size()) - 1));
            }
        }
        return;
    }

    if (key == 'D' && !arrow && cf_focus_ != 0) {   // SHIFT+d: remove the album title (it is wrong or should be set again in the meta editor)
        if (!cf_selected_album()) { cf_status_ = "select an album (not a folder) first"; return; }
        cf_prompt_ = CfPrompt::ClearAlbum;
        return;
    }
    if (key == 't' && !arrow && cf_focus_ != 0) {   // [t] the cover as ASCII (braille dots) / in full resolution
        if (gfx_proto_ == GfxProto::None) { cf_status_ = "this terminal shows no pictures (no Kitty / Sixel graphics): the cover stays ASCII"; return; }
        cf_full_res_ = !cf_full_res_;
        cf_pic_key_.clear();
        cf_status_ = cf_full_res_ ? "cover: full resolution" : "cover: ASCII";
        return;
    }

    if (cf_focus_ == 0) {   // --- search bar ---
        if (arrow && (key == 'A' || key == 'B')) { cf_selected_ = cf_nav(cf_selected_, static_cast<int>(cf_rows_.size()), key == 'A'); cf_art_sel_ = 0; return; }
        if (key == '\r' || key == '\n') { text_disengage(); cf_focus_ = 1; return; }
        edit_focus("cf-search", cf_query_);
        text_engage("cf-search");
        if (edit_text_key(cf_query_, edit_caret_, edit_anchor_, key, 120, &cf_status_)) {
            cf_selected_ = 0;
            cf_refresh();
        }
        return;
    }

    if (cf_focus_ == 1) {   // --- album library ---
        const int total = static_cast<int>(cf_rows_.size());
        if (arrow && (key == 'A' || key == 'B')) { cf_selected_ = cf_nav(cf_selected_, total, key == 'A'); cf_art_sel_ = 0; return; }
        if (key == '\r' || key == '\n' || key == ' ') {
            if (total == 0) return;
            const CfRow& r = cf_rows_[static_cast<size_t>(cf_selected_)];
            if (r.album < 0) {
                const std::string& p = cf_folders_[static_cast<size_t>(r.folder)].path;
                if (cf_open_.count(p)) cf_open_.erase(p); else cf_open_.insert(p);
                cf_rebuild_rows();
            } else {
                cf_focus_ = 2;
                cf_art_sel_ = 0;
            }
            return;
        }
        if (key == 'a') { cf_add_selected(); return; }
        if (key == 'F' && !arrow) { cf_add_view(); return; }
        if (key == 'x' || key == 'o' || key == 's' || key == 'p' || key == 'r') {
            const int f = key == 'x' ? 1 : key == 'o' ? 2 : key == 's' ? 3 : key == 'p' ? 4 : 5;
            cf_filter_ = cf_filter_ == f ? 0 : f;
            cf_rebuild_rows();
            if (cf_scanning_.load() && cf_filter_ != 0 && cf_filter_ < 4) cf_status_ = "the covers are still being counted ...";
            return;
        }
        return;
    }

    // --- the pictures of the highlighted album ---
    cf_update_current();
    const CfFolder* f = nullptr;
    const CfAlbum* a = cf_selected_album(&f);
    if (!a) { cf_focus_ = 1; return; }
    const int total = static_cast<int>(cf_cands_.size());
    if (arrow && (key == 'A' || key == 'B')) { cf_art_sel_ = cf_nav(cf_art_sel_, total, key == 'A'); return; }
    if ((key == '\r' || key == '\n') && total > 0) {
        const CoverCandidate& c = cf_cands_[static_cast<size_t>(std::clamp(cf_art_sel_, 0, total - 1))];
        set_cover_album_choice(a->name, c.kind == CoverCandidate::Embedded ? std::string(kCoverEmbedded) : path_utf8(c.file));
        cover_store_touch();
        cf_cur_key_.clear();
        cf_status_ = "\"" + c.label + "\" is the cover of the album now (a choice for a single track in the meta editor still goes first)";
        return;
    }
    if (key == 'n' && !arrow) {
        set_cover_album_choice(a->name, kCoverNone);
        cover_store_touch();
        cf_cur_key_.clear();
        cf_status_ = "this album shows no cover now";
        return;
    }
    if (key == 'c' && !arrow) {
        set_cover_album_choice(a->name, "");
        cover_store_touch();
        cf_cur_key_.clear();
        cf_status_ = "the choice for this album is cleared: Settings -> Cover Search decides again";
        return;
    }
    if (key == kKeyDelete || key == 127 || key == 'd') {
        if (total == 0) return;
        const CoverCandidate& c = cf_cands_[static_cast<size_t>(std::clamp(cf_art_sel_, 0, total - 1))];
        if (!cover_deletable(c)) { cf_status_ = "the picture inside a file cannot be deleted here"; return; }
        cf_prompt_ = CfPrompt::Delete;
        return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------------
// screen
// ---------------------------------------------------------------------------------------------------------------------------

std::vector<std::string> App::build_cf_library_panel(int total_width, int height) const {
    const int inner = total_width - 4;
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    std::string label = "ALBUM LIBRARY";
    if (cf_filter_ != 0) {
        const int room = total_width - 5 - display_width(label) - 2;
        const char* names[] = {"", "NO COVER", "ONE COVER", "SEVERAL COVERS", "PREVIOUS UNSUCCESSFUL FETCH", "RECENTLY FETCHED"};
        if (room >= 4) label += truncate_str(std::string(" [") + names[cf_filter_] + "]", room);
    }
    if (cf_focus_ == 1) label += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(label, total_width, border_ansi));

    const int total = static_cast<int>(cf_rows_.size());
    const std::string list_ansi = ansi_for(settings_.list_color, false);
    if (total == 0) {
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == height / 2) {
                const std::string plain = cf_scanning_.load() && !cf_index_ready_ ? "READING THE TAGS ..."
                                        : cf_filter_ != 0 ? "NO ALBUM MATCHES THE FILTER"
                                        : "NO ALBUMS FOUND";
                const std::string text = apply_font_map(plain, settings_.font_map);
                content = std::string(static_cast<size_t>(std::max(0, (inner - display_width(text)) / 2)), ' ') + text;
            }
            out.push_back(bar + " " + list_ansi + pad_right(truncate_str(content, inner), inner) + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    const int scroll = std::clamp(cf_selected_ - height / 2, 0, std::max(0, total - height));
    for (int row = 0; row < height; ++row) {
        const int idx = scroll + row;
        std::string content;
        bool missing = false;
        if (idx < total) {
            const CfRow& r = cf_rows_[static_cast<size_t>(idx)];
            const CfFolder& f = cf_folders_[static_cast<size_t>(r.folder)];
            if (r.album < 0) {
                const bool open = cf_open_.count(f.path) || cf_filter_ != 0;
                int shown = 0;
                for (const auto& al : f.albums) if (cf_album_shown(f, al)) ++shown;
                content = std::string(open ? "\xe2\x96\xbe " : "\xe2\x96\xb8 ") + f.name + "  (" + std::to_string(shown) + " album" + (shown == 1 ? "" : "s") + ")";
            } else {
                const CfAlbum& a = f.albums[static_cast<size_t>(r.album)];
                const int n = cf_covers_of(f, a);
                missing = n == 0 && cf_filter_ != 4 && cf_filter_ != 5;
                // (RECENTLY FETCHED and PREVIOUS UNSUCCESSFUL FETCH show nothing beside the album: the list stays tidy -- the cover itself is in the picture pane)
                const std::string cnt = (cf_filter_ == 4 || cf_filter_ == 5) ? std::string() : n < 0 ? "..." : n == 0 ? "no cover" : n == 1 ? "1 cover" : std::to_string(n) + " covers";
                const int right_w = cnt.empty() ? 0 : static_cast<int>(cnt.size()) + 1;
                std::string name = a.name + (a.artist.empty() ? std::string() : "  -  " + a.artist);
                const int name_w = std::max(4, inner - 4 - right_w);
                const std::string shown_name = idx == cf_selected_ ? marquee_or_truncate(apply_font_map(name, settings_.font_map), name_w, idx, cf_marq_idx_, cf_marq_since_)   // (the hovered album scrolls when too long)
                                                                   : pad_right(truncate_str(apply_font_map(name, settings_.font_map), name_w), name_w);
                content = "    " + shown_name + (cnt.empty() ? std::string() : " " + cnt);
            }
        }
        const bool sel = idx == cf_selected_;
        const std::string padded = pad_right(truncate_str(content, inner), inner);
        std::string base = sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                               : list_ansi + bg_ansi_for(settings_.list_inactive_bg_color);
        if (missing) out.push_back(bar + " " + base + legend_sgr(settings_) + padded + "\x1b[0m" + base + "\x1b[0m " + bar);   // missing covers stand out
        else out.push_back(bar + " " + base + padded + "\x1b[0m " + bar);
    }
    std::string footer;
    const int remaining = total - (scroll + height);
    if (remaining > 0) footer = "(" + std::to_string(remaining) + " more)";
    if (cf_scanning_.load()) footer += std::string(footer.empty() ? "" : " ") + "(counting covers " + std::to_string(cf_progress_.load()) + "/" + std::to_string(cf_total_.load()) + ")";
    else if (cf_untagged_ > 0) footer += std::string(footer.empty() ? "" : " ") + "(" + std::to_string(cf_untagged_) + " tracks excluded: no album tag)";
    out.push_back(box_bottom(total_width, truncate_str(footer, std::max(4, total_width - 6)), border_ansi_bottom));   // (cut with "..." so it can never break the border)
    return out;
}

// A picture file as a real picture (Kitty / Sixel) in the main slot gfx_, `cols` x `rows` cells at screen column / row (0-based).
// false: the picture is not read yet or cannot be read (the caller shows braille dots / a message instead).
bool App::cover_full_picture(const std::string& pfile, bool pemb, int pcols, int preview_rows, int scr_col, int scr_row, bool second) const {
    return cover_full_picture_img(preview_cover(pfile, pemb), pcols, preview_rows, scr_col, scr_row, second);
}

bool App::cover_full_picture_img(const CoverImage* img, int pcols, int preview_rows, int scr_col, int scr_row, bool second) const {
    if (!img) return false;
    int cw0 = 10, ch0 = 20;
    gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
    cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
    const int cws = std::min(cw0, std::max(2, 640 / pcols)), chs = std::min(ch0, std::max(4, 640 / preview_rows));
    const int w = pcols * cws, h = preview_rows * chs;
    const std::string key = img->key + "|" + std::to_string(w) + "x" + std::to_string(h) + "|" + std::to_string(scr_col);
    GfxFrame& g = second ? gfx2_ : gfx_;
    std::string& pic_key = second ? vs_pic_key_ : cf_pic_key_;
    if (key != pic_key || g.level.size() != static_cast<size_t>(w) * h || g.id != (second ? 1 : 0)) {
        pic_key = key;
        const CoverScaled& cs = cover_scaled(*img, w, h, true, 0);
        g.level.assign(static_cast<size_t>(w) * h, 0);
        g.hue.assign(static_cast<size_t>(w) * h, 0);
        g.pal = cs.pal;
        const int ox = (w - cs.w) / 2, oy = (h - cs.h) / 2;
        for (int y = 0; y < cs.h; ++y)
            for (int x = 0; x < cs.w; ++x) {
                if (x + ox < 0 || x + ox >= w || y + oy < 0 || y + oy >= h) continue;
                g.level[static_cast<size_t>(y + oy) * w + x + ox] = cs.level[static_cast<size_t>(y) * cs.w + x];
                g.hue[static_cast<size_t>(y + oy) * w + x + ox] = cs.hue[static_cast<size_t>(y) * cs.w + x];
            }
        g.fresh = true;
    } else g.fresh = false;
    g.plain = true; g.direct = true; g.scale = 1; g.out_cw = cw0; g.out_ch = ch0;
    g.cols = pcols; g.rows = preview_rows; g.w = w; g.h = h;
    g.col = scr_col; g.row = scr_row;
    g.id = second ? 1 : 0; g.grid.clear(); g.cut_w = 0; g.crop = 0;
    g.opaque = settings_.black_background; g.erase = !settings_.black_background;
    g.active = true;
    return true;
}

std::vector<std::string> App::build_cf_art_panel(int total_width, int height) const {
    const int inner = total_width - 4;
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string list_ansi = ansi_for(settings_.list_color, false);
    std::vector<std::string> out;

    cf_update_current();
    const CfFolder* f = nullptr;
    const CfAlbum* a = cf_selected_album(&f);
    std::string label = "ALBUM ART";
    if (cf_focus_ == 2) label += focus_indicator(settings_, border_ansi);
    out.push_back(box_top(label, total_width, border_ansi));

    auto line = [&](const std::string& content) { out.push_back(bar + " " + list_ansi + pad_right(truncate_str(content, inner), inner) + "\x1b[0m " + bar); };
    if (!a) {
        for (int row = 0; row < height; ++row) {
            std::string c;
            if (row == height / 2) {
                const std::string t = apply_font_map("SELECT AN ALBUM", settings_.font_map);
                c = std::string(static_cast<size_t>(std::max(0, (inner - display_width(t)) / 2)), ' ') + t;
            }
            line(c);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    const int ncand = static_cast<int>(cf_cands_.size());
    const int list_rows = std::min(std::max(ncand, 1), std::max(1, std::min(6, height / 3)));
    // album title and artist inside the pane (they scroll as a marquee when too long, like the hovered row of the library)
    {
        const std::string artist = a->artist.empty() ? std::string("-") : a->artist;
        line(" " + marquee_or_truncate("Album:  " + apply_font_map(a->name, settings_.font_map), inner - 1, cf_selected_, cf_marq_art_[0], cf_marq_art_since_[0]));
        line(" " + marquee_or_truncate("Artist: " + apply_font_map(artist, settings_.font_map), inner - 1, cf_selected_, cf_marq_art_[1], cf_marq_art_since_[1]));
    }
    const int preview_rows = std::max(2, height - list_rows - 4);
    // what is previewed: the picture under the cursor of the list, else the cover the album has now
    std::string pfile;
    bool pemb = false;
    std::string pmsg;
    if (cf_focus_ == 2 && ncand > 0) {
        const CoverCandidate& c = cf_cands_[static_cast<size_t>(std::clamp(cf_art_sel_, 0, ncand - 1))];
        pfile = path_utf8(c.file);
        pemb = c.kind == CoverCandidate::Embedded;
    } else if (cf_cur_src_.none || cf_cur_fallback_) {
        pmsg = "no cover";
    } else {
        pfile = cf_cur_src_.file;
        pemb = cf_cur_src_.embedded;
        if (pfile.empty()) pmsg = "no cover";
    }
    int pcols = std::min(inner, preview_rows * 2);
    std::vector<std::string> pv;
    gfx_.active = false;
    bool as_picture = false;
    if (cf_full_res_ && gfx_proto_ != GfxProto::None && !pfile.empty() && pmsg.empty()) {
        // full resolution: the picture itself (Kitty / Sixel) over the preview cells; the text frame holds placeholders there
        if (cover_full_picture(pfile, pemb, inner, preview_rows, cf_art_col_ + 2, 6)) {   // (below the border, the album and the artist line)
            pcols = inner;
            as_picture = true;
            for (int r = 0; r < preview_rows; ++r) pv.push_back(gfx_hole(pcols));
        } else {
            pv = cover_preview_lines(pfile, pemb, pcols, preview_rows, preview_cover_loading(pfile, pemb) ? "reading ..." : "(the picture cannot be read)");
        }
    } else {
        pv = cover_preview_lines(pfile, pemb, pcols, preview_rows, pmsg);
    }
    (void)as_picture;
    const int lpad = std::max(0, (inner - pcols) / 2);
    for (int r = 0; r < preview_rows; ++r) {
        const std::string row = r < static_cast<int>(pv.size()) ? pv[static_cast<size_t>(r)] : std::string(static_cast<size_t>(pcols), ' ');
        const int rest = std::max(0, inner - lpad - pcols);
        out.push_back(bar + " " + list_ansi + std::string(static_cast<size_t>(lpad), ' ') + row + "\x1b[0m" + list_ansi + std::string(static_cast<size_t>(rest), ' ') + "\x1b[0m " + bar);
    }
    const std::string cur = cf_cur_fallback_ ? "now: no cover of its own (playback takes " + cf_cur_src_.label + " from the folder)" : cf_cur_src_.none ? "now: no cover" : cf_cur_src_.label.empty() ? "now: no cover" : "now: " + cf_cur_src_.label + (cf_cur_src_.chosen ? "  (chosen)" : "");
    line(" " + cur);
    const std::string choice = cover_album_choice(a->name);
    const int scroll = std::clamp(cf_art_sel_ - list_rows / 2, 0, std::max(0, ncand - list_rows));
    if (ncand == 0) line(" (no picture for this album -- add it to the fetch list)");
    for (int row = 0; row < list_rows && ncand > 0; ++row) {
        const int idx = scroll + row;
        std::string content;
        if (idx < ncand) {
            const CoverCandidate& c = cf_cands_[static_cast<size_t>(idx)];
            const std::string id = c.kind == CoverCandidate::Embedded ? std::string(kCoverEmbedded) : path_utf8(c.file);
            content = std::string(id == choice ? " * " : "   ") + c.label;
        }
        const bool sel = cf_focus_ == 2 && idx == cf_art_sel_;
        const std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) out.push_back(bar + " " + cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) + padded + "\x1b[0m " + bar);
        else out.push_back(bar + " " + list_ansi + bg_ansi_for(settings_.list_inactive_bg_color) + padded + "\x1b[0m " + bar);
    }
    while (static_cast<int>(out.size()) < height + 1) line("");
    out.push_back(box_bottom(total_width, choice.empty() ? "" : truncate_str("(* = album default)", std::max(4, total_width - 6)), border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_cf_list_panel(int total_width, int height) const {
    const int inner = total_width - 4;
    const std::string border_ansi = ansi_for(settings_.border_color, false);
    const std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string list_ansi = ansi_for(settings_.list_color, false);
    std::vector<std::string> out;
    std::vector<CfFetchItem> items;
    { std::lock_guard<std::mutex> lk(cf_mutex_); items = cf_list_; }
    out.push_back(box_top("FETCH LIST (" + std::to_string(items.size()) + ")" + focus_indicator(settings_, border_ansi), total_width, border_ansi));
    const int total = static_cast<int>(items.size());
    if (total == 0) {
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == height / 2) {
                const std::string t = apply_font_map("NOTHING QUEUED -- PRESS 'a' ON AN ALBUM IN THE ALBUM LIBRARY TAB", settings_.font_map);
                content = std::string(static_cast<size_t>(std::max(0, (inner - display_width(t)) / 2)), ' ') + t;
            }
            out.push_back(bar + " " + list_ansi + pad_right(truncate_str(content, inner), inner) + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }
    const int scroll = std::clamp(cf_list_sel_ - height / 2, 0, std::max(0, total - height));
    for (int row = 0; row < height; ++row) {
        const int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const CfFetchItem& it = items[static_cast<size_t>(idx)];
            const std::string state = it.state.empty() ? "-" : it.state;
            const int state_w = std::min(static_cast<int>(display_width(state)), inner / 2);
            const int name_w = std::max(8, inner - state_w - 6);
            std::string name = it.album + (it.artist.empty() ? std::string() : "  -  " + it.artist) + "  [" + std::to_string(it.folders.size()) + (it.folders.size() == 1 ? " folder]" : " folders]");
            content = pad_right(std::to_string(idx + 1), 3) + " " + pad_right(truncate_str(name, name_w), name_w) + " " + truncate_str(state, state_w);
        }
        const bool sel = idx == cf_list_sel_;
        const std::string padded = pad_right(truncate_str(content, inner), inner);
        const bool bad = idx < total && items[static_cast<size_t>(idx)].done && !items[static_cast<size_t>(idx)].ok;
        const std::string base = sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                                     : list_ansi + bg_ansi_for(settings_.list_inactive_bg_color);
        if (bad) out.push_back(bar + " " + base + legend_sgr(settings_) + padded + "\x1b[0m" + base + "\x1b[0m " + bar);
        else out.push_back(bar + " " + base + padded + "\x1b[0m " + bar);
    }
    std::string footer;
    const int remaining = total - (scroll + height);
    if (remaining > 0) footer = "(" + std::to_string(remaining) + " more)";
    out.push_back(box_bottom(total_width, truncate_str(footer, std::max(4, total_width - 6)), border_ansi_bottom));
    return out;
}

void App::build_cf_screen(std::ostringstream& frame, int W, int target_height) const {
    if (W < 60) W = 60;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const int fixed_rows = 3;
    {
        const int search_max = 25;
        const std::string prefix = " Search: ";
        const int field_w = search_max + 1;
        const int inner_w = static_cast<int>(prefix.size()) + field_w + 2 + 1;
        std::string line2;
        int line2_cols = 0;
        if (cf_tab_ == 0) {
            std::string field;
            int field_cols = 0;
            if (cf_focus_ == 0) {
                EditPaint p = paint_edit_field(cf_query_, edit_caret_, edit_anchor_, field_w,
                                               cf_query_.empty() ? search_idle_sgr(settings_) : search_sgr(settings_), true);
                field = search_run(settings_, p.s, !cf_query_.empty());
                field_cols = p.cols;
            } else {
                const std::string plain = truncate_str(cf_query_.empty() ? std::string("(f: = folders)") : cf_query_, field_w);
                field = search_run(settings_, plain, !cf_query_.empty());
                field_cols = static_cast<int>(display_width(plain));
            }
            std::string focus_mark;
            int focus_w = 0;
            if (cf_focus_ == 0) { focus_mark = " " + focus_sgr(settings_) + "\xe2\x97\x80" + "\x1b[0m"; focus_w = 2; }
            line2 = search_idle_sgr(settings_) + prefix + field + "\x1b[0m" + focus_mark;
            line2_cols = static_cast<int>(prefix.size()) + field_cols + focus_w;
        }
        const auto strip = menu_tab_strip(W, "COVER FETCH", {"ALBUM LIBRARY", "FETCH LIST"}, cf_tab_, border, border,
                                          settings_.box_upper_left, settings_.box_upper_right, settings_.box_vertical,
                                          inner_w, line2, line2_cols,
                                          ansi_for(settings_.tab_current_color, false), ansi_for(settings_.tab_other_color, false));
        frame << strip.first << "\n" << strip.second << "\n";
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    const int budget = std::min(target_height, term_rows_ - 1);
    const int panel_h = std::max(6, budget - fixed_rows - 7);
    cf_page_rows_ = std::max(1, panel_h - 3);
    if (cf_tab_ == 0) {
        const int left_w = W / 2, right_w = W - left_w;
        cf_art_col_ = left_w;
        const auto left = build_cf_library_panel(left_w, panel_h);
        const auto right = build_cf_art_panel(right_w, panel_h);
        const size_t rows = std::max(left.size(), right.size());
        for (size_t i = 0; i < rows; ++i)
            frame << (i < left.size() ? left[i] : std::string(static_cast<size_t>(left_w), ' '))
                  << (i < right.size() ? right[i] : std::string(static_cast<size_t>(right_w), ' ')) << "\n";
    } else {
        for (auto& l : build_cf_list_panel(W, panel_h)) frame << l << "\n";
    }

    if (cf_prompt_ != CfPrompt::None) {
        std::string text, colors = "\x1b[43;30m";
        if (cf_prompt_ == CfPrompt::Fetch) {
            text = "Send the album names on the list (and the artist most tracks have; song titles only as a fallback) to MusicBrainz / the Cover Art Archive to fetch their front covers? "
                   "Covers are saved in a \"fetched_covers\" folder in every folder holding the album.   [Y]es   [N]o   [ESC] cancel";
        } else if (cf_prompt_ == CfPrompt::Delete) {
            text = "Delete this picture from the disk? Choices that point to it are removed too.   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[41;97m";
        } else if (cf_prompt_ == CfPrompt::ClearAlbum) {
            const CfAlbum* ca = cf_selected_album();
            text = "Remove the album title \"" + (ca ? ca->name : std::string()) + "\" from its " + std::to_string(ca ? ca->tracks.size() : 0) + " track(s)? It becomes a pending edit of the meta data editor: save it there with CTRL+s (CTRL+SHIFT+x discards).   [Y]es   [N]o   [ESC] cancel";
        } else {
            text = "Empty the fetch list?   [Y]es   [N]o   [ESC] cancel";
        }
        auto lines = wrap_lines(text, std::max(10, W - 2), 3);
        while (lines.size() < 5) lines.push_back(std::string());
        for (const auto& l : lines) {
            if (l.empty()) frame << "\n";
            else frame << colors << " " << l << " \x1b[0m\n";
        }
        return;
    }

    const bool in_text = cf_tab_ == 0 && cf_focus_ == 0 && text_engaged("cf-search");
    const std::string lr = in_text ? "[←→] Cursor" : "[←→] Tab";
    const std::string esc = (cf_tab_ == 0 && cf_focus_ == 0) ? "[ESC] Clear/Exit\001Exit" : "[ESC] Back/Exit\001Exit";
    std::vector<LegendGroup> groups;
    const std::string rescan = "[" + hotkey_text("HKeyRescan", "R") + "] Rescan paths\001Rescan";
    if (cf_tab_ == 0) {
        groups.push_back({"Move", {lr, "[TAB] Focus", "[↑↓] Navi.", "[SHIFT+↑↓] Page\001Page", "[CTRL+↑↓] Top/bottom\001Top/bot.", "[ENTER] Open"}});
        groups.push_back({"Fetch", {"[a] Album/folder to list\001To list", "[SHIFT+f] All searched to list\001Searched to list", "[SHIFT+d] Remove album title\001Remove title", rescan}});
        groups.push_back({"Filter", {"[x] No cover\001None", "[o] One cover\001One", "[s] Several covers\001Several", "[p] Previous unsuccessful fetch\001Failed before", "[r] Recently fetched\001Recent"}});
        groups.push_back({"Picture", {"[ENTER] Use for album\001Use", "[n] No cover\001None", "[c] Clear choice\001Clear", "[DEL] Delete", "[t] ASCII / Full Resolution\001ASCII/Full"}});
    } else {
        groups.push_back({"Move", {lr, "[↑↓] Navi.", "[SHIFT+↑↓] Page\001Page", "[CTRL+↑↓] Top/bottom\001Top/bot."}});
        groups.push_back({"Fetch", {"[ENTER] Fetch list\001Fetch", "[SHIFT+c] Cancel fetch\001Cancel", rescan}});
        groups.push_back({"List", {"[DEL] Remove", "[SHIFT+d] Empty list\001Empty"}});
        groups.push_back({"Session", {esc}});
    }
    if (cf_tab_ == 0) groups.back().cmds.push_back(esc);
    {
        const auto lrows = layout_legend_tree(groups, W - 1);
        for (size_t i = 0; i < lrows.size(); ++i) frame << " " << paint_legend(settings_, lrows, i, border, 0, true) << "\n";
    }
    std::string status = cf_status_;
    if (cf_fetch_running_.load()) {
        std::lock_guard<std::mutex> lk(cf_mutex_);
        status = cf_fetch_progress_;
    }
    if (!status.empty()) frame << "\x1b[38;5;246m" << truncate_str(status, W) << "\x1b[0m\n";
    else frame << "\n";
}

}  // namespace muisc
