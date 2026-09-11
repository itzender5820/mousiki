#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "disk_art.h"
#include "fft_visualizer.h"
#include "local_source.h"
#include "lyrics_fetcher.h"
#include "metadata_probe.h"
#include "native_duration.h"
#include "online_source.h"
#include "player.h"
#include "settings.h"
#include "sphere_visualizer.h"
#include "streaming_pcm.h"
#include "terminal_ui.h"
#include "waveform.h"
#include "youtube_source.h"
#include "cache_manager.h"

namespace muisc {

enum class Mode { Browse, Search, Settings, ColorEdit };
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
    static constexpr int kListVisibleRows = 8;

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

    // --- async track loading ---
    struct PendingLoad {
        bool success = false;
        std::string title, artist, location_label, error;
        fs::path path;
        std::shared_ptr<StreamingPcm> pcm;
        size_t total_sec = 0;
        TrackMetadata metadata;
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
    int main_frame_height(int w) const; // total rows the Browse-mode player view renders -- Settings must match this exactly
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
