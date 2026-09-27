#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "disk_art.h"
#include "external_control.h"
#include "fft_visualizer.h"
#include "local_source.h"
#include "lyrics_fetcher.h"
#include "metadata_probe.h"
#include "native_duration.h"
#include "online_source.h"
#include "player.h"
#include "settings.h"
#include "snapshot.h"
#include "sphere_visualizer.h"
#include "streaming_pcm.h"
#include "terminal_ui.h"
#include "waveform.h"
#include "youtube_source.h"
#include "cache_manager.h"

namespace muisc {

enum class Mode { Browse, Search, Settings, ColorEdit, Console, Cheatsheet, BulkAdd, RetryLyrics, External };
enum class ListSource { Local, Online };

struct QueueItem {
    bool is_local;
    std::string title;
    std::string artist;
    fs::path local_path;   // valid if is_local
    std::string video_id;  // valid if !is_local
};

class App {
public:
    App();
    int run();

private:
    // --- infrastructure ---
    CacheManager cache_;
    YoutubeSource youtube_{cache_};
    OnlineSource online_;
    LocalSource local_source_;
    DiskArt disk_;
    mutable Player player_;
    fs::path lyrics_script_;

    // --- lists / navigation ---
    Mode mode_ = Mode::Browse;
    ListSource list_source_ = ListSource::Local;
    std::vector<LocalTrack> all_local_tracks_;
    std::vector<LocalTrack> local_view_;     // filtered
    std::vector<OnlineResult> online_view_;
    int selected_ = 0;
    int scroll_ = 0;
    std::string search_buffer_;
    std::string last_local_query_;
    ListSource pre_search_list_source_ = ListSource::Local;
    std::string pre_search_local_query_;
    std::string last_online_query_;
    int local_sort_mode_ = 0; // 0=folder order, 1=title A-Z, 2=artist A-Z
    static constexpr int kListVisibleRows = 8; // the *maximum*/preferred list height when there's room for it

    // --- terminal-height awareness --------------------------------------
    // The render loop used to only ever look at term.cols() (see the
    // comment on the width clamp in render_frame()) and unconditionally
    // emitted a fixed-height frame — metadata panel + progress + search
    // bar + a hardcoded kListVisibleRows-row list/queue panel + status
    // line — redrawn purely with "\x1b[H" + "\x1b[0J" and no alternate-
    // screen buffer. On a terminal shorter than that fixed height, each
    // frame overflows the viewport and scrolls; the next frame's
    // "\x1b[H" then homes to the top of the *new* scrolled-into-view
    // position rather than the top of the previous frame, so it draws a
    // fresh copy further down, which overflows again, forever — visible
    // as the panel endlessly re-duplicating itself downward.
    //
    // term_rows_ is the real, current terminal row count (from
    // TerminalIO::rows(), which does read the OS via ioctl(TIOCGWINSZ)
    // correctly -- it just wasn't being consulted anywhere). Refreshed
    // once per frame at the top of render_frame(). list_visible_rows_ is
    // how many list/queue rows *actually* fit this frame -- clamped
    // between 0 and kListVisibleRows based on how much room term_rows_
    // leaves after the fixed chrome (metadata/progress/search bar/status
    // line). Every place that used to scroll-clamp or size against the
    // kListVisibleRows constant now uses this instead, so what's
    // rendered and what the scroll math thinks is visible never
    // disagree. render_frame() also applies a hard line-count safety net
    // (see clamp_output_rows()) on top of this, so even a terminal too
    // short for the fixed chrome alone (metadata+progress+search bar)
    // still can never scroll — the two mechanisms are independent, not
    // "either/or".
    int term_rows_ = 24;
    int list_visible_rows_ = kListVisibleRows;
    std::string clamp_output_rows(const std::string& frame, int term_rows) const;

    mutable std::mutex row_meta_mutex_;
    std::unordered_map<std::string, RowMeta> row_meta_cache_;
    std::atomic<bool> row_meta_resolver_started_{false};
    void launch_row_meta_resolver();

    // --- queue ---
    std::vector<QueueItem> queue_;
    int queue_selected_ = 0;   // cursor/"hovering" row, only meaningful once queue_focus_ has been used
    int queue_scroll_ = 0;
    bool queue_focus_ = false; // Tab toggles which panel Up/Down navigates

