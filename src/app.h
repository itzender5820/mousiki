#pragma once
#include "wizard.h"
#include "creator.h"
#include "mic_capture.h"
#include <future>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include "video_player.h"
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "book_notes.h"
#include "book_types.h"
#include "book_cue.h"
#include "book_split.h"
#include "cover_art.h"
#include "cover_store.h"
#include "disk_art.h"
#include "remote_server.h"
#include "remote_overlay.h"
#include "fft_visualizer.h"
#include "history.h"
#include "local_source.h"
#include "lyrics_fetcher.h"
#include "lyrics_translate.h"
#include "name_lookup.h"
#include "meta_editor.h"
#include "metadata_probe.h"
#include "native_duration.h"
#include "online_source.h"
#include "oscilloscope_visualizer.h"
#include "terminal_gfx.h"
#include "player.h"
#include "console_log.h"
#include "audio_backend.h"
#include "playlist_manager.h"
#include "settings.h"
#include "snapshot.h"
#include "sphere_visualizer.h"
#include "streaming_pcm.h"
#include "terminal_ui.h"
#include "themes.h"
#include "waveform.h"
#include "youtube_source.h"
#include "cache_manager.h"
#include "converter.h"
#include "app_types.h"

namespace muisc {
namespace org { struct Theme; }   // (organigram.h)

enum class Mode { Browse, Search, Settings, ColorEdit, Console, Cheatsheet, BulkAdd, RetryLyrics, Playlist, MetaEdit, CoverFetch, History, ClearQueue, OsciMenu, NormMenu, BarsMenu, Equalizer, SleepTimer, LyricsEdit, Karaoke, SpectroMenu, Wizard, ReadLyrics, Creator, MenuIndex, UpdateNotice, QueueToPlaylist, BookOverlay, AudioDevice, VideoMenu, VideoLink, VideoSearch, VideoFetch };
enum class ListSource { Local, Online, Playlist, Folder };

// One row of the main UI's "/f:" folder list: a folder that directly
// contains at least one scanned local track.
struct FolderSummary {
    std::string path;   // parent-directory path (same form as folder_filter_)
    std::string name;   // folder's own name
    std::string parent; // name of its parent folder (usually the artist)
    int track_count = 0;
    // The audio book mode's books pane is a tree you walk through: a folder that holds other books is a container (ENTER goes in,
    // `sub_count` books below it) and the first row of a deeper level is "..", the way back (see App::book_build_view).
    bool container = false;
    bool up = false;
    bool self = false;      // inside a container: the audio files of the container folder itself
    int sub_count = 0;
};

struct QueueItem {
    bool is_local;
    std::string title;
    std::string artist;
    fs::path local_path;   // valid if is_local
    std::string video_id;  // valid if !is_local
    // Queue-then-stop mode on a locked queue only: set once this item has been
    // played in the current pass through the queue (it then sits at the back).
    // The pass is over -- and playback stops -- when every item has it set.
    bool played = false;
    // Saved playlist this item was queued from ("" = hand-picked track): the history files the play under it (TOP PLAYLISTS).
    std::string playlist;
};

struct CoverImage;   // cover_art.h

struct LyricsFileHit;      // (readlyrics_state.h)
struct ReadLyricsState;    // the READ LYRICS reader
struct PlaylistState;
struct MetaState;
struct BookState;
struct CreatorState;
struct CoverFetchState;
struct CfAlbum;    // (coverfetch_state.h)
struct CfFolder;
struct CfRow;
struct CfFetchItem;

class App {
public:
    // `shared`: the settings main() loaded, used by every App instance (the music player and the audio book player are two
    // instances; changes made in one are seen by the other at once). nullptr = this App loads its own copy.
    // `audiobook`: this instance is the audio book player (see is_audiobook()).
    explicit App(Settings* shared = nullptr, bool audiobook = false);
    ~App();   // (app.cpp: the parts kept behind a pointer, like the READ LYRICS state, are complete types only there)
    int run();
    void run_poll_terminal(TerminalIO& term, bool& gfx_shown);   // (app.cpp) parts of one pass of the main loop of run()
    void run_tick();
    void shutdown();                       // really end the session (also when it was suspended for the radio)
    bool suspended() const { return suspended_; }
    // The audio book player is the same engine as the music player with a different library (the AUDIO BOOK PATH, only its
    // folders are listed), a chapter pane instead of the queue, bookmarks and notes.
    bool is_audiobook() const { return audiobook_; }

private:
    const bool audiobook_ = false;
    std::vector<std::string> library_roots() const;   // what the library scan walks (see app.cpp)
    Settings own_settings_;   // used when no shared settings were handed in (declared before settings_, which refers to it)
    // --- infrastructure ---
    CacheManager cache_;
    YoutubeSource youtube_{cache_};
    OnlineSource online_;
    LocalSource local_source_;
    DiskArt disk_;
    mutable Player player_;
    fs::path lyrics_script_;
    fs::path fast_search_script_; // empty if not found -- OnlineSource falls back to yt-dlp

    // --- lists / navigation ---
    Mode mode_ = Mode::Browse;
    ListSource list_source_ = ListSource::Local;
    std::vector<LocalTrack> all_local_tracks_;
    // Search index parallel to all_local_tracks_ (rebuilt when the scan changes): the strings a local search compares,
    // already lowercased / separator-folded, and the cache key, so a keystroke does no allocation and no path conversion
    // per track. The tag fields are filled in as the background sweep resolves them (tags_seen).
    using SearchIdx = app_types::SearchIdx;
    mutable std::vector<SearchIdx> search_idx_;
    // scan() runs from the constructor, before ConsoleLog::instance().init()
    // is called at the top of run() -- and init() wipes the in-memory log
    // buffer, so anything logged before it would just be discarded. This
    // holds scan()'s per-root diagnostics (found/missing, file counts) until
    // run() can actually flush them into the log.
    std::vector<std::string> local_scan_diagnostics_;
    std::vector<LocalTrack> local_view_;     // filtered
    std::vector<OnlineResult> online_view_;
    int selected_ = 0;
    int scroll_ = 0;
    std::string search_buffer_;
    std::string last_local_query_;
    ListSource pre_search_list_source_ = ListSource::Local;
    std::string pre_search_local_query_;
    std::string last_online_query_;
    int local_sort_mode_ = 0; // 0=folder order, 1=title A-Z, 2=artist A-Z, 3=recently added (newest first), 4=only videos (Video mode on)
    // Video mode (app_video.cpp)
    mutable VideoPlayer video_;
    std::string format_text() const;   // the Format line: the file type; "MP3, MP4" for a track paired with a video, "MP4" for a video
    mutable std::string fmt_key_, fmt_val_;
    mutable std::vector<std::string> video_ov_ref_;   // the lyric line fully revealed (same wrap): the size of the box over the picture comes from it, so it does not change with every word
    mutable std::string video_for_track_;   // the track video_ was pointed at
    mutable std::string video_pic_key_;
    void video_auto_apply();
    mutable std::string snap_track_, snap_video_;   // the video whose frame is the cover (Cover or Video Snapshot), found once per track
    mutable unsigned snap_rev_ = 0;
    unsigned video_rev_ = 1;   // bumped when the pairing of tracks and videos changed (a link made / removed / mended)
    // --- VIDEO (HKeyVideo) and VIDEO OPTION / SEARCH YT (src/app_videomenu.cpp, src/app_videosearch.cpp) ---
    int vmenu_sel_ = 0;
    // The two questions of "Delete Current Video" (stage 1, then 2) and the one of "Refetch this Video" (stage 3), drawn over the VIDEO overlay.
    int vdel_stage_ = 0, vdel_sel_ = 0;                    // vdel_sel_: 0 No, 1 Yes (always starts on No)
    std::string vdel_video_;                               // the video file in question
    std::unordered_map<std::string, std::string> vf_replace_;   // song -> the downloaded video a refetch replaces (deleted when the new one has arrived)
    std::string vf_auto_;                                  // the song whose refetch chooses the closest hit and downloads by itself
    std::vector<std::string> build_video_delete_panel() const;
    void video_refetch_start();
    bool video_is_download(const std::string& file) const;
    void video_menu_open();
    void video_menu_close();
    void video_menu_change(int row, int dir);
    void video_menu_key(int key);
    std::vector<std::string> build_video_menu_panel() const;
    static constexpr int kVideoMenuPanelWidth = 52;   // (narrow: most of the picture stays visible at the right while the options are changed)
    struct VLEntry { fs::path path; std::string label, folder, hay; bool video = false, has_video = false; };   // (has_video: a song with a connected / same-name video)
    std::vector<VLEntry> vl_tracks_, vl_videos_;
    std::vector<int> vl_tview_, vl_vview_;
    std::string vl_tq_, vl_vq_, vl_msg_;
    int vl_pane_ = 0, vl_tsel_ = 0, vl_vsel_ = 0, vl_filter_ = 0;
    fs::path vl_lock_track_, vl_lock_video_;
    bool vl_warn_ = false, vl_confirm_ = false, vl_del_confirm_ = false;   // (vl_del_confirm_: "Delete COMPLETE VIDEO FILE connected to track" waits for its second ENTER)
    Mode vl_return_mode_ = Mode::Browse;
    void video_link_show_current(const fs::path& track);
    void video_link_open(const fs::path& target, Mode return_mode);
    void video_link_close();
    void video_link_rebuild();
    void video_link_do();
    void video_link_key(int key);
    int video_link_list_rows() const;
    std::string video_link_label(const fs::path& p) const;
    std::vector<std::string> build_video_link_panel(int W) const;
    // SEARCH YT
    std::string vs_query_, vs_query_for_, vs_msg_;
    std::vector<OnlineResult> vs_results_;
    std::future<std::vector<OnlineResult>> vs_future_;
    bool vs_busy_ = false, vs_warn_ = false, vs_by_tags_ = false;   // vs_by_tags_: the query is "artist title" (CTRL+t), else the file name
    int vs_pane_ = 0, vs_sel_ = 0;
    fs::path vs_target_;                 // the song the download is for
    bool vs_mp3_ = true;                 // SEARCH YT: also make an mp3 of the sound (CTRL+a); on by default when no song is the target
    using VideoDownload = app_types::VideoDownload;
    VideoDownload vd_;
    fs::path video_down_dir() const;     // <download folder>/video_down
    std::string video_search_default_query() const;
    void video_search_start();
    void video_search_close();
    void video_download_start(const OnlineResult& r);
    void video_search_poll();
    std::vector<std::string> video_search_preview(const OnlineResult& r, int cols, int rows) const;
    void video_search_open();
    void video_search_open(const std::string& query);   // /sv:query
    void video_search_key(int key);
    std::vector<std::string> build_video_search_panel(int W) const;
    // FETCH VIDEOS (ALT+v, src/app_videofetch.cpp): a full-screen overlay like the cover fetch menu: the songs on the left, the videos found
    // for the hovered song (to choose from) and the preview of the hovered video on the right.
    using VFEntry = app_types::VFEntry;
    std::unordered_map<std::string, VFEntry> vf_saved_;   // what video_fetch_state.json holds (songs that are not listed at the moment too)
    bool vf_saved_loaded_ = false;
    struct VFJob { fs::path track; std::string label, base, video_id, title; };
    std::vector<VFEntry> vf_all_;
    std::vector<int> vf_view_;
    std::string vf_query_, vf_msg_;
    int vf_focus_ = 1, vf_sel_ = 0, vf_cand_sel_ = 0, vf_src_ = 0, vf_filter_ = 0;   // vf_filter_: 0 missing video only, 1 all songs, 2 songs with a video, 3 the songs with a chosen video; focus: 0 filter field, 1 songs, 2 videos; vf_src_: 0 file name + tags, 1 file name, 2 tags
    mutable int vf_page_rows_ = 10;
    mutable int vf_art_col_ = 0;                         // the screen column where the right pane starts (the preview picture)
    bool vf_warn_ = false;
    std::string vf_confirm_;                               // the song whose re-fetch waits for the confirmation
    std::string vf_hover_;                                // the song the cursor rests on (for the automatic search)
    std::chrono::steady_clock::time_point vf_hover_since_{};
    std::deque<std::string> vf_queue_;                     // songs waiting for their search
    std::string vf_last_queries_;                          // the texts the running / last search used (shown when it finds nothing)
    std::string vf_searching_;                             // the song whose search runs
    std::future<std::vector<OnlineResult>> vf_search_;
    mutable std::string vf_pic_key_;
    mutable int vl_marq_[6] = {-1, -1, -1, -1, -1, -1};     // marquees of the VIDEO OPTION panel: song row, video row, locked song, locked video, 'now' line, folder line
    mutable std::chrono::steady_clock::time_point vl_marq_since_[6]{};
    mutable int vf_marq_ = -1;                             // marquee of the song under the cursor (restarts when the cursor moves)
    mutable std::chrono::steady_clock::time_point vf_marq_since_{};
    using VideoFetchRun = app_types::VideoFetchRun;
    VideoFetchRun vf_;
    // The second tab of FETCH VIDEOS: the VIDEO FILES (src/app_videomanage.cpp): every video file of the library (and every video a song is connected to),
    // which songs use it, delete one for good. Filters: all / coupled with a song / video only (no song uses it).
    using VMEntry = app_types::VMEntry;
    std::vector<VMEntry> vm_all_;
    std::vector<int> vm_view_;
    std::string vm_query_;
    int vf_tab_ = 0;                                       // 0 SONGS AND VIDEOS, 1 VIDEO FILES (vf_focus_: 0 filter field, 1 list)
    int vm_sel_ = 0, vm_filter_ = 0;                       // vm_filter_: 0 all videos, 1 coupled with a song, 2 video only
    std::string vm_confirm_;                               // the video whose deletion waits for the confirmation
    std::future<std::pair<std::string, std::string>> vm_mp3_fut_;   // an mp3 being made from a video file: (mp3 path, error)
    std::string vm_mp3_video_;                             // ... and the video it comes from
    void video_manage_make_mp3();                          // CTRL+3 (or m) in the VIDEO FILES tab: the sound of a video-only file as an mp3 song
    mutable std::string vm_pic_key_;
    void video_manage_rebuild();
    void video_manage_refilter();
    void video_manage_key(int key);
    std::vector<std::string> build_vm_list_panel(int w, int h) const;
    std::vector<std::string> build_vm_detail_panel(int w, int h) const;
    // Deletes a video file for good: its connections (VideoLinks) go first, the library is read again. Returns false (and says why in `msg`) when the file stays.
    bool video_delete_file(const std::string& video, std::string& msg, bool undoable = true);
    struct VDelUndo { std::string orig, held; std::vector<std::string> tracks; };   // a deleted video: where it was, where it waits, the songs that showed it
    std::vector<VDelUndo> vdel_undo_;
    fs::path video_trash_dir() const;
    bool video_undelete(std::string& msg);                // brings the last deleted video (and its connections) back
    void video_trash_purge();                             // the held files go for good (at exit)
    void video_fetch_open(const fs::path& focus = fs::path());   // (focus: the song the cursor starts on, default the playing one)
    void video_fetch_close();
    void video_fetch_key(int key);
    void video_fetch_start(bool only_current = false);   // only_current: just the song under the cursor (its chosen video)
    void video_fetch_save_state();                       // the found videos and the choices go to video_fetch_state.json (kept until downloaded)
    void video_fetch_refilter_keep();                    // refilter, the cursor stays on its song
    void video_fetch_poll();
    void video_fetch_rebuild_list();
    void video_fetch_refilter();
    void video_fetch_cancel_and_join();
    void video_fetch_enqueue(const std::string& path, bool front);
    void video_fetch_clear_searches();
    int video_fetch_nav(int selected, int total, bool up) const;
    const VFEntry* video_fetch_hovered() const;
    void build_vf_screen(std::ostringstream& frame, int W, int target_height) const;
    std::vector<std::string> build_vf_songs_panel(int w, int h) const;
    std::vector<std::string> build_vf_choice_panel(int w, int h) const;
    mutable std::chrono::steady_clock::time_point video_away_since_{};   // since when the field is not on the video view     // size / place of the picture last built
    std::string video_source_for(const fs::path& track) const;
    // For a video file: the song it is paired with (a link, else the song of the same file name in the library); "" for anything else. A linked song
    // that is gone is still returned: the cover choices made for it keep serving the video.
    std::string paired_song_for_video(const std::string& video) const;
    void video_toggle();
    // The video of a track as it is shown: video_source_for() unless the track's Style Option hides it; its size and effect (the track's own, else the VIDEO overlay's).
    std::string video_shown_for(const fs::path& track) const;
    int video_size_for(const fs::path& track) const;
    bool video_lyrics_for(const fs::path& track) const;   // Use Lyrics on Video (the lyric line in the video view): the track's own, else the VIDEO overlay
    bool video_round_for(const fs::path& track) const;   // Rounded Corners of the picture: the track's own, else Settings
    int video_fx_for(const fs::path& track) const;
    VideoLook video_look_for(const fs::path& track) const;   // the effect, contrast, colours and chromatic aberration of the track's video (its own choices, else Settings)
    int video_pic_width() const;
    int video_res_for() const;   // the resolution of the playing track's video: its own (Style Option / big video panel), else the Setting   // the widest picture that is decoded (Settings -> Video Resolution; never wider than the video itself unless standard)
    void video_ensure_source() const;
    // The lyric block of the big video overlay: `mode` 1 lyrics, 2 lyrics + translation, 3 translation only; `tr_idx` picks the translation.
    std::vector<std::string> video_lyric_rows(int width, double elapsed, int mode, int tr_idx, bool all_shown = false) const;
    int video_lyr_mode_for(const fs::path& track) const;   // Include Lyrics / Use Lyrics on Video: 0 none, 1 lyrics, 2 lyrics + translation, 3 translation only (the track's own, else the VIDEO overlay)
    // The saved translations of the lyrics that play, aligned to `cur` (one entry per lyric line, empty where a line has none). They come from the
    // lyrics file that was edited last (of those that have a translation); the one for Translate To comes first. Cached per track.
    struct VTr { std::string label; std::vector<std::string> lines; };
    const std::vector<VTr>& video_translations(const std::vector<LyricLine>& cur) const;
    mutable int vfull_tr_idx_ = 0;      // the big video overlay: which translation TAB has reached
    mutable int video_need_rows_ = 2;   // the rows the lyric block of the small field needs at most on this track (see video_field_rows)
    mutable std::string video_need_key_;
    mutable std::string vtr_key_;
    mutable std::vector<VTr> vtr_all_;
    mutable std::chrono::steady_clock::time_point vtr_at_{};   // when the lookup was made (an empty result is looked up again after a few seconds: a translation may have been saved meanwhile)
    void clap_loading_fill(std::vector<std::string>& rows, int W, int H, int abs_col, int abs_row, GfxFrame& g, std::string& key_ref, int id) const;   // the small clapperboard while a video loads (picture or braille)
    bool video_draw(std::vector<std::string>& rows, int W, int H, int abs_col, int abs_row, bool second_slot, double t, int mode,
                    const std::vector<std::string>& ov, int ov_w, bool flush_bottom = false, bool trim_right = false, VideoPlayer* vp = nullptr, const fs::path* look_track = nullptr) const;
    // The Style Option preview of a track that is not the playing one: a decoder of its own (stopped when the overlay is closed)
    mutable VideoPlayer sty_video_;
    mutable std::string sty_video_src_;
    mutable std::chrono::steady_clock::time_point sty_video_t0_{}, sty_video_seen_{};
    std::string build_video_full(int W, const char* clear_prefix) const;
    bool video_full_key(int key);
    // The layout of the full screen video (SHIFT+UP / DOWN): video, video + oscilloscope, video + oscilloscope + spectrogram. The
    // oscilloscope / spectrogram options opened from there show (and change) the layout's own style / mode / view (Settings::vl_*);
    // layout_osci_view() / layout_spectro_view() are the shared settings with these laid over them (what the layout draws).
    bool video_layout_menu() const { return video_full_ && settings_.video_layout > 0; }
    static bool vl_has_osci(int layout) { return layout == 1 || layout == 3; }
    static bool vl_has_spectro(int layout) { return layout == 2 || layout == 3; }
    VizPrefs layout_osci_view() const;
    SpectroSettings layout_spectro_view() const;
    std::vector<int> osci_menu_rows() const;
    mutable SpectroSettings layout_spectro_;   // the spectrogram this frame draws in the layout (while layout_spectro_on_)
    mutable bool layout_spectro_on_ = false;
    // The options panel inside the full screen video (SHIFT+s): the track's own effect, contrast, colours and chromatic aberration, the same
    // choices (and storage) as the video rows of the Style Option overlay.
    bool video_opts_open_ = false;
    int video_opts_sel_ = 0;
    bool video_opts_pair_ = false;   // the playing track is a song paired with a video: the row "Use mp4 audio for this song" is listed
    bool song_paired_with_video(const fs::path& track) const;   // not a video itself, the Video mode is on and a video belongs to it
    static constexpr int kVideoOptsPanelWidth = 66;
    std::vector<std::string> build_video_opts_panel() const;
    void video_opts_change(int row, int dir, bool reset);
    void video_full_toggle();
    void video_set_size(int size);
    void video_set_fx(int fx);
    void video_track_fx_step(int dir);   // the next / previous effect of this track (Style Option's "Video Effect")
    bool video_full_ = false;   // HKeyVideoFull: the video over the whole screen
    bool video_field_rows(std::vector<std::string>& rows, int W, int H, int area_col, bool second_slot, double t, const std::vector<std::string>& line_rows, bool trim_right = false) const;
    long long added_time_of(const fs::path& p) const;   // file_added_time(), looked up once per file (cleared on rescan)
    mutable std::mutex added_time_mutex_;
    mutable std::unordered_map<std::string, long long> added_time_cache_;   // mode 3: when each file arrived (looked up once per rescan / choice of the mode)
    static constexpr int kListVisibleRows = 8; // initial list height before the first frame; from then on the list/queue panes take ALL the rows the terminal leaves (render_frame()), so a maximised window is filled instead of ending in blank lines

