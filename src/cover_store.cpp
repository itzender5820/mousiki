// Fetched album covers: saves covers downloaded for an album in a folder next to the music and finds
// them again by the album name.
#include "cover_store.h"
#include "video_fx.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <ctime>
#include <map>
#include <mutex>
#include <sstream>

#include "path_utf8.h"
#include "picture_formats.h"
#include "process_util.h"
#include "tiny_json.h"
#include "settings.h"   // write_text_atomic()
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

namespace fs = std::filesystem;

std::string cover_name_key(const std::string& s) {
    std::string o;
    bool space = true;
    for (unsigned char c : s) {
        if (c >= 0x80) { o += static_cast<char>(c); space = false; }
        else if (std::isalnum(c)) { o += static_cast<char>(std::tolower(c)); space = false; }
        else if (!space) { o += ' '; space = true; }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

std::string cover_file_name(const std::string& album) {
    std::string o;
    for (unsigned char c : album) o += (c < 32 || std::string("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string::npos) ? '_' : static_cast<char>(c);
    while (!o.empty() && (o.back() == ' ' || o.back() == '.')) o.pop_back();
    while (!o.empty() && o.front() == ' ') o.erase(o.begin());
    if (o.size() > 100) { o.resize(100); while (!o.empty() && (static_cast<unsigned char>(o.back()) & 0xC0) == 0x80) o.pop_back(); if (!o.empty()) o.pop_back(); }
    return o.empty() ? std::string("album") : o;
}

namespace {

std::string lower_ext(const fs::path& f) {
    std::string e = path_utf8(f.extension());
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool is_picture(const fs::path& f) {
    const std::string e = lower_ext(f);
    for (const char* x : kCoverPictureExtensions) if (e == x) return true;
    return false;
}

std::vector<fs::path> pictures_in(const fs::path& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && is_picture(it->path())) v.push_back(it->path());
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

fs::path fetched_cover_stem(const fs::path& folder, const std::string& album) {
    return folder / kFetchedCoversDir / path_from_utf8(cover_file_name(album));
}

fs::path find_fetched_cover(const fs::path& folder, const std::string& album) {
    const std::string want = cover_name_key(album);
    if (want.empty()) return {};
    for (const fs::path& f : pictures_in(folder / kFetchedCoversDir))
        if (cover_name_key(path_utf8(f.stem())) == cover_name_key(cover_file_name(album))) return f;
    return {};
}

std::vector<CoverCandidate> cover_candidates(const fs::path& track, bool with_embedded) {
    std::vector<CoverCandidate> v;
    const fs::path dir = track.parent_path();
    if (with_embedded) v.push_back({CoverCandidate::Embedded, track, "inside the file"});
    for (const fs::path& f : pictures_in(dir)) v.push_back({CoverCandidate::Folder, f, path_utf8(f.filename())});
    for (const fs::path& f : pictures_in(dir / kFetchedCoversDir))
        v.push_back({CoverCandidate::Fetched, f, std::string(kFetchedCoversDir) + "/" + path_utf8(f.filename())});
    return v;
}

// ---- the choices -----------------------------------------------------------------------------------------------------------
namespace {

struct Choices {
    std::mutex mu;
    bool loaded = false;
    std::map<std::string, std::string> track, album;   // track path -> choice, album key -> choice
    std::map<std::string, std::string> fetched;         // album key -> "epoch|what MusicBrainz matched" (successful fetches)
    std::map<std::string, std::string> failed;          // album key -> why the last fetch found nothing
    std::map<std::string, std::string> look;            // album key (or "dir:<folder>") -> the cover look of that album ("0".."7")
    std::map<std::string, std::string> style;           // track path -> "look,contrast,brightness,colors" (-1 = as in Settings)
    std::map<std::string, std::string> vopts;           // track path -> "on,size,fx,contrast,colors,chroma,res" (-1 = as in the VIDEO overlay)
    std::map<std::string, std::string> vmode;           // track path -> "1" cover / "2" video thumbnail / "3" plain disk (absent: as in the VIDEO overlay)
    std::atomic<unsigned> revision{1};
};

Choices& choices() {
    static Choices c;
    return c;
}

fs::path choices_file() {
    const char* home = std::getenv("HOME");
    const fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return base / ".cache" / "mousiki" / "covers" / "choices.txt";
}

// Reads choices.txt once (later calls return at once). Each line is a tab-separated record with a one-letter tag:
// T = track path -> choice, A = album key -> choice, R = successful fetch, F = failed fetch, L = look of an album, S = style of a track. Lines with a bad layout or an unknown tag are skipped.
void load_locked(Choices& c) {
    if (c.loaded) return;
    c.loaded = true;
    std::ifstream in(choices_file(), std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t a = line.find('\t');
        const size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos || a != 1) continue;
        const std::string key = line.substr(2, b - 2), val = line.substr(b + 1);
        if (val.empty()) continue;
        if (line[0] == 'T') {
            // (an earlier version kept the video choice as a picture choice: "@cover" / "@thumbnail" -- that is a mode of its own now)
            if (val == "@cover") c.vmode[key] = "1";
            else if (val == "@thumbnail") c.vmode[key] = "2";
            else if (val == "-") c.vmode[key] = "3";   // "No cover" of a track is the plain disk now ("The disk shows")
            else c.track[key] = val;
        }
        else if (line[0] == 'V') c.vmode[key] = val;
        else if (line[0] == 'A') c.album[key] = val;
        else if (line[0] == 'F') c.failed[key] = val;
        else if (line[0] == 'R') c.fetched[key] = val;
        else if (line[0] == 'L') c.look[key] = val;
        else if (line[0] == 'S') c.style[key] = val;
        else if (line[0] == 'W') c.vopts[key] = val;
    }
}

// Rewrites the whole file from the in-memory maps (the same T/A/R/F records that load_locked() reads).
void save_locked(Choices& c) {
    const fs::path p = choices_file();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);   // errors are ignored here; if the folder could not be created, the write below simply fails
    std::ostringstream out;
    for (const auto& kv : c.track) out << "T\t" << kv.first << "\t" << kv.second << "\n";
    for (const auto& kv : c.album) out << "A\t" << kv.first << "\t" << kv.second << "\n";
    for (const auto& kv : c.fetched) out << "R\t" << kv.first << "\t" << kv.second << "\n";
    for (const auto& kv : c.failed) out << "F\t" << kv.first << "\t" << kv.second << "\n";
    for (const auto& kv : c.look) out << "L\t" << kv.first << "\t" << kv.second << "\n";
    for (const auto& kv : c.vmode) out << "V\t" << kv.first << "\t" << kv.second << "\n";
    for (const auto& kv : c.vopts) out << "W\t" << kv.first << "\t" << kv.second << "\n";
    for (const auto& kv : c.style) out << "S\t" << kv.first << "\t" << kv.second << "\n";
    write_text_atomic(p, out.str());   // (never a cut-off file: every choice is in it)
}

} // namespace

bool cover_recently_fetched(const std::string& album, std::string* info, int days) {
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const auto it = c.fetched.find(cover_name_key(album));
    if (it == c.fetched.end()) return false;
    const size_t bar = it->second.find('|');
    const long long when = std::atoll(it->second.substr(0, bar).c_str());
    if (std::time(nullptr) - when > static_cast<long long>(days) * 86400LL) return false;
    if (info) *info = bar == std::string::npos ? std::string() : it->second.substr(bar + 1);
    return true;
}

void mark_cover_fetched(const std::string& album, const std::string& info) {
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const std::string key = cover_name_key(album);
    if (key.empty()) return;
    std::string t = info;
    for (char& ch : t) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    c.fetched[key] = std::to_string(static_cast<long long>(std::time(nullptr))) + "|" + t;
    save_locked(c);
}

namespace {
// An album's key for its look: the album name (as for the album's picture); a track without an album tag: its folder.
std::string look_key(const std::string& album, const std::string& folder) {
    const std::string a = album == "-" ? std::string() : album;
    const std::string k = cover_name_key(a);
    return k.empty() ? (folder.empty() ? std::string() : "dir:" + folder) : k;
}
}  // namespace

int cover_album_look(const std::string& album, const std::string& folder) {
    const std::string key = look_key(album, folder);
    if (key.empty()) return -1;
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const auto it = c.look.find(key);
    if (it == c.look.end()) return -1;
    const int v = std::atoi(it->second.c_str());
    return v >= 0 && v <= kCoverLookCount - 1 ? v : -1;
}

void set_cover_album_look(const std::string& album, const std::string& folder, int look) {
    const std::string key = look_key(album, folder);
    if (key.empty()) return;
    Choices& c = choices();
    {
        std::lock_guard<std::mutex> lk(c.mu);
        load_locked(c);
        if (look < 0 || look >= kCoverLookCount) c.look.erase(key); else c.look[key] = std::to_string(look);
        save_locked(c);
    }
    cover_store_touch();
}

CoverStyle cover_track_style(const std::string& track_path) {
    CoverStyle st;
    if (track_path.empty()) return st;
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const auto it = c.style.find(track_path);
    if (it == c.style.end()) return st;
    int v[5] = {-1, -1, -1, -1, -1};
    std::istringstream in(it->second);
    std::string part;
    for (int i = 0; i < 5 && std::getline(in, part, ','); ++i) v[i] = std::atoi(part.c_str());
    st.look = v[0] >= 0 && v[0] < kCoverLookCount ? v[0] : -1;
    st.contrast = v[1] >= 0 && v[1] <= 100 ? v[1] : -1;
    st.brightness = v[2] >= 0 && v[2] <= 100 ? v[2] : -1;
    st.colors = v[3] >= 0 && v[3] <= 100 ? v[3] : -1;
    st.chroma = v[4] >= 0 && v[4] < kChromaLevels ? v[4] : -1;
    return st;
}

void set_cover_track_style(const std::string& track_path, const CoverStyle& st) {
    if (track_path.empty()) return;
    Choices& c = choices();
    {
        std::lock_guard<std::mutex> lk(c.mu);
        load_locked(c);
        if (st.look < 0 && st.contrast < 0 && st.brightness < 0 && st.colors < 0 && st.chroma < 0) c.style.erase(track_path);
        else c.style[track_path] = std::to_string(st.look) + "," + std::to_string(st.contrast) + "," + std::to_string(st.brightness) + "," + std::to_string(st.colors) + "," + std::to_string(st.chroma);
        save_locked(c);
    }
    cover_store_touch();
}

std::string cover_fetch_failure(const std::string& album) {
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const auto it = c.failed.find(cover_name_key(album));
    return it == c.failed.end() ? std::string() : it->second;
}

void set_cover_fetch_failure(const std::string& album, const std::string& reason) {
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const std::string key = cover_name_key(album);
    if (key.empty()) return;
    std::string r = reason;
    for (char& ch : r) if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    if (reason.empty()) { if (c.failed.erase(key) == 0) return; }
    else c.failed[key] = r.empty() ? std::string("not found") : r;
    save_locked(c);
}

VideoTrackOpts cover_track_video_opts(const std::string& track_path) {
    VideoTrackOpts o;
    if (track_path.empty()) return o;
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const auto it = c.vopts.find(track_path);
    if (it == c.vopts.end()) return o;
    int v[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::istringstream in(it->second);
    std::string part;
    for (int i = 0; i < 10 && std::getline(in, part, ','); ++i) v[i] = std::atoi(part.c_str());
    o.on = v[0] == 0 ? 0 : -1;
    o.size = v[1] >= 0 && v[1] <= 3 ? v[1] : -1;
    o.fx = v[2] >= 0 && v[2] < kVideoFxCount ? video_fx_canon(v[2]) : -1;
    o.contrast = v[3] >= 0 && v[3] <= 100 ? v[3] : -1;
    o.colors = v[4] >= 0 && v[4] <= 100 ? v[4] : -1;
    o.chroma = v[5] >= 0 && v[5] < kChromaLevels ? v[5] : -1;
    o.res = v[6] >= 0 && v[6] <= 2 ? v[6] : -1;
    o.round = v[7] == 0 || v[7] == 1 ? v[7] : -1;
    o.lyrics = v[8] == 0 || v[8] == 1 ? v[8] : -1;
    o.brightness = v[9] >= 0 && v[9] <= 100 ? v[9] : -1;
    return o;
}

void set_cover_track_video_opts(const std::string& track_path, const VideoTrackOpts& o) {
    if (track_path.empty()) return;
    const int old_on = cover_track_video_opts(track_path).on;
    Choices& c = choices();
    {
        std::lock_guard<std::mutex> lk(c.mu);
        load_locked(c);
        if (o.on != 0 && o.size < 0 && o.fx < 0 && o.contrast < 0 && o.colors < 0 && o.chroma < 0 && o.res < 0 && o.round < 0 && o.lyrics < 0 && o.brightness < 0) c.vopts.erase(track_path);
        else c.vopts[track_path] = std::to_string(o.on == 0 ? 0 : -1) + "," + std::to_string(o.size) + "," + std::to_string(o.fx) + "," + std::to_string(o.contrast) + "," + std::to_string(o.colors) + "," + std::to_string(o.chroma) + "," + std::to_string(o.res) + "," + std::to_string(o.round) + "," + std::to_string(o.lyrics) + "," + std::to_string(o.brightness);
        save_locked(c);
    }
    if (old_on != o.on) cover_store_touch();   // (only "video of this track" can change what the disk shows; a look change must not make the cover load again: it blinked away)
}

int cover_track_video_mode(const std::string& track_path) {
    if (track_path.empty()) return 0;
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const auto it = c.vmode.find(track_path);
    if (it == c.vmode.end()) return 0;
    const int v = std::atoi(it->second.c_str());
    return v >= 1 && v <= 3 ? v : 0;
}

void set_cover_track_video_mode(const std::string& track_path, int mode) {
    if (track_path.empty()) return;
    Choices& c = choices();
    {
        std::lock_guard<std::mutex> lk(c.mu);
        load_locked(c);
        if (mode < 1 || mode > 3) c.vmode.erase(track_path); else c.vmode[track_path] = std::to_string(mode);
        save_locked(c);
    }
    cover_store_touch();
}

std::string cover_track_choice(const std::string& track_path) {
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const auto it = c.track.find(track_path);
    return it == c.track.end() ? std::string() : it->second;
}

void set_cover_track_choice(const std::string& track_path, const std::string& choice) {
    Choices& c = choices();
    {
        std::lock_guard<std::mutex> lk(c.mu);
        load_locked(c);
        if (choice.empty()) c.track.erase(track_path); else c.track[track_path] = choice;
        save_locked(c);
    }
    cover_store_touch();
}

std::string cover_album_choice(const std::string& album) {
    const std::string key = cover_name_key(album);
    if (key.empty()) return {};
    Choices& c = choices();
    std::lock_guard<std::mutex> lk(c.mu);
    load_locked(c);
    const auto it = c.album.find(key);
    return it == c.album.end() ? std::string() : it->second;
}

void set_cover_album_choice(const std::string& album, const std::string& choice) {
    const std::string key = cover_name_key(album);
    if (key.empty()) return;
    Choices& c = choices();
    {
        std::lock_guard<std::mutex> lk(c.mu);
        load_locked(c);
        if (choice.empty()) c.album.erase(key); else c.album[key] = choice;
        save_locked(c);
    }
    cover_store_touch();
}

unsigned cover_store_revision() { return choices().revision.load(); }
void cover_store_touch() { choices().revision.fetch_add(1); }

bool cover_deletable(const CoverCandidate& c) { return c.kind != CoverCandidate::Embedded; }

bool delete_cover_candidate(const CoverCandidate& c, std::string* why) {
    if (!cover_deletable(c)) {
        if (why) *why = "the picture inside the file cannot be deleted here (the tag editor does not change pictures)";
        return false;
    }
    std::error_code ec;
    if (!fs::remove(c.file, ec) || ec) {
        if (why) *why = "could not delete " + path_utf8(c.file.filename());
        return false;
    }
    const std::string gone = path_utf8(c.file);
    Choices& ch = choices();
    {
        std::lock_guard<std::mutex> lk(ch.mu);
        load_locked(ch);
        for (auto it = ch.track.begin(); it != ch.track.end();) it = it->second == gone ? ch.track.erase(it) : std::next(it);
        for (auto it = ch.album.begin(); it != ch.album.end();) it = it->second == gone ? ch.album.erase(it) : std::next(it);
        save_locked(ch);
    }
    // an empty fetched_covers folder goes with its last picture
    if (c.kind == CoverCandidate::Fetched) fs::remove(c.file.parent_path(), ec);
    cover_store_touch();
    return true;
}


// ---- what belongs to an album ------------------------------------------------------------------------------------------------
bool has_embedded_picture(const fs::path& audio_file) {
    static std::mutex mu;
    static std::map<std::string, bool> seen;
    const std::string key = path_utf8(audio_file);
    {
        std::lock_guard<std::mutex> lk(mu);
        const auto it = seen.find(key);
        if (it != seen.end()) return it->second;
    }
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=codec_type", "-of", "csv=p=0", key}, false);
    const bool has = r.out.find("video") != std::string::npos;
    std::lock_guard<std::mutex> lk(mu);
    seen[key] = has;
    return has;
}

AlbumCovers album_covers(const fs::path& folder, const std::string& album, const fs::path& first_track, bool only_album, bool probe_embedded) {
    AlbumCovers a;
    auto add = [&](const fs::path& f) {
        if (f.empty()) return;
        for (const fs::path& g : a.files) if (g == f) return;
        a.files.push_back(f);
    };
    // Candidate order: embedded art (if probed), the fetched cover, then picture files whose names match the album.
    if (probe_embedded && !first_track.empty()) a.embedded = has_embedded_picture(first_track);
    add(find_fetched_cover(folder, album));
    // A file counts as a match when its normalised name contains the album name or vice versa (at least 3
    // characters, so short names do not match everything). Generic names such as "cover" or "folder" count only
    // when the caller asked for album-level covers.
    const std::string want = cover_name_key(album);
    for (const fs::path& f : pictures_in(folder)) {
        const std::string stem = cover_name_key(path_utf8(f.stem()));
        if (!stem.empty() && !want.empty() && (stem == want || stem.find(want) != std::string::npos || want.find(stem) != std::string::npos) && stem.size() >= 3) add(f);
        else if (only_album && (stem == "cover" || stem == "folder" || stem == "front" || stem == "albumart" || stem == "album" || stem == "thumb")) add(f);
    }
    // Choices the user made earlier (per track, then per album) are added last, if the file still exists.
    const std::string tc = first_track.empty() ? std::string() : cover_track_choice(path_utf8(first_track));
    const std::string ac = cover_album_choice(album);
    for (const std::string& c : {tc, ac})
        if (!c.empty() && c != kCoverNone && c != kCoverEmbedded) { std::error_code ec; if (fs::exists(path_from_utf8(c), ec)) add(path_from_utf8(c)); }
    return a;
}

// ---- fetching ---------------------------------------------------------------------------------------------------------------
// Parses one JSON answer line from the cover fetch helper. Anything that is not a JSON object becomes an error result
// rather than an exception, so a broken helper cannot crash the player.
CoverFetchResult parse_cover_fetch_line(const std::string& line) {
    CoverFetchResult r;
    tinyjson::Value v;
    if (line.empty() || line[0] != '{' || !tinyjson::parse(line, v) || v.type != tinyjson::Type::Object) {
        r.error = "unreadable answer";
        return r;
    }
    if (auto* ok = v.find("ok")) r.ok = ok->as_bool(false);
    if (r.ok) {
        if (auto* f = v.find("file")) r.file = f->as_string();
        if (auto* t = v.find("release")) r.release = t->as_string();
        if (auto* a = v.find("artist")) r.artist = a->as_string();
        if (auto* m = v.find("match")) r.match = m->as_number(0.0);
        if (auto* c = v.find("confirmed")) r.confirmed = c->as_bool(false);
        if (r.file.empty()) { r.ok = false; r.error = "no file"; }
    } else if (auto* e = v.find("error")) r.error = e->as_string();
    if (!r.ok && r.error.empty()) r.error = "no cover found";
    return r;
}

CoverFetchResult fetch_album_cover(const fs::path& script, const std::string& album, const std::string& artist, const fs::path& stem,
                                   MetaFetchCancel* cancel, const std::vector<std::string>& titles) {
    CoverFetchResult r;
    std::error_code ec;
    if (!fs::exists(script, ec)) { r.script_missing = true; r.error = "fetch script not found: " + path_utf8(script); return r; }
    Args cmd = python_command();
    if (cmd.empty()) { r.python_missing = true; r.error = "python not found (Python 3 is required to fetch covers)"; return r; }
    fs::create_directories(stem.parent_path(), ec);
    cmd.push_back(path_utf8(script));
    cmd.push_back(album);
    cmd.push_back(artist);
    cmd.push_back(path_utf8(stem));
    for (const std::string& t : titles) if (!t.empty()) cmd.push_back(t);   // song titles of the album: they confirm the right release
    auto child = spawn_capture(cmd, /*merge_stderr=*/false);
    if (!child) { r.error = "could not start " + command_text(cmd); return r; }
    if (cancel) cancel->attach(child.get());
    std::string buffer;
    char chunk[2048];
    for (;;) {
        const std::ptrdiff_t n = child->read(chunk, sizeof chunk);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
    }
    child->wait();
    if (cancel) cancel->attach(nullptr);
    if (cancel && cancel->requested()) { r.error = "cancelled"; return r; }
    std::istringstream in(buffer);
    std::string line, last;
    while (std::getline(in, line)) { while (!line.empty() && (line.back() == '\r')) line.pop_back(); if (!line.empty() && line[0] == '{') last = line; }
    if (last.empty()) { r.error = "no answer from the fetch script"; return r; }
    return parse_cover_fetch_line(last);
}

} // namespace muisc