    // --- now playing ---
    bool has_track_ = false;
    fs::path current_path_;
    bool current_is_local_ = true;  // for snapshot identity -- current_path_ alone is ambiguous
                                     // (online tracks resolve to a cache path too)
    std::string current_video_id_;  // valid when !current_is_local_
    TrackMetadata metadata_;
    std::vector<float> waveform_envelope_;
    std::chrono::steady_clock::time_point waveform_reveal_start_;
    bool waveform_ready_ = false;
    std::shared_ptr<StreamingPcm> current_pcm_;
    size_t total_sec_ = 0;
    double angle_ = 0.0;
    std::chrono::steady_clock::time_point last_frame_time_;
    static constexpr double kAngularVelocity = (2.0 * 3.14159265358979323846 / 48.0) / 0.035;
    mutable FftVisualizer fft_;
    mutable SphereVisualizer sphere_;
    mutable std::string last_lyrics_status_;
    mutable std::chrono::steady_clock::time_point lyrics_status_shown_at_;
    mutable double viz_dt_ = 0.08;

    // --- lyrics (background-fetched) ---
    mutable std::mutex lyrics_mutex_;
    mutable LyricsResult lyrics_result_;
    std::atomic<bool> lyrics_ready_{false};
    std::atomic<int> lyrics_epoch_{0};
    void launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network = false);