    // --- terminal-height awareness --------------------------------------
    // The frame is sized to the terminal, so it never scrolls. term_rows_ is the current terminal
    // row count, read on every frame in render_frame(). list_visible_rows_ is how many list and queue
    // rows fit in the room that is left after the fixed parts (metadata, progress bar, search bar,
    // status line). It can grow with a maximised window and has no upper cap. Anything that scrolls
    // or sizes a list uses it, so what is drawn and what the scroll math assumes stay the same.
    // render_frame() also clamps the line count (clamp_output_rows()), so even a very short terminal
    // cannot scroll the screen.
    int term_rows_ = 24;
    int list_visible_rows_ = kListVisibleRows;

    // --- list overlay (HKeyListOverlay, SHIFT+l) -------------------------
    // A large floating copy of the list pane (LOCAL AUDIO FILES when the
    // local library is showing), drawn over the main UI like the Retry
    // Lyrics form. It is NOT a Mode: it stays a plain flag on top of
    // Browse/Search so every list command (play, sort, folder filter,
    // search, queue add, ...) keeps working through the normal key
    // dispatch, and it shares selected_/scroll_ with the small pane so
    // both always show the same cursor. Only the number of rows differs,
    // which is what list_nav_rows() abstracts -- every scroll-follows-the-
    // cursor calculation asks it instead of using list_visible_rows_ directly.
    bool list_overlay_open_ = false;
    int overlay_list_rows_ = kListVisibleRows; // list rows the overlay fits; recomputed every frame
    static constexpr int kListOverlayChromeRows = 5; // search bar (3) + list box top/bottom border (2)
    // The key legend sits BELOW the frame as separate gray lines (like the
    // playlist / meta editor footers) and takes rows away from the list so
    // the overlay never grows past the terminal (term_rows_ - 2 rows, from
    // row 2): geometry and painter both ask this. Entries are never split
    // across two lines.
    std::vector<std::string> list_overlay_legend(int panel_w) const;
    bool list_overlay_active() const; // open AND in a mode that shows the main UI underneath it
    int list_nav_rows() const { return list_overlay_active() ? overlay_list_rows_ : list_visible_rows_; }
    void list_overlay_geometry(int W, int& panel_w, int& list_rows) const;
    void list_overlay_fit_scroll(int rows); // keep selected_ inside a `rows`-row window and the window inside the list
    void list_overlay_open();
    void list_overlay_close();
    void list_overlay_page(int dir); // dir=-1 page up, +1 page down
    std::vector<std::string> build_list_overlay_panel(int panel_w, int list_rows) const;
    std::string clamp_output_rows(const std::string& frame, int term_rows) const;

    // --- folder overlay (HKeyFolderOverlay, SHIFT+f) ---------------------
    // A big floating folder browser: a search bar and the LOCAL AUDIO FILES FOLDER pane, a tree of the library's
    // folders. ENTER opens / closes a folder (its subfolders and files appear slightly indented below it); `f` on a
    // folder or on a file does what `f` does in the list (show that folder's files in the LOCAL AUDIO FILES pane,
    // folder order) and closes the overlay. Like the list overlay it is a plain flag on top of Browse; it owns the
    // keys it needs and lets the playback keys fall through.
    using FoRow = app_types::FoRow;
    bool folder_overlay_open_ = false;
    bool fo_search_ = false;                 // the search field has the keyboard
    std::string fo_query_;
    int fo_sel_ = 0, fo_scroll_ = 0;
    int fo_rows_ = kListVisibleRows;         // list rows the overlay fits; recomputed every frame
    std::set<std::string> fo_expanded_;
    std::set<std::string> fo_marked_;          // audio book mode: files (full paths) that carry a bookmark
    std::vector<FoRow> fo_view_;             // the visible rows (tree walk + search filter)
    std::map<std::string, std::vector<std::string>> fo_subdirs_;   // folder -> sorted subfolders
    std::map<std::string, std::vector<std::string>> fo_files_;     // folder -> sorted files
    std::map<std::string, int> fo_count_;                          // folder -> tracks at or below it
    std::vector<std::string> fo_roots_;

    // --- file conversion overlay (HKeyConvertOverlay, SHIFT+c) -----------
    // A big floating window to turn high resolution files into MP3 / AAC / Opus: a search bar, a pre-filter (type,
    // minimum bitrate, hi-res only), a list of the library's files with checkboxes, and the target preset. The marked
    // files (or just the hovered one) are converted by a background ffmpeg job into the CONVERSION EXPORT PATH; the
    // originals are never touched. Like the other big overlays it is a plain flag on top of Browse.
    using CvFile = app_types::CvFile;
    bool convert_overlay_open_ = false;
    bool cv_search_ = false;
    std::string cv_query_;
    int cv_sel_ = 0, cv_scroll_ = 0;
    int cv_rows_ = kListVisibleRows;          // file rows the overlay fits; recomputed every frame
    int cv_type_ = 1;                         // pre-filter: 0 all, 1 lossless, 2 FLAC, 3 WAV, 4 M4A, 5 OGG, 6 OPUS, 7 AAC, 8 MP3
    int cv_minkbps_ = 0;                      // pre-filter: index into the minimum-bitrate steps (0 = any)
    bool cv_hires_ = false;                   // pre-filter: only more than 16 bit or 48 kHz
    int cv_preset_ = 0;                       // target (convert_preset())
    std::vector<CvFile> cv_files_;            // every library file outside the export folder
    std::vector<int> cv_view_;                // indices into cv_files_: what the filters + the search leave
    std::set<std::string> cv_marked_;
    // Search bar modes: 0 = files ("/"), 1 = folders ("/f:"), 2 = playlists ("/p:"). In the last two the list shows the
    // folders / playlists matching the query; ENTER on one narrows the file list to its files (the "scope").
    int cv_pick_ = 0;
    std::vector<FolderSummary> cv_folders_;
    std::vector<PlaylistSummary> cv_playlists_;
    bool cv_scope_ = false;
    std::string cv_scope_label_;             // "folder Rock" / "playlist Road trip"
    std::set<std::string> cv_scope_paths_;   // the files of the scope (UTF-8 paths)
    mutable std::mutex cv_mu_;
    std::map<std::string, AudioInfo> cv_info_;   // probe results (guarded by cv_mu_)
    std::thread cv_probe_th_;
    std::atomic<bool> cv_probe_stop_{false};
    std::atomic<int> cv_probe_done_{0};       // probes finished since the overlay opened
    int cv_probe_seen_ = -1;                  // what the last view rebuild saw
    int cv_probe_total_ = 0;                  // files the probe thread was asked for
    int cv_last_done_ = -1;                   // conversion progress last reported on the status line
    Converter converter_;
    bool convert_overlay_active() const;
    void convert_overlay_open();
    void convert_overlay_close();
    void convert_overlay_load_files();
    void convert_overlay_probe_restart();
    void convert_overlay_rebuild_view();
    void convert_overlay_start();
    void convert_overlay_rebuild_picks();     // the folder / playlist list of the "/f:" / "/p:" modes
    void convert_overlay_pick();              // ENTER on a folder / playlist row: narrow the file list to it
    int convert_overlay_total() const;        // rows of whatever the list shows right now (files, folders or playlists)
    void convert_overlay_poll();              // once per frame: re-filter when probes arrived, report progress
    bool convert_overlay_key(int key);        // true = consumed
    void convert_overlay_geometry(int W, int& panel_w, int& rows) const;
    std::vector<LegendRow> convert_overlay_legend(int panel_w) const;
    std::vector<std::string> build_convert_overlay_panel(int panel_w, int rows) const;
    std::string conversion_export_dir() const;
    AudioInfo cv_info_of(const std::string& path) const;
    bool folder_overlay_active() const;
    void folder_overlay_open();
    void folder_overlay_close();
    void folder_overlay_build_tree();
    void folder_overlay_rebuild_view();
    bool folder_overlay_key(int key);        // true = consumed
    void folder_overlay_filter();            // `f`: show the hovered folder / the folder of the hovered file in the list
    void folder_overlay_geometry(int W, int& panel_w, int& rows) const;
    std::vector<std::string> folder_overlay_legend(int panel_w) const;
    std::vector<LegendRow> folder_overlay_book_legend(int panel_w) const;   // the tree-table legend of the audio book library overlay
    std::vector<std::string> build_folder_overlay_panel(int panel_w, int rows) const;

    // --- queue overlay (HKeyQueueOverlay, SHIFT+k) -----------------------
    // The same idea as the list overlay above, for the QUEUE pane: a large
    // floating copy drawn over the main UI. Also a plain flag (not a Mode),
    // so every queue command (move, remove, bulk add, clear, playback keys)
    // keeps working through the normal key dispatch; it shares
    // queue_selected_/queue_scroll_ with the small pane and only the row
    // count differs (queue_nav_rows()). The queue gets focus while it is
    // open and gets its previous focus back on close. The two overlays are
    // mutually exclusive: opening one closes the other.
    bool queue_overlay_open_ = false;
    // The search bar of the big queue overlay (at the bottom, under the queue box): the rows that match are marked and the
    // cursor jumps to them. The queue keeps its order (it is an order), so nothing is hidden.
    std::string queue_find_;
    bool queue_find_edit_ = false;                 // the caret is in the bar
    static constexpr int kQueueSearchRows = 3;     // the bar's box
    bool queue_find_match(const std::string& query, size_t idx) const;
    // Every tag of the queue's local files (also the ones no screen shows), read by ffprobe in the background once the search
    // starts; queue_find_match() looks in it as soon as a file's tags are in.
    void queue_all_tags_resolve();
    mutable std::unordered_map<std::string, std::string> queue_all_tags_;   // path -> all tag values (guarded by row_meta_mutex_)
    std::unordered_set<std::string> queue_all_tags_asked_;                   // paths already handed to the thread (UI thread only)
    std::atomic<bool> queue_all_tags_busy_{false};
    void queue_find_jump(int dir);                 // dir 0: the first match from the cursor on, 1 next, -1 previous (wraps)
    int queue_find_count() const;
    void queue_find_key(int key);
    bool queue_overlay_prev_focus_ = false;       // queue_focus_ before the overlay opened
    int overlay_queue_rows_ = kListVisibleRows;   // queue rows the overlay fits; recomputed every frame
    static constexpr int kQueueOverlayChromeRows = 2; // queue box top/bottom border
    // The menus index (HKeyMenus, "<"): a floating panel listing every menu / overlay by category with its key; ENTER opens one.
    int menu_index_sel_ = 0;                          // the highlighted node of the organigram (organigram_trees.h)
    std::string build_menu_index_full(int window_w) const;   // the organigram over the whole screen
    std::vector<std::string> build_menu_index_classic_panel(int window_w, int& panel_w) const;   // the classic two-column view: a panel of its own size
    org::Theme menu_index_theme() const;
    void menu_index_key(int key);
    // The legend of a big overlay hangs below its frame. With more than one row the block is centred on the main screen's bottom
    // border line (3 rows: the second row lies on it), so it reaches this many rows further down than a one-row legend would.
    static constexpr int overlay_legend_shift(int rows) { return rows > 1 ? (rows - 1) - rows / 2 : 0; }
    std::vector<LegendRow> queue_overlay_legend(int panel_w) const; // see list_overlay_legend()
    bool queue_overlay_active() const; // open AND in a mode that shows the main UI underneath it
    int queue_nav_rows() const { return queue_overlay_active() ? overlay_queue_rows_ : list_visible_rows_; }
    void queue_overlay_geometry(int W, int& panel_w, int& queue_rows) const;
    void queue_overlay_open();
    void queue_overlay_close();
    void queue_overlay_page(int dir); // dir=-1 page up, +1 page down
    std::vector<std::string> build_queue_overlay_panel(int panel_w, int queue_rows) const;

    mutable std::mutex row_meta_mutex_;
    // Videos of the library that are paired with a track show that track's artist / title / album / year in the lists while the
    // video has none of its own (guarded by row_meta_mutex_): what was copied into row_meta_cache_, so it can be taken out again.
    std::unordered_map<std::string, RowMeta> borrowed_tags_;
    uint64_t bv_ver_ = 0, bv_rev_ = 0;
    size_t bv_n_ = 0;
    bool bv_mode_ = false;
public:
    void borrow_video_tags();
private:
    std::unordered_map<std::string, RowMeta> row_meta_cache_;
    std::atomic<bool> row_meta_resolver_started_{false};
    void launch_row_meta_resolver();
    // Reads the tags of the playlist editor's tracks that are not cached yet
    // (files outside the library sweep, e.g. a download folder) in the background.
    void resolve_playlist_tracks_async();
    // Bumped by launch_row_meta_resolver()'s background thread every time it
    // resolves a new file's real tags (artist/title/album), so a search view
    // computed BEFORE that file's tags arrived can be told "something
    // changed, re-filter" instead of sitting stale until the user retypes
    // their query. See poll_pending_row_meta_tags() -- same
    // background-thread-finished-do-something-on-the-main-thread pattern as
    // poll_pending_waveform()/poll_pending_search() just below it.
    std::atomic<uint64_t> row_meta_tags_version_{0};
    uint64_t row_meta_tags_seen_ = 0; // main-thread only, no atomic needed
    std::chrono::steady_clock::time_point row_meta_refilter_at_{};   // throttle of poll_pending_row_meta_tags()
    // Seeking with the arrow keys lights the << / >> labels of the buttons in BORDER_COLOR BOTTOM (build_progress_panel) for a moment.
    std::chrono::steady_clock::time_point seek_flash_until_{};
    int seek_flash_dir_ = 0;                                           // -1: the << button, 1: the >> button
    void seek_flash(int dir);                                          // called by the seek keys
    void poll_pending_row_meta_tags();

    // --- queue ---
    std::vector<QueueItem> queue_;
    int queue_selected_ = 0;   // cursor/"hovering" row, only meaningful once queue_focus_ has been used
    int queue_scroll_ = 0;
    bool search_nav_moved_ = false;   // Up/Down used in the search box: ENTER then plays the highlighted entry
    bool queue_focus_ = false; // Tab toggles which panel Up/Down navigates

