#pragma once
// Cover store: the folder of fetched covers, the choices a user made (which picture belongs to a track / to an album name), the
// pictures that can be chosen from, and the MusicBrainz / Cover Art Archive fetch (scripts/fetch_cover.py).
// Used by the cover search (cover_art.cpp), the meta editor's cover pane and the cover fetch overlay (ALT+c).
#include <filesystem>
#include <string>
#include <vector>

#include "meta_editor.h"   // MetaFetchCancel
#include "video_fx.h"

namespace muisc {

// Fetched covers are saved in a folder of this name inside the folder of the tracks (one copy per folder that holds the album).
inline constexpr const char* kFetchedCoversDir = "fetched_covers";
// The folder whose fetched_covers a video in the download folder's video_down also looks into: the download folder itself (where the
// covers of the downloaded songs are kept). Empty for any other folder.
inline std::filesystem::path shared_covers_home(const std::filesystem::path& dir) {
    return dir.filename() == "video_down" && dir.has_parent_path() ? dir.parent_path() : std::filesystem::path();
}

// A name as a key: lower case, punctuation and spacing ignored (the album "Abbey Road" and "abbey_road" are the same one).
std::string cover_name_key(const std::string& name);

// <track folder>/fetched_covers/<album>.<ext> if the album has a fetched cover there (any picture format), else an empty path.
std::filesystem::path find_fetched_cover(const std::filesystem::path& track_folder, const std::string& album);
// The folders of the music library (set by the music player after every scan). A file that lives outside them (the download folder's
// video_down, a cache folder) has no fetched_covers folder of its own: find_fetched_cover() and the cover search then look for the album's
// cover in the fetched_covers folders of the library, by the album's name. The index of those folders is made in the background (the
// first answer may be "none"; the covers are asked for again when it is ready).
void set_cover_library_roots(const std::vector<std::filesystem::path>& roots);
bool cover_outside_library(const std::filesystem::path& folder);   // video_down, or a folder under none of the library folders
std::filesystem::path fetched_cover_in_library(const std::string& album, bool wait = false);
// The path (without extension) a fetched cover of the album is written to.
std::filesystem::path fetched_cover_stem(const std::filesystem::path& track_folder, const std::string& album);
// The album name as part of a file name (characters Windows does not allow become "_").
std::string cover_file_name(const std::string& album);

// One picture a track can use as its cover.
struct CoverCandidate {
    enum Kind { Embedded, Folder, Fetched } kind = Folder;
    std::filesystem::path file;   // the picture file (for Embedded: the audio file)
    std::string label;            // "inside the file", "cover.jpg", "fetched_covers/Thriller.jpg"
};
// Everything that can be chosen for a track: the picture inside the file (if `with_embedded`), the pictures in its folder, the pictures
// in its fetched_covers folder.
std::vector<CoverCandidate> cover_candidates(const std::filesystem::path& track, bool with_embedded);

// ---- the choices -----------------------------------------------------------------------------------------------------------
// A choice is a picture file (UTF-8 path), kCoverEmbedded (the picture inside the file) or kCoverNone (no cover: the disk).
inline constexpr const char* kCoverEmbedded = "@embedded";
inline constexpr const char* kCoverNone = "-";

// What the disk shows of a track that has a video, apart from its picture choice (meta editor -> COVER OPTIONS / STYLE OPTION; the VIDEO
// overlay holds the default for all tracks): 0 as in the VIDEO overlay, 1 the cover, 2 a frame of the video (else the cover), 3 the plain disk.
int cover_track_video_mode(const std::string& track_path);
void set_cover_track_video_mode(const std::string& track_path, int mode);   // 0 removes it
// The video of one track (meta editor -> Style Option): whether it is shown at all (0 = hidden; -1 = shown, as always), its picture size
// (0 regular, 1 cropped, 2 full; -1 = as in the VIDEO overlay) and its effect (0 off .. 17 interlace; -1 = as in the VIDEO overlay).
struct VideoTrackOpts { int on = -1, size = -1, fx = -1, contrast = -1, colors = -1, chroma = -1, res = -1, round = -1, lyrics = -1, brightness = -1, mp4audio = -1; };   // (round: 0 / 1)   // contrast / colors 0..100 %, chroma 0..3, res 0..2 (standard / high / full); -1 = as in Settings
VideoTrackOpts cover_track_video_opts(const std::string& track_path);
void set_cover_track_video_opts(const std::string& track_path, const VideoTrackOpts& o);   // all -1 removes it
std::string cover_track_choice(const std::string& track_path);               // "" = none made
void set_cover_track_choice(const std::string& track_path, const std::string& choice);   // "" removes it
std::string cover_album_choice(const std::string& album);                    // the default picture of an album name ("" = none made)
void set_cover_album_choice(const std::string& album, const std::string& choice);
// The cover look chosen for one album (meta editor -> COVER OPTIONS -> [l]); it wins over Settings -> Album Cover / Cover Mode.
// 0 = the plain disk (no cover), 1..7 = the Cover Modes (Just Cover ... Cover ASCII with Gradient), -1 = none chosen. The album is
// its name (any track with the same album tag); a track without one: its folder.
inline constexpr int kCoverLookCount = 8;
int cover_album_look(const std::string& album, const std::string& folder);
void set_cover_album_look(const std::string& album, const std::string& folder, int look);   // -1 removes it
// The style of one track (meta editor -> Style Option): its own look and its own Cover Contrast / Brightness / Colors. -1 = as in Settings
// (look: then the album's look, then Settings). It wins over everything else for that track.
struct CoverStyle { int look = -1, contrast = -1, brightness = -1, colors = -1, chroma = -1; };   // chroma: chromatic aberration 0..3
CoverStyle cover_track_style(const std::string& track_path);
void set_cover_track_style(const std::string& track_path, const CoverStyle& st);   // all -1 removes it
// The albums a cover fetch found nothing for (remembered, so they are not asked for again and again): the reason, "" = not on the list.
std::string cover_fetch_failure(const std::string& album);
void set_cover_fetch_failure(const std::string& album, const std::string& reason);   // reason "" removes it
// Successful fetches are remembered with the time and what MusicBrainz matched (to check them: the filter [r] of the ALT+c overlay).
bool cover_recently_fetched(const std::string& album, std::string* info = nullptr, int days = 14);
void mark_cover_fetched(const std::string& album, const std::string& info);
// Counts up whenever a choice changes or a cover is fetched or deleted (the cover search is made again then).
unsigned cover_store_revision();
void cover_store_touch();

// Whether the picture can be deleted from the disk: pictures in a folder / in fetched_covers, never the one inside a file.
bool cover_deletable(const CoverCandidate& c);
// Deletes it (and every choice that points to it). Returns false (with `why`) when not allowed or on failure.
bool delete_cover_candidate(const CoverCandidate& c, std::string* why = nullptr);

// Whether an audio file carries a picture (ffprobe). Slow (a process per call), so only used for the overlays; remembered per path.
bool has_embedded_picture(const std::filesystem::path& audio_file);

// The pictures that belong to an album in a folder, for the ALT+c overlay's "missing / one / several": the one inside the first track,
// the fetched cover of the album, the pictures of the folder whose name is close to the album's, cover.jpg / folder.jpg ... when the
// folder holds this album alone, and the pictures chosen for the album or its tracks. Each picture once. `files` gets the
// non-embedded ones, `embedded` whether the first track carries one.
struct AlbumCovers {
    std::vector<std::filesystem::path> files;
    bool embedded = false;
    int count() const { return static_cast<int>(files.size()) + (embedded ? 1 : 0); }
};
AlbumCovers album_covers(const std::filesystem::path& folder, const std::string& album, const std::filesystem::path& first_track,
                         bool folder_holds_only_this_album, bool probe_embedded);

// ---- fetching ---------------------------------------------------------------------------------------------------------------
struct CoverFetchResult {
    bool ok = false;
    std::string file;      // the downloaded picture
    std::string release;   // the MusicBrainz release it belongs to
    std::string artist;
    bool confirmed = false;   // MusicBrainz found one of the song titles on that release
    double match = 0.0;    // how close the release title is to the album asked for (0..1)
    std::string error;
    bool python_missing = false, script_missing = false;
};
// Fetches the front cover of an album to <stem>.jpg (or .png). `titles`: a few song titles of the album, which MusicBrainz is asked to find on the release (a confirmation). One request at a time (the script paces itself for MusicBrainz).
CoverFetchResult fetch_album_cover(const std::filesystem::path& script, const std::string& album, const std::string& artist,
                                   const std::filesystem::path& stem, MetaFetchCancel* cancel,
                                   const std::vector<std::string>& titles = {});
// Parses the script's answer line (public for the tests).
CoverFetchResult parse_cover_fetch_line(const std::string& line);

} // namespace muisc
