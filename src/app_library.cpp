// app_library.cpp
// Part of class App (src/app.h): The library: row titles, the search, sort and filter of the local list, folders, playlists on disk, rescan.
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "video_links.h"
#include "text_edit.h"
#include "overlay_common.h"
#include "remote_server.h"
#include "remote_control.h"
#include "about_text.h"
#include "border_gradient.h"
#include "mode_switch.h"
#include "book_art.h"
#include "keyboard_layout.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include "karaoke_font.h"
#include "glyph_raster.h"
#include "chiptune.h"
#include "scope_big.h"
#include "scope_window.h"
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include "process_util.h"
#include "app_internal.h"
#include "update_check.h"
#include "open_url.h"
#include "app_settings_tables.h"
#include <cstring>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/types.h>
#include <sys/stat.h>   // file_added_time(): stat() / _wstat64()
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace muisc {

namespace {

// ---- the same matching as fuzzy_score(), for a library of tens of thousands of tracks ------------------------------------
// The query is normalised and split once, the targets are normalised once per scan (App::search_idx_), and the edit
// distance stops as soon as a word cannot reach the 0.55 quality floor any more. The results are identical.
struct PreparedQuery {
    std::string q;
    std::vector<std::string> words;
    std::vector<uint64_t> masks;   // per word: which characters it contains (hashed to 64 bits)
    std::string compact;           // the words run together (for the abbreviation match)
};
inline int popcount64(uint64_t x) {   // portable (no compiler intrinsic)
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<int>((x * 0x0101010101010101ULL) >> 56);
}
inline uint64_t char_bit(char c) { return uint64_t{1} << (static_cast<unsigned char>(c) & 63); }
PreparedQuery prepare_query(const std::string& query) {
    PreparedQuery pq;
    pq.q = normalize_for_search(query);
    pq.words = split_words(pq.q);
    for (const auto& w : pq.words) pq.compact += w;
    for (const auto& w : pq.words) {
        uint64_t m = 0;
        for (char c : w) m |= char_bit(c);
        pq.masks.push_back(m);
    }
    return pq;
}
// Edit distance that gives up above `cutoff`: returns cutoff + 1 then (the exact value is never needed above it).
int levenshtein_capped(std::string_view a, std::string_view b, int cutoff) {   // (an adjacent swap counts as one edit, like levenshtein())
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    if (std::abs(n - m) > cutoff) return cutoff + 1;
    thread_local std::vector<int> prev2, prev, cur;
    prev2.assign(static_cast<size_t>(m) + 1, 0);
    prev.assign(static_cast<size_t>(m) + 1, 0);
    cur.assign(static_cast<size_t>(m) + 1, 0);
    for (int j = 0; j <= m; ++j) prev[static_cast<size_t>(j)] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = a[static_cast<size_t>(i) - 1] == b[static_cast<size_t>(j) - 1] ? 0 : 1;
            int v = std::min({prev[static_cast<size_t>(j)] + 1, cur[static_cast<size_t>(j) - 1] + 1, prev[static_cast<size_t>(j) - 1] + cost});
            if (i > 1 && j > 1 && a[static_cast<size_t>(i) - 1] == b[static_cast<size_t>(j) - 2] && a[static_cast<size_t>(i) - 2] == b[static_cast<size_t>(j) - 1])
                v = std::min(v, prev2[static_cast<size_t>(j) - 2] + 1);
            cur[static_cast<size_t>(j)] = v;
            if (v < row_min) row_min = v;
        }
        if (row_min > cutoff + 1) return cutoff + 1;   // (+1: a swap can still lower the next row by one)
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[static_cast<size_t>(m)];
}
// Tier 1 of fuzzy_score(): exact substring, earlier position ranks higher. -1 = no substring.
double substring_score(const PreparedQuery& pq, const std::string& t_norm) {
    if (pq.q.empty()) return 0.0;
    const size_t pos = t_norm.find(pq.q);
    return pos == std::string::npos ? -1.0 : 1000.0 - std::min<double>(static_cast<double>(pos), 900.0);
}
// Tier 2 of fuzzy_score(): every query word must find a close word in the target. -1 = no match.
double word_fuzzy_core(const PreparedQuery& pq, const std::string& t_norm) {
    if (pq.words.empty() || t_norm.empty()) return -1.0;
    const std::string_view t(t_norm);
    double total_quality = 0.0;
    bool any_word = false;
    for (size_t qi = 0; qi < pq.words.size(); ++qi) {
        const std::string& qw = pq.words[qi];
        const uint64_t qmask = pq.masks[qi];
        double best_quality = -1.0;
        size_t i = 0;
        while (i < t.size()) {   // the target's words, without copying them
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
            const size_t start = i;
            uint64_t tmask = 0;
            while (i < t.size() && t[i] != ' ' && t[i] != '\t') { tmask |= char_bit(t[i]); ++i; }
            if (i == start) break;
            any_word = true;
            const std::string_view tw = t.substr(start, i - start);
            const size_t max_len = std::max(qw.size(), tw.size());
            double quality = -1.0;
            // a word typed only as far as it goes ("cov" for "cover3") is a good fit
            if (qw.size() >= 2 && tw.size() > qw.size() && tw.compare(0, qw.size(), qw) == 0)
                quality = 0.80 + 0.15 * static_cast<double>(qw.size()) / static_cast<double>(tw.size());
            const int cutoff = static_cast<int>(0.45 * static_cast<double>(max_len)) + 1;
            // Every distinct character of one word that the other lacks costs an edit (collisions of the hash only
            // lower the count), so this is a lower bound of the distance: no need to compute it when it is too big.
            if (std::max(popcount64(qmask & ~tmask), popcount64(tmask & ~qmask)) <= cutoff)
                quality = std::max(quality, 1.0 - static_cast<double>(levenshtein_capped(qw, tw, cutoff)) / static_cast<double>(max_len));
            // a trailing number of the text's word ("cover3") does not count
            if (tw.size() > 1 && tw.back() >= '0' && tw.back() <= '9' && !(qw.back() >= '0' && qw.back() <= '9')) {
                size_t e = tw.size();
                while (e > 1 && tw[e - 1] >= '0' && tw[e - 1] <= '9') --e;
                const std::string_view tws = tw.substr(0, e);
                const size_t ml = std::max(qw.size(), tws.size());
                const int co = static_cast<int>(0.45 * static_cast<double>(ml)) + 1;
                double q2 = 1.0 - static_cast<double>(levenshtein_capped(qw, tws, co)) / static_cast<double>(ml) - 0.02;
                if (qw.size() >= 2 && tws.size() > qw.size() && tws.compare(0, qw.size(), qw) == 0) q2 = std::max(q2, 0.78 + 0.15 * static_cast<double>(qw.size()) / static_cast<double>(tws.size()));
                quality = std::max(quality, q2);
            }
            if (quality > best_quality) best_quality = quality;
        }
        if (!any_word) return -1.0;
        if (best_quality < 0.55) return -1.0;
        total_quality += best_quality;
    }
    return (total_quality / static_cast<double>(pq.words.size())) * 100.0;
}
double word_fuzzy_score(const PreparedQuery& pq, const std::string& t_norm) {
    double best = word_fuzzy_core(pq, t_norm);
    const int skips = abbreviation_skips(pq.compact, t_norm);   // tier 2b: the query as one run-together word ("testcover" for "test_song-cover3"); the fewer words skipped, the better
    if (skips >= 0) best = std::max(best, std::max(60.0, 90.0 - 5.0 * skips));
    return best;
}

} // namespace