    std::string status_line_;
    bool quit_ = false;
    bool force_redraw_ = false;
    int last_render_w_ = -1;
    Mode last_render_mode_ = Mode::Browse;

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
    void draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines, int panel_w, int W) const;
    static constexpr int kFloatingPanelUpShift = 3; // rows nudged above true vertical center

    // --- console / log overlay (HKeyConsole) ----------------------------
    // Backed by the global ConsoleLog (console_log.h/.cpp), which owns
    // both the on-disk $HOME/.cache/mousiki/logs/console.log and the
    // in-memory buffer this overlay renders -- see log_event() and
    // build_console_screen().
    void log_event(const std::string& msg);
    void build_console_screen(std::ostringstream& frame, int W, int target_height) const;

    // --- cheatsheet overlay (HKeyCheatsheet) ----------------------------
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

    // --- autosave / session snapshot ------------------------------------
    // Consumed exactly once, right after a startup restore: the very
    // first launch_device_play_async() call for the restored track uses
    // this as its start position instead of 0.0, then zeroes it out so
    // every normal track change afterwards starts at 0 like always.
    double resume_start_sec_ = 0.0;
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
    struct BulkAddResult {
        bool success = false;
        std::string error;
        std::vector<OnlineResult> items;
    };
    std::thread bulk_add_thread_;
    std::mutex bulk_add_mutex_;
    std::atomic<bool> bulk_add_in_progress_{false};
    std::atomic<bool> bulk_add_ready_{false};
    BulkAddResult pending_bulk_add_;
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
    static constexpr int kRetryLyricsPanelWidth = 62; // matches the reference design exactly
    // The fields actually navigable right now, in on-screen order --
    // RemixText/OtherText only appear in this list once their checkbox
    // is on, which is what makes them "dynamically available".
    std::vector<RLField> rl_visible_fields() const;
    void rl_open_from_current_track();     // pre-fill + reset state, called when 'l' opens the panel
    void rl_submit();                       // builds the override query and launches the fetch
    bool* rl_bool_ptr(RLField f);           // nullptr for non-checkbox fields
    std::string* rl_text_ptr(RLField f);    // nullptr for checkbox fields
    std::vector<std::string> build_retry_lyrics_panel() const; // fixed-width, fixed-height lines for draw_floating_panel()

    // --- external player control (MPRIS: browser / Spotify) ------------
    // mousiki's own playback (player_/has_track_) only ever covers audio
    // *it* decoded. This block is the other half: attaching to a player
    // running outside the process -- a browser tab, the Spotify desktop
    // app -- and driving it over MPRIS, so the same transport keys work
    // whether the sound is coming from us or from something else.
    //
    // Every ExternalControl call is a blocking subprocess round-trip
    // (tens of milliseconds), which is far too slow for a 40ms render
    // loop, so *nothing* here is called from the UI thread. ext_thread_
    // owns all of it: it re-polls the attached player every
    // settings_.external_poll_ms, rescans the player list while the
    // picker is open, and executes transport commands pushed onto
    // ext_cmd_queue_ by handle_key(). The UI thread only ever touches
    // the published snapshots under ext_mutex_.
    ExternalControl ext_;
    bool ext_attached_ = false;
    ExternalPlayer ext_target_;              // which player we're driving; valid when ext_attached_

    mutable std::mutex ext_mutex_;           // guards ext_state_/ext_players_/ext_state_at_/ext_cmd_queue_
    ExtTrackState ext_state_;                // last published snapshot of the attached player
    std::vector<ExternalPlayer> ext_players_;// last published discovery result
    // When ext_state_ was sampled. The attached player's real position
    // only refreshes once per poll interval, which would make the
    // progress bar visibly step rather than move; ext_elapsed() adds the
    // wall-clock time since this instant while the player is Playing so
    // the bar advances smoothly between polls, then snaps back to truth
    // on the next one.
    std::chrono::steady_clock::time_point ext_state_at_;
    std::vector<std::function<void()>> ext_cmd_queue_;

    std::thread ext_thread_;
    std::condition_variable ext_cv_;         // wakes the thread early for a queued command / fresh scan
    std::atomic<bool> ext_thread_stop_{false};
    std::atomic<bool> ext_scan_pending_{false};
    // A bare notify_one() on a predicate-guarded wait_for() does nothing:
    // the thread wakes, re-evaluates the predicate, finds no queued
    // command and goes straight back to waiting out the rest of the
    // interval. This flag is what actually lets the UI thread say "stop
    // waiting and do a pass right now" -- on attach, and when the picker
    // opens and would otherwise show an empty list for half a second.
    std::atomic<bool> ext_wake_{false};
    std::atomic<bool> ext_dirty_{false};     // a new snapshot is waiting to be folded into the UI state

    // Volume is the one control with no safe "read it back later" story:
    // MPRIS volume is per-player and detaching should not leave the
    // browser muted, so the pre-attach value is remembered and restored.
    int ext_pre_attach_volume_ = -1;
    // A single failed poll is not proof the player died -- D-Bus returns
    // a transient ServiceUnknown while a browser is swapping which tab
    // owns the MPRIS name, and dropping the attachment on that blip
    // would make the feature feel flaky. Only a run of them counts.
    int ext_invalid_polls_ = 0;
    static constexpr int kExtInvalidPollsBeforeDetach = 3;
    bool ext_muted_ = false;
    int ext_pre_mute_volume_ = 100;
    // Guards against re-firing a lyrics fetch every poll: holds the
    // "title\x1fartist" of whatever we last fetched for, so a fetch only
    // happens when the external player actually changes track.
    std::string ext_lyrics_key_;

    // While attached, the external player's title/artist/length are
    // mirrored into metadata_/total_sec_/has_track_ so every existing
    // panel keeps rendering from one source of truth instead of each
    // learning about a second engine. That overwrites whatever the local
    // track had, so the originals are parked here and put back on
    // detach -- detaching returns you to exactly the view you left.
    TrackMetadata ext_saved_metadata_;
    size_t ext_saved_total_sec_ = 0;
    bool ext_saved_has_track_ = false;
    std::vector<float> ext_saved_waveform_;
    bool ext_saved_waveform_ready_ = false;

    // Picker panel (floating, same mechanism as Bulk Add).
    int ext_cursor_ = 0;
    int ext_scroll_ = 0;
    static constexpr int kExternalPanelWidth = 62;   // matches the Bulk Add / Retry Lyrics panels
    static constexpr int kExternalVisibleRows = 7;

    void ext_start_thread();
    void ext_stop_thread();
    // Pushes a transport command onto the queue and wakes the thread.
    // Deliberately fire-and-forget: a keypress must never block a frame
    // waiting on a D-Bus round-trip to a player that may be wedged.
    void ext_enqueue(std::function<void()> cmd);
    // Reads the published volume, adjusts by delta and writes it back --
    // external volume is the player's state, not ours.
    void ext_nudge_volume(int delta_pct);
    // Browsers accept MPRIS Seek and then ignore the offset, stepping
    // their own fixed ~5s instead, so an accurate jump has to go through
    // SetPosition with the current trackid. This picks whichever the
    // attached player can actually honor.
    void ext_seek(double delta_sec);
    // Flips the published status optimistically, because PlaybackStatus
    // lags the command by a few hundred milliseconds and the button
    // would otherwise look like it ignored the first press.
    void ext_toggle_play_pause();
    void poll_pending_external();            // UI-thread side: folds a published snapshot into metadata_/total_sec_
    void ext_attach(const ExternalPlayer& p);
    void ext_detach();
    // Position with the between-polls interpolation described above.
    double ext_elapsed() const;
    std::vector<std::string> build_external_panel() const;

    // --- unified transport view -----------------------------------------
    // What the UI should *display* and what the transport keys should
    // *drive*, independent of which engine is actually producing sound.
    // While attached these read the external player's published snapshot;
    // otherwise they fall through to player_ exactly as before.
    double current_elapsed() const;
    bool current_paused() const;

    // --- async track loading ---
    struct PendingLoad {
        bool success = false;
        std::string title, artist, location_label, error;
        fs::path path;
        std::shared_ptr<StreamingPcm> pcm;
        size_t total_sec = 0;
        TrackMetadata metadata;
        bool is_local = true;   // for snapshot/resume identity -- which of path/video_id is authoritative
        std::string video_id;   // valid if !is_local
    };
    std::thread load_thread_;
    std::mutex load_mutex_;
    std::atomic<bool> load_ready_{false};
    std::atomic<bool> load_in_progress_{false};
    std::atomic<int> load_stage_{0};
    std::chrono::steady_clock::time_point load_started_at_;
    std::thread device_thread_;
    std::mutex device_mutex_;
    std::atomic<int> device_gen_{0}; // incremented each launch; stale threads abort when their gen != current
    void launch_device_play_async();

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
    void launch_search_async(const std::string& query);
    void poll_pending_search();

    // --- settings panel (5 tabs: Colors, On/Off, Animation, Reference, About App) ---
    // Rendering uses absolute cursor positioning (\x1b[y;xH) rather than
    // building padded strings line by line -- each field goes exactly
    // where it's told regardless of what else is on that row, which is
    // what actually fixes the truncation-corrupts-everything fragility
    // class of bug (a mis-sized pad on one row used to bleed into
    // whatever the next escape code was).
    Settings settings_;
    static constexpr int kSettingsTabCount = 5; // Colors, On/Off, Animation, Reference, About App
    int settings_tab_ = 0;
    int settings_row_ = 0;   // resets to 0 on every tab switch
    int settings_col_ = 0;   // 0 or 1 -- only the Colors tab has 2-cell rows
    std::string color_edit_buffer_;      // live text while mode_==ColorEdit
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
    // Left/Right quick-cycle for rows with a fixed set of options (bools,
    // enums). No-op for rows that don't have one (colors, hotkeys) --
    // those are Enter-to-type only.
    void settings_cycle(int dir);
    std::vector<std::string> settings_options_for(int tab, int row) const;
    int settings_max_row() const; // last valid row index for the current tab
    // (main_frame_height() was removed -- see the comment where it used to
    // live in app.cpp, right before build_settings_screen(). Every overlay
    // now sizes off term_rows_ directly instead.)
    void build_settings_screen(std::ostringstream& frame, int W, int player_h) const;
    void handle_settings_key(int key);

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
    static const char* sort_mode_name(int mode);
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
    // Pops (or, in Repeat Queue mode, rotates to the back instead of
    // discarding) the next item to play from queue_, honoring the
    // current play_mode: Shuffle picks a random queue item rather than
    // strictly FIFO order, Repeat Queue keeps the queue looping
    // indefinitely instead of draining it. Shared by advance_track()
    // (auto-advance on finish) and the manual "n" key (explicit skip),
    // so both respect the queue exactly the same way. Caller must check
    // !queue_.empty() first.
    void play_next_from_queue();
    // Single letter for the mode-indicator button after the search bar:
    // L=list, R=repeat, S=shuffle, Q=repeat queue, O=stop (play-and-stop
    // -- not "S", that's shuffle's letter already).
    char play_mode_letter() const;
    void queue_add_selected();
    void queue_remove_last();
    void queue_remove_hovering();
    void queue_move_hovering(int dir); // dir=-1 up, +1 down
    void clamp_queue_selected();
    void handle_key(int key);
    void ensure_visible_row_meta();
    void recompute_waveform_for_current_track();
    std::string render_frame(TerminalIO& term);

    // --- box drawing helpers (use configured border chars) ---
    std::string box_top(const std::string& label, int total_width, const std::string& border_ansi = "") const;
    std::string box_bottom(int total_width, const std::string& footer = "", const std::string& border_ansi = "") const;
    std::string box_line(const std::string& content, int total_width, const std::string& border_ansi = "") const;

    // panel builders
    std::vector<std::string> build_metadata_panel(int width) const;
    std::vector<std::string> build_progress_panel(int width) const;
    std::vector<std::string> build_search_bar(int width) const;
    std::vector<std::string> build_list_panel(int width, int height) const;
    std::vector<std::string> build_queue_panel(int width, int height) const;
};

} // namespace muisc
