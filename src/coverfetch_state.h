// coverfetch_state.h -- the cover fetch overlay's state, kept apart from app.h so that a change here only rebuilds the files that include this header
// and not the whole program. App holds it behind a pointer (App::cf_).
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

namespace muisc {

namespace fs = std::filesystem;

struct CfAlbum { std::string name, artist; std::vector<std::string> tracks; };
struct CfFolder { std::string path, name; std::vector<CfAlbum> albums; bool only_album = false; };
struct CfRow { int folder = 0; int album = -1; };   // album -1: the folder's own row
struct CfFetchItem { std::string album, artist; std::vector<std::string> folders, tracks; std::string state; bool done = false, ok = false; };

enum class CfPrompt { None, Fetch, Delete, ClearList, ClearAlbum };

struct CoverFetchState {
    int tab = 0;
    int focus = 1;                  // 0 search bar, 1 album library, 2 the pictures of the highlighted album
    std::string query;
    int filter = 0;                 // 0 all albums, 1 no cover, 2 one cover, 3 several covers, 4 previous unsuccessful fetch
    bool full_res = true;          // [t]: the cover as a real picture (cover fetch menu and meta editor; the default) (Kitty / Sixel) instead of braille dots
    int art_col = 0;        // screen column of the ALBUM ART pane (set while drawing)
    bool cur_fallback = false;   // the picture playback would use is another album's (loose name match in the folder)
    std::string pic_key;    // what the picture in gfx_ shows
    int selected = 0, art_sel = 0, list_sel = 0;
    int page_rows = 10;
    std::string status;
    std::vector<CfFolder> folders;  // the index of what the search shows
    std::vector<CfRow> rows;        // the visible rows (folders and, under the open ones, their albums)
    std::set<std::string> open;     // the folders that are opened
    std::vector<CfFetchItem> list;
    std::map<std::string, int> covers;      // "folder|album key" -> number of covers (filled in by the worker)
    std::mutex mutex;
    std::thread index_thread;
    std::atomic<unsigned> gen{0};           // a new search / close stops the running index worker
    bool index_ready = false;               // folders was swapped in (poll_cover_fetch)
    std::vector<CfFolder> pending_folders;  // the worker's result, taken by poll_cover_fetch()
    std::atomic<bool> pending_ready{false};
    std::atomic<int> progress{0}, total{0};
    std::atomic<bool> scanning{false};
    int untagged = 0;                       // tracks of the search without an album tag (not listed)
    std::thread fetch_thread;
    std::atomic<bool> fetch_running{false};
    MetaFetchCancel fetch_cancel;
    fs::path script;                        // scripts/fetch_cover.py
    std::string fetch_progress;
    CfPrompt prompt = CfPrompt::None;
    std::string cur_key;
    CoverSource cur_src;
    std::vector<CoverCandidate> cands;
    std::string keep_folder, keep_album;
    bool keep_ready = false;
    std::unordered_map<std::string, std::string> album_override;   // path -> album tag pending in the meta editor (for the index worker)
    int marq_idx = -1;
    std::chrono::steady_clock::time_point marq_since;
    int marq_art[2] = {-1, -1};
    std::chrono::steady_clock::time_point marq_art_since[2];
};

} // namespace muisc