// ---------------------------------------------------------------------
// Search / list state
// ---------------------------------------------------------------------

// Shared by refresh_local_view() (final, committed query) and the live
// incremental-search preview (whatever's currently typed, before Enter)
// so both behave identically -- what you see while typing is exactly
// what you'll get if you confirm it.
//
// An empty query returns the library sorted per local_sort_mode_ (see
// apply_local_sort()). A real query filters to fuzzy-matching tracks
// only and sorts PURELY by match quality, highest first -- this
// deliberately throws away the sort order entirely rather than using it
// as a tiebreak, so the best match always sits at the top regardless of
// where it happened to fall alphabetically/by-folder.
//
// Matches against filename-derived title, real artist tag, embedded
// title tag, and album tag -- using whatever's already been resolved
// for that row in row_meta_cache_, falling back to the cheap
// parent-folder guess for artist where the tag hasn't been probed yet.
// This is why matching against embedded metadata gets more complete the
// more of the library you've scrolled past / the longer the one-time
// background sweep (launch_row_meta_resolver) has had to run: each row's
// real tags only become searchable once resolved. A file whose tags
// haven't resolved yet is still findable by filename in the meantime.
// Title shown for `path` in the (search-)lists. Normally that is just the
// filename stem the row was built from; with "Show meta data only"
// (settings_.meta_only) on, the embedded title tag wins as soon as one has
// been resolved for that file, so the row shows metadata rather than a
// filename. Files with no title tag (or tags not probed yet) still fall
// back to the filename -- otherwise turning the toggle on would blank out
// an untagged library instead of just re-describing it.
std::string App::list_row_title(const fs::path& path, const std::string& filename_title) const {
    if (!settings_.meta_only) return filename_title;
    std::lock_guard<std::mutex> lk(row_meta_mutex_);
    auto it = row_meta_cache_.find(path_utf8(path));
    if (it != row_meta_cache_.end() && !it->second.title.empty()) return it->second.title;
    return filename_title;
}