    // "!" (HKeyQueueLock): a LOCKED queue (the default) keeps its tracks when
    // they are played: the played track moves to the END of the queue (also
    // for "n"), so the queue loops instead of draining. UNLOCKED, a played
    // track leaves the queue. The head is always what plays next (or a random
    // item in Shuffle). "d" and Shift+X still remove tracks: locking only
    // stops tracks from disappearing by themselves.
    bool queue_locked_ = true;
    // Number of tracks "a" (add as NEXT) has put in front of everything else
    // since the queue head last moved, so pressing "a" on A, B, C plays them
    // as A, B, C rather than C, B, A. Reset whenever the head moves / the
    // queue is reshuffled by hand.
    int queue_next_run_ = 0;
    // What Shift+X cleared, so Ctrl+Shift+Z can bring it back (one level).
    std::vector<QueueItem> queue_undo_;
    void queue_toggle_lock();
    void queue_add_selected_end();       // "e": hovering track to the END of the queue
    std::vector<QueueItem> queue_shuffle_undo_;   // the order before the last shuffle (one level)
    int queue_shuffle_undo_sel_ = 0;
    bool queue_playing_is_last() const;  // locked queue: the track that plays now sits at its end and stays there
    void queue_undo_shuffle();           // SHIFT+u in the big queue overlay: the order from before the shuffle
    void queue_shuffle_order();          // SHIFT+s in the big queue overlay: the queue gets a random order
    void queue_move_to_edge(int dir);    // Shift+4 / Shift+5: dir=-1 top, +1 bottom
    void queue_reset_lap(); // clears every item's "played in this pass" flag (queue-then-stop)
    void queue_after_move(int from, int to); // a manual move just ends the current "a" run
    void queue_undo_clear();             // Ctrl+Shift+Z
    // Ctrl+Shift+U asks first: a NEW playlist (editor, name field) or an EXISTING one (search + list of the saved
    // playlists); the queue's tracks are added to it and the editor opens, where the change still has to be saved.
    std::vector<PlaylistTrack> queue_local_tracks(int& skipped) const;
    void queue_to_playlist_apply(const std::string& existing);   // existing empty = a new playlist
    void qtp_refresh();
    int qtp_stage_ = 0;                  // 0 = new / existing, 1 = pick the existing playlist
    int qtp_choice_ = 0;                 // 0 = New playlist, 1 = Existing playlist
    std::string qtp_query_;
    std::vector<PlaylistSummary> qtp_view_;
    int qtp_selected_ = 0;
    static constexpr int kQtpPanelWidth = 56;
    static constexpr int kQtpListRows = 8;
    std::vector<std::string> build_queue_to_playlist_panel() const;
    void queue_to_playlist();            // Ctrl+Shift+U: queue -> playlist editor (name field)
    // SHIFT+a: the highlighted track of the library pane -> a playlist, with the same overlay (qtp_track_ = true). A
    // new playlist asks for its name (stage 2); the playlist is saved at once, the playlist editor does not open. An
    // online track (YouTube / SoundCloud / Bandcamp) is downloaded into the download folder first, in the background,
    // and then added as the local file it has become.
    void track_to_playlist();
    void track_to_playlist_apply(const std::string& playlist);   // the chosen / typed playlist name
    void atp_add_file(const std::string& playlist, const std::string& title, const std::string& artist, const fs::path& file);
    void poll_pending_atp();
    bool qtp_track_ = false;
    std::string qtp_new_name_;           // stage 2: the name of a new playlist
    struct AtpTrack { std::string title, artist, video_id; fs::path path; bool is_local = true; };
    AtpTrack atp_track_;                 // the track chosen with SHIFT+a
    struct AtpJob { bool ok = false; std::string error, playlist, title, artist; fs::path file; };
    std::mutex atp_mutex_;
    AtpJob atp_done_;
    std::atomic<bool> atp_busy_{false};  // a download for a playlist is running
    std::atomic<bool> atp_ready_{false}; // ... and has finished (atp_done_ holds the result)
    std::string hotkey_text(const char* action, const char* fallback) const; // bound key, as shown in a legend

    // --- playlists (local-files-only; see playlist_manager.h) -----------
    // Main UI: results of a "/p:" search (list_source_==Playlist), i.e.
    // browsing saved playlists the same way "/s:" browses online results.
    // Selecting one and hitting Enter queues every (non-missing) track it
    // contains -- see playlist_add_selected_to_queue().
    std::unique_ptr<PlaylistState> playlist_;   // the playlist editor's state (src/playlist_state.h) -- allocated in the constructor
    std::string last_playlist_query_;
    std::vector<PlaylistSummary> filter_playlists(const std::string& query) const;

    // Main UI: results of a "/f:" search (list_source_==Folder). Enter on a
    // row opens that folder in the LOCAL AUDIO FILES pane -- it sets
    // folder_filter_ (exactly what 'f' does) and switches back to the local
    // list, so every file of the folder is listed.
    std::vector<FolderSummary> folder_view_;
    std::string last_folder_query_;
    std::vector<FolderSummary> filter_folders(const std::string& query) const;
    void open_selected_folder();
    // First configured PlaylistsPath (settings_.playlists_paths[0]) if the
    // user set one (config.txt's PlaylistsPath=), else
    // local_music_paths[0]/playlists -- this is the folder NEW playlists
    // are written to and deleted from.
    fs::path playlists_dir() const;
    // Every configured PlaylistsPath (all of them are searched when
    // listing/loading playlists); just {playlists_dir()} when none is set,
    // so every caller has at least one folder to look in.
    std::vector<fs::path> playlist_dirs() const;
    // Playlists found across playlist_dirs(), merged and de-duplicated by
    // name (the first folder containing a name wins) -- what both the
    // main "/p:" list and the playlist editor's manage tab show.
    std::vector<PlaylistSummary> playlist_summaries() const;
    // Loads a playlist by name from whichever playlist_dirs() folder has
    // it; std::nullopt if none does.
    std::optional<Playlist> load_playlist(const std::string& name) const;
    void relocate_missing_tracks(Playlist& pl) const;
    void refresh_playlists_after_rescan();
    void playlist_add_selected_to_queue();

    // --- playlist editor overlay (Mode::Playlist, HKeyPlaylist) ----------
    // Two tabs: 0 = create/edit (name field + a local-library picker to
    // fuzzy-search/add from + the in-progress track list), 1 = browse
    // saved playlists (Enter loads one into tab 0 for re-editing).
    // HOME saves (deliberately not a plain letter -- "s" collided with
    // typing an "s" into the name/search fields). ESC exits; if tab 0 has
    // unsaved changes it asks first (playlist_->confirm_exit) rather than
    // silently discarding them.
    std::vector<LocalTrack> filter_local_or_folders(const std::string& query, int pane_media = -1) const;   // search, or "/f:" folder search
    // What a list shows of the video files (the library always holds them, the Video mode only decides what is shown):
    // 0 audio = the audio files and the videos that have no audio file of their name beside them (their sound is the track),
    // 1 video + audio = everything, 2 video = the video files only. A "v:" / "x:" in front of a query sets it for that search.
    // `pane` is the SHIFT+v choice of a pane (-1 = not chosen: the Video mode decides, a search lists "audio").
    int media_view_for(std::string& query, int pane, bool search_default_audio = true) const;
    void apply_media_view(std::vector<LocalTrack>& v, int view) const;
    bool video_is_paired(const fs::path& video) const;   // an audio file of the same name beside it, or a link made by hand
    static const char* media_view_name(int view) { return view == 2 ? "video" : view == 1 ? "video + audio" : "audio"; }
    mutable std::unordered_set<std::string> paired_videos_;
    mutable size_t paired_n_ = 0;
    mutable unsigned paired_rev_ = 0;
    mutable bool paired_ok_ = false;
    int pl_media_ = -1;       // SHIFT+v in the playlist editor's library pane (-1 = the Video mode decides)
    int fo_media_ = -1;       // SHIFT+v in the big folder overlay
    std::unique_ptr<MetaState> meta_;   // the meta data editor's state (src/meta_state.h) -- allocated in the constructor
    static const char* playlist_sort_name(int mode);
    // Tab 1 got a search box of its own (the same caret/selection/clipboard
    // treatment as every other text field -- see edit_text_key()). Focus
    // decides who owns the keys: in the box, typing filters and Left/Right
    // are caret keys; in the list, Up/Down/Enter/DEL work and Left/Right
    // keep switching tabs.
    // Export overlay ('e' on the Saved Playlists tab): writes the hovered
    // playlist as an M3U8 (default) or M3U file into a folder that starts as
    // Settings -> PATHS -> PLAYLIST EXPORT PATH and can be changed in place.
    void playlist_export_begin();          // opens the overlay for the hovered saved playlist
    void playlist_export_run();            // ENTER in the overlay
    std::vector<std::string> build_playlist_export_panel(int panel_w) const;

    // Import overlay ('i' on the Saved Playlists tab): reads the .m3u / .m3u8 files of a folder
    // (default: the same folder the export overlay starts in) and saves them as playlists of their own.
    void playlist_import_begin();
    void playlist_import_scan();                      // refills the file list from the folder field
    void playlist_import_run();                       // ENTER in the overlay
    std::vector<std::string> build_playlist_import_panel(int panel_w) const;
    std::vector<std::string> build_playlist_manage_search_panel(int width) const; // SEARCH PLAYLISTS

    void playlist_refresh_lib_view();
    void playlist_refresh_manage_view();
    void playlist_open_editor();  // HKeyPlaylist entry point -- resets to a blank new playlist on tab 0
    void playlist_load_into_editor(const std::string& name);
    void playlist_add_hovering_to_edit();
    // "/f:" in the library search: the hits are shown as folders (header rows, the files indented below an opened one)
    struct PlLibRow { bool header; int lib; std::string folder, name; int count; bool open; };
    static bool is_folder_query(const std::string& raw);
    std::vector<PlLibRow> playlist_lib_rows() const;
    void playlist_lib_move(int delta);
    void playlist_add_folder_to_edit();
    void playlist_undo_add();
    void playlist_remove_hovering_track();
    void playlist_move_hovering_track(int dir); // dir=-1 up, +1 down -- keys 4/5, mirrors queue_move_hovering
    void playlist_delete_selected(); // tab 1's DEL, after playlist_->confirm_delete confirms
    void playlist_save_current(bool leave = false); // leave: also close the editor (only the "save before exiting?" prompt does)
    void handle_playlist_key(int key);
    void build_playlist_screen(std::ostringstream& frame, int W, int player_h) const;
    std::vector<std::string> build_playlist_search_panel(int width) const;
    std::vector<std::string> build_playlist_library_panel(int width, int height) const;
    std::vector<std::string> build_playlist_tracks_panel(int width, int height) const;
    std::vector<std::string> build_playlist_manage_panel(int width, int height) const;


    // --- cover fetch overlay (Mode::CoverFetch, HKeyCoverFetch = ALT+c) ---
    // An overlay of its own (and not a tab of the meta editor) because MusicBrainz often misjudges album titles: fetching covers
    // should be a deliberate step. Tab 0 = ALBUM LIBRARY (a search bar, the folders of the library with the albums in them, one row
    // per album name, missing covers highlighted) and the cover of the highlighted album with the pictures to choose from;
    // tab 1 = the FETCH LIST, fetched through scripts/fetch_cover.py (MusicBrainz + Cover Art Archive). A fetched cover is saved in
    // a "fetched_covers" folder inside every folder that holds the album. The album's default picture chosen here is overruled by
    // the choice for a single track in the meta editor (cover_store.h).
    std::unique_ptr<CoverFetchState> cf_;   // the cover fetch overlay's state (src/coverfetch_state.h) -- allocated in the constructor
    bool cover_full_picture_img(const CoverImage* img, int pcols, int preview_rows, int scr_col, int scr_row, bool second = false) const;
    bool cover_full_picture(const std::string& pfile, bool pemb, int pcols, int preview_rows, int scr_col, int scr_row, bool second = false) const;
    // fetching
    // --- the Latin name lookup (Settings -> GENERAL -> TRANSLITERATION -> Latin Name Lookup): artists written in Chinese characters are asked
    // for on MusicBrainz in the background, one at a time (scripts/lookup_artist_name.py); the answers are kept in kanji_readings_learned.txt
    struct NameLookupJob { std::atomic<bool> done{false}; std::string name; NameLookupResult result; };
    std::shared_ptr<NameLookupJob> name_job_;
    std::chrono::steady_clock::time_point name_lookup_next_{};
    int name_lookup_fails_ = 0;
    bool name_lookup_broken_ = false;           // no Python / no script: not tried again in this session
    fs::path name_lookup_script_;
    void poll_name_lookup();
    // --- Japanese kanji read by pykakasi (optional package, scripts/kanji_reading.py), in batches, while the Latin letters are on
    struct KanjiJob { std::atomic<bool> done{false}; std::vector<std::string> texts; KanjiReadResult result; };
    std::shared_ptr<KanjiJob> kanji_job_;
    std::chrono::steady_clock::time_point kanji_next_{};
    bool kanji_broken_ = false;
    int kanji_fails_ = 0;
    fs::path kanji_script_;
    void poll_kanji_reader();
    // the cover of the highlighted album and the pictures to choose from, made again when the highlight or the store changes
    void cf_open();
    void cf_close();
    void cf_refresh();                          // search again (starts the index worker)
    void cf_rebuild_rows();
    void cf_remember_selection();               // before cf_->folders is replaced: the rows index into it
    enum class CfPromptX { None };              // (placeholder, keeps the enum below readable)
    void cf_clear_album_title();                // SHIFT+d: the album tag of the highlighted album becomes a pending (empty) edit of the meta editor
    int cf_covers_of(const CfFolder& f, const CfAlbum& a) const;
    bool cf_album_shown(const CfFolder& f, const CfAlbum& a) const;   // the filter keeps it (and a failed fetch hides it)   // -1 = not known yet
    void poll_cover_fetch();
    void handle_cf_key(int key);
    void cf_add_album(const CfFolder& f, const CfAlbum& a);
    void cf_add_selected();
    void cf_add_view();
    void cf_start_fetch();
    void cf_delete_candidate();
    void cf_update_current() const;             // cf_->cur_src / cf_->cands for the highlighted album
    const CfAlbum* cf_selected_album(const CfFolder** folder = nullptr) const;
    int cf_nav(int selected, int total, bool up) const;
    void build_cf_screen(std::ostringstream& frame, int W, int target_height) const;
    std::vector<std::string> build_cf_library_panel(int width, int height) const;
    std::vector<std::string> build_cf_art_panel(int width, int height) const;
    std::vector<std::string> build_cf_list_panel(int width, int height) const;
    int cover_mode_look() const;                 // cover_mode with the ASCII modes (5, 6) folded onto 0, 1
    // The cover look of the playing track: its album's own look (COVER OPTIONS -> [l], cover_album_look()) or Settings -> Album
    // Cover / Cover Mode. album_look_override(): -1 none, 0 plain disk, 1..7 the Cover Modes (remembered per track and choice).
    int album_look_override() const;
    bool eff_album_cover() const;
    int eff_cover_mode() const;
    // The style of the playing track (Style Option in the meta editor): its own look / contrast / brightness / colours, else the album's
    // look, else Settings. Cached by path and cover store revision.
    int eff_cover_contrast() const;
    int eff_cover_brightness() const;
    int eff_cover_colors() const;
    int eff_cover_chroma() const;
    const CoverStyle& track_style() const;
    mutable std::string look_cache_path_;
    mutable unsigned look_cache_rev_ = 0;
    mutable int look_cache_val_ = -1;
    mutable CoverStyle style_cache_;
    // Sixel and Cover Colors > 0 in a Cover behind Disk look: the picture is drawn with a fixed colour palette (`direct`), see cover_fill()
    bool cover_sixel_colours(int mode) const { return gfx_proto_ == GfxProto::Sixel && eff_cover_colors() > 0 && mode >= 2 && mode <= 4; }
    void cycle_cover_look(int dir);              // CTRL+t / CTRL+z: the plain disk and the cover looks (audio books: default picture, cover with wave, cover)
    std::vector<std::string> cover_preview_lines_img(const CoverImage& img, int cols, int rows) const;   // a picture already in memory as braille dots
    std::vector<std::string> cover_preview_lines(const std::string& file, bool embedded, int cols, int rows, const std::string& msg = "") const;
    std::vector<std::string> album_cover_lines_for(const CoverImage& cover, int mode, int contrast, int brightness, int colors, double angle, int cols, int rows, int chroma = 0) const;   // any cover, any style (previews)

