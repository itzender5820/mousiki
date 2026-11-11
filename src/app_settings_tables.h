#pragma once
// The row ids (SetId) and the layouts of the GENERAL and ANIMA/VIZ tabs of the Settings screen. The layouts themselves are built
// in app_settings_tables.cpp; the REFERENCE tab's hotkey rows are in app_settings_ref.h. Nothing here is part of the program's interface.
#include <string>
#include <utility>
#include <vector>

namespace muisc {

// The GENERAL tab (tab 1, formerly ON/OFF) and the ANIMA/VIZ tab (tab 2) are laid out as categories: a header line, then its rows. Every
// selectable row has an id (SetId, the same numbers on both tabs); settings_row_ counts the selectable rows of the tab
// only, so settings_item_id() turns it into the id that settings_get_value() / settings_options_for() /
// settings_commit_edit() / settings_cycle() and the renderer work with. Adding a row = one id + one line in a layout.
enum SetId {
    kSidVizField, kSidDisk, kSidDummy, kSidQueue, kSidWave, kSidLyricsEngine, kSidLyricViz, kSidVisualizer, kSidNoLyricsViz,
    kSidStereo, kSidNormalize, kSidMetaOnly, kSidEmoji, kSidMarquee, kSidUseLyrics, kSidUseOsci, kSidUseSpectro, kSidUseSphere,
    kSidOrder1, kSidOrder2, kSidOrder3, kSidOrder4, kSidOrder5, kSidUseVideo, kSidNoVideoViz, kSidVideoSize, kSidVideoFx, kSidVideoContrast, kSidVideoBrightness, kSidVideoColors, kSidVideoChroma, kSidVideoDither, kSidVideoBase, kSidVideoGradSat, kSidVideoRound, kSidVideoClap, kSidVideoCover, kSidVideoRes, kSidVideoBuffer, kSidVideoMp3, kSidCoverChroma,
    kSidPlayback, kSidStartMode, kSidBlackBg, kSidShareProfiles, kSidRemoteApp, kSidUpdateCheck, kSidMediaKeys, kSidUseTermSettings, kSidTermSize, kSidFullScreen, kSidLatinLookup, kSidLatinMacrons, kSidLatinWo, kSidLatinHanzi, kSidLyricsVariants, kSidRateCap, kSidTrackBuffer,
    kSidVisFluidity, kSidWaveStyle, kSidDiskSpeed, kSidVisDegr, kSidVisVisc, kSidLyrAlign, kSidLyrAnim,
    kSidVolIndicator,
    kSidUseModePlayer, kSidUseModeRadio, kSidUseModeBook, kSidModeOrder1, kSidModeOrder2, kSidModeOrder3, kSidBookAsk, kSidBookCover, kSidBookCoverWave,
    kSidAlbumCover, kSidCoverMode, kSidCoverContrast, kSidCoverSearch, kSidCoverBrightness, kSidCoverColors, kSidCoverGradSat,
    kSidTermDefaultFont, // TEXT AND TRANSLITERATION: leave the terminal's own font alone (terminal_font.h)
    kSidVideoLyrics,   // Include Lyrics (the lines under the video: none / lyrics / lyrics + translation / translation)
    kSidCheckUpdate,   // [ENTER] checks for a newer release now (an action row, no value to switch)
    kSidBookSpeed,     // audio book player: the playback speed (0.5x - 3.0x, the pitch stays)
    kSidBarStyle, kSidHifiRes, kSidHifiChan, kSidHifiColor, kSidHifiScale, kSidHifiAdapt, kSidHifiSplit, kSidHifiFScale, kSidHifiGain, kSidHifiRange, kSidBarBehavior,   // SPECTRUM BARS: classic / Hi-Fi and the three rows only Hi-Fi shows
    kSidReduceMotion,  // TERMINAL: a low frame rate while paused and untouched
    kSidDiskBorder,    // DISK: the ":  :" column between the disk and the meta data on / off
    kSidPauseFade,     // AUDIO: pause / resume fade over about 8 ms (no click)
    kSidCliScripts,    // CLI / REMOTE / UPDATES: [ENTER] opens the script overlay
    kSidAudioDevice,   // the sound output: shows the device in use, [ENTER] opens the CHOOSE AUDIO DEVICE overlay
};
// True while the audio book player is the mode that runs (set by App::run()): its GENERAL and ANIMA/VIZ tabs leave out what only
// belongs to the music player and name the lyrics view "notes".
inline bool g_settings_book_layout = false;
// True while the bars style is Hi-Fi (set by App): only then the Hi-Fi rows of SPECTRUM BARS are listed.
inline bool g_settings_hifi_rows = false;
struct SetLine { bool header; int id; const char* text; };   // a category header, or a row (id + label)
// The layout of tab 1 (GENERAL) / 2 (ANIMA/VIZ): categories and rows (defined in app_settings_tables.cpp).
const std::vector<SetLine>& settings_layout(int tab);
inline int settings_item_count(int tab) {
    int n = 0;
    for (const auto& l : settings_layout(tab)) if (!l.header) ++n;
    return n;
}
// The id of the row-th selectable row of tab 1 / 2 (-1 when there is none).
inline int settings_item_id(int tab, int row) {
    int n = 0;
    for (const auto& l : settings_layout(tab)) if (!l.header && n++ == row) return l.id;
    return -1;
}
// The line (headers counted) the row-th selectable row is drawn on.
inline int settings_item_line(int tab, int row) {
    int n = 0, line = 0;
    for (const auto& l : settings_layout(tab)) { if (!l.header && n++ == row) return line; ++line; }
    return 0;
}
inline const char* const kVizNames[5] = {"sphere", "osci", "spectro", "lyrics", "video"};   // viz_order values, row values
// The name of mode `m` (0 music player, 1 radio, 2 audio books) on a row: the words of config.txt.
inline const char* mode_row_name(int m) { return m == 1 ? "radio" : m == 2 ? "audiobook" : "player"; }
// The name of view `i` on a row: the audio book player calls the lyrics view "notes".
inline const char* viz_row_name(int i) { return (g_settings_book_layout && i == 3) ? "notes" : kVizNames[i < 0 ? 0 : i > 4 ? 4 : i]; }

} // namespace muisc