std::string App::playlist_row_label(const fs::path& path, const std::string& filename_title) const {
    std::lock_guard<std::mutex> lk(row_meta_mutex_);
    auto it = row_meta_cache_.find(path_utf8(path));
    if (it != row_meta_cache_.end() && !it->second.title.empty() && it->second.title != filename_title) {
        return filename_title + "  \u2014 " + it->second.title; // em dash separator
    }
    return filename_title;
}

std::string App::marquee_or_truncate(const std::string& text, int width, int row_idx,
                                      int& tracked_idx, std::chrono::steady_clock::time_point& since) const {
    if (row_idx != tracked_idx) {
        tracked_idx = row_idx;
        since = std::chrono::steady_clock::now();
    }
    if (display_width(text) <= width || !settings_.marquee) return pad_right(truncate_str(text, width), width);   // fits, or scrolling is off: cut with "..."

    const double hold_secs = 1.2;    // pause on the title's start before scrolling
    const double cols_per_sec = 4.0; // scroll speed
    const std::string gap = "    ";  // seam between one loop and the next
    std::string loop_text = text + gap;
    int period = display_width(loop_text);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    int start_col = 0;
    if (elapsed > hold_secs && period > 0) {
        double scrolled = (elapsed - hold_secs) * cols_per_sec;
        start_col = static_cast<int>(scrolled) % period;
    }
    // Three repeats guarantee a full-width window is always available no
    // matter where start_col lands in the cycle.
    std::string doubled = loop_text + loop_text + loop_text;
    return pad_right(utf8_skip_take(doubled, start_col, width), width);
}


std::vector<LocalTrack> App::filter_and_rank_local(const std::string& raw_query) const {
    std::string query;
    const int scope = local_query_scope(raw_query, query);   // 0 everything, 1 artist only, 2 album only
    if (query.empty()) {
        auto result = all_local_tracks_;
        apply_local_sort(result);
        return result;
    }

    // The search index: the normalised strings of every track (built once per scan), the tag fields added as the
    // background sweep resolves them. A keystroke then only compares strings.
    if (search_idx_.size() != all_local_tracks_.size()) {
        search_idx_.clear();
        search_idx_.reserve(all_local_tracks_.size());
        for (const auto& t : all_local_tracks_) {
            SearchIdx e;
            e.key = path_utf8(t.path);
            e.title_n = normalize_for_search(with_latin_spelling(t.title));   // (a title in kana / Hangul / hanzi is also found by its Latin spelling)
            e.folder_artist_n = normalize_for_search(with_latin_spelling(t.folder_artist));
            search_idx_.push_back(std::move(e));
        }
    }
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);   // once for the whole library, not once per track
        for (auto& e : search_idx_) {
            if (e.tags_seen) continue;
            auto it = row_meta_cache_.find(e.key);
            if (it == row_meta_cache_.end()) continue;
            e.tag_title_n = normalize_for_search(with_latin_spelling(it->second.title));
            e.artist_n = normalize_for_search(with_latin_spelling(it->second.artist));
            e.album_n = normalize_for_search(with_latin_spelling(it->second.album));
            e.tags_seen = it->second.tags_resolved;
        }
    }
    const PreparedQuery pq = prepare_query(query);
    std::vector<std::pair<double, const LocalTrack*>> scored;
    scored.reserve(all_local_tracks_.size());
    for (size_t i = 0; i < all_local_tracks_.size(); ++i) {
        const SearchIdx& e = search_idx_[i];
        const std::string& artist = e.artist_n.empty() ? e.folder_artist_n : e.artist_n;
        double score = 0.0;
        if (scope == 1) {
            score = substring_score(pq, artist);
            if (score < 0.0) score = word_fuzzy_score(pq, artist);
        } else if (scope == 2) {
            if (!e.album_n.empty()) {
                score = substring_score(pq, e.album_n);
                if (score < 0.0) score = word_fuzzy_score(pq, e.album_n);
            }
        } else {
            // The best score over the fields. An exact substring (tier 1, 100 .. 1000) always beats a fuzzy one (< 100),
            // so the fuzzy pass only runs when no field contains the query.
            const std::string* fields[4] = {&e.title_n, &artist, &e.tag_title_n, &e.album_n};
            double best = -1.0;
            for (const std::string* f : fields) if (!f->empty()) best = std::max(best, substring_score(pq, *f));
            if (best < 0.0) for (const std::string* f : fields) best = std::max(best, word_fuzzy_score(pq, *f));
            score = best;
        }
        if (score > 0.0) scored.emplace_back(score, &all_local_tracks_[i]);
    }
    std::stable_sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<LocalTrack> result;
    result.reserve(scored.size());
    for (auto& [score, t] : scored) result.push_back(*t);
    return result;
}

