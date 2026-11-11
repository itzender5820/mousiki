// The local library: scans the music folders for audio files (including retro formats) and finds
// tracks by name.
#include "local_source.h"
#include <map>
#include "chiptune.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

namespace {
// The UTF-8 conversion of a path lives in path_utf8.h, which the other files use too; this alias
// keeps the call sites below short.
inline std::string path_str(const fs::path& p) { return path_utf8(p); }
} // namespace

bool LocalSource::is_audio_file(const fs::path& p) {
    // (.m4b: the audio book format; the others are what ffmpeg decodes and a music collection may hold: Windows Media, AIFF, Monkey's Audio,
    // WavPack, Ogg audio, Matroska audio, MP2, AC-3, DSD, Musepack)
    static const std::set<std::string> exts = {".wav", ".mp3", ".opus", ".flac", ".ogg", ".m4a", ".m4b", ".aac", ".webm",
                                               ".wma", ".aiff", ".aif", ".ape", ".wv", ".oga", ".mka", ".mp2", ".ac3", ".dsf", ".mpc"};
    // fs::path::extension() preserves whatever case is actually stored on
    // disk -- it does not normalize it. On Linux that's rarely an issue
    // since lowercase extensions are close to universal convention there,
    // but Windows files routinely carry ".MP3"/".Mp3"/etc. (ripped CDs,
    // older downloads, files that passed through Explorer at some point),
    // and a case-sensitive set lookup silently excluded every one of them
    // from the scan -- not an error, just an empty-looking library.
    std::string ext = ascii_lower_str(path_str(p.extension()));
    // Tracker modules and chiptunes too (MOD / XM / IT / ..., NSF / SPC / VGM / ..., SID) -- see chiptune.h.
    return exts.count(ext) > 0 || is_chiptune_file(p);
}

bool LocalSource::is_video_file(const fs::path& p) {
    static const std::set<std::string> exts = {".mp4", ".m4v", ".mkv", ".mov", ".avi", ".webm", ".wmv", ".flv", ".mpg", ".mpeg", ".ogv", ".3gp"};
    return exts.count(ascii_lower_str(path_str(p.extension()))) > 0;
}

std::vector<LocalTrack> LocalSource::scan(const std::vector<std::string>& custom_paths,
                                           std::vector<std::string>* diagnostics) const {
    std::vector<LocalTrack> tracks;
    std::vector<fs::path> roots;

    if (!custom_paths.empty()) {
        for (const auto& cp : custom_paths) {
            // path_from_utf8, not fs::path(cp): `cp` came out of
            // config.txt as UTF-8, and fs::path(std::string) reinterprets
            // it as ANSI on Windows -- so a music folder with an accent in
            // its name resolved to a path that does not exist, and the
            // scan silently found nothing.
            roots.push_back(path_from_utf8(cp));
        }
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            roots.push_back(path_from_utf8(home) / "Music");
            roots.push_back(path_from_utf8(home) / "disk" / "Music");
        }
    }

    if (roots.empty()) return tracks;

    // Dedupe by resolved (symlink-following) path — if one root is a
    // symlink that overlaps with the other (common on Android, e.g.
    // "disk" pointing into shared storage that also contains "Music"),
    // recursive_directory_iterator would otherwise walk and list the
    // exact same file twice, once per root.
    std::set<std::string> seen_canonical;

    for (const auto& root : roots) {
        std::error_code ec;
        bool exists = fs::exists(root, ec);
        bool is_dir = exists && !ec && fs::is_directory(root, ec);
        if (!exists || ec || !is_dir) {
            if (diagnostics) {
                diagnostics->push_back(
                    "local scan: '" + path_str(root) + "' -- " +
                    (ec ? ("error: " + ec.message())
                        : (!exists ? "does not exist" : "exists but is not a directory")));
            }
            continue;
        }

        size_t before = tracks.size();
        std::map<std::string, bool> creator_dirs;   // folder -> is it the creator's (see kCreatorMarker): its files are not part of the library
        // (An explicit loop: the range-for advances with the throwing operator++, so an I/O error half way through a network share
        // would have ended the program at startup. increment(ec) stops this root's walk instead and keeps what was found.)
        const fs::recursive_directory_iterator walk_end;
        for (fs::recursive_directory_iterator walk(root, fs::directory_options::skip_permission_denied, ec); !ec && walk != walk_end; walk.increment(ec)) {
            const fs::directory_entry& entry = *walk;
            // is_regular_file() with no error_code argument throws on
            // anything the OS can't stat cleanly -- which, unlike a plain
            // local folder, a OneDrive-backed folder (the common case for
            // "Music"/"Musik" on Windows, per the Known Folder note above)
            // routinely contains: cloud-only placeholder files that don't
            // have real on-disk content until opened. Every other call in
            // this loop already goes through an error_code out-param for
            // exactly this reason; this one was missed, and an uncaught
            // exception here unwinds straight out of App's constructor.
            std::error_code fec;
            bool is_file = entry.is_regular_file(fec);
            if (fec) continue; // couldn't stat it (cloud placeholder, permissions, ...) -- skip, don't crash
            if (is_file && (is_audio_file(entry.path()) || (include_video_ && is_video_file(entry.path())))) {
                {   // the folder of the creator (^ overlay) holds files that are only listed in that overlay
                    const std::string dir_key = path_str(entry.path().parent_path());
                    auto it = creator_dirs.find(dir_key);
                    if (it == creator_dirs.end()) {
                        std::error_code mec;
                        it = creator_dirs.emplace(dir_key, fs::exists(entry.path().parent_path() / kCreatorMarker, mec)).first;
                    }
                    if (it->second) continue;
                }
                std::error_code cec;
                fs::path canon = fs::canonical(entry.path(), cec);
                std::string key = cec ? path_str(entry.path()) : path_str(canon);
                if (!seen_canonical.insert(key).second) continue; // already listed via another root

                std::string folder = path_str(entry.path().parent_path().filename());
                // Compare as UTF-8 strings. Round-tripping `folder` back
                // through fs::path(std::string) would re-encode it via the
                // ANSI code page and make the comparison fail (or throw).
                if (folder.empty() || folder == path_str(root.filename())) folder = "-";
                tracks.push_back({path_str(entry.path().stem()), entry.path(), folder});
            }
        }
        if (diagnostics) {
            diagnostics->push_back(
                "local scan: '" + path_str(root) + "' -- found " +
                std::to_string(tracks.size() - before) + " audio file(s)" +
                (ec ? (" (stopped early: " + ec.message() + ")") : ""));
        }
    }
    return tracks;
}

std::optional<LocalTrack> LocalSource::find(const std::string& query, const std::vector<std::string>& custom_paths) const {
    // ascii_lower_str, not ::tolower over bytes: `query` and the titles it is
    // matched against are UTF-8, and a byte-wise locale-aware fold corrupts
    // every multi-byte character in them (see utf8_util.h). ASCII folding is
    // all a case-insensitive substring match needs, and it leaves Japanese,
    // Cyrillic, accented latin and everything else intact.
    std::string needle = ascii_lower_str(query);

    for (const auto& track : scan(custom_paths)) {
        std::string hay = ascii_lower_str(track.title);
        if (hay.find(needle) != std::string::npos) return track;
    }
    return std::nullopt;
}

} // namespace muisc