    // --- meta/tag editor overlay (Mode::MetaEdit, HKeyMetaEditor = Shift+M) ---
    // Deliberately shaped like the playlist editor above: same tab strip,
    // same boxed side-by-side panels, same hint/status footer, same Y/N
    // confirmation prompts in place of the hint line. Tab 0 = edit (a search
    // field, a library picker, and the five editable fields -- FILE plus
    // ARTIST/TITLE/ALBUM/YEAR -- for the row the picker is on); tab 1 = the
    // fetch list, filled with 'a' and run through AcoustID with Enter.
    //
    // Everything typed or fetched here goes into meta_->session -- a pending
    // edit session that is autosaved to disk after every change (meta_editor.h)
    // but is NEVER written to the audio files on its own. Ctrl+Shift+S applies
    // it, Ctrl+Shift+X throws it away, and simply leaving (ESC or quitting)
    // keeps the autosave backup, so no amount of editing can lose work by
    // accident.
    // 'r' in the library pane: park every file that carries a pending edit
    // at the top of the pane. Off by default, so merely opening the editor
    // never reorders the list behind the user's back -- see
    // meta_refresh_lib_view(), which does the sorting on every refresh.
    // Folder view of the FILE LIBRARY ([f]): the files grouped by folder, every folder a header row ("v folder (5)") with its files numbered
    // from 1 below it. The cursor is on a file (meta_->lib_selected) or on a folder header (meta_->folder_hover = the folder); [a] on a header
    // queues the folder's titles, ENTER opens / closes it.
    struct MetaFolderRow { bool header = false; int lib = 0; std::string folder, name; int num = 0, count = 0; bool closed = false; };
    bool meta_take_linked_tags(const std::string& video);   // a video paired with a track: its pending tags become the track's (false: nothing to take)
    std::vector<MetaFolderRow> meta_folder_rows() const;
    int meta_folder_cursor(const std::vector<MetaFolderRow>& rows) const;
    void meta_lib_move(bool up);
    void meta_add_folder_to_fetch(const std::string& folder);
    // Two-stage field editor (meta_->focus == 2): a hovered tag is only
    // highlighted until ENTER arms this, showing the caret in the CHANGE
    // colours (change_sgr()) -- the same split the PATHS tab uses between
    // a selected folder row and one actually being typed into. Reset
    // whenever focus leaves the field editor or moves to another tag.
    // The FETCH options (bit mask, see meta_editor.h): which tags a fetch fills in. Set on the FETCH LIST tab (its option
    // line in the header); one setting for the whole list and for SHIFT+b. Default: missing only.
    std::string meta_fetch_disclaimer() const;
    // Per path of the running batch: the tag fields the fetch was allowed to fill in (set when it starts).
    // --- the cover of a track in the meta editor: the picture under the fields and the "Cover Options" overlay ---
    // The choice made here is kept per track (cover_store.h) and goes before the album's default and before Settings -> Cover Search
    // (except "Within File Only").
    int order_swap_from_ = -1;       // Viz Order rows: the place the chosen view came from (the row that changed with it)
    int auto_prev_view_ = 3;        // Auto Video: the view before a track with a video took the field (0..2 a visual, 3 lyrics)
    bool auto_prev_set_ = false;
    // the main screen kept standing behind the Style Option / video search overlays (App::run); per App like everything else
    std::string frozen_body_, frozen_sig_;
    int frozen_cols_ = -1, frozen_rows_ = -1;
    // video_source_for(): the answer of the folder / library search per track (the meta panel asks for it every frame). Dropped when a
    // video connection or the library changes (video_rev_, track count) and after 2 seconds anyway (a file that just appeared).
    mutable std::mutex psong_mu_;   // paired_song_for_video(): the answer per video, kept for 2 seconds like vsrc_
    mutable std::unordered_map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> psong_;
    mutable std::mutex vsrc_mu_;
    mutable std::unordered_map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> vsrc_;
    mutable uint64_t vsrc_rev_ = ~0ull;
    mutable size_t vsrc_n_ = 0;
    std::chrono::steady_clock::time_point last_input_ = std::chrono::steady_clock::now();   // the last key (App::run: Reduce Motion When Paused)
    bool auto_prev_known_ = false;  // auto_prev_view_ was seen this run (a start on the video view has no earlier view)
    void meta_cov_open();
    void meta_cov_rebuild();
    bool handle_meta_cov_key(int key);
    std::vector<std::string> build_meta_cov_panel(int screen_w) const;   // (screen_w: where the floating panel will stand, for the picture)
    void meta_toggle_cover_res();
    std::vector<std::string> meta_cover_pane(int inner, int rows) const;   // the track's cover as braille lines, `rows` x `inner` columns
    static constexpr int kMetaCovPanelWidth = 86;
    // The "Style Option" overlay (next to the Cover Options button): the look and the Cover Contrast / Brightness / Colors of one track
    // (cover_store.h CoverStyle), and the look of its whole album. "As in Settings" = nothing of its own.
    // Style Option preview as a real picture: 0 = none this frame, 1 = it is in gfx_, 2 = in gfx2_ (over the main screen, whose own cover
    // picture keeps gfx_). The picture lies inside its own panel, so the panel must not crop it.
    mutable int vl_pic_ = 0;                                       // VIDEO OPTION: the preview of the connected video is a picture in gfx_ (1) / gfx2_ (2) inside the panel, 0: none
    void video_link_refresh_flags();
    void video_link_ask_delete(const std::string& video);
    mutable int vs_pic_ = 0;                                       // SEARCH YT: the thumbnail is a picture in gfx_ (1) / gfx2_ (2) inside the panel, 0: braille
    mutable std::string vs_pic_key_;
    mutable std::string sty2_key_;                                  // what the preview in gfx2_ shows
    mutable std::chrono::steady_clock::time_point sty2_at_{};
    void meta_sty_open();
    void meta_open_style_playing();     // HKeyStyleOptions: the Style Option overlay of the playing track
    bool handle_meta_sty_key(int key);
    std::vector<std::string> build_meta_sty_panel(int screen_w) const;
    int meta_sty_panel_width(int screen_w) const;
    std::vector<std::string> meta_sty_preview(int prev_w, int rows, const CoverStyle& st, int album_look, int screen_w) const;
    bool meta_sty_gfx(const CoverImage& cover, int mode, int contrast, int brightness, int colors, int chroma, double angle, int col, int row, int cols, int rows) const;   // the Style preview as a picture (gfx_)
    // While the Style overlay builds its picture, these replace the playing track's own cover style (eff_cover_*)
    mutable struct CoverOvr { bool on = false; int contrast = 0, brightness = 0, colors = 0, chroma = 0; } cover_ovr_;
    static constexpr int kMetaStyPanelWidth = 64;
    // The choice overlay (ENTER on a field with several fetched candidates).
    void meta_toggle_fetch_opt(int i);                 // tick / untick one FETCH option (the exclusivity rules apply)
    std::array<bool, kMetaFieldCount> meta_want_for(const std::string& path, unsigned opts);
    void meta_choice_open();
    void meta_choice_pick();          // ENTER: the highlighted candidate becomes the value
    void meta_choice_discard();       // DEL / d: drop the fetched value and every candidate
    bool handle_meta_choice_key(int key);
    std::vector<std::string> build_meta_choice_panel() const;
    static constexpr int kMetaChoicePanelWidth = 66;
    static constexpr int kMetaSavePanelWidth = 56;
    // Writing the pending edits to the files (CTRL+s -> Y). Every file is a copy through ffmpeg, so hundreds of edits take a
    // while: the work is done in slices from the render loop (poll_meta_save(), ~0.12 s per frame) instead of in one go, the
    // screen stays alive, a "please wait" panel with a progress bar shows up when it takes longer than a moment, and ESC
    // stops after the current file (what is not saved yet stays pending).
    void poll_meta_save();                                    // render loop: one slice of the job
    void meta_save_finish();                                  // lists, status line, log
    std::vector<std::string> build_meta_save_panel() const;   // the "please wait" panel
    // Y/N confirmation currently covering the footer (in the menu) or the
    // status line (in Browse, for Shift+B), plus the exact paths it will act
    // on once confirmed. Every key is swallowed while one is up.
    // AcoustID batch: runs on its own thread, publishes results under
    // meta_->fetch_mutex, applied to the session on the main thread by
    // poll_pending_meta_fetch() (called from the render loop).
    // Paths the current batch has already delivered successfully (filled in
    // by poll_pending_meta_fetch() on the main thread, so no lock needed).
    // On completion they leave the fetch list -- what stays queued is
    // exactly what still needs another try.
    // What the field editor panel draws for the currently hovered path.
    // Precomputed once per frame (meta_refresh_hover_values(), just before
    // rendering) because the current tag values need a mutable cache lookup,
    // while build_meta_screen() itself is const.

    void meta_open();                  // HKeyMetaEditor entry point
    void meta_ensure_session_loaded(); // restores the autosaved session, once
    void meta_refresh_lib_view();
    void meta_toggle_resort();         // 'r', edited files to the top of the pane
    std::string meta_filter_note() const;   // "  NOTE: [MISSING TITLE] filter active, ..." while a missing-tag filter is on
    void meta_toggle_sort_added();     // 'n', recently added files first
    // Missing-tag filters over the library pane: 0 = off, 1 = files with no
    // metadata at all ('x'), 2 = missing title (Shift+T), 3 = missing artist
    // (Shift+A), 4 = missing year (Shift+Y), 5 = missing album (Shift+L). Pressing the same key again
    // clears it -- see meta_toggle_filter().
    // Up / Down in a meta editor list: plain = one row, SHIFT = a page, CTRL = first / last. Returns the new row.
    int meta_nav(int selected, int total, bool up) const;
    void meta_toggle_filter(int filter);
    void meta_jump_to_current();   // SHIFT+c: hover the song that plays now, search and filters removed
    const char* meta_filter_label() const;
    void meta_persist();               // save (or delete) the autosave backup file
    const MetaEditEntry* meta_entry(const std::string& path) const;
    MetaEditEntry& meta_touch_entry(const std::string& path); // create on first edit
    void meta_set_field(const std::string& path, int field, const std::string& value);
    std::string meta_display_value(const std::string& path, int field, const MetaEditEntry* e);
    RowMeta meta_row_meta(const fs::path& path); // current tags of a file, cached + native
    std::string meta_hovering_path() const;      // path the picker/editor/fetch tab is on
    void meta_refresh_hover_values();            // fills meta_->hover_values for this frame
    void meta_add_hovering_to_fetch();           // 'a', mirrors the queue's add key
    void meta_cancel_fetch();                    // SHIFT+c on the Fetch List tab: stop the running AcoustID batch
    void meta_add_view_to_fetch();               // SHIFT+f: every title of the (filtered) library pane
    void meta_remove_hovering();                 // DEL/'d'
    void meta_play_hovering();                   // SHIFT+p: start the hovered track (library pane / fetch list)
    void meta_prompt_single_fetch(const std::string& path, bool all = false); // Shift+B: missing tags only; Shift+N (meta editor): all tags
    void meta_prompt_list_fetch();                          // Enter on tab 1
    void meta_start_fetch();
    void poll_pending_meta_fetch();
    void meta_apply_session();  // Ctrl+Shift+S, after Y
    void meta_discard_session();// Ctrl+Shift+X, after Y
    // True if a confirmation was up and the key was consumed by it (incl. the
    // swallow-everything-else case). Checked first thing in handle_key().
    bool handle_meta_prompt_key(int key);
    void handle_meta_key(int key);
    void build_meta_screen(std::ostringstream& frame, int W, int player_h) const;
    std::vector<std::string> build_meta_library_panel(int width, int height) const;
    std::vector<std::string> build_meta_fields_panel(int width, int height) const;
    std::vector<std::string> build_meta_fetch_panel(int width, int height) const;

    // --- listening history overlay (Mode::History, HKeyHistory = Shift+H) ---
    // Four tabs in the shape of the two overlays above: 1 HISTORY (the last
    // 100 plays, newest first), 2 TOP TRACKS (per title, sorted by play count
    // -- 'r' flips between most- and least-played first), 3 TOP PLAYLISTS (top 5
    // playlists per period, scrolls), 4 HABITS (session
    // and play-behaviour aggregates, each category under a header_sgr()
    // header). The data itself lives in HistoryStore history_ (appended to on
    // the main thread only, persisted to ~/.cache/mousiki/history/history.json
    // after every finished track and again at shutdown).
    int history_tab_ = 0;              // 0=History, 1=Top Tracks, 2=Top Playlists, 3=Habits
    static constexpr int kHistTabs = 4;
    static constexpr int kHistTabTop = 1, kHistTabPlaylists = 2, kHistTabHabits = 3;
    // Playlist context of the track being played / loaded (see QueueItem::playlist). pending_ is set by
    // play_next_from_queue() just before it starts the item and is consumed by start_*_track(); load_ follows the
    // load in flight; current_ is what history_begin_current_play() files the play under.
    std::string pending_queue_playlist_, load_playlist_ctx_, current_playlist_;
    int history_selected_ = 0;         // cursor within tabs 0/1
    int history_scroll_ = 0;           // manual scroll offset (tab 2's content)
    int history_view_top_ = 0;         // first visible list row on tabs 0/1 (window moves only when the cursor leaves it)
    bool history_most_first_ = true;   // 'r' on the Top Tracks tab
    std::vector<HistoryTopRow> history_top_view_; // rebuilt by history_refresh_top()
    std::string history_status_;       // footer status line, set by 'r' / the queue adds
    // Top Tracks tab only: it is split into two stacked panes. 0 = the track
    // list (Up/Down move its cursor), 1 = the "ADD SMART HISTORY TO QUEUE"
    // pane below it: a 4 x 4 grid of lists (column 0 Top 10/25/50/100, then
    // top of the week/month/quarter/year, top by time of day, and the
    // "rediscover" lists). Arrows move in the grid, Enter queues the list,
    // TAB toggles the panes, ESC leaves the grid for the track list.
    int history_pane_ = 0;
    int history_add_sel_ = 0;          // row 0..3 in the smart grid
    int history_add_col_ = 0;          // column 0..3 in the smart grid
    static constexpr int kHistoryAddCounts[4] = {10, 25, 50, 100};
    static constexpr int kSmartListSize = 25; // every smart list except column 0
    HistoryStore history_;             // the store itself (also used outside this overlay)

    void history_open();               // HKeyHistory entry point
    void history_refresh_top();        // rebuilds history_top_view_ from history_
    void handle_history_key(int key);
    // Both non-const: they clamp history_selected_/history_scroll_ against the
    // content they actually ended up drawing (see the window code at the end
    // of build_history_panel()).
    void build_history_screen(std::ostringstream& frame, int W, int player_h);
    std::vector<std::string> build_history_panel(int width, int height);
    void history_playlists_lines(int inner, std::vector<std::pair<std::string, int>>& lines) const; // TOP PLAYLISTS tab content
    std::vector<std::string> build_history_add_panel(int width, int height) const; // Top Tracks tab, 2nd pane
    // Queues the n most-played titles (always most-played first, whatever
    // order the Top Tracks list is currently showing). Local files that no
    // longer exist are skipped and reported, like a playlist add.
    void history_add_top_to_queue(int n);
    // The smart grid: queues the list at (col, row) -- see history_pane_.
    void history_queue_smart(int col, int row);
    // Appends up to `take` rows to the queue (missing local files are skipped)
    // and reports it in the history status line under `label`.
    void history_queue_rows(const std::vector<HistoryTopRow>& rows, int take, const std::string& label,
                            const std::string& empty_msg);
    // Play bookkeeping: called from poll_pending_load()/advance_track() when a
    // track starts or is handed over, and from the frame loop to accrue time.
    void history_end_current_play();   // closes the live record (no-op if none) + saves
    void history_begin_current_play(); // opens one for current_path_/metadata_

    // --- now playing ---
    bool has_track_ = false;
    // True from the moment advance_track() hands off to a load until
    // poll_pending_load() publishes its result (success or failure). While
    // it's set the finished track stays "current" (has_track_ remains true, so
    // the UI doesn't flash "no track loaded" between songs), and this flag is
    // what stops the main loop re-firing advance_track() every frame on the
    // still-set finished flag.
    bool advancing_ = false;
    fs::path current_path_;
    bool current_is_local_ = true;  // for snapshot identity -- current_path_ alone is ambiguous
                                     // (online tracks resolve to a cache path too)
    std::string current_video_id_;  // valid when !current_is_local_
    TrackMetadata metadata_;
    // metadata_ with the playing file's pending meta-editor edits laid over it -- what the main screen shows.
    TrackMetadata display_metadata() const;
    std::vector<float> waveform_envelope_;
    std::chrono::steady_clock::time_point waveform_reveal_start_;
    bool waveform_ready_ = false;
    std::shared_ptr<StreamingPcm> current_pcm_;
    const StreamingPcm* pcm_cut_noted_ = nullptr;   // the track whose "longer than its buffer" note was shown (once per track)
    size_t total_sec_ = 0;
    double angle_ = 0.0;
    std::chrono::steady_clock::time_point last_frame_time_;
    static constexpr double kAngularVelocity = (2.0 * 3.14159265358979323846 / 48.0) / 0.035;
    mutable FftVisualizer fft_;
    mutable SphereVisualizer sphere_;
    // The scope alternative to sphere_ (Settings -> ANIMA/VIZ -> "Lyrics/Viz"):
    // fed live by the audio callback via player_.play(..., &scope_), drawn
    // by whoever renders the lyrics panel. mutable, like sphere_, because
    // that renderer is const and the panel's visuals are animation state.
    mutable OscilloscopeVisualizer scope_;
    mutable std::string last_lyrics_status_;
    mutable std::chrono::steady_clock::time_point lyrics_status_shown_at_;
    mutable double viz_dt_ = 0.08;
    // The scope as a pixel image (Settings osci_style 1): filled while the lyrics column is built, emitted by run().
    GfxProto gfx_proto_ = GfxProto::None;
    std::string gfx_probed_pref_;              // the protocol preference gfx_proto_ was probed for ("" = not yet)
    mutable GfxFrame gfx_;
    mutable GfxFrame gfx2_;                    // the big oscilloscope overlay's second picture (gfx_ is its YT picture there)
    mutable bool gfx2_shown_ = false;
    mutable bool gfx_ok_ = false;              // the picture belongs on screen in the frame just rendered
    mutable int gfx_last_crop_ = -1;
    mutable int gfx2_last_crop_ = -1;
    mutable std::vector<uint8_t> gfx_full_level_;      // the audio book picture without a hole for a floating panel (see render_frame)
    mutable std::vector<uint8_t> gfx2_full_level_;     // the same for the second picture slot (the video next to the disk)
    mutable int gfx2_last_cut_[4] = {0, 0, 0, 0};
    mutable int gfx_last_cut_[4] = {0, 0, 0, 0};       // the hole that is cut into it now (pixels x0, y0, x1, y1)
    mutable int scope_cut_last_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};   // the panel cut-outs of the overlay's pictures of the last frame
    mutable int cell_w_ = 9, cell_h_ = 18;
    bool gfx_due_ = true;
    // The "fetching lyrics ..." box laid over the bottom row of the lyrics area's visual this frame (0-based row, col,
    // width; width 0 = none) and its text with colours: the image styles keep these cells free / write it again on top.
    mutable int caption_rect_[3] = {0, 0, 0};
    mutable std::string caption_text_;
    static std::string overlay_cells(const std::string& line, int x, const std::string& text, int text_w, int line_w);
    mutable int float_rect_[4] = {0, 0, 0, 0}; // x, y, w, h of the floating panel drawn this frame (0-based cells)

    // --- local list marquee (hovered row's title, when too long to fit) ---
    // Tracks which row the scroll animation is currently following and
    // when it started, so moving the cursor to a different row always
    // restarts the scroll from the beginning of that row's title instead
    // of resuming wherever the previous row's animation had reached.
    std::unique_ptr<BookState> book_;   // the audio book player's state (src/book_state.h) -- allocated in the constructor
    mutable int marquee_ch_idx_ = -1;      // the audio book's chapter pane: the row that scrolls (see build_chapter_panel)
    mutable std::chrono::steady_clock::time_point marquee_ch_since_;
    mutable int marquee_row_idx_ = -1;
    mutable std::chrono::steady_clock::time_point marquee_since_;
    // Same idea, kept separate because the playlist editor's LIBRARY and
    // TRACKS panels are two independent lists with their own selection
    // cursor, visible on screen at the same time -- sharing one row/clock
    // pair between them would make switching focus between the two panels
    // restart (or skip restarting) the wrong one's scroll.
    mutable int marquee_pl_lib_row_idx_ = -1;
    mutable std::chrono::steady_clock::time_point marquee_pl_lib_since_;
    mutable int marquee_pl_track_row_idx_ = -1;
    // --- main UI metadata block: Name / Artist / Album each stay on ONE row and scroll when too long ---
    mutable int marquee_meta_idx_[3] = {-1, -1, -1};
    mutable std::chrono::steady_clock::time_point marquee_meta_since_[3];
    mutable std::chrono::steady_clock::time_point marquee_pl_track_since_;

