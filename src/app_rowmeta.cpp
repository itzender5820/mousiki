// app_rowmeta.cpp
// Part of class App (src/app.h): Tags of the rows on screen, read in the background (and the waveform of the current track).
// Split out of app.cpp; the includes are the same as there.
#include "app.h"
#include "playlist_state.h"
#include "tag_cache.h"
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


// ---------------------------------------------------------------------
// Lazy metadata probing for whatever's currently visible in the list
// ---------------------------------------------------------------------

// Tries to fully resolve a row -- duration AND tags -- using nothing but
// in-process header parsing (native_duration.h): no subprocess, safe to
// call from the render thread. For a well-formed tag in a format this
// covers (MP3/ID3v2, FLAC/Ogg/Opus's Vorbis comments, or M4A/MP4/AAC's
// iTunes-style atoms -- covers every format probe_duration_native()
// already parses for duration) this is a complete answer on its own. For
// anything it can't handle (an unsupported/unusual tag layout, or a
// container this doesn't recognize at all), it comes back with whatever
// duration native parsing found, tags_resolved left false -- callers fall
// back to ffprobe for tags in that case.
//
// This is what turns "resolving an 800-track library's metadata" from
// hundreds of ffprobe subprocess spawns (the actual bottleneck -- each one
// costs tens to hundreds of milliseconds, worse under antivirus real-time
// scanning on Windows) into a few pread() syscalls per file for the
// overwhelming majority of a typical library, whatever mix of formats it's in.
RowMeta try_native_row_meta(const fs::path& path) {
    RowMeta rm;
    uint32_t dur = probe_duration_native(path);
    if (dur > 0) rm.duration_sec = static_cast<double>(dur);

    std::string ext = ascii_lower_str(path_utf8(path.extension()));
    NativeId3Tags tags;
    if (ext == ".mp3") tags = probe_id3v2_native(path);
    else if (ext == ".flac") tags = probe_flac_native(path);
    else if (ext == ".ogg" || ext == ".opus") tags = probe_ogg_native(path);
    else if (ext == ".m4a" || ext == ".m4b" || ext == ".mp4" || ext == ".aac") tags = probe_mp4_native(path);

    if (tags.resolved) {
        // clean_tag_text(): a tag can hold malformed UTF-8 (cut in the middle of a character) or control characters,
        // which the terminal does not draw the way they are measured -- the row would come out narrower than its pane
        rm.title = clean_tag_text(tags.title);
        rm.artist = clean_tag_text(tags.artist);
        rm.album = clean_tag_text(tags.album);
        rm.year = clean_tag_text(tags.year); // TYER/TDRC (MP3), DATE (Vorbis), ©day (MP4)
        rm.tags_resolved = true; // fully resolved without ffprobe
    }
    return rm;
}

// Reads the duration and tags of the rows on screen. It uses the in-process header parser
// (native_duration.h: plain pread() calls, no subprocess, so it cannot hang the screen) and
// never runs ffprobe here: one ffprobe call can take long on a slow or damaged file. Files the
// native parser cannot read are left for the background sweep (launch_row_meta_resolver()).
void App::ensure_visible_row_meta() {
    if (list_source_ != ListSource::Local) return;
    for (int i = scroll_; i < std::min(static_cast<int>(local_view_.size()), scroll_ + list_nav_rows()); ++i) {
        const auto& t = local_view_[i];
        std::string key = path_utf8(t.path);
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            if (row_meta_cache_.count(key)) continue; // already resolved (native path or background sweep)
        }
        RowMeta rm = try_native_row_meta(t.path);
        if (rm.duration_sec > 0 || rm.tags_resolved) {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            // No cap on the cache (about 200 bytes a track): a cap left the tags of every file past it unsearchable.
            row_meta_cache_[key] = rm;
            translit_learn(rm.title, rm.artist, rm.album, key);
        }
        // A visible row whose tags resolved right here (native, no ffprobe
        // needed) still needs the version bump: this row scrolling into
        // view generally happens well before the background sweep ever
        // gets to it, so without this the same "search doesn't notice
        // metadata that arrived after the filter ran" gap would just
        // reappear for the fast native path instead of the slow ffprobe
        // one. See poll_pending_row_meta_tags().
        if (rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
        // else: leave unresolved — native parsing is cheap enough to just
        // retry next frame, and the background sweep will fill it in via
        // ffprobe regardless, so there's no real cost to not caching a miss.
    }
}

