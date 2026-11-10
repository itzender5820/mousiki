// Radio YouTube search: finds the helper script (through MOUSIKI_SCRIPTS_DIR when it is set) and runs
// the search for the radio's online station lookup.
#include "radio_yt.h"
#include "online_source.h"
#include "youtube_source.h"
#include "path_utf8.h"
#include "scripts_env.h"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <thread>
#ifdef _WIN32
#include "win_compat.h"
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace muisc::radio {
namespace fs = std::filesystem;

namespace {
// Same candidate list as the player's find_scripts_file() (which lives in app.cpp's anonymous namespace): the env
// override, then scripts/ next to the executable and one level above it -- never ./scripts of the folder the program
// was started in (that folder's Python code would run). Missing is fine -- OnlineSource then falls back to yt-dlp's
// own search.
fs::path find_search_script() {
    const std::string name = "fast_yt_search.py";
    {
        fs::path p = muisc::scripts_file_from_env(name);   // MOUSIKI_SCRIPTS_DIR, checked (see scripts_env.h)
        if (!p.empty()) return p;
    }
    fs::path exe_dir;
#ifdef _WIN32
    std::string exe = win_executable_path();
    if (!exe.empty()) exe_dir = path_from_utf8(exe).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        fs::path p = fs::canonical(fs::path(buf), ec);
        if (!ec) exe_dir = p.parent_path();
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; exe_dir = fs::path(buf).parent_path(); }
#endif
    if (!exe_dir.empty()) {
        fs::path p = exe_dir / "scripts" / name;
        if (fs::exists(p)) return p;
        p = exe_dir.parent_path() / "scripts" / name;
        if (fs::exists(p)) return p;
    }
    return exe_dir.empty() ? fs::path() : exe_dir / "scripts" / name;
}
} // namespace

struct RadioYt::State {
    mutable std::mutex m;
    std::condition_variable cv;
    int jobs = 0;
    YtSnapshot snap;
    unsigned generation = 0;       // bumped by every search / clear; a stale job's result is dropped
    fs::path dir;
    fs::path script;
    bool script_looked = false;
};

RadioYt::RadioYt() : st_(std::make_shared<State>()) {}

RadioYt::~RadioYt() {
    std::unique_lock<std::mutex> lk(st_->m);
    st_->cv.wait(lk, [&] { return st_->jobs == 0; });
}

void RadioYt::set_download_dir(const fs::path& dir) {
    std::lock_guard<std::mutex> lk(st_->m);
    st_->dir = dir;
}

YtSnapshot RadioYt::snapshot() const {
    std::lock_guard<std::mutex> lk(st_->m);
    return st_->snap;
}

void RadioYt::clear() {
    std::lock_guard<std::mutex> lk(st_->m);
    ++st_->generation;
    // keep items that are still downloading out of the picture -- their jobs find the generation changed and stop
    st_->snap = YtSnapshot{};
}

void RadioYt::search(const std::string& query) {
    unsigned gen;
    fs::path script;
    {
        std::lock_guard<std::mutex> lk(st_->m);
        gen = ++st_->generation;
        st_->snap = YtSnapshot{};
        st_->snap.searching = true;
        st_->snap.query = query;
        if (!st_->script_looked) { st_->script = find_search_script(); st_->script_looked = true; }
        script = st_->script;
        ++st_->jobs;
    }
    auto st = st_;
    std::thread([st, gen, query, script] {
        OnlineSource src;
        std::vector<OnlineResult> res;
        try { res = src.search(query, 8, path_utf8(script)); } catch (...) {}   // (an exception out of this thread would end the program: "nothing found")
        std::lock_guard<std::mutex> lk(st->m);
        if (st->generation == gen) {
            st->snap.searching = false;
            st->snap.searched = true;
            for (auto& r : res) {
                YtItem it;
                it.video_id = r.video_id; it.title = r.title; it.uploader = r.uploader; it.duration_sec = r.duration_sec;
                st->snap.items.push_back(std::move(it));
            }
            if (res.empty()) st->snap.error = "nothing found (is yt-dlp installed and the network reachable?)";
        }
        --st->jobs;
        st->cv.notify_all();
    }).detach();
}

void RadioYt::download(size_t index) {
    unsigned gen;
    YtItem item;
    fs::path dir;
    {
        std::lock_guard<std::mutex> lk(st_->m);
        if (index >= st_->snap.items.size()) return;
        YtItem& it = st_->snap.items[index];
        if (it.state == 1 || it.state == 2) return;
        it.state = 1;
        it.note.clear();
        item = it;
        gen = st_->generation;
        dir = st_->dir;
        ++st_->jobs;
    }
    auto st = st_;
    std::thread([st, gen, index, item, dir] {
        std::string err;
        std::string file;
        bool ok = false;
        try {
            CacheManager cache;
            if (!dir.empty()) cache.set_download_dir(dir);
            YoutubeSource yt(cache);
            // the player splits "Artist - Title" itself; a YouTube result's uploader is the best artist guess we have
            auto r = yt.resolve_by_id(item.video_id, item.title, "", &err);
            if (r) { ok = true; file = path_utf8(r->cached_path.filename()); }
        } catch (const std::exception& e) {
            err = e.what();
        }
        std::lock_guard<std::mutex> lk(st->m);
        if (st->generation == gen && index < st->snap.items.size() && st->snap.items[index].video_id == item.video_id) {
            YtItem& it = st->snap.items[index];
            it.state = ok ? 2 : 3;
            it.note = ok ? file : (err.empty() ? std::string("download failed") : err);
        }
        --st->jobs;
        st->cv.notify_all();
    }).detach();
}

} // namespace muisc::radio