    // --- lyrics (background-fetched) ---
    mutable std::mutex lyrics_mutex_;
    mutable LyricsResult lyrics_result_;
    std::atomic<bool> lyrics_ready_{false};
    std::atomic<int> lyrics_epoch_{0};
    void launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network = false);

    std::string status_line_;
    bool quit_ = false;
    // Settings screen: the settings as they were when it opened. `s` saves and closes, ESC / q puts them back (discard).
    Settings settings_snapshot_;
    std::string settings_snapshot_text_;
    bool settings_dirty_ = false;
    int leave_prompt_ = 0;               // 1: "unsaved settings" question on ESC, 2: "unsaved theme" question (CREATE THEME), 3: "overwrite the theme?", 4: "delete the theme?" (CHOOSE THEME)
    std::string theme_name_initial_;     // the NAME the creator opened with ("" for a new theme, the theme in use for [e])
    std::string theme_draft_text_;       // the CREATE THEME draft as it was when the creator opened (to see whether anything changed)
    void settings_open();
    void settings_close(bool save);
    void settings_update_dirty();
    bool suspended_ = false;     // run() returned kExitSwitchMode: the App is kept alive and run() is called again on return
    void rescan_now();
    bool switch_mode_ = false;   // quit_ was set by the mode switch (SHIFT and +, the '*' key): run() returns kExitSwitchMode
    bool force_redraw_ = false;
    int last_render_w_ = -1;
    int last_render_rows_ = -1; // terminal height of the previous frame: a height change needs a full repaint too
    Mode last_render_mode_ = Mode::Browse;
    bool last_big_overlay_ = false;   // a big list / queue / convert / folder overlay was on the previous frame: closing it repaints everything (its legend can hang on the last terminal row, below the main frame)

    // --- mute (volume forced to 0 without touching pause state) --------
    bool muted_ = false;
    int pre_mute_volume_ = 70;

    // --- folder filter (HKeyFilterForFolder / HKeyClearFilter) ---------
    // Parent-directory path of the currently filtered folder, or empty
    // for "no folder filter". Applied on top of whatever the search/sort
    // already produced, in refresh_local_view().
    std::string folder_filter_;

    // --- floating panels (Bulk Add, Retry Lyrics) ------------------------
    // Unlike Console/Settings/Cheatsheet (full-screen overlays that
    // replace the whole view), these render *on top of* the still-live
    // Browse view behind them: render_frame() draws the normal
    // metadata/progress/search/list/status background exactly as always,
    // then this stamps a pre-built block of lines over it at an absolute
    // screen position via "\x1b[{row};{col}H" writes -- no clear, so
    // whatever was already drawn underneath stays visible around the
    // panel's edges. `lines` must already be exactly `panel_w` display
    // columns wide (pad_right them before calling) since this does no
    // width accounting of its own, just placement.
    //
    // Horizontally centered against the background's own width (W), not
    // the raw terminal width -- if the terminal's wider than W the
    // background content itself is left-anchored, and centering against
    // the full terminal would visually detach the panel from the
    // content it's supposed to be floating over. Vertically centered
    // against term_rows_, nudged up a few rows rather than dead-center.
    std::vector<std::string> panel_gradient(const std::vector<std::string>& lines) const;   // BorderGradient=smooth for a stamped panel
    int meta_sty_start_row(int panel_h) const { return std::clamp(1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift + 1), 1, std::max(1, term_rows_ - panel_h)); }   // the Style Option panel stands one row lower
    void draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines, int panel_w, int W, int fixed_col = 0, bool exact_center = false, int fixed_row = 0) const;   // fixed_row > 0: the panel's first row (it only grows downwards)
    static constexpr int kFloatingPanelUpShift = 3; // rows nudged above true vertical center

    // --- console / log overlay (HKeyConsole) ----------------------------
    // Backed by the global ConsoleLog (console_log.h/.cpp), which owns
    // both the on-disk $HOME/.cache/mousiki/logs/console.log and the
    // in-memory buffer this overlay renders -- see log_event() and
    // build_console_screen().
    void log_event(const std::string& msg);
    void toggle_latin_letters();
    // The status messages ("Latin letters: on", "added to queue", Settings notes ...) are not shown on the main screen: every new one is
    // kept here with its time and shown in the second pane of the console overlay. capture_status() looks at the message lines each tick.
    std::vector<std::string> status_history_;       // "[HH:MM:SS] message", oldest first (the last kMaxStatusHistory)
    std::vector<std::string> status_seen_;          // the last value of each message line that was looked at
    void capture_status();
    int console_pane_ = 0;                          // 0 = logs, 1 = status messages: the pane the arrow keys scroll
    mutable int console_up_[2] = {0, 0};            // lines scrolled up from the newest, per pane (0 = following the newest)
    void build_console_screen(std::ostringstream& frame, int W, int target_height) const;

    // --- cheatsheet overlay (HKeyCheatsheet) ----------------------------
    // Scroll position within the key table (the table is longer than a small terminal can show,
    // so the overlay scrolls instead of dropping rows). Mutable
    // because build_cheatsheet_screen() is const and clamps it against the
    // rows that actually fit as it draws.
    mutable int cheatsheet_scroll_ = 0;
    // SHIFT+s inside the cheat sheet: search the commands (help_search.h). The result list is rebuilt by every frame
    // (it is const), so the key handler only reads which table row the selected result stands for.
    bool cheat_searching_ = false;
    std::string cheat_query_;
    mutable int cheat_sel_ = 0;
    mutable std::vector<int> cheat_result_rows_;   // the table row of each result, best first
    mutable int cheat_first_ = 0;                  // the first result shown (the list scrolls to keep the selected one in view)
    mutable int cheat_jump_row_ = -1;              // set by ENTER: the next frame scrolls the table to this row
    // Karaoke overlay (Mode::Karaoke, HKeyKaraoke = "k"): the lyrics of the
    // playing track filling the screen, the active line highlighted word by
    // word, with the karaoke braille art on the left (disk colours, a moving
    // colour wave like the radio's ON AIR sign). Playback keys keep working.
    void build_karaoke_screen(std::ostringstream& frame, int W, int H);
    bool karaoke_size_dirty_ = false;   // lyrics size changed in the overlay: saved to config.txt when it closes
    std::chrono::steady_clock::time_point karaoke_flash_until_{};   // "[ lyrics size n / 5 ]" on the bottom border until then
    void build_cheatsheet_screen(std::ostringstream& frame, int W) const;

    // The Console and Settings overlays must always be exactly as tall as
    // the Browse-mode player view actually renders at right now -- not
    // just "whatever fits the terminal" (that's term_rows_, an upper
    // bound, not the target). Rebuilds the same panels Browse mode would
    // and sums their line counts, using the *current* list_visible_rows_
    // (itself already term_rows_-aware) for the list/queue panel's share,
    // so this always matches frame-for-frame regardless of which panels
    // are currently enabled/how tall lyrics or disk art are configured.
    int player_view_height(int w) const;

    // --- hotkey rebinding conflict check --------------------------------
    // Returns the action name already bound to key_str (excluding
    // except_action), or "" if key_str is free. Used by the Settings
    // Reference tab so rebinding a hotkey to a key another action already
    // owns is rejected instead of silently creating an overlap.
    std::string hotkey_conflict(const std::string& key_str, const std::string& except_action) const;
    // True when `key` (as read from the terminal) is the key currently bound to `action` -- for the places that have to
    // recognise a binding outside the Browse dispatch (a second press that closes an overlay, the meta editor).
    bool key_is(const char* action, int key) const;

    // --- autosave / session snapshot ------------------------------------
    // Consumed exactly once, right after a startup restore: the very
    // first launch_device_play_async() call for the restored track uses
    // this as its start position instead of 0.0, then zeroes it out so
    // every normal track change afterwards starts at 0 like always.
    double resume_start_sec_ = 0.0;
    // "Use mp4 audio for this song" changed while the song plays: the audio is loaded again and put in at the same time (reload_current_audio()).
    // No new play for the listening history, no new lyrics, the pause state stays.
    void reload_current_audio();
    bool swap_pending_ = false, swap_paused_ = false, swap_repause_ = false;
    double swap_pos_ = 0.0;
    fs::path swap_path_;
    std::chrono::steady_clock::time_point last_autosave_at_;
    // Drives the indicator's brief "something just saved" animation --
    // ticks for kAutosavePulseSeconds after each save, then goes idle.
    std::chrono::steady_clock::time_point autosave_pulse_started_at_;
    bool autosave_pulse_active_ = false;
    static constexpr double kAutosavePulseSeconds = 1.2;
    SnapshotData build_snapshot() const;
    // Applies a loaded snapshot: queue, play_mode, mute/volume, and kicks
    // off loading the saved "now playing" track at resume_start_sec_.
    // Called once at startup, before the render loop starts.
    void restore_snapshot(const SnapshotData& snap);
    // Called every frame from run(); saves (and pulses the indicator)
    // once settings_.autosave_delay_sec has elapsed since the last save.
    void maybe_autosave();
    // Builds the little "•" (or configured glyph) indicator string for
    // the volume-bar row, honoring AutoSave/AutoSaveIndicator/
    // AutoSaveIndicatorType. Returns "" when there's no room or the
    // feature's fully off (see build_progress_panel()'s comment on why
    // that also collapses the gap rather than just hiding a char in it).
    std::string autosave_indicator_glyph() const;

    // --- bulk add (paste a YouTube playlist link while Queue is
    // focused; hovering-song add on 'a' stays the List-panel behavior) --
    // Floating panel (see draw_floating_panel()), fixed total line count
    // across both phases -- unused rows are just blank-padded rather
    // than the panel changing size -- so its on-screen footprint never
    // moves/resizes frame to frame while open. Two phases:
    //   1. Input: bulk_add_results_ready_ == false -- typing the link.
    //   2. Results: fetch succeeded -- a compact starred checklist,
    //      navigable with Up/Down, toggled per-row with Space. "a" adds
    //      every fetched track regardless of star state ("ALL"); Enter
    //      adds only the starred ones ("[SELECT]").
    std::string bulk_add_buffer_;
    bool bulk_add_results_ready_ = false;
    std::vector<bool> bulk_add_selected_;   // parallel to pending_bulk_add_.items, default all-starred
    int bulk_add_cursor_ = 0;
    int bulk_add_scroll_ = 0;
    static constexpr int kBulkAddVisibleRows = 9; // preview list height cap -- this is what keeps the panel "tiny"
    using BulkAddResult = app_types::BulkAddResult;
    std::thread bulk_add_thread_;
    std::mutex bulk_add_mutex_;
    std::atomic<bool> bulk_add_in_progress_{false};
    std::atomic<bool> bulk_add_ready_{false};
    BulkAddResult pending_bulk_add_;
    void bulk_add_open();   // the bulk add panel (HKeyBulkAdd)
    void launch_bulk_add_async(const std::string& url);
    void poll_pending_bulk_add();
    void commit_bulk_add(bool all); // all=true -> every fetched track; all=false -> only starred ones
    static constexpr int kBulkAddPanelWidth = 62; // matches the reference design exactly
    std::vector<std::string> build_bulk_add_panel() const; // returns fixed-width, fixed-height lines for draw_floating_panel()

    // --- retry lyrics (HKeyRetryLyrics, 'l') ------------------------------
    // A manual override form: rather than instantly re-fetching with the
    // track's own metadata, this lets the person edit the title/artist
    // mousiki searches with and tack on edit-qualifier tags (slowed,
    // reverb, ...) -- for tracks whose auto-fetched lyrics are wrong or
    // missing because the real upload's title doesn't match what's
    // playing. Opens pre-filled from the current track's metadata_ (with
    // a best-effort "ft./feat." split out of the title into its own
    // field) rather than blank.
    enum class RLField {
        Source,   // the list of lyrics versions at the top (embedded in the file / fetched / older fetches)
        Title, Artist, Ft,
        TypeReverb, TypeSlowed, TypeUltraSlowed, TypeSpedup, TypeRemix, TypeOther,
        RemixText, OtherText,
    };
    std::string rl_title_, rl_artist_, rl_ft_;
    std::string rl_remix_text_, rl_other_text_;
    // TypeSlowed/TypeUltraSlowed/TypeSpedup are mutually exclusive (a
    // radio group -- at most one true); TypeReverb/TypeRemix/TypeOther
    // are independent toggles, any combination.
    bool rl_reverb_ = false, rl_slowed_ = false, rl_ultra_slowed_ = false;
    bool rl_spedup_ = false, rl_remix_ = false, rl_other_ = false;
    RLField rl_focus_ = RLField::Title;
    std::vector<LyricsVersion> rl_versions_;   // what list_lyrics_versions() found for the playing track
    int rl_src_sel_ = 0;                       // the highlighted row of that list
    void rl_choose_version();                  // SPACE on the list: that version is used from now on
    int rl_src_scroll_ = 0;                    // first visible row of the list (it shows 3 at a time)
    bool rl_confirm_delete_ = false;           // [d] asked "delete these lyrics?", waiting for y / n
    void rl_fix_scroll();                      // keeps the highlighted row inside the 3 visible ones
    void rl_delete_version();                  // after the y: removes the highlighted saved copy
    static constexpr int kRetryLyricsPanelWidth = 72; // wide enough for the legend on the bottom border
    // The fields actually navigable right now, in on-screen order --
    // RemixText/OtherText only appear in this list once their checkbox
    // is on, which is what makes them "dynamically available".
    std::vector<RLField> rl_visible_fields() const;
    void rl_open_from_current_track();     // pre-fill + reset state, called when 'l' opens the panel
    bool rl_from_reader_ = false;          // opened over READ LYRICS: drawn on it, closing returns to it
    bool rl_from_karaoke_ = false;         // opened over the karaoke screen: drawn on it, closing returns to it
    void rl_open_over_reader();            // ... over READ LYRICS (only for the track that plays)
    void rl_open_over_karaoke();           // ... over the karaoke screen
    void rl_close();                       // back to the screen it was opened from
    void rl_submit();                       // builds the override query and launches the fetch
    bool* rl_bool_ptr(RLField f);           // nullptr for non-checkbox fields
    std::string* rl_text_ptr(RLField f);
    static std::string rl_owner(RLField f) { return "rl-" + std::to_string(static_cast<int>(f)); }   // edit_focus() owner of one field    // nullptr for checkbox fields
    std::vector<std::string> build_retry_lyrics_panel() const; // fixed-width, fixed-height lines for draw_floating_panel()

    // --- async track loading ---
    using PendingLoad = app_types::PendingLoad;
    std::thread load_thread_;
    std::mutex load_mutex_;
    std::atomic<bool> load_ready_{false};
    std::atomic<bool> load_in_progress_{false};
    std::atomic<int> load_stage_{0};
    std::chrono::steady_clock::time_point load_started_at_;

    // Every Player call (play/stop/seek/volume/...) funnels through one
    // persistent thread that lives for the whole app session, rather than a
    // fresh std::thread per track switch. On Windows this is load-bearing,
    // not just tidy: WASAPI's underlying COM objects are apartment-affine
    // to the thread that created them, and the old per-track-thread design
    // meant track 2's play() call -- which starts by tearing down track 1's
    // device -- ran on a *different* OS thread than the one that created
    // that device. That mismatch is exactly why playback worked once and
    // then silently stopped starting on every subsequent switch. Routing
    // every device operation through one fixed thread removes the mismatch
    // outright, on every platform (Linux/PulseAudio never had this
    // constraint, but there's no downside to the safer design there either).
    //
    // player_mutex_ guards every call into player_ from either this worker
    // thread or the main thread (seek/volume/pause hotkeys, the shutdown
    // path's player_.stop()) -- Player's public methods were never
    // documented as safe to call concurrently from two threads, and with a
    // long-lived worker thread now genuinely overlapping the main loop for
    // the whole session (instead of a short-lived ad-hoc thread that mostly
    // wasn't), that latent race needed closing rather than just getting
    // more likely to bite.
    std::mutex player_mutex_;
    std::thread device_worker_thread_;
    std::mutex device_request_mutex_;
    std::condition_variable device_request_cv_;
    std::atomic<int> device_gen_{0}; // incremented each launch; guards against a stale request read racing a newer post
    // Generation of the device handoff currently in flight: set the moment a
    // request is posted, cleared by the worker once player_.play() has
    // actually swapped the new track in (0 = none in flight). While this is
    // nonzero the main loop must NOT act on player_.finished(): until play()
    // runs, the PREVIOUS pcm is still the installed one, so a track that
    // already ended keeps re-latching finished_ from the audio callback every
    // few milliseconds. clear_finished() in poll_pending_load() erases it
    // once, but the old device immediately set it again -- which made a freshly
    // loaded track flip straight back to "no track loaded" (advance_track() ->
    // Stop mode -> has_track_ = false) a frame later, while its own audio
    // started a few hundred ms after that. Pressing play again "fixed" it only
    // because by then the installed pcm was the *playing* track, which can't
    // latch the flag. This holds the stale flag off for the whole handoff.
    std::atomic<int> device_play_pending_gen_{0};
    bool device_worker_stop_ = false;
    bool device_request_ready_ = false;
    using DevicePlayRequest = app_types::DevicePlayRequest;
    DevicePlayRequest device_request_;
    void device_worker_loop();       // body of device_worker_thread_, runs for the app's whole session
    void start_device_worker();      // called once, from the constructor
    void stop_device_worker();       // called once, from run()'s shutdown, before joining device_worker_thread_
    void launch_device_play_async();
    // Any other call that opens, moves or closes the audio device (Player::stop(), reopen_device()) runs on the device worker too,
    // and the caller waits until it is done: device_run() posts `fn` and blocks until the worker has run it (under
    // player_mutex_). When the worker is not running (yet / any more) `fn` runs right here. device_stop() = device_run() of
    // player_.stop(), and a track switch that is still waiting for the worker is dropped (it would start the old track again).
    std::deque<std::function<void()>> device_tasks_;   // guarded by device_request_mutex_
    bool device_worker_running_ = false;               // guarded by device_request_mutex_
    void device_run(const std::function<void()>& fn);
    void device_stop();

    PendingLoad pending_load_;
    void launch_load_async(fs::path local_path, std::string title, std::string artist,
                            std::string location_label, bool is_local, std::string video_id);
    void poll_pending_load();
    static void write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                       double t_probe, double t_total, const std::string& error);

    // --- deferred mini-waveform pass ---
    std::mutex waveform_mutex_;
    std::atomic<bool> waveform_pending_ready_{false};
    std::atomic<int> waveform_epoch_{0}; // incremented on each recompute; stale threads discard their result
    std::vector<float> pending_waveform_envelope_;
    void poll_pending_waveform();

    // --- async online search ---
    std::thread search_thread_;
    std::mutex search_mutex_;
    std::atomic<bool> search_ready_{false};
    std::atomic<bool> search_in_progress_{false};
    std::vector<OnlineResult> pending_search_results_;
    void launch_search_async(const std::string& query, int source = 0);   // 0 YouTube, 1 SoundCloud, 2 Bandcamp
    int last_online_source_ = 0;   // the source of the last online search (the search box shows /s:, /sc: or /b:)
    void poll_pending_search();

    // --- settings panel (6 tabs: Colors, On/Off, Animation, Paths, Reference, About App) ---
    // Rendering uses absolute cursor positioning (\x1b[y;xH) rather than building padded strings
    // line by line: each field is placed where it belongs, so a wrong width on one row cannot shift
    // the escape sequences of the next one.
    Settings& settings_;
    static constexpr int kSettingsTabCount = 6; // Colors, On/Off, Animation, Paths, Reference, About App
    int settings_tab_ = 0;
    int settings_row_ = 0;   // resets to 0 on every tab switch
    int settings_col_ = 0;   // 0 or 1 -- only the Colors tab has 2-cell rows
    // REFERENCE tab: the last 5 key changes can be undone with Ctrl+Shift+U. An entry is the hotkey map from before the
    // change and the selectable row it concerned (0 = every key was reset; undoing puts the cursor on the first key that came back).
    struct RefUndo { std::unordered_map<std::string, std::string> hotkeys; int row = 0; };
    std::vector<RefUndo> ref_undo_;
    void ref_undo_push(int row);
    void ref_undo_pop();
    void ref_reset_keys(int row);                      // row 0: every key, else that one row (1-based like settings_row_)
    std::string color_edit_buffer_;      // live text while mode_==ColorEdit

    // --- caret / selection for the single-line text fields ----------------
    // Two BYTE offsets into whichever field is being edited right now:
    // edit_caret_ is where the next typed byte lands, edit_anchor_ is the
    // other end of the selection (equal to the caret when there is none, so
    // `caret != anchor` IS the selection test). Byte offsets, but every
    // movement is quantised to UTF-8 codepoint boundaries (see le_*() in
    // app.cpp), so a multi-byte character is never split in half.
    //
    // The pair is shared by every editor in the app, one at a time:
    // Mode::ColorEdit's buffer, the meta editor's field editor, and the
    // search/filter boxes (the main UI's "/", the meta editor's Search line,
    // the playlist editor's name/library boxes and the playlist list's
    // search). edit_owner_ tags the string the offsets were last clamped
    // against, so switching to another field -- or another row, or the 'r'
    // resort reordering the list -- retargets the caret instead of leaving
    // it pointing into a different string. edit_focus() (app.cpp) is the one
    // place that does the retarget.
    size_t edit_caret_ = 0;
    size_t edit_anchor_ = 0;
    std::string edit_owner_;
    void edit_focus(const std::string& owner, const std::string& text);
    // "Is this text box in use?" -- the rule that lets plain Left/Right switch
    // the tabs of the playlist editor and the meta editor while still being
    // the caret keys of their text boxes. A box becomes engaged with the first
    // key typed / edited into it (text_engage(), with the same owner name
    // edit_focus() uses); from then on Left/Right move the caret. ESC, TAB,
    // ENTER or a tab switch disengage it, and Left/Right switch tabs again.
    std::string engaged_owner_;
    bool text_engaged(const std::string& owner) const { return !owner.empty() && engaged_owner_ == owner; }
    void text_engage(const std::string& owner) { engaged_owner_ = owner; }
    void text_disengage() { engaged_owner_.clear(); }
    // Returns the current value of (tab, row, col) as plain text, for
    // display and as the starting buffer when editing.
    // Returns a pointer to the color field for (row, col) on the Colors
    // tab (tab 0), or nullptr if that row/col isn't a real cell.
    std::string* color_field_ptr(int row, int col);
    std::string settings_get_value(int row, int col) const;
    // Commits color_edit_buffer_ into (settings_tab_, settings_row_,
    // settings_col_). Colors are clamped/validated as a 0-255 code;
    // everything else is stored close to verbatim.
    void settings_commit_edit();
    void settings_commit_edit_impl();
    std::string settings_note_;        // what enforce_visual_switches() changed on its own after the last edit of a setting
    void lyrics_view_nudge();           // fetch the lyrics of the playing track (the lyrics view has just been switched on)

    // --- colour themes (Colors tab, last row: THEME | CREATE THEME) ---------
    // theme_ui_: 0 = the plain Settings screen | 1 = pick a theme to apply | 2 = the CREATE THEME overlay |
    // 3 = pick a template from inside the creator. All of it lives inside Mode::Settings (text editing inside
    // the creator uses Mode::ColorEdit with theme_ui_ still set).
    int theme_ui_ = 0;
    bool theme_peek_ = false, peek_busy_ = false;   // SHIFT+p in the theme picker: a still copy of the main screen in the highlighted theme
    std::string peek_frame_;
    int peek_sel_ = -1, peek_cols_ = 0, peek_rows_ = 0;
    std::vector<themes::Theme> theme_list_;
    int theme_sel_ = 0;               // picker cursor
    mutable int theme_page_ = 20;     // picker: rows on screen, set while drawing (SHIFT+up / down move by a page)
    Settings theme_draft_;            // creator: the colours being edited (a copy; settings_ is untouched until saved)
    themes::Theme theme_base_;        // creator: the template whose other keys (the radio's) the new theme keeps
    std::string theme_name_;          // creator: name of the new theme (25 characters at most)
    int theme_row_ = -1;              // creator: -1 = the NAME / TEMPLATE row, else the Colors row
    int theme_col_ = 0;               // creator: 0/1 -- name|button on the top row, FG|BG on a colour row
    int theme_current_index() const;  // index of the theme settings_ matches exactly, or -1 (custom)
    void theme_open_picker();
    void theme_open_creator(bool edit_current = false);
    void theme_try_save();
    void theme_save_new();
    void theme_delete_selected();
    // SHIFT+e / SHIFT+i on the Colors tab: theme_ui_ 4 = export the current colours as a theme file, 5 = import theme files.
    std::string theme_io_name_;       // export: the theme's name
    std::string theme_io_dir_;        // export / import: the folder
    int theme_io_row_ = 0;            // export: 0 name, 1 folder | import: 0 folder, 1 file list
    int theme_io_sel_ = 0;            // import: the hovered file
    std::vector<std::filesystem::path> theme_io_files_;
    void theme_open_export();
    void theme_open_import();
    void theme_io_rescan();
    void theme_import_files(const std::vector<std::filesystem::path>& files);
    void handle_theme_key(int key);
    void build_theme_screen(std::ostringstream& frame, int W, int player_h) const;
    // Left/Right quick-cycle for rows with a fixed set of options (bools,
    // enums). No-op for rows that don't have one (colors, hotkeys) --
    // those are Enter-to-type only.
    void settings_cycle(int dir);
    std::vector<std::string> settings_options_for(int tab, int row) const;
    int settings_max_row() const; // last valid row index for the current tab
    int settings_tab_first_line(int W, int visible) const;       // first layout line shown on tab 1 / 2 (scrolling)
    // Every overlay sizes itself from term_rows_ directly.
    void build_settings_screen(std::ostringstream& frame, int W, int player_h) const;
    struct SettingsPaint;   // (app_settings.cpp) what the tab functions of the settings screen share
    void settings_tab_colors(SettingsPaint& c) const;
    void settings_tab_general(SettingsPaint& c) const;
    void settings_tab_paths(SettingsPaint& c) const;
    void settings_tab_reference(SettingsPaint& c) const;
    void settings_tab_about(SettingsPaint& c) const;
    // The preview box right of the rows of GENERAL / ANIMA/VIZ: a live example of the highlighted option only (settings_preview.cpp)
    void settings_preview_draw(std::ostringstream& frame, int W, int MAX_Y, int tab, const std::function<std::string(int)>& border_at) const;
    mutable std::shared_ptr<struct SettingsPreviewState> pv_;
    void handle_settings_key(int key);

    // --- PATHS tab (tab 3) --------------------------------------------------
    // The LOCAL PATH, DOWNLOAD PATH and PLAYLIST PATH sections (they used
    // to live on the GENERAL tab, which is now a plain list of toggles). Two
    // of them are editable path lists, each introduced by a section header
    // and ending in a "+ new path" row.
    // Headers are painted but never selectable, so this struct maps the
    // flat selectable index the arrow keys walk (settings_row_) onto the
    // display row actually drawn on screen. Both the renderer and the
    // ColorEdit cursor placement go through it (via path_display_row()), so
    // they can never disagree on where a row landed. The selectable rows
    // are numbered 0 .. path_row_count()-1.
    using PathRow = app_types::PathRow;
    // Every display row of the path section, in paint order, with `sel`
    // assigned over the selectable ones. Always at least one path row per
    // list even while the underlying vector is empty (an unset path is
    // shown as an empty field rather than as no field at all).
    std::vector<PathRow> build_path_rows() const;
    // The row backing the given selectable row; its `sel` field is -1 when
    // the index is out of range.
    PathRow path_row(int selectable_row) const;
    // Number of selectable path rows (everything but the headers).
    int path_row_count() const;
    // Maps a selectable PATHS row to the display line it is drawn on,
    // headers and spacers included.
    int path_display_row(int selectable_row) const;
    int path_display_total() const;
    // True when the given selectable PATHS row edits free text -- a path
    // row rather than a "+ new path" button -- since those need far more
    // characters than a hotkey field does. Drives the edit-buffer length
    // limit.
    bool path_row_is_text(int selectable_row) const;
    // Maps a selectable REFERENCE row (hotkey or font-map row) to the
    // display line it is drawn on, headers and spacers included.
    int ref_display_row(int selectable_row) const;

    // Title shown for `path` in the (search-)lists. Honours
    // settings_.meta_only: metadata-only mode substitutes the embedded
    // title tag for the filename stem as soon as one has been resolved
    // for that file, and falls back to the filename when there is no tag
    // (or none resolved yet), so an untagged library still renders rows.
    std::string list_row_title(const fs::path& path, const std::string& filename_title) const;
    // Playlist LIBRARY/TRACKS panels have no separate Artist/Duration
    // columns and no "Show meta data only" toggle to pick one representation
    // over the other, so unlike list_row_title() above this always shows
    // both: the filename, plus the embedded title tag appended after it
    // once/if that tag has been resolved and actually differs from the
    // filename. Untagged or not-yet-resolved files just show the filename,
    // same as before.
    std::string playlist_row_label(const fs::path& path, const std::string& filename_title) const;
    // Shared marquee-scroll math used by the LOCAL AUDIO FILES pane and the
    // playlist LIBRARY/TRACKS panels: scrolls `text` within `width` columns
    // once it no longer fits, restarting from the beginning whenever
    // `row_idx` (the row currently being drawn) differs from whatever
    // `tracked_idx` last recorded -- so switching the hovered row always
    // resumes the animation from the start rather than mid-scroll. Returns
    // `text` truncated/padded to `width` unmodified when it already fits.
    std::string marquee_or_truncate(const std::string& text, int width, int row_idx,
                                     int& tracked_idx, std::chrono::steady_clock::time_point& since) const;

    // Scans the music folders again (LocalSource::scan()) and rebuilds local_view_. It runs when a
    // path is changed in the PATHS tab, so the change takes effect at once.
    void rescan_library();

    // Max row count per tab (set in build_settings_screen)


    // --- hotkey support ---
    // Resolves a key code from poll_key() to the hotkey action name.
    // Returns empty string if no match.
    std::string resolve_hotkey_action(int key) const;
    // Returns the key code that a hotkey string maps to for poll_key().
    static int hotkey_string_to_key(const std::string& s);

    // --- helpers ---
    void refresh_local_view();
    void update_live_search_preview();
    std::vector<LocalTrack> filter_and_rank_local(const std::string& query) const;
    void apply_local_sort(std::vector<LocalTrack>& tracks) const;
    std::vector<LocalTrack> filter_and_rank_local_view(const std::string& query) const; // + folder filter
    void resort_local_view_keep_selection(); // after Shift+N: re-sort, cursor stays on the same track
    static const char* sort_mode_name(int mode, bool meta_only);
    void submit_search();
    void start_local_track(const LocalTrack& track);
    void start_online_track(const OnlineResult& result);
    void play_selected();
    void play_relative(int delta);
    void play_relative_random();
    void advance_track();
    // Where the currently-playing track sits within *this list source's*
    // current view (local_view_ or online_view_, whichever list_source_
    // is showing), by identity match (path for local, video_id for
    // online) rather than by whatever row happens to be hovered. -1 if
    // nothing's playing, or what's playing came from a different source
    // than the one currently displayed (e.g. playing local while
    // browsing online results) -- there's no meaningful "relative to
    // current" position in that case. This is what play_relative() uses
    // instead of the hover cursor, so "next" always means "next after
    // what's actually playing", not "next after wherever you happen to
    // be looking".
    int current_track_list_index() const;
    // Takes the next item to play from queue_ -- it leaves the queue, or goes
    // to the back of it while the queue is locked -- honoring the current
    // play_mode: Shuffle picks a random queue item rather than strictly FIFO
    // order, Queue-then-stop (4) plays a locked queue through once (see
    // QueueItem::played). Shared by advance_track()
    // (auto-advance on finish) and the manual "n" key (explicit skip),
    // so both respect the queue exactly the same way. Caller must check
    // !queue_.empty() first.
    void play_next_from_queue(bool random = false);   // random: any item of the queue (#), otherwise its order
    void play_queue_item(int idx, bool move_along = false);   // start the queue item at idx (see play_next_from_queue); move_along: SHIFT+ENTER, the items in front of it go to the end
    // Single letter for the mode-indicator button after the search bar:
    // L=list, R=repeat, S=shuffle, Q=queue then stop, O=stop (play-and-stop
    // -- not "S", that's shuffle's letter already).
    char play_mode_letter() const;
    void queue_add_selected_impl(bool at_end);
    void queue_add_selected();           // "a": hovering track as NEXT (see queue_next_run_)
    void queue_remove_last();
    void queue_remove_hovering();
    // Shift+X (HKeyClearQueue): asks "Want to clear queue?" (Mode::ClearQueue,
    // a floating Yes/No panel like Bulk Add / Retry Lyrics) before queue_clear()
    // actually empties queue_. The default choice is No.
    void queue_clear();
    int clear_queue_choice_ = 1;       // 0 = Yes, 1 = No
    static constexpr int kClearQueuePanelWidth = 40;
    std::vector<std::string> build_clear_queue_panel() const;

    // "A newer version is out" (update_check.h): a small floating panel (Mode::UpdateNotice) shortly after the start,
    // [ESC] closes it, [o] opens the release page. It waits (update_pending_) until the main screen is showing.
    bool update_pending_ = false;
    std::string update_latest_, update_current_, update_url_;

    // Background tasks that are started and left to run (tags, lyrics, waveforms, translations ...): spawn_bg() counts them in
    // bg_, long loops stop early once bg_->quit is set, and wait_background() (main.cpp, at exit) waits for them. A task that is
    // still busy after that wait keeps the App alive until the process ends (main.cpp leaves it), so no task can reach a
    // destroyed App.
    struct BgTasks { std::atomic<int> running{0}; std::atomic<bool> quit{false}; };
    std::shared_ptr<BgTasks> bg_ = std::make_shared<BgTasks>();
    template <class F> void spawn_bg(const char* what, F&& body);   // app_internal.h