// Mode 1 names what it sorts by, which follows Shift+N (see apply_local_sort()):
// the file name normally, the embedded title tag in metadata-only mode.
const char* App::sort_mode_name(int mode, bool meta_only) {
    switch (mode) {
        case 1: return meta_only ? "title A-Z" : "file name A-Z";
        case 2: return "artist A-Z";
        case 3: return "recently added";
        case 4: return "videos";
        default: return "folder order";
    }
}

long long App::added_time_of(const fs::path& p) const {
    const std::string key = path_utf8(p);
    std::lock_guard<std::mutex> lk(added_time_mutex_);
    auto it = added_time_cache_.find(key);
    if (it == added_time_cache_.end()) it = added_time_cache_.emplace(key, file_added_time(p)).first;
    return it->second;
}

// Applied only when browsing with no active search query -- a fuzzy
// search's relevance ranking always wins over the manual sort mode.
// Duration isn't a sort option (yet): most rows only get a real duration
// once they've been lazily probed for display, so sorting by it up front
// would show mostly-unprobed rows in an arbitrary order until the
// background resolver catches up.
void App::apply_local_sort(std::vector<LocalTrack>& tracks) const {
    if (local_sort_mode_ == 1) {
        // Sort by the title the row actually SHOWS: the filename-derived one,
        // or -- with "Show metadata only" (Shift+N) on -- the embedded title
        // tag where one has been resolved (list_row_title() falls back to the
        // filename otherwise, exactly like the row does). Keys are built once
        // up front instead of taking row_meta_mutex_ on every comparison.
        std::vector<std::pair<std::string, size_t>> keyed;
        keyed.reserve(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i)
            keyed.emplace_back(ascii_lower_str(list_row_title(tracks[i].path, tracks[i].title)), i);
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(tracks.size());
        for (const auto& k : keyed) sorted.push_back(std::move(tracks[k.second]));
        tracks = std::move(sorted);
    } else if (local_sort_mode_ == 2) {
        std::stable_sort(tracks.begin(), tracks.end(), [this](const LocalTrack& a, const LocalTrack& b) {
            auto artist_of = [this](const LocalTrack& t) {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                return (it != row_meta_cache_.end() && !it->second.artist.empty()) ? it->second.artist : t.folder_artist;
            };
            return ascii_lower_str(artist_of(a)) < ascii_lower_str(artist_of(b));
        });
    }
    else if (local_sort_mode_ == 3) {
        // Newest first: the time a file arrived (file_added_time(): later of modification and creation / change time, so
        // a file that was copied or downloaded into the library counts from then). Looked up once per file and kept
        // until the next rescan, because this runs again every time background tags re-filter the list.
        std::vector<std::pair<long long, size_t>> keyed;
        keyed.reserve(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i) keyed.emplace_back(added_time_of(tracks[i].path), i);
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(tracks.size());
        for (const auto& k : keyed) sorted.push_back(std::move(tracks[k.second]));
        tracks = std::move(sorted);
    }
    // mode 0: leave as scanned (folder order) -- no-op
}

// Folder filter (HKeyFilterForFolder) stacks on top of the search/sort
// result rather than replacing it, so filtering-by-folder while a search is
// active narrows to just that folder's matches. Every place that rebuilds
// local_view_ must go through this (or refresh_local_view()): the background
// tag resolver re-filters the list every time new tags land, and a rebuild
// that skipped the folder filter silently undid 'f' a frame or two later.
std::vector<LocalTrack> App::filter_and_rank_local_view(const std::string& query_in) const {
    // "v:" in front of the query (/v:): only the video files, whatever the SHIFT+t step is (they are listed while the Video mode is on)
    std::string query = query_in;
    bool videos_only = false;
    if (query.size() >= 2 && ascii_lower_str(query.substr(0, 2)) == "v:") {
        videos_only = true;
        query = query.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
    }
    std::vector<LocalTrack> view = filter_and_rank_local(query);
    if (local_sort_mode_ == 4 || videos_only) {   // SHIFT+t's last step (Video mode on): only the video files
        view.erase(std::remove_if(view.begin(), view.end(), [](const LocalTrack& t) { return !LocalSource::is_video_file(t.path); }), view.end());
    } else if (!query.empty()) {   // a search lists audio files only; the videos show up in the video filter (SHIFT+t's last step, /v:)
        view.erase(std::remove_if(view.begin(), view.end(), [](const LocalTrack& t) { return LocalSource::is_video_file(t.path); }), view.end());
    }
    if (folder_filter_.empty()) return view;
    std::vector<LocalTrack> filtered;
    filtered.reserve(view.size());
    for (auto& t : view) {
        if (path_utf8(t.path.parent_path()) == folder_filter_) filtered.push_back(std::move(t));
    }
    return filtered;
}

