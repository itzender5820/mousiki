// meta_state.h -- the meta data editor's state, kept apart from app.h so that a change here only rebuilds the files that include this header
// and not the whole program. App holds it behind a pointer (App::meta_).
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <memory>
#include <thread>
#include <future>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <array>
#include "meta_editor.h"
#include "cover_art.h"
#include "local_source.h"

namespace muisc {

namespace fs = std::filesystem;

struct MetaCovItem { int special = 0; CoverCandidate c; };   // special: 0 a picture, 1 "automatic" (the search decides), 2 "no cover", 3 "video thumbnail, else cover"

struct MetaSaveJob {
    bool active = false;
    std::vector<MetaEditEntry> todo;    // the session as it was when saving started
    size_t next = 0;                    // first entry not processed yet
    std::vector<MetaEditEntry> failed;  // processed, could not be written (stay pending)
    int ok = 0;
    std::vector<std::pair<std::string, std::string>> renamed;   // old -> new path of files the save renamed
    std::string first_error;
    bool cancel = false;
    std::chrono::steady_clock::time_point started;
    std::chrono::steady_clock::time_point last_persist;
};

enum class MetaPrompt { None, Save, Discard, Fetch, ClearFetch };   // ClearFetch: SHIFT+d / SHIFT+DEL on the fetch list

struct MetaState {
    int media = -1;     // SHIFT+v in the meta editor's library pane
    int tab = 0;
    int focus = 0; // 0=search field, 1=library picker, 2=field editor -- cycled with Tab
    std::string query;
    std::vector<LocalTrack> lib_view;  // filter_and_rank_local(query)
    bool folder_view = false;
    std::string folder_hover;
    std::unordered_set<std::string> folder_closed;
    bool resort_edited = false;
    bool sort_added = false;     // 'n': the library pane newest-arrived first
    int lib_selected = 0;
    int field = 0; // hovered row of the field editor (0=FILE .. 4=YEAR, kMetaFieldCount = the "Cover Options" button)
    bool field_editing = false;
    std::vector<MetaEditEntry> session;          // the pending edits
    std::vector<std::string> fetch_list;         // paths queued for an AcoustID lookup
    unsigned fetch_opts = kMetaFetchDefault;   // the ticked boxes of the FETCH LIST tab (used by ENTER = fetch the full list)
    unsigned prompt_opts = kMetaFetchDefault;  // what the pending fetch confirmation will fetch (SHIFT+b missing / SHIFT+n all / the list's boxes)
    int opt_cursor = 0;                          // the box the option line's cursor is on
    int fetch_skipped = 0;                       // files of the running batch that had nothing missing
    std::string fetch_note;                      // first MusicBrainz hiccup of the running batch
    std::unordered_map<std::string, std::array<bool, kMetaFieldCount>> fetch_want;
    bool cov_open = false;
    bool cov_search = false;       // the file name search field has the keyboard
    bool cov_ask_delete = false;
    int cov_sel = 0;
    std::string cov_query;
    std::string cov_track;
    std::string cov_msg;
    std::vector<MetaCovItem> cov_items;
    int pane_col = 0;     // screen column of the FIELDS panel (where a full-resolution cover picture starts)
    bool sty_open = false;
    int sty_sel = 0;
    bool sty_pair = false;   // the track is a song paired with a video: the row "Use mp4 audio for this song" is listed (worked out when the overlay opens)
    std::string sty_track;
    std::string sty_msg;
    int sty_pic = 0;
    bool sty_from_main = false;   // opened by HKeyStyleOptions: ESC returns to the main UI
    bool choice_open = false;
    int choice_field = 0;
    int choice_sel = 0;
    int fetch_focus = 0;                         // FETCH LIST tab: 0 = the list, 1 = the option line in the header
    MetaSaveJob save;
    int fetch_selected = 0;
    bool session_loaded = false; // load_session() already ran (once per run, not once per open)
    std::string status; // bottom line of the overlay; cleared on (re)entry
    MetaPrompt prompt = MetaPrompt::None;
    std::vector<std::string> prompt_paths;
    fs::path script; // scripts/fetch_meta.py, next to the exe
    std::thread fetch_thread;
    std::atomic<bool> fetch_running{false};
    MetaFetchCancel fetch_cancel;          // SHIFT+c on the Fetch List tab
    std::mutex fetch_mutex;
    std::vector<MetaFetchResult> fetch_results;
    std::vector<std::string> fetch_ok_paths;
    bool fetch_done = false;
    MetaFetchOutcome fetch_outcome;
    std::string fetch_progress; // "3/7", shown while a batch runs
    std::array<std::string, kMetaFieldCount> hover_values{};
    bool hover_resolved = false; // tags for that path are final (vs. not read yet)
    int filter = 0;
    int page_rows = 10;  // rows of a list pane (set while drawing): SHIFT+up / down move this far
};

} // namespace muisc