public:
    // Asks the background tasks to stop and waits up to `ms` for them; false when some are still running.
    bool wait_background(int ms);
private:

    // CHOOSE AUDIO DEVICE (src/app_audiodevice.cpp): a small floating panel (Mode::AudioDevice) with the sound outputs of this
    // computer; from Settings (GENERAL -> AUDIO, [ENTER]) it is drawn over the settings screen and ESC goes back there. The choice
    // is settings_.audio_device (config.txt AudioDevice=, "" = the system default); audio_device_apply() hands a changed value to
    // the player once per frame (adev_applied_ = the value it has).
    static constexpr int kAudioDevicePanelWidth = 64;
    bool adev_from_settings_ = false;
    int adev_sel_ = 0;                          // 0 = System Default, 1.. = adev_list_
    std::vector<AudioOutputDevice> adev_list_;  // read when the overlay opens ([r] reads it again)
    std::string adev_err_;
    std::string adev_applied_;
    std::string audio_device_label() const;     // "System Default" or the chosen name (Settings row, status line)
    void audio_device_open(bool from_settings);
    void audio_device_close();
    void audio_device_key(int key);
    void audio_device_apply();
    std::vector<std::string> build_audio_device_panel() const;
    static constexpr int kUpdatePanelWidth = 56;
    std::vector<std::string> build_update_panel() const;

    // Shift+O (HKeyOscMenu): small overlay (Mode::OsciMenu) to tune the
    // oscilloscope's afterglow / dot threshold / tail brightness live. It is
    // a small centred floating panel (draw_floating_panel()), which leaves
    // the scope on the right of the top panel visible while it is adjusted.
    // Up/Down pick a row, Left/Right change it, R resets, ESC / Shift+O close
    // (and save the values to config.txt).
    int osci_menu_row_ = 0;
    // Shift+9 (HKeyScopeWindow): the oscilloscope in its own window (src/scope_window.h). scope_window_sync() runs
    // every frame: it sends the current scope settings / colours / title and notices a window closed from its side.
    bool scope_win_on_ = false;
    // Spectrogram (src/spectrogram.h): the third lyrics-area visual (LyricViz=spectro), SHIFT+i options overlay
    // (Mode::SpectroMenu), SHIFT+u full screen. spectro_area_ = where this frame put the picture (row, col, cols, rows,
    // 0-based; row -1 = no picture), spectro_used_ = a spectrogram was drawn this frame (else the feed is switched off).
    mutable bool spectro_used_ = false;
    mutable int spectro_area_[4] = {-1, 0, 0, 0};
    SpectroGfx spectro_gfx_;
    // The spectrogram of the Settings preview box (settings_preview.cpp) as a picture: where this frame put it, its settings, its own picture state
    mutable int pv_spectro_area_[4] = {-1, 0, 0, 0};
    mutable bool pv_cover_pic_ = false;   // the Settings cover preview is a picture this frame (the only picture the Settings screen may show)
    mutable SpectroSettings pv_spectro_set_;
    SpectroGfx pv_spectro_gfx_;
    SpectroAnalyzer* settings_preview_spectro() const;   // nullptr until a preview was drawn
    bool spectro_full_ = false;
    // SHIFT+0: the big oscilloscope overlay (YT left, XY right). Each picture has its own scope (own phosphor buffer) fed from
    // scope_; SHIFT+a toggles the grid, SHIFT+v the axis values, SHIFT+c asks for its colour (the digits typed so far are in scope_big_color_buf_).
    bool scope_big_ = false;
    bool scope_big_color_edit_ = false;
    bool scope_metrics_open_ = false;   // SHIFT+p inside it: the window for the grid metrics (units, divisions, measurements)
    int scope_metrics_row_ = 0;
    std::string scope_big_color_buf_;
    mutable OscilloscopeVisualizer scope_big_xy_, scope_big_yt_;
    std::string build_scope_big(int W, const char* clear_prefix) const;
    std::vector<std::string> build_scope_color_panel() const;   // the small window SHIFT+c opens
    static constexpr int kScopeColorPanelWidth = 48;
    std::vector<std::string> build_scope_metrics_panel() const;   // the window SHIFT+p opens
    static constexpr int kScopeMetricsPanelWidth = 52;
    bool scope_big_key(int key);   // keys of the overlay itself; true = used
    int spectro_menu_row_ = 0;
    static constexpr int kSpectroMenuPanelWidth = 52;
    std::vector<std::string> build_spectro_menu_panel() const;
    std::string build_spectro_full(int W, const char* clear_prefix) const;
    void spectro_menu_adjust(int dir);
    size_t frame_main_len_ = std::string::npos;   // render_frame(): end of the main screen's lines (the overlays follow)
    void scope_window_toggle();
    void scope_window_sync(bool check_alive = true);
    // SHIFT+8 (HKeySpectroWindow): the spectrogram in its own window, the same way as the scope window.
    bool spectro_win_on_ = false;
    void spectro_window_toggle();
    void spectro_window_sync(bool check_alive = true);
    std::string enforce_visual_switches(int changed = -1);   // returns what it changed, for the status line
    static constexpr int kOsciMenuPanelWidth = 50; // just wide enough for the key legend
    std::vector<std::string> build_osci_menu_panel() const;
    void osci_menu_adjust(int dir);
    // Shift+V (HKeyNormMenu): the loudness normalisation overlay
    // (Mode::NormMenu), built on the same small centred floating panel as
    // the oscilloscope overlay above. Rows: Normalize (on/off), Target level
    // (LUFS) and Max boost (dB) -- the three values behind NormalizeVolume /
    // NormalizeTargetLufs / NormalizeMaxBoostDb in config.txt. Every change
    // is pushed into the player immediately (it glides to the new gain within
    // about a second), so what you adjust is what you hear; a live line shows
    // the playing track's measured loudness and the gain being applied.
    // Up/Down pick a row, Left/Right change it (on the first row they flip
    // on/off), SPACE or the toggle key (HKeyToggleNormalize) switches
    // normalisation on/off from any row, R resets target and boost to their
    // defaults, ESC / Shift+V close (and save the values to config.txt).
    // SHIFT+y (HKeyBarsMenu): the options of the spectrum bars (style, the Hi-Fi rows, motion). It sits UNDER the upper pane, so the bars stay visible.
    int bars_menu_row_ = 0;
    mutable int bars_menu_scroll_ = 0;   // the first row shown when the overlay has more rows than the room under the upper pane (scroll bar)
    static constexpr int kBarsMenuPanelWidth = 54;
    std::vector<int> bars_menu_rows() const;
    std::vector<std::string> build_bars_menu_panel(int main_w) const;   // main_w: the width of the main screen (the height of the upper pane depends on it)
    void bars_menu_adjust(int dir);
    int norm_menu_row_ = 0;
    static constexpr int kNormMenuPanelWidth = 54; // wide enough for the key legend in the bottom border
    std::vector<std::string> build_norm_menu_panel() const;
    void norm_menu_adjust(int dir);
    void norm_apply();                 // pushes settings_ -> player_
    // Shift+Z (HKeySleepTimer): the sleep timer overlay (Mode::SleepTimer), a
    // small floating panel over the live playback UI. Pick 15 / 30 / 60 / 90 /
    // 120 minutes (playback is PAUSED when it runs out, so it can be resumed),
    // "stop after current song", or Off. Up/Down pick a row, ENTER sets it and
    // closes, ESC / Shift+Z close without changing anything. Playback keeps
    // running underneath. Not persisted (a sleep timer must not survive a restart).
    //
    // Interplay with the Stop play mode (settings_.play_mode == 3): "stop after
    // current song" does NOT touch play_mode. It is a one-shot flag consumed in
    // advance_track(), checked BEFORE the repeat / stop / queue logic, so it wins
    // over Repeat and over the queue, and the play mode is exactly what it was
    // afterwards. With Stop mode already on it is simply redundant. The minute
    // timers and the one-shot flag are mutually exclusive (setting one clears the
    // other); a minute timer never changes the play mode either, it only pauses.
    int sleep_menu_row_ = 0;
    int sleep_timer_minutes_ = 0;                       // the minute choice that is running (0 = none)
    bool sleep_timer_active_ = false;
    std::chrono::steady_clock::time_point sleep_timer_deadline_{};
    bool sleep_stop_after_track_ = false;               // one-shot: end playback when the current song ends
    static constexpr int kSleepTimerPanelWidth = 44;
    std::vector<std::string> build_sleep_timer_panel() const;
    void sleep_timer_apply(int row);                    // row of the menu: 0..4 minutes, 5 stop after song, 6 fade out (toggle), 7 off
    void sleep_timer_cancel();
    void sleep_timer_tick();                            // once per frame
    std::string sleep_timer_label() const;              // "" when nothing is armed, else e.g. "SLEEP 24:10"
    // ^ (HKeyCreator): the creator overlay (Mode::Creator). Turns a text or a picture into a WAV file -- either
    // oscilloscope music (left = x, right = y, the XY scope draws the picture) or a spectrogram sound (the picture is
    // the spectrogram, played as notes of a scale) -- saves it in <music folder>/creator and plays it. The work is
    // done by creator.h; this is only the panel. The settings of the panel are kept until Mousiki is closed.
    static constexpr int kCreatorPanelWidth = 62;
    std::unique_ptr<CreatorState> creator_;   // the creator overlay's state (src/creator_state.h) -- allocated in the constructor
    std::vector<int> creator_rows() const;              // the row ids on the screen for the current mode
    bool creator_on_text_field() const;
    std::vector<std::string> build_creator_panel() const;
    // The preview that opens after "Create": a full-screen picture of what the file draws (the XY scope in music mode, or the
    // spectrogram) that loops until ESC. It has its own scope / spectrogram and its own settings (creator_->spectro_set, the
    // parameters in build_creator_preview()), so the SHIFT+o / SHIFT+i settings are neither used nor changed. The pictures
    // are made from the samples of the file itself, in step with the player's position.
    // The microphone preview: the same big overlay, but the pictures are made from the live microphone and ENTER records it.
    void creator_live_start();
    void creator_live_tick();
    void creator_record_toggle();
    void creator_record_save();
    void creator_preview_open();
    void creator_remember_previous();
    void creator_resume_tick();                         // once per frame: restarts that song as soon as the player is free
    void creator_preview_close();                       // stops the playback and the loop
    void creator_preview_loop();                        // the file ended: play it again
    void creator_preview_tick();                        // once per frame: feeds the pictures
    void creator_preview_key(int key);
    std::string build_creator_preview(int W, const char* clear_prefix) const;
    void creator_key(int key);
    bool creator_load();                                // (re)loads the picture when the source changed; false = nothing to work with
    void creator_render_and_play();
    fs::path creator_dir() const;                       // <first music folder>/creator
    bool creator_dir_prepare();                         // creates it and marks it so the library scan skips it; true = it was not marked before
    void creator_list_refresh();
    void creator_open_file(const fs::path& wav);        // plays a created file and opens its preview
    // Alt+L (kKeyAltL, fixed key): the lyrics timing overlay (Mode::LyricsEdit),
    // a small floating panel over the live playback UI for tracks whose lyrics
    // run early or late. The offset ("delay", seconds, + = lyrics later) lives in
    // lyrics_result_.delay and is applied live by render (lyrics time = elapsed -
    // delay), so the lyrics behind / inside the panel move while it is adjusted.
    // Left/Right = -/+ 0.1 s, Down/Up = -/+ 0.5 s, R = back to 0, ENTER / S =
    // save into the track's .lrc ("[offset:...]" tag, so it is remembered and
    // read back with the lyrics), ESC / Alt+L = cancel (the value from before the
    // overlay opened comes back). Opens only while synced lyrics are loaded.
    double lyrics_edit_orig_delay_ = 0.0;               // delay when the overlay opened (ESC restores it)
    bool lyrics_edit_from_reader_ = false;              // opened over READ LYRICS: drawn on it, closing returns to it
    bool lyrics_edit_from_karaoke_ = false;             // opened over the karaoke screen: it is drawn on it and closing returns to it
    void lyrics_edit_open_over_reader();    // ... over the READ LYRICS overlay (only for the track that plays)
    void lyrics_edit_open_over_karaoke();   // the lyrics timing overlay over the karaoke screen
    void karaoke_ensure_lyrics();   // the karaoke overlay fetches the lyrics itself when the engine is off
    // SHIFT+w (HKeyWizard): the Mousiki Plus Help Wizard (Mode::Wizard), a full-screen help book. src/wizard.{h,cpp}
    // holds the text, the page / part / contents state and the key handling; this class only draws it (the radio draws
    // the same pages the same way). It opens from the main screen and from every menu / overlay that does not take
    // typed text (wizard_allowed_over_mode()), and closing it returns to the mode it was opened from. The first
    // close also sets Settings' wizard_seen and writes config.txt, so the first-start greeting shows only once.
    wizard::State wizard_st_;
    Mode wizard_return_mode_ = Mode::Browse;
    double wizard_angle_ = 0.0;       // the first page's disk, spun at 0.10 whatever is playing
    void wizard_open();
    void wizard_close();
    bool wizard_allowed_over_mode() const;   // false while a text field has the keyboard
    void build_wizard_screen(std::ostringstream& frame, int W);
    // CTRL+L (kKeyCtrlL, fixed key): READ LYRICS (Mode::ReadLyrics), a full-screen reader for the saved lyrics of the
    // playing (else the hovered) track. The top pane is ONE scrollable line with the lyrics files of the track
    // (embedded / fetched / older copies, LEFT / RIGHT choose), below it the plain lyrics without timestamps
    // (UP / DOWN / SPACE / b / HOME / END scroll). `t` translates non-English lyrics into English
    // (scripts/translate_lyrics.py, in the background): the translation is saved in lyrics/translations/ and shown in
    // "( )" right below each original line; `t` again hides it, `r` translates anew.
    void readlyrics_play();                             // [p]: start this track, else pause / resume it
    bool readlyrics_track_plays() const;                // the track of the reader is the loaded one
    bool readlyrics_reader_working() const;             // the kanji reader can still answer (it is on, not broken)
    std::vector<std::string> build_readlyrics_export_panel() const;
    void readlyrics_export_key(int key);
    int readlyrics_export_choices(std::vector<int>& flags, std::vector<std::string>& names) const;   // what can be chosen now
    fs::path readlyrics_export_dir() const;             // <lyrics folder>/export
    fs::path readlyrics_export_file(int flags) const;   // the file an export with these contents goes to
    void readlyrics_export_do();
    std::vector<std::string> build_readlyrics_options_panel() const;
    void readlyrics_options_key(int key);
    // The running translation: what it was asked for and which lyrics it belongs to.
    std::unique_ptr<ReadLyricsState> rd_;                // the READ LYRICS reader (src/readlyrics_state.h) -- allocated in the constructor
    fs::path translate_script_;                         // scripts/translate_lyrics.py

    void readlyrics_open();
    void readlyrics_close();
    void readlyrics_load_version();                     // reads the chosen file (and its saved translation)
    void readlyrics_refresh_translations(const std::string& prefer_to, const std::string& prefer_svc = std::string());   // re-reads the saved translations of the chosen file
    void readlyrics_select_translation(int index);
    void readlyrics_toggle_translations();              // SHIFT+t: show / next translation / hide
    void readlyrics_delete_translation();
    void readlyrics_translate();                        // ENTER in the translate overlay
    void readlyrics_poll();                             // once per frame: takes a finished translation over
    void handle_readlyrics_key(int key);
    // CTRL+s in READ LYRICS: a search over EVERY lyrics file (by file name) in the lyrics folders of the library, as an
    // alternative to the lyrics of the current track. ENTER shows the chosen file in the reader (it is read like a
    // track's own lyrics, translations included); the reader is opened for the playing track again with CTRL+L.
    // (LyricsFileHit: see readlyrics_state.h)
    void readlyrics_search_open();
    void readlyrics_search_rebuild();
    void readlyrics_search_key(int key);
    void readlyrics_show_file(const LyricsFileHit& hit);
    // "Add manually" ([a] / CTRL+n): assigns a .lrc / .txt file to a track by hand. Two search panes: the lyrics files
    // (every lyrics folder of the library, subfolders included, or a path typed in) and the tracks of the library, whose
    // first entry is always the current track. TAB changes the pane, ENTER assigns.
    void readlyrics_add_open();                         // in READ LYRICS
    void readlyrics_add_open_from_playing();            // from the re-fetch form and the karaoke screen: the reader opens for the playing track, with the overlay open
    void readlyrics_add_rebuild();
    void readlyrics_add_key(int key);
    void readlyrics_add_do();
    void readlyrics_add_clear();                        // [c]: clear the locked lyrics file and track
    void readlyrics_add_unassign();                     // DEL in the overlay: takes the manually added lyrics of the chosen track out of use
    fs::path readlyrics_add_target() const;             // the track chosen in the lower list
    void readlyrics_add_lock_refresh();                 // re-reads rd_->add_lock_info / rd_->add_manual of the locked track
    // The search of both lists: the strings that are compared are made once, and a query that only adds letters to the one before
    // looks through the matches of the one before, not through the whole library again (a keystroke costs the size of the result).
    int readlyrics_add_list_rows() const;               // rows of the result lists (the same for both)
    static std::string readlyrics_add_track_text(const LocalTrack& t);   // "folder - title" (just the title when the folder says nothing)
    std::string readlyrics_add_track_label(int idx) const;
    std::vector<std::string> build_readlyrics_add_panel(int W) const;
    std::vector<std::string> build_readlyrics_add_legend(int W) const;   // the key legend under the panel: a small tree table, as in READ LYRICS
    static constexpr int kAddLegendRows = 4;
    // --- READ LYRICS, [SHIFT+e]: edit a translation by hand / write a new one from scratch ("Manual Translation")
    void readlyrics_te_open();
    void readlyrics_create_translation();   // SHIFT+c: an empty translation file for the chosen lyrics file
    void readlyrics_te_key(int key);
    bool readlyrics_te_save();
    void readlyrics_te_step(int dir);                   // to the next / previous line that has an original text
    int readlyrics_te_body_rows() const;
    bool readlyrics_te_blank(size_t i) const;
    std::vector<std::string> build_readlyrics_te_panel(int W) const;
    std::vector<std::string> build_readlyrics_te_legend(int W) const;
    static constexpr int kTeLegendRows = 5;
    std::vector<std::string> build_readlyrics_search_panel() const;
    void build_readlyrics_screen(std::ostringstream& frame, int W, int target_height);
    fs::path lyrics_path_;                              // track path the current lyrics belong to (sidecar location)
    static constexpr int kLyricsEditPanelWidth = 60;
    void lyrics_edit_open();
    void lyrics_edit_close();                           // back to the main screen
    void lyrics_edit_adjust(double step);
    std::vector<std::string> build_lyrics_edit_panel() const;
    // Shift+E (HKeyEqualizer): the equaliser overlay (Mode::Equalizer), a
    // floating panel over the live playback UI. Ten vertical sliders
    // (31 Hz .. 16 kHz, +/-12 dB), a preset line and an on/off state. Left/
    // Right pick a band, Up/Down change it by 1 dB, ,/. (or TAB) step through
    // the presets (the built-in ones first, then the user's own), E or SPACE
    // switches the EQ on/off, R resets to Flat, ESC / Shift+E close and save.
    // S saves the current curve as a custom preset (a name prompt takes over
    // the row under the sliders until ENTER / ESC), DEL or X deletes the
    // selected custom preset after a second press. Custom presets live in
    // settings_.eq_custom_presets and are written to config.txt right away.
    // Touching a band or choosing a preset also turns the EQ on, so what you
    // change is always what you hear. The values live in settings_.eq_enabled
    // / eq_gains and reach the audio thread through Player::set_equalizer().
    int eq_band_ = 0;                  // selected band 0..kEqBands-1
    int eq_last_preset_ = 0;           // unified preset index (built-ins, then custom): where the cycle continues from while the gains are Custom
    bool eq_naming_ = false;           // the "Save as:" prompt is open
    std::string eq_name_buf_;          // its text (edit_caret_/edit_anchor_ belong to it while open)
    std::string eq_status_;            // one-line feedback under the sliders, cleared by the next key
    bool eq_delete_armed_ = false;     // DEL / X pressed once on a custom preset: the next press deletes it
    static constexpr int kEqPanelWidth = 58;
    std::vector<std::string> build_eq_panel() const;
    void eq_open();
    void eq_apply();                   // pushes settings_ -> player_
    void eq_set_gain(int band, float db);
    void eq_select_preset(int dir);    // dir = +1 next, -1 previous
    int eq_preset_count() const;                    // built-in presets + custom presets
    const EqGains& eq_preset_gains(int index) const;
    std::string eq_preset_name(int index) const;
    int eq_current_preset() const;                  // unified index whose gains match the sliders, or -1 (Custom)
    void eq_begin_naming();                         // S
    void eq_commit_name();                          // ENTER in the prompt
    void eq_delete_custom(bool confirmed);          // DEL / X
    void queue_move_hovering(int dir); // dir=-1 up, +1 down
    void clamp_queue_selected();
    void queue_reveal(int idx);   // scroll the queue so that this entry is visible (after adding tracks)
    void handle_key(int key);
    void handle_browse_key(int key);
    bool browse_open_key(const std::string& action, int key, int lookup_key, size_t list_len);
    bool browse_nav_queue_key(const std::string& action, int key, int lookup_key, size_t list_len);
    bool browse_playback_key(const std::string& action, int key, int lookup_key, size_t list_len);
    bool browse_queue_misc_key(const std::string& action, int key, int lookup_key, size_t list_len);
    bool browse_rest_key(const std::string& action, int key, int lookup_key, size_t list_len);
    void handle_console_key(int key);
    void handle_update_notice_key(int key);
    void handle_qtp_key(int key);
    void handle_clear_queue_key(int key);
    void handle_spectro_menu_key(int key);
    void handle_osci_menu_key(int key);
    void handle_bars_menu_key(int key);
    void handle_norm_menu_key(int key);
    void handle_lyrics_edit_key(int key);
    void handle_sleep_timer_key(int key);
    void handle_equalizer_key(int key);
    void handle_karaoke_key(int key);
    void handle_cheatsheet_key(int key);
    void handle_wizard_key(int key);
    void handle_bulk_add_key(int key);
    void handle_retry_lyrics_key(int key);
    void handle_search_key(int key);
    // Actions shared by the hotkeys and the remote control (`mousiki --cmd ...`, src/remote_control.h).
    void act_play_pause();
    void act_volume_change(int delta);
    void act_mute(int how);          // -1 toggle, 0 unmute, 1 mute
    void act_next();
    void act_prev();
    void act_cycle_play_mode(int dir = 1);      // m: list -> repeat -> shuffle -> stop -> queue then stop
    void act_wizard_toggle();        // SHIFT+w from the remote control: open / close the help wizard
    // --- audio book player (app_audiobook.cpp) ---
    struct BookChapter { fs::path path; std::string title; };
    std::vector<BookChapter> chapters_;       // its chapters, in natural file name order (shown in the chapter pane)
    void book_build_view();                   // folder_view_ for book_->nav (a flat list while a search is typed)
    void book_refresh_view();                 // the books list (left pane) from the scanned library
    void book_load_chapters(const std::string& dir);
    void book_open(int row);                  // ENTER on a book: its chapters fill the chapter pane
    void book_fit_scroll();
    int book_current_chapter() const;         // index in chapters_ of the loaded track, -1 when none
    void book_play_chapter(int idx);
    void book_step(int delta, bool direct = false);   // direct (the remote app): no questions, the chapter plays from its start                // n / b
    void book_advance();                      // the chapter ended
    void book_seek(int dir);                  // accelerating seek
    // Playback speed (audio book player only; time_stretch.h): set rounds to 0.05 and clamps to 0.5 .. 3.0, applies it at once and
    // says so in the status line (save: written to config.txt now, as the keys do); step moves it by one 0.05 step (SHIFT+LEFT / RIGHT).
    void book_speed_set(double speed, bool save);
    void book_speed_step(int dir);
    bool book_intercept(const std::string& action, int key);   // keys of the audio book player (true = used)
    std::vector<std::string> build_chapter_panel(int total_width, int height) const;
    // ---- the album cover in the disk's place (music player; Settings -> Album Cover / Cover Mode / Cover Search; app_cover.cpp) ----
    const CoverImage* player_cover() const;   // the cover of the playing track (nullptr: off, none, or still being read)
    void cover_fill(GfxFrame& g, const CoverImage& cover, int mode, double angle, int w, int h, int cws, int chs) const;   // the pixels of a cover look
    const std::string* player_phone_png(std::string& key) const;   // the cover look as a PNG for the remote app (nullptr: no cover)
    bool album_cover_gfx(int col, int cols, int rows) const;   // as a pixel image (Kitty / Sixel) into gfx_; false: no cover / no protocol
    std::vector<std::string> album_cover_lines(int cols, int rows) const;   // as braille dots (empty: no cover)
    mutable std::string cover_pic_key_;       // what gfx_ holds when it holds the cover (a still picture is only built again when this changes)
    mutable std::chrono::steady_clock::time_point cover_pic_at_{};
    bool book_picture_gfx(int col, int cols, int rows) const;   // the picture as a real pixel image (Kitty / Sixel) into gfx_; false: use braille
    std::vector<std::string> book_art_lines(int which, double t) const;   // which: 0 the opening book, 1 the stack of books
    // The picture that takes the disk's place on the remote app's screen: the book's cover (Book Cover on) as braille dots in its own
    // colours (or in the wave's), else the standard picture; cols x rows cells. Empty while the cover is still being read.
    std::vector<std::string> book_phone_picture(int& cols, int& rows) const;
    const std::string* book_phone_png(std::string& key) const;   // the same picture as a PNG for the remote app (nullptr: not ready)
    static const char* book_tag_label(int i);
    static BookTagVals book_read_tags(const fs::path& file);
    static constexpr int kBookOverlayWidth = 96;   // at most (a narrow terminal gets less)
    void book_tags_open();
    void book_ov_load(int chapter);           // fills book_->ov_val / book_->ov_orig from the file
    bool book_ov_dirty() const;
    void book_ov_save(bool whole_book);       // queues the job: this chapter only, or the book-wide rows for every chapter
    std::vector<std::pair<fs::path, fs::path>> book_rename_plan(bool whole_book) const;   // the renames a save would do (book-wide: numbered)
    void book_finish_job(BookTagJob& job);    // the .cue and the renames, after the last tag write
    std::string book_cover_report() const;    // what the book cover needs / why it is not shown (for the status line)
    int book_numbering() const;               // 0 the file names carry no chapter number, 1 it is in front, 2 it is at the end
    bool book_overlay_key(int key);
    bool book_tags_key(int key);
    std::vector<std::string> build_book_overlay_panel(int panel_w) const;
    std::vector<std::string> build_book_tags_panel(int panel_w) const;
    int book_mark_in_chapter(int idx) const;  // index in book_->marks of the newest bookmark in chapter idx, -1 when none
    // the auto-mark: written every few seconds while a chapter plays; "leave, but keep the time stamp" holds it back
    void book_automark_tick(bool force);      // writes the auto-mark of the chapter that plays (force: now, whatever the interval)
    std::vector<BookStamp> book_chapter_stamps(int idx, bool with_auto) const;   // auto-mark, bookmarks and notes of chapter idx
    static std::string book_stamp_name(const BookStamp& s);   // "Auto-mark", "Bookmark 2", the note's title or "Note 3"
    static const char* book_stamp_type(const BookStamp& s);   // "Auto-mark", "Bookmark", "Note"
    bool book_stamp_remove(BookStamp s);      // deletes it from the .cue / notes file and remembers it for the undo
    bool book_stamp_restore(const BookStamp& s);
    std::string book_undo_pop();              // CTRL+SHIFT+u: brings back the last deleted time stamp; the text says what happened
    void book_stamps_changed(const std::string& dir);
    int book_automark_convert(const std::string& dir, const BookMark& am);   // auto-mark -> bookmark (returns its number, 0 on an error)
    void book_continue_open(int idx);         // "continue where you left?" (a chapter with an auto-mark and no bookmark)
    bool book_continue_key(int key);
    std::vector<std::string> build_book_continue_panel(int panel_w) const;
    void book_pick_open(int idx, std::vector<BookStamp> rows);   // the time stamps of the chapter to choose from
    bool book_pick_key(int key);
    std::vector<std::string> build_book_pick_panel(int panel_w) const;
    // --- chapter marks and cutting a file into chapters (Phase 3; app_audiobook_chapters.cpp) ---
    void book_cm_open();                      // ALT+L in the audio book player
    bool book_cm_key(int key);
    std::vector<std::string> build_book_cm_panel(int panel_w, int panel_h) const;
    void book_cm_save();                      // the marks into the .cue
    void book_cm_insert(double sec, const std::string& title, bool ask_title);
    std::vector<fs::path> book_backup_files() const;   // <name>.<ending>.mousiki-backup files in the open book's folder
    void book_cut_start(bool trim);
    void book_cut_step();                     // one part per call (from book_poll)
    void book_cut_finish();
    void book_marks_open();
    void book_marks_fill();
    bool book_marks_key(int key);
    std::vector<std::string> build_book_marks_panel(int panel_w, int panel_h) const;
    bool book_open_dir(const std::string& dir);   // opens the book of that folder (false when it is not in the library)
    void book_marks_write(const std::string& dir, const std::vector<BookMark>& marks);
    void book_bookmark_load();                // book_->dir -> book_->bookmark and book_->notes
    void book_bookmark_store(const std::string& file, double sec);
    int book_bookmark_chapter() const;        // index of the chapter that carries the bookmark, -1 when none
    void book_set_bookmark_now();             // SHIFT+b
    // the question "set a bookmark?" (it pauses the audio) and the resume choices
    bool book_ask_due() const;
    void book_ask_then(const std::string& what, std::function<void()> then);   // asks when it is due (then runs `then` after the answer), else runs it at once
    void book_go(int idx);                    // the user picks chapter idx: the question, the resume choices, then playing
    bool book_play_file(const std::string& path);   // reading history: opens the book of the file and goes to its chapter
    void book_play_at(int idx, double sec);   // starts chapter idx and goes to sec
    void book_resume_open(int idx, const BookMark& mark, const std::string& label = "bookmark");
    bool book_ask_key(int key);
    void book_delmark_open(int idx);          // [d] in the chapter pane: the small question before the bookmark of chapter idx is deleted
    bool book_delmark_key(int key);
    std::vector<std::string> build_book_delmark_panel(int panel_w) const;
    bool book_resume_key(int key);
    std::vector<std::string> build_book_ask_panel(int panel_w) const;
    std::vector<std::string> build_book_resume_panel(int panel_w) const;
    // the note editor in the field (SHIFT+n; the viz / notes field shows it instead of the visualization)
    bool book_overlay_switch(int key);        // SHIFT+s / k / m / l inside one of those overlays: the other one opens without closing first
    void book_viz_remember();                 // called when a note enters the field
    void book_viz_restore();                  // called when it leaves: that view is on again
    void book_note_new();
    void book_note_edit_existing(int idx);
    void book_note_to_overlay();              // CTRL+n: the note that is written (or shown) goes on in the big notes overlay
    std::string book_note_where(const std::string& file, double sec, int width) const;   // "chapter  @ 0:00:05" that always keeps the time
    bool book_note_key(int key);
    void book_note_save();
    void book_note_show(const BookNote& n, bool automatic);
    void book_notes_follow();                 // render loop: playback passed a note -> show it
    bool book_field_has_note() const;
    std::vector<std::string> book_note_field_rows(int w, int h) const;   // what the field shows instead of the visualization
    // the notes overlay (SHIFT+l)
    std::vector<LegendRow> book_overlay_legend(int panel_w, const std::vector<LegendGroup>& groups, int max_rows) const;   // the tree-table legend of the book overlays
    std::vector<int> book_notes_order() const;   // entries of the list: -1 = the bookmark, else an index into book_->notes (chapter, time)
    void book_notes_open();
    bool book_notes_key(int key);
    std::vector<std::string> build_book_notes_panel(int panel_w, int panel_h) const;
    int book_ov_width(int term_w) const;
    int book_chapter_of(const std::string& file) const;   // chapter index by file name, -1 when none
    std::string scanned_book_path_;           // the AUDIO BOOK PATH the library was scanned with (a change meanwhile means a rescan)
    void exclude_book_files(std::vector<LocalTrack>& tracks) const;   // the music player's library leaves out what lies in the audio book folder
    void book_totals(double elapsed, double& total, double& done, bool& complete) const;   // length of the book / listened so far (sums the known chapter lengths)
    void book_poll();                         // render loop: the tags of the loaded chapter, a running save
    bool request_mode_switch(int dir);   // SHIFT and + / SHIFT and -: leave for the next / previous mode (false: no other mode is on)
    void sync_after_radio();         // adopt what the radio changed in config.txt meanwhile
    void remote_poll();              // commands from outside + the status file; called every frame
    void remote_app_apply();         // starts / stops the remote app's web server to match settings_
    // The remote app (src/remote_server.h, src/app_phone.cpp): the phone screen, built when a phone asks for it.
    void phone_publish();
    std::vector<std::string> build_phone_screen(int W, int H, std::vector<remote::net::Zone>& zones) const;
    double phone_angle_ = 0.0;       // the phone's disk: its own turn at speed 0.10
    remote_ui::State remote_ov_;     // Settings -> GENERAL -> REMOTE / UPDATES -> Remote App: the REMOTE APP overlay (address, QR code, port, PIN)
    void remote_overlay_draw(std::ostringstream& frame, int W) const;
    remote_ui::Style settings_overlay_style() const;   // the colours / glyphs of the overlays over the settings screen
    std::chrono::steady_clock::time_point phone_last_{}, phone_built_{};
    int phone_view_ = 0;             // 0 the phone's main screen, 1 its LISTS screen
    int phone_focus_ = 0;            // LISTS: the pane UP / DOWN page through (0 the local list, 1 the saved playlists)
    int phone_page_ = 0;             // LISTS: the page of the local list ...
    int phone_page2_ = 0;            // ... and of the playlists
    // Audio book mode: LISTS shows the library as a tree (the library overlay's folders, books and chapters). phone_book_open_ holds
    // the opened folders; the rows are numbered from 1 for the commands book-open (folder) and track (chapter).
    struct PhonePic { bool active = false; int x = 0, y = 0, w = 0, h = 0; std::string key; };
    mutable std::string phone_cover_key_;   // music player: the key of the cover picture the phone shows
    mutable PhonePic phone_pic_;     // audio book: where the phone draws the picture (cells) and which one
    std::set<std::string> phone_book_open_;
    std::vector<FoRow> phone_book_rows() const;
    void phone_book_reveal();        // opens the way to the book that is open and sets the page that shows it
    std::string phone_note_;         // its status line ("queued 12 titles from ...")
    std::chrono::steady_clock::time_point phone_clear_armed_{};   // CLEAR QUEUE tapped once: a second tap within 5 s clears
    std::string queue_playlist(const std::string& name);   // every title of a saved playlist into the queue; the message
    void ensure_visible_row_meta();
    void recompute_waveform_for_current_track();
    std::string render_frame(TerminalIO& term);
    std::optional<std::string> render_takeover_screen(int term_cols, int W, bool hard_clear);   // the full-screen modes of render_frame() (app_render.cpp)

    // --- box drawing helpers (use configured border chars) ---
    std::string box_top(const std::string& label, int total_width, const std::string& border_ansi = "") const;
    std::string local_pane_label(int total_width) const; // LOCAL AUDIO FILES title, folder name shortened to fit
    std::string box_bottom(int total_width, const std::string& footer = "", const std::string& border_ansi = "") const;
    std::string box_line(const std::string& content, int total_width, const std::string& border_ansi = "") const;
    // box_top()/box_line() for a row whose TAIL is a text field with a caret
    // and a marked range: the field arrives already painted (it carries
    // reverse-video escapes), so these two pad/measure against `field_cols`
    // instead of running the rendered string through display_width(), which
    // counts escape bytes as columns. `prefix` stays plain text.
    std::string box_top_field(const std::string& prefix, const std::string& field, int field_cols,
                              int total_width, const std::string& border_ansi = "") const;
    std::string box_line_field(const std::string& prefix, const std::string& field, int field_cols,
                               int total_width, const std::string& border_ansi = "",
                               const std::string& text_ansi = "") const; // text_ansi: colours for prefix+field+fill

    // panel builders
    struct MetaPanel;   // (app_panels.cpp) the layout + rows the parts of build_metadata_panel() share
    void meta_panel_disk(MetaPanel& m) const;
    void meta_panel_info(MetaPanel& m) const;
    void meta_panel_field(MetaPanel& m) const;
    std::vector<std::string> build_metadata_panel(int width) const;   // (empty when the window is too small for it)
    bool show_metadata_panel(int width) const;
    std::vector<std::string> build_progress_panel(int width) const;
    std::vector<std::string> build_search_bar(int width) const;
    std::vector<std::string> build_list_panel(int width, int height) const;
    std::vector<std::string> build_queue_panel(int width, int height, bool wide_artist = false) const;   // wide_artist: the big overlay gives the artist column twice the room
};

// A background task of the App (see spawn_bg() above): counted in bg_ while it runs; whatever it throws becomes a log line (an
// exception escaping a std::thread would end the program). The counter is a shared object of its own, so the task can still count
// itself out when it outlasts the wait at exit.
template <class F>
void App::spawn_bg(const char* what, F&& body) {
    std::shared_ptr<BgTasks> st = bg_;
    st->running.fetch_add(1);
    std::thread([st, what, b = std::forward<F>(body)]() mutable {
        try { b(); }
        catch (const std::exception& e) { ConsoleLog::instance().log_basic(std::string("internal error in ") + what + ": " + e.what()); }
        catch (...) { ConsoleLog::instance().log_basic(std::string("internal error in ") + what + " (unknown exception)"); }
        st->running.fetch_sub(1);
    }).detach();
}

} // namespace muisc