void App::refresh_local_view() {
    if (audiobook_) { book_refresh_view(); return; }   // (the audio book player has no track list, only the books)
    local_view_ = filter_and_rank_local_view(last_local_query_);
    selected_ = 0;
    scroll_ = 0;
}

// Rebuilds the local list after something changed HOW rows are titled or
// ordered (the Shift+N metadata-only toggle), keeping the cursor on the same
// track instead of jumping to the top like refresh_local_view() does.
void App::resort_local_view_keep_selection() {
    if (list_source_ != ListSource::Local) return;
    fs::path prev;
    const bool had = selected_ >= 0 && selected_ < static_cast<int>(local_view_.size());
    if (had) prev = local_view_[static_cast<size_t>(selected_)].path;
    local_view_ = filter_and_rank_local_view(last_local_query_);
    selected_ = 0;
    if (had) {
        for (size_t i = 0; i < local_view_.size(); ++i)
            if (local_view_[i].path == prev) { selected_ = static_cast<int>(i); break; }
    }
    scroll_ = std::max(0, selected_ - list_nav_rows() / 2);
    if (scroll_ > selected_) scroll_ = selected_;
}


// Re-runs the local library scan over whatever settings_.local_music_paths
// holds right now, then rebuilds the view on top of the result. This is
// what makes a path edited in the PATHS tab take effect immediately --
// Editing a local path in Settings rescans the library at once. The change is written to
// config.txt when settings are saved ([S] or quit).
// After a rescan the saved playlists are brought up to date as well, not only
// when they are saved again: tracks whose file moved are pointed at the new
// path (and the playlist file is rewritten), and the cached tags of every
// playlist track are dropped so they are read from the files again.
void App::refresh_playlists_after_rescan() {
    int rewritten = 0;
    std::vector<std::string> keys;
    for (const fs::path& dir : playlist_dirs()) {
        for (const auto& summary : PlaylistManager::list(dir)) {
            auto pl = PlaylistManager::load(dir, summary.name);
            if (!pl) continue;
            Playlist moved = *pl;
            relocate_missing_tracks(moved);
            bool changed = false;
            for (size_t i = 0; i < moved.tracks.size(); ++i) {
                if (moved.tracks[i].path != pl->tracks[i].path) changed = true;
                keys.push_back(path_utf8(moved.tracks[i].path));
            }
            std::string err;
            if (changed && PlaylistManager::save(dir, moved, &err)) ++rewritten;
        }
    }
    {
        // Titles already in the queue: files moved since they were queued are found again,
        // and their tags are read anew (the queue shows the artist from the tags).
        Playlist q;
        std::vector<size_t> at;
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (!queue_[i].is_local) continue;
            PlaylistTrack t;
            t.path = queue_[i].local_path;
            std::error_code ec;
            t.missing = !fs::exists(t.path, ec);
            q.tracks.push_back(std::move(t));
            at.push_back(i);
        }
        relocate_missing_tracks(q);
        for (size_t k = 0; k < at.size(); ++k) {
            QueueItem& it = queue_[at[k]];
            if (q.tracks[k].path != it.local_path) {
                if (has_track_ && current_path_ == it.local_path) current_path_ = q.tracks[k].path;
                it.local_path = q.tracks[k].path;
                it.title = path_utf8(it.local_path.stem());
                it.artist = path_utf8(it.local_path.parent_path().filename());
            }
            keys.push_back(path_utf8(it.local_path));
        }
    }
    if (!playlist_edit_tracks_.empty()) {
        Playlist open;
        open.tracks = playlist_edit_tracks_;
        relocate_missing_tracks(open);
        playlist_edit_tracks_ = open.tracks;   // not marked as changed: the saved file was already updated above
        for (const auto& t : playlist_edit_tracks_) keys.push_back(path_utf8(t.path));
    }
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        for (const auto& k : keys) row_meta_cache_.erase(k);
    }
    search_idx_.clear();
    resolve_playlist_tracks_async();
    if (rewritten > 0) log_event("playlists updated after the rescan: " + std::to_string(rewritten));
}