// Re-runs quantization on the already-decoded PCM for whatever's
// currently playing — used when the smooth/raw toggle changes mid-track,
// so the effect shows up right away instead of only on the next track.
// No re-decode needed: current_.pcm already holds everything decoded so
// far (streaming decode may still be filling it in, hence the acquire
// load of `available` rather than assuming it's complete).
void App::recompute_waveform_for_current_track() {
    if (!has_track_ || !current_.pcm) return;
    std::shared_ptr<StreamingPcm> pcm = current_.pcm;
    bool smooth = settings_.waveform_smooth;
    // Bump the epoch before the thread starts: a waveform thread that is still running sees that its
    // epoch is stale and drops its result, instead of overwriting pending_waveform_envelope_.
    int my_epoch = ++waveform_.epoch;
    spawn_bg("waveform pass", [this, pcm, smooth, my_epoch]() {
        if (pcm->windowed) {
            // a long track: from the figures of its analysis pass (the samples are not all held)
            if (!pcm->analysis_done.load(std::memory_order_acquire) || pcm->env_sumsq.empty()) return;
            auto envelope = WaveformQuantizer::envelope_from_bins(pcm->env_sumsq, StreamingPcm::kEnvGran,
                                                                  pcm->total_frames.load(), 4096, smooth);
            std::lock_guard<std::mutex> lk(waveform_.mutex);
            if (my_epoch != waveform_.epoch.load()) return;
            pending_waveform_envelope_ = std::move(envelope);
            waveform_.pending_ready = true;
            return;
        }
        size_t n = pcm->available.load(std::memory_order_acquire);
        if (n == 0) return;
        // `n` counts frames; the buffer is interleaved, so copy n * channels floats.
        std::vector<float> snapshot(pcm->data.begin(),
                                    pcm->data.begin() + static_cast<long>(n * static_cast<size_t>(pcm->channels)));
        auto envelope = WaveformQuantizer::generate_high_res_envelope(snapshot, 4096, smooth, pcm->channels);
        std::lock_guard<std::mutex> lk(waveform_.mutex);
        if (my_epoch != waveform_.epoch.load()) return; // superseded — discard
        pending_waveform_envelope_ = std::move(envelope);
        waveform_.pending_ready = true;
    });
}

void App::resolve_playlist_tracks_async() {
    auto paths = std::make_shared<std::vector<fs::path>>();
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        for (const auto& t : playlist_->edit_tracks) {
            if (t.missing) continue;
            auto it = row_meta_cache_.find(path_utf8(t.path));
            if (it == row_meta_cache_.end() || !it->second.tags_resolved) paths->push_back(t.path);
        }
    }
    for (const auto& q : queue_) {
        if (!q.is_local) continue;
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        auto it = row_meta_cache_.find(path_utf8(q.local_path));
        if (it == row_meta_cache_.end() || !it->second.tags_resolved) paths->push_back(q.local_path);
    }
    if (paths->empty()) return;
    spawn_bg("playlist metadata", [this, paths]() {
        for (const fs::path& p : *paths) {
            if (bg_->quit.load()) return;   // quitting
            std::string key = path_utf8(p);
            RowMeta rm = try_native_row_meta(p);
            if (!rm.tags_resolved) {
                RowMeta ff = probe_row_meta(p);
                if (rm.duration_sec <= 0) rm.duration_sec = ff.duration_sec;
                rm.title = ff.title;
                rm.artist = ff.artist;
                rm.album = ff.album;
                rm.tags_resolved = ff.tags_resolved;
            }
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                row_meta_cache_[key] = rm;
                translit_learn(rm.title, rm.artist, rm.album, key);
            }
            row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
        }
    });
}

