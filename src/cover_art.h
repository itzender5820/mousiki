#pragma once
#include "picture_formats.h"
// Album covers for the music player: where a track's cover comes from (the picture inside the file, a picture in its folder
// whose name is close to the track's title or album), how it is decoded (ffmpeg, in the background) and how it is scaled for the
// disk's place. Settings -> GENERAL -> AUDIO -> Album Cover / Cover Mode / Cover Search.
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace muisc {

// A decoded cover, at most 640 x 640 pixels.
struct CoverImage {
    std::string key;                      // what it was made for (see track_cover())
    std::string dir, album;               // the folder and album tag of the track it was made for
    int w = 0, h = 0;                     // size of `level`
    std::vector<unsigned char> level;     // grey level 1..255 (never 0 where the cover is: 0 would be transparent)
    std::vector<unsigned char> rgb, alpha;   // the colours (w * h * 3) and the opacity (w * h)
    bool ok = false;
    std::string source;                   // "cover.jpg" or "song.mp3 (inside the file)" -- for the status line
    std::string note;                     // why there is no cover (when !ok)
    bool per_track = false;               // chosen for this track alone (not shared with the other tracks of its album)
};

// A cover scaled to a size: grey (`grey`) or in its own colours (`level` = opacity, `hue` = palette index into `pal`, 240 colours).
// A few sizes at once: the terminal's picture, the phone's picture and a preview are made from the same cover at different
// sizes in the same frame, and with one remembered size each of them scaled the cover again every frame. A slot is only
// ever overwritten (the least recently used one), never moved, so a reference handed out stays valid until that slot is reused.
template <class T, size_t N>
struct SizeCache {
    std::array<T, N> slot{};
    std::array<unsigned long long, N> used{};
    unsigned long long clock = 0;
    T* find(const std::string& key) {
        for (size_t i = 0; i < N; ++i)
            if (!slot[i].key.empty() && slot[i].key == key) { used[i] = ++clock; return &slot[i]; }
        return nullptr;
    }
    T& fresh() {   // the least recently used slot, emptied
        size_t pick = 0;
        for (size_t i = 1; i < N; ++i) if (used[i] < used[pick]) pick = i;
        used[pick] = ++clock;
        slot[pick] = T();
        return slot[pick];
    }
};

struct CoverScaled {
    std::string key;
    int w = 0, h = 0;
    std::vector<unsigned char> grey;      // grey mode: the grey level (already times the opacity)
    std::vector<unsigned char> alpha;     // grey mode: the opacity (255 = opaque, 0 = transparent: a cover with a removed background)
    std::vector<unsigned char> level, hue;
    std::array<std::array<uint8_t, 3>, 256> pal{};
};

// Median cut over a 5-bit-per-channel histogram: the palette (at most `n` colours) and the palette index of every pixel.
void quantize_colours(const std::vector<unsigned char>& rgb, const std::vector<unsigned char>& alpha, int n,
                      std::array<std::array<uint8_t, 3>, 256>& pal, std::vector<unsigned char>& hue);
// The cover fitted into max_w x max_h pixels (proportions kept). `key_id` names the cover (a new id = a new scaling).
// `contrast` 0..150 (%): 0 = the cover as it is; more pushes the brightnesses (the colours) away from the middle grey.
const CoverScaled& cover_scaled(const CoverImage& c, int max_w, int max_h, bool colour, int contrast = 0, int chroma = 0);   // chroma: chromatic aberration 0..3 (colour pictures)
// The cover fitted into max_w x max_h in its own colours, unquantised: `rgb` (3 bytes a pixel) and `alpha`, w x h (the same size
// cover_scaled() gives for these arguments). A cache of its own, so the grey and the colour version of the same cover can be used
// in one frame (the cover behind the disk with Cover Colors).
struct CoverRgb { std::string key; int w = 0, h = 0; std::vector<unsigned char> rgb, alpha; };
const CoverRgb& cover_scaled_rgb(const CoverImage& c, int max_w, int max_h, int contrast = 0, int chroma = 0);

// ---- where a cover comes from ----------------------------------------------------------------------------------------------
// The places that are looked at, in this order, until one gives a picture:
enum class CoverStep {
    Embedded,       // the picture inside the audio file (mp3, flac, m4a ...)
    FolderClose,    // a picture in the track's folder whose name is close to the title or the album (a fuzzy match)
    FolderGeneric,  // a picture in the folder that is named like a cover (cover, folder, front, albumart, album)
    FolderAny,      // the last resort of every search: the first picture of the folder by name (alphabetically)
};
// Settings -> Cover Search (0 Default, 1 Folder First, 2 Within File First, 3 Image in Folder Only, 4 Within File Only).
std::vector<CoverStep> cover_steps(int search);

// How close two names are, 0 (nothing in common) .. 1 (the same after case, punctuation and spacing are ignored).
double name_closeness(const std::string& a, const std::string& b);
// The picture in `dir` (not below it) for FolderClose (`generic` false: the one whose name is closest to any of `names`, at least
// kCoverCloseness) or FolderGeneric (`generic` true), or, with `any`, for FolderAny: the first picture of the folder by name. Empty path = none.
inline constexpr double kCoverCloseness = 0.62;

// (with `fetched` the pictures of <dir>/fetched_covers count for the closest-name search too)
std::filesystem::path pick_folder_picture(const std::filesystem::path& dir, const std::vector<std::string>& names, bool generic, bool any = false, bool fetched = false);

// Decodes a picture file (or, with `embedded`, the picture inside an audio file) with ffmpeg: at most 640 x 640.
bool decode_cover_file(const std::filesystem::path& file, bool embedded, CoverImage& d, double seek = 0.0);
// A thumbnail of a video: a frame from about a tenth into it (at most 30 s), so it is not the black first frame.
bool decode_video_snapshot(const std::filesystem::path& video, CoverImage& d);
// A picture for a preview, decoded in the background and kept (the last 32): nullptr until it is ready or when it cannot be read.
const CoverImage* preview_cover(const std::string& file, bool embedded);
// The same for a picture that is already known to be unreadable / still loading: true while it is being read.
bool preview_cover_loading(const std::string& file, bool embedded);
const CoverImage* preview_video_frame(const std::string& video);   // a frame of a video file (read in the background; nullptr while it is read or when it cannot be)
bool preview_video_frame_loading(const std::string& video);

// ---- the cover of the playing track ---------------------------------------------------------------------------------------
struct CoverRequest {
    std::string path;     // the audio file (UTF-8)
    std::string title;    // title tag, else the file name without extension
    std::string album;    // album tag ("" = none)
    int search = 0;       // Settings -> Cover Search
    std::string pair_path;   // a video file: the song it is paired with (its choices, folder and fetched covers are used too; the song may be gone)
    bool thumb_fallback = false;   // Thumbnail fallback: the usual search first; only when it finds no picture, a frame of snapshot_video
    std::string snapshot_video;   // a video file whose frame is the cover (Settings: Cover or Video Snapshot); "" = the usual search
    bool ignore_track_choice = false;   // resolve_cover_source() only: what "Automatic" would find (the picture chosen for the track is left out)
    unsigned store_rev = 0;   // cover_store_revision(): the cover is made again when a choice was made or a cover fetched / deleted
};
// The cover of the track, once it has been read in the background. nullptr: not read yet, or the track has none (cover_note()
// says which). While the next track of the same album is read, the cover of the one before stays.
// preview = true: the slot of the overlays' previews (Style Option, Cover Options ...). The playing track's cover and a preview of another track
// asked in turn in one slot would throw each other out every frame (the picture blinks: the cover, the plain disk, the cover ...).
const CoverImage* track_cover(const CoverRequest& rq, bool preview = false);
// "" while the cover is being read or exists; otherwise why there is none ("no picture inside the file or in the folder").
std::string cover_note(const CoverRequest& rq);
// Which picture a track would get, without decoding it (the overlays' previews): the same order as the cover search incl. the choices.
struct CoverSource {
    std::string file;      // the picture file, or for `embedded` the audio file
    bool embedded = false;
    bool chosen = false;   // by a choice (not by the search)
    bool none = false;     // no picture (found, or "no cover" was chosen)
    std::string label;
};
CoverSource resolve_cover_source(const CoverRequest& rq);
// What the shown cover was read from ("cover.jpg", "song.mp3 (inside the file)"), "" = none.
std::string cover_source();

} // namespace muisc