// The folders the library scan walks: the local music paths (with the download / cache folders), or in the audio book
// player only the AUDIO BOOK PATH.
std::vector<std::string> App::library_roots() const {
    if (audiobook_) return settings_.audiobook_path.empty() ? std::vector<std::string>{} : std::vector<std::string>{settings_.audiobook_path};
    return settings_.local_music_paths;
}

void App::rescan_library() {
    { std::lock_guard<std::mutex> lk(added_time_mutex_); added_time_cache_.clear(); }   // "recently added" sort: files may have arrived
    // Keep the streamed-track cache folder in the list even if the edit
    // happened to drop it, for the same reason the constructor injects it
    // at all: downloaded songs belong in the local view.
    std::string cache_dir_str = path_utf8(cache_.cache_dir());
    if (!audiobook_ && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), cache_dir_str)
        == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(cache_dir_str);
    }
    // And the configured download folder, for the same reason: whatever
    // yt-dlp writes has to be scannable without a second LocalMusicPath
    // line, which is what the DOWNLOAD PATH setting promises. Skipped
    // while it is still the default -- that IS the cache dir just above.
    std::string download_dir_str = path_utf8(cache_.download_dir());
    if (!audiobook_ && download_dir_str != cache_dir_str
        && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), download_dir_str)
               == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(download_dir_str);
    }

    // A local diagnostics vector, not local_scan_diagnostics_ -- that one
    // is read back once at startup by run() (before any rescan can happen)
    // and never again, so per-scan results go straight to the log instead.
    std::vector<std::string> diagnostics;
    all_local_tracks_ = local_source_.scan(library_roots(), &diagnostics);
    exclude_book_files(all_local_tracks_);
    if (settings_.video_mode && VideoLinks::get().size() > 0) {   // a linked video that moved is found again by its file name
        std::vector<std::string> vids;
        for (const auto& t : all_local_tracks_) if (LocalSource::is_video_file(t.path)) vids.push_back(path_utf8(t.path));
        if (VideoLinks::get().mend(vids) > 0) { video_for_track_.clear(); ++video_rev_; }
    }
    scanned_book_path_ = settings_.audiobook_path;
    search_idx_.clear();
    refresh_local_view(); // applies the active query/sort/filter to the new set
    for (const auto& line : diagnostics) ConsoleLog::instance().log_basic(line);

    if (!audiobook_) refresh_playlists_after_rescan();

    // Newly added files need their tags resolved too -- for search and for
    // metadata-only rows. Re-arm the background sweep, which skips every
    // path already present in row_meta_cache_; if a previous sweep is
    // still winding down the two simply overlap, idempotently.
    row_meta_resolver_started_.store(false, std::memory_order_relaxed);
    launch_row_meta_resolver();
}