// The parallel workers of the background tag sweep (a constant outside the function: MSVC does not let a nested lambda use a
// local constexpr without capturing it).
static constexpr int kResolverWorkers = 4;

void App::launch_row_meta_resolver() {
    // One sweep per session -- except that rescan_library() clears the
    // flag when the configured path set changes, which is the one
    // situation where files this sweep has already passed (or never saw)
    // can appear.
    if (row_meta_resolver_started_.exchange(true)) return;
    auto paths = std::make_shared<std::vector<fs::path>>();
    paths->reserve(all_local_tracks_.size());
    for (auto& t : all_local_tracks_) paths->push_back(t.path);
    const bool books = audiobook_;

    // Everything runs in the background. First the tag cache of the last run (tag_cache.h): every file whose size and
    // modification time did not change gets its tags from there at once. Then the sweep over the rest: ffprobe is a separate
    // process per call and slow to start (especially on Windows), so a few workers pull file indices from a shared counter and
    // run calls in parallel. The last worker to finish writes the tag cache anew (also when the program quits half-way: what was
    // read so far is kept).
    spawn_bg("library tag cache", [this, paths, books]() {
        auto stamps = std::make_shared<std::vector<TagStamp>>(paths->size());
        {
            const TagCacheMap cached = tag_cache_load(tag_cache_path(books));
            size_t hits = 0;
            for (size_t i = 0; i < paths->size() && !bg_->quit.load(); ++i) {
                (*stamps)[i] = tag_stamp((*paths)[i]);
                if (cached.empty() || !(*stamps)[i].ok()) continue;
                const std::string key = path_utf8((*paths)[i]);
                const auto it = cached.find(key);
                if (it == cached.end() || !(it->second.stamp == (*stamps)[i])) continue;   // new or changed: read again
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto& slot = row_meta_cache_[key];
                if (slot.tags_resolved) continue;   // read meanwhile (the rows on screen, the meta editor)
                slot = it->second.meta;
                translit_learn(slot.title, slot.artist, slot.album, key);
                ++hits;
            }
            if (hits > 0) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);   // search / sort see them now
            ConsoleLog::instance().log_verbose("tag cache: " + std::to_string(hits) + " of " + std::to_string(paths->size()) + " files unchanged");
        }
        auto next_index = std::make_shared<std::atomic<size_t>>(0);
        auto finished = std::make_shared<std::atomic<int>>(0);
        for (int worker = 0; worker < kResolverWorkers; ++worker) {
            spawn_bg("library metadata sweep", [this, paths, stamps, next_index, finished, books]() {
                for (;;) {
                    if (bg_->quit.load()) break;   // quitting: stop here (what was read is written below)
                    size_t i = next_index->fetch_add(1, std::memory_order_relaxed);
                    if (i >= paths->size()) break;
                    const fs::path& p = (*paths)[i];
                    std::string key = path_utf8(p);
                    {
                        std::lock_guard<std::mutex> lk(row_meta_mutex_);
                        auto it = row_meta_cache_.find(key);
                        // A cache entry counts as done only when its tags came from a real probe. The render
                        // thread may cache just the duration (from the native header parser), so such an entry
                        // must not stop the sweep: its tags still need ffprobe.
                        if (it != row_meta_cache_.end() && it->second.tags_resolved) continue;
                    }
                    RowMeta rm = try_native_row_meta(p);
                    // Try the native header reader first: for a well-formed tag (the common case) it needs a
                    // few pread() calls, while ffprobe costs a whole process spawn per file. Only what the
                    // native reader declines (another format, or an unusual tag layout) goes to ffprobe.
                    if (!rm.tags_resolved) {
                        RowMeta ff = probe_row_meta(p);
                        if (rm.duration_sec <= 0) rm.duration_sec = ff.duration_sec; // keep native's duration if we already had it
                        rm.title = ff.title;
                        rm.artist = ff.artist;
                        rm.album = ff.album;
                        rm.tags_resolved = ff.tags_resolved;
                    }
                    {
                        std::lock_guard<std::mutex> lk(row_meta_mutex_);
                        translit_learn(rm.title, rm.artist, rm.album, key);
                        row_meta_cache_[key] = rm;   // no cap: ~200 bytes a track, and a cap left the tags of every file past it unsearchable
                    }
                    // Tell the main thread that tags arrived: poll_pending_row_meta_tags() (called every
                    // frame) re-runs an active search when this version changes, so a file that becomes
                    // searchable by its tags is found without retyping the query.
                    if (rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
                }
                if (finished->fetch_add(1) + 1 != kResolverWorkers) return;
                // the last worker: the tags of every library file as they are now, with the stamps taken before they were read
                // (a file changed since then has a newer stamp on disk and is read again next time)
                TagCacheMap out;
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    out.reserve(paths->size());
                    for (size_t i = 0; i < paths->size(); ++i) {
                        if (!(*stamps)[i].ok()) continue;
                        const std::string key = path_utf8((*paths)[i]);
                        const auto it = row_meta_cache_.find(key);
                        if (it == row_meta_cache_.end() || !it->second.tags_resolved) continue;
                        if (borrowed_tags_.count(key)) continue;   // a video showing the tags of its track: not its own
                        out[key] = TagCacheEntry{(*stamps)[i], it->second};
                    }
                }
                if (!tag_cache_save(tag_cache_path(books), out))
                    ConsoleLog::instance().log_basic("tag cache: could not write " + path_utf8(tag_cache_path(books)));
            });
        }
    });
}

