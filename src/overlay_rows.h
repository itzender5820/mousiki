#pragma once
// The rows of the overlays whose rows are selected by a number, with names instead of bare numbers. The order in each enum IS the order on
// screen; the "...Count" at the end follows by itself. Adding a row means adding a name here (at its place) and handling it in the switch
// of the overlay -- no other number has to be changed by hand.
#include <vector>

namespace muisc {

// The VIDEO SETTINGS overlay (&), top to bottom.
enum VMenuRow : int {
    kVmVideoMode,       // Use Video in Lyrics Field
    kVmLyrics,          // Include Lyrics
    kVmAutoVideo,       // Auto Video
    kVmCover,           // Cover, Disk or Thumbnail
    kVmCoverSat,        // Cover Gradient Saturation (0 .. 100 %)
    kVmSize,            // Video Size
    kVmRounded,         // Rounded Corners
    kVmEffect,          // Effect
    kVmBase,            // Base Layer (right under the effect it belongs to)
    kVmContrast,
    kVmBrightness,
    kVmColors,
    kVmChroma,          // Chromatic Aberration
    kVmDither,          // Dither Pattern
    kVmGradSat,         // Video Gradient Saturation (0 .. 100 %)
    kVmResolution,
    kVmBuffer,
    kVmClap,            // Show Clap Loading
    kVmLastSetting = kVmClap,   // (rows up to here change a setting with the arrow keys; the rest are buttons)
    kVmFullScreen,      // [ Full Screen Video ]
    kVmCouple,          // [ Manually Couple Local Files ]
    kVmQuickSearch,     // [ Quick Search/Down. YT ]
    kVmFetchMenu,       // [ General Fetch Videos Menu ]
    kVmDeleteVideo,     // [ Delete Current Video ]
    kVmRefetchVideo,    // [ Refetch this Video ]
    kVmRowCount
};

// The VIDEO OPTIONS panel of the big video (SHIFT+s): the track's own video look. kVoMp4Audio is only listed for a song paired with a video.
enum VOptsRow : int {
    kVoEffect,
    kVoBase,
    kVoContrast,
    kVoBrightness,
    kVoColors,
    kVoChroma,
    kVoDither,
    kVoGradSat,
    kVoResolution,
    kVoRounded,
    kVoMp4Audio,
    kVoRowCount
};
inline int vopts_row_count(bool with_mp4_audio) { return with_mp4_audio ? kVoRowCount : kVoRowCount - 1; }

// The STYLE OPTION overlay (meta editor / SHIFT+s of the main screen). Which rows are listed depends on the Video mode and on the track
// (style_rows() below); the selection is an index into that list, never a bare number.
enum StyleRow : int {
    kStLook,            // Look of this track
    kStCoverContrast,
    kStCoverBrightness,
    kStCoverColors,
    kStCoverGradSat,    // Cover Gradient Saturation
    kStCoverChroma,
    kStCoverOrThumb,    // Use Disk/Cover/Thumbnail                (Video mode)
    kStVideoOn,         // Video of this track                    (Video mode)
    kStVideoLyrics,     // Use Lyrics on Video
    kStVideoSize,
    kStVideoEffect,
    kStVideoBase,
    kStVideoContrast,
    kStVideoBrightness,
    kStVideoColors,
    kStVideoChroma,
    kStVideoDither,
    kStVideoGradSat,
    kStVideoResolution,
    kStVideoRounded,
    kStMp4Audio,        // Use mp4 audio for this song            (a song paired with a video)
    kStAlbum            // Look of the whole album (always the last row)
};
inline bool style_row_is_video(StyleRow r) { return r >= kStVideoOn && r <= kStMp4Audio; }   // the rows of the "Video" block
inline std::vector<StyleRow> style_rows(bool video_mode, bool paired) {
    std::vector<StyleRow> v = {kStLook, kStCoverContrast, kStCoverBrightness, kStCoverColors, kStCoverGradSat, kStCoverChroma};
    if (video_mode) {
        for (int r = kStCoverOrThumb; r <= kStVideoRounded; ++r) v.push_back(static_cast<StyleRow>(r));
        if (paired) v.push_back(kStMp4Audio);
    }
    v.push_back(kStAlbum);
    return v;
}

}  // namespace muisc