void App::update_live_search_preview() {
    std::string buf = search_buffer_;
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    if (audiobook_) {   // the audio book player only searches its books (the folders): plain words, an "f:" in front is allowed
        std::string q = buf;
        if (q.size() >= 2 && ascii_lower_str(q.substr(0, 2)) == "f:") q = q.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }
    size_t plen = 0;
    if (online_prefix(buf, plen) >= 0 || (buf.size() >= 3 && ascii_lower_str(buf.substr(0, 3)) == "sv:")) {   // online (and "sv:", the video search): nothing until ENTER
        list_source_ = pre_search_list_source_;
        local_view_ = filter_and_rank_local_view(pre_search_local_query_);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    // "p:" (playlist search) is local like a plain query -- no network
    // call to defer -- so unlike "s:" above, it's fine to actually run
    // the filter live on every keystroke rather than waiting for Enter.
    if (buf.size() >= 2 && ascii_lower_str(buf.substr(0, 2)) == "p:") {
        std::string q = buf.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        list_source_ = ListSource::Playlist;
        playlist_view_ = filter_playlists(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    // "f:" (folder search) is local too -- filter the folder list live.
    if (buf.size() >= 2 && ascii_lower_str(buf.substr(0, 2)) == "f:") {
        std::string q = buf.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    list_source_ = ListSource::Local;
    local_view_ = filter_and_rank_local_view(buf);
    selected_ = 0;
    scroll_ = 0;
}

void App::submit_search() {
    std::string buf = search_buffer_;
    // trim
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    if (audiobook_) {   // search the books (see update_live_search_preview())
        std::string q = buf;
        if (q.size() >= 2 && ascii_lower_str(q.substr(0, 2)) == "f:") q = q.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        last_folder_query_ = q;
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }
    size_t plen = 0;
    if (buf.size() >= 3 && ascii_lower_str(buf.substr(0, 3)) == "sv:") {   // /sv: search videos on YouTube (SEARCH YT) to download
        std::string query = buf.substr(3);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        video_search_open(query);
        return;
    }
    if (buf.size() >= 2 && ascii_lower_str(buf.substr(0, 2)) == "v:" && !settings_.video_mode)
        status_line_ = "/v: lists the videos while the Video mode is on (" + hotkey_text("HKeyVideo", "&") + " -> Video Mode)";
    if (const int src = online_prefix(buf, plen); src >= 0) {   // s: YouTube, sc: SoundCloud, b: Bandcamp
        std::string query = buf.substr(plen);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_online_query_ = query;
        last_online_source_ = src;
        list_source_ = ListSource::Online;
        if (search_in_progress_.load()) {
            status_line_ = "still searching, hang on ...";
            return;
        }
        launch_search_async(query.empty() ? "music" : query, src);
    } else if (buf.size() >= 2 && ascii_lower_str(buf.substr(0, 2)) == "p:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_playlist_query_ = query;
        list_source_ = ListSource::Playlist;
        queue_focus_ = false;   // the saved playlists pane takes the focus, also when the queue had it before
        playlist_view_ = filter_playlists(query);
        selected_ = 0;
        scroll_ = 0;
    } else if (buf.size() >= 2 && ascii_lower_str(buf.substr(0, 2)) == "f:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_folder_query_ = query;
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(query);
        selected_ = 0;
        scroll_ = 0;
    } else {
        last_local_query_ = buf;
        list_source_ = ListSource::Local;
        refresh_local_view();
    }
}

// "/f:" folder search: every folder that directly holds at least one scanned
// track, filtered by the query. Each whitespace-separated word of the query
// must appear (case-insensitively) in "<parent folder> <folder name>", so
// "beatles abbey" finds Music/The Beatles/Abbey Road. An empty query lists
// every folder. Sorted by folder name.
std::vector<FolderSummary> App::filter_folders(const std::string& query) const {
    std::map<std::string, FolderSummary> by_path;
    for (const auto& t : all_local_tracks_) {
        fs::path dir = t.path.parent_path();
        std::string key = path_utf8(dir);
        auto it = by_path.find(key);
        if (it == by_path.end()) {
            FolderSummary f;
            f.path = key;
            f.name = path_utf8(dir.filename());
            if (f.name.empty()) f.name = key; // drive root etc.
            f.parent = path_utf8(dir.parent_path().filename());
            f.track_count = 1;
            by_path.emplace(key, std::move(f));
        } else {
            ++it->second.track_count;
        }
    }

    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : query) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }

    std::vector<FolderSummary> out;
    out.reserve(by_path.size());
    for (auto& [key, f] : by_path) {
        const std::string hay = f.parent + " " + f.name;
        bool ok = true;
        for (const auto& w : words) if (!contains_ci(hay, w)) { ok = false; break; }
        if (ok) out.push_back(f);
    }
    std::stable_sort(out.begin(), out.end(), [](const FolderSummary& a, const FolderSummary& b) {
        return ascii_lower_str(a.name) < ascii_lower_str(b.name);
    });
    return out;
}

// Enter on a row of the "/f:" list: list all files of that folder in the
// LOCAL AUDIO FILES pane. Same mechanism as the 'f' filter (folder_filter_),
// so [c] clears it again and the pane title shows the folder name. Any
// active local query is dropped so really every file of the folder shows up.
void App::open_selected_folder() {
    if (audiobook_) { book_open(selected_); return; }
    if (folder_view_.empty() || selected_ < 0 || selected_ >= static_cast<int>(folder_view_.size())) return;
    folder_filter_ = folder_view_[static_cast<size_t>(selected_)].path;
    last_local_query_.clear();
    local_sort_mode_ = 0; // folder order, like 'f'
    list_source_ = ListSource::Local;
    refresh_local_view();
    log_event("filtered: " + path_utf8(path_from_utf8(folder_filter_).filename()));
}

// Local, case-insensitive substring match on playlist name -- cheap
// enough (just a directory scan) to re-run on every keystroke, same as
// the "p:" live preview above does.
std::vector<PlaylistSummary> App::filter_playlists(const std::string& query) const {
    auto all = playlist_summaries();
    if (query.empty()) return all;
    std::vector<PlaylistSummary> out;
    out.reserve(all.size());
    for (auto& p : all) if (contains_ci(p.name, query)) out.push_back(p);
    return out;
}

// The folder NEW playlists are written to and deleted from: the first
// configured PlaylistsPath (settings_.playlists_paths[0] -- config.txt's
// PlaylistsPath= line, editable from the PATHS tab's PLAYLIST PATH
// list), otherwise settings_.local_music_paths[0]/playlists (~/Music if
// none configured at all, which by the time this runs may itself have
// become the cache folder -- see load_library()'s cache-dir injection).
// Unrelated to HKeyDownloadStream's folder, which comes from the
// separate DOWNLOAD PATH setting (cache_.download_dir()) instead.
// Computed fresh every call, not cached, so it always reflects whatever
// the user currently has set in Settings.
fs::path App::playlists_dir() const {
    if (!settings_.playlists_paths.empty() && !settings_.playlists_paths[0].empty())
        return path_from_utf8(settings_.playlists_paths[0]);
    std::string base;
    if (!settings_.local_music_paths.empty()) {
        base = settings_.local_music_paths[0];
    } else {
        const char* home = std::getenv("HOME");
        base = home ? (std::string(home) + "/Music") : "./Music";
    }
    return path_from_utf8(base) / "playlists";
}

// Every folder playlists are searched in: all of the configured ones, or
// the single folder playlists_dir() resolves to when none is (so every
// caller always has at least one directory to look in). Blanks are
// skipped -- an empty entry is what a "+ new path" line looks like until
// the user has finished typing it -- and duplicates collapse, since
// scanning the same folder twice would just re-yield its playlists.
std::vector<fs::path> App::playlist_dirs() const {
    std::vector<fs::path> out;
    for (const auto& p : settings_.playlists_paths) {
        if (p.empty()) continue;
        fs::path dir = path_from_utf8(p);
        if (std::find(out.begin(), out.end(), dir) == out.end()) out.push_back(dir);
    }
    if (out.empty()) out.push_back(playlists_dir());
    return out;
}

// Playlists across every folder in playlist_dirs(), merged into one
// list: what both the main "/p:" list and the playlist editor's manage
// tab show. De-duplicated by case-insensitive name with the first folder
// containing it winning (so a row always resolves to one concrete file,
// wherever it gets opened from), then re-sorted with exactly the
// case-insensitive order PlaylistManager::list() already guarantees
// within a single folder -- so several configured folders read as one
// alphabetized list rather than as concatenated per-folder blocks.
std::vector<PlaylistSummary> App::playlist_summaries() const {
    std::vector<PlaylistSummary> merged;
    std::unordered_set<std::string> seen;
    for (const fs::path& dir : playlist_dirs()) {
        for (PlaylistSummary& s : PlaylistManager::list(dir)) {
            if (seen.insert(ascii_lower_str(s.name)).second) merged.push_back(std::move(s));
        }
    }
    std::sort(merged.begin(), merged.end(),
              [](const PlaylistSummary& a, const PlaylistSummary& b) { return ascii_lower_str(a.name) < ascii_lower_str(b.name); });
    return merged;
}

// Loads a playlist by name from whichever playlist_dirs() folder has it
// -- first match wins, the same rule playlist_summaries() de-duplicated
// with, so what the list showed always loads. nullopt when no folder
// holds that name (anymore).
std::optional<Playlist> App::load_playlist(const std::string& name) const {
    for (const fs::path& dir : playlist_dirs()) {
        if (auto pl = PlaylistManager::load(dir, name)) {
            relocate_missing_tracks(*pl);
            return pl;
        }
    }
    return std::nullopt;
}

// A playlist stores paths. When a file is gone from its stored path (moved to
// another folder, or a folder renamed), look for the same file name in the
// library (every folder added in the settings) and use that path. One match
// is taken; with several, the one whose parent folder name matches wins, else
// the first. The playlist file itself is rewritten only when it is saved.
void App::relocate_missing_tracks(Playlist& pl) const {
    bool any = false;
    for (const auto& t : pl.tracks) if (t.missing) { any = true; break; }
    if (!any) return;
    std::unordered_multimap<std::string, const LocalTrack*> by_name;
    for (const auto& lt : all_local_tracks_) by_name.emplace(ascii_lower_str(path_utf8(lt.path.filename())), &lt);
    for (auto& t : pl.tracks) {
        if (!t.missing) continue;
        auto range = by_name.equal_range(ascii_lower_str(path_utf8(t.path.filename())));
        const LocalTrack* pick = nullptr;
        const std::string old_dir = ascii_lower_str(path_utf8(t.path.parent_path().filename()));
        for (auto it = range.first; it != range.second; ++it) {
            if (!pick) pick = it->second;
            if (ascii_lower_str(path_utf8(it->second->path.parent_path().filename())) == old_dir) { pick = it->second; break; }
        }
        if (!pick) continue;
        t.path = pick->path;
        t.title = path_utf8(t.path.stem());
        t.artist = path_utf8(t.path.parent_path().filename());
        t.missing = false;
    }
}

} // namespace muisc
