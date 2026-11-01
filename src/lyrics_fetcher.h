// Synced lyrics (see lyrics_fetcher.cpp): the LRC line and result types, fetching by title and artist,
// and the list of stored lyric versions.
#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "translit.h"

namespace muisc {

namespace fs = std::filesystem;

// One LRC line, optionally with per-word timestamps (enhanced/A2 LRC),
// which is what makes word-level highlighting possible during playback.
struct LyricLine {
    double start_time = 0.0;
    std::string full_text;
    std::vector<std::pair<double, std::string>> words; // empty if not word-synced
};

// The lyrics in Latin letters (the display switch, see translit.h): the original is kept, a copy is converted.
inline void latinize_lyrics(std::vector<LyricLine>& lines) {
    if (!translit_enabled()) return;
    for (LyricLine& l : lines) {
        l.full_text = transliterate(l.full_text);
        for (auto& w : l.words) w.second = transliterate(w.second);
    }
}

enum class LyricsStatus {
    Ok,
    ModuleMissing,   // scripts/lrc.py could not be loaded (damaged installation) -> show a reinstall hint
    PythonMissing,   // python3 not found on PATH
    NotFound,        // ran fine, no lyrics available for this track
    Error,
};

struct LyricsResult {
    LyricsStatus status = LyricsStatus::Error;
    std::vector<LyricLine> lines;
    std::string message;   // human-readable status/error, shown in the lyrics panel
    std::string source;    // "local" | "better-lyrics" | "lrclib" | ""
    std::string raw_lrc;   // the raw LRC text, kept so it can be cached to a sidecar file
    // Timing correction in seconds, as the lyrics editor (ALT+L) shows it:
    // POSITIVE = the lyrics appear LATER than their timestamps say, negative =
    // earlier. Read from the sidecar's "[offset:...]" tag (which uses the LRC
    // convention, in milliseconds, positive = earlier -- the sign is flipped on
    // the way in and out), 0 when there is none.
    double delay = 0.0;
    // The text carried no timestamps (lyrics embedded in the file as plain text): the lines are spread evenly over the
    // track, which is only a rough guide.
    bool unsynced = false;
};

// Priority chain: a local sidecar .lrc file in a "lyrics" subfolder of the
// track's own folder (checked first, no subprocess spawned at all; a sidecar
// left next to the track by an older build is still read as a fallback) -> the Python helper script
// (scripts/fetch_lyrics.py), which tries Better Lyrics first (word-level
// TTML, converted to enhanced LRC) and falls back to LRCLIB (line-synced
// only) if Better Lyrics has nothing -- see scripts/lrc.py. Whatever
// comes back from a network fetch is written back to the sidecar file,
// so the next time this track plays (even offline) it's a local-file
// hit.
// Network lyrics searches also try other spellings of the names when the first search finds nothing: the Latin-letters spelling Mousiki
// shows, the title / artist without their Latin or without their Japanese part, the title alone (LyricsNameVariants=, default on).
void set_lyrics_name_variants(bool on);
LyricsResult fetch_synced_lyrics(const std::string& title, const std::string& artist,
                                  const std::string& helper_script_path,
                                  const fs::path& track_path = fs::path(),
                                  bool force_network = false,
                                  double track_seconds = 0.0);

// --- the versions of a track's lyrics ------------------------------------------------------------------------
// Lyrics can come from three places: written INTO the audio file (tags), a FETCHED copy in the "lyrics" folder next to
// the track, and older fetched copies that a refetch (or a switch) put aside so nothing is ever overwritten -- a timing
// correction saved with the lyrics editor survives a refetch that way. Chain when a track loads: the fetched copy in use
// (if any), else the lyrics in the file, else the network.
struct LyricsVersion {
    enum class Kind { Embedded, Fetched, Archived };
    Kind kind = Kind::Fetched;
    fs::path file;          // Fetched: the copy in use; Archived: the saved older copy; empty for Embedded
    std::string label;      // "Embedded in the file" / "Fetched 2026-10-11 06:15"
    std::string detail;     // "lrclib, word-synced, timing +0.5 s"
    bool active = false;    // the one that loads when the track plays
};
std::vector<LyricsVersion> list_lyrics_versions(const fs::path& track_path);
// Makes `v` the one in use: the copy in use is put aside first (never lost), then `v` takes its place. False on failure.
bool activate_lyrics_version(const fs::path& track_path, const LyricsVersion& v);
// Deletes the saved copy behind `v` (the fetched copy in use or an older one). Lyrics embedded in the audio file are
// part of the file and are never touched: false for them and on failure.
bool delete_lyrics_version(const LyricsVersion& v);
// Assigns a lyrics file (.lrc or plain .txt) to a track by hand: the copy in use is put aside first (never lost, and
// nothing happens when it already has exactly this text), then the file's text becomes the copy in use, tagged
// "[re:manual]". `error` (optional) says why it failed (unreadable, empty, too big, not text).
bool assign_lyrics_file(const fs::path& track_path, const fs::path& source_file, std::string* error = nullptr);
// One line about the copy in use ("added manually, 42 lines", "fetched from lrclib, synced, 42 lines"); empty when the track has none saved.
// Cheap (reads the one file, never the audio file).
std::string lyrics_in_use_summary(const fs::path& track_path);
// The name of the file the manually added lyrics came from (kept in the copy as "[file:...]"); empty when not known.
std::string lyrics_manual_origin_name(const fs::path& track_path);
// The file of the copy in use (empty when the track has none saved).
fs::path lyrics_copy_in_use(const fs::path& track_path);
// True when the copy in use was added by hand (assign_lyrics_file).
bool lyrics_added_manually(const fs::path& track_path);
// Takes a manually added copy out of use again (the file it came from is NOT touched or deleted): the newest older copy
// of the track comes back in its place, or the track has no saved lyrics (it fetches them again as usual). False, with
// `error`, when the copy in use was not added manually.
bool unassign_lyrics_file(const fs::path& track_path, std::string* error = nullptr);

// --- reading the saved lyrics (READ LYRICS overlay, CTRL+L) ---------------------------------------------------
// The raw text behind one version: the .lrc of a fetched / older copy, or the lyrics written into the audio file.
// Empty when it cannot be read.
std::string read_lyrics_version_text(const fs::path& track_path, const LyricsVersion& v);
// The "lyrics" folder of a track (fetched copies, older copies and the "translations" subfolder live in it).
fs::path lyrics_folder_of(const fs::path& track_path);
// Readable lines of an LRC / plain lyrics text: timestamps, word tags and [ar:]-style header tags are removed, a
// run of blank lines becomes one, leading / trailing blank lines are dropped.
std::vector<std::string> lyrics_plain_lines(const std::string& raw);
// The same lines WITH their times (READ LYRICS plays along with the song): lines[i].full_text == lyrics_plain_lines(raw)[i], start_time = -1
// where the line has no time stamp, words = the word times of an enhanced LRC line. `delay` (optional) gets the [offset:] of the text, in the
// sign LyricsResult::delay uses.
std::vector<LyricLine> lyrics_read_lines(const std::string& raw, double* delay = nullptr);

// Writes `delay_sec` (same sign as LyricsResult::delay) into the track's sidecar
// .lrc as an "[offset:...]" tag, replacing an existing one (0 removes the tag).
// The timestamps themselves are not touched. False when there is no sidecar to
// write into (nothing fetched/cached for this track) or the write failed.
bool save_lyrics_delay(const fs::path& track_path, double delay_sec);

} // namespace muisc