// Every frame's counterpart to launch_row_meta_resolver(): re-filters the
// currently-shown local list once new tags have arrived in the background,
// so a metadata match (like an artist tag) shows up on its own instead of
// requiring the user to retype the query after the sweep happens to catch
// up. A no-op the vast majority of frames (the version check is a single
// relaxed atomic load), so this is cheap to call unconditionally.
void App::poll_pending_row_meta_tags() {
    uint64_t v = row_meta_tags_version_.load(std::memory_order_relaxed);
    if (v == row_meta_tags_seen_) return;
    // The sweep bumps the version for every file it resolves; with tens of thousands of files, re-filtering the whole
    // library on each of those frames would keep the UI busy. At most four times a second -- the rest waits for the
    // next frame (row_meta_tags_seen_ stays behind, so nothing is lost).
    const auto now = std::chrono::steady_clock::now();
    if (now - row_meta_refilter_at_ < std::chrono::milliseconds(250)) return;
    row_meta_refilter_at_ = now;
    row_meta_tags_seen_ = v;

    if (list_source_ != ListSource::Local) return; // nothing local is even on screen right now

    // Mirror update_live_search_preview()'s "which query is live right now"
    // logic: mid-typing uses search_.buffer, otherwise the last committed
    // query. Either way, leave "y:"/"p:" alone -- those aren't local
    // filters and don't read row_meta_cache_ at all.
    std::string q = (mode_ == Mode::Search) ? search_.buffer : last_local_query_;
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.size() >= 2) {
        std::string prefix = ascii_lower_str(q.substr(0, 2));
        size_t plen = 0;
        if (online_prefix(q, plen) >= 0 || prefix == "p:") return;
    }

    // Re-filtering can reorder/shrink the list (a track that just became a
    // metadata match can appear anywhere by rank), so re-anchor on the
    // previously-selected track's identity rather than leaving `selected_`
    // pointing at whatever index now happens to sit there.
    fs::path prev_selected_path;
    bool had_selection = selected_ >= 0 && selected_ < static_cast<int>(local_view_.size());
    if (had_selection) prev_selected_path = local_view_[static_cast<size_t>(selected_)].path;

    local_view_ = filter_and_rank_local_view(q);

    if (had_selection) {
        selected_ = 0;
        for (size_t i = 0; i < local_view_.size(); ++i) {
            if (local_view_[i].path == prev_selected_path) { selected_ = static_cast<int>(i); break; }
        }
    }
    if (local_view_.empty()) selected_ = 0;
    else if (selected_ >= static_cast<int>(local_view_.size())) selected_ = static_cast<int>(local_view_.size()) - 1;
    if (scroll_ > selected_) scroll_ = selected_;
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
}

} // namespace muisc
