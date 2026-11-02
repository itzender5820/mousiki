#pragma once
// The tables of the Settings screen (the REFERENCE tab's hotkey rows, the row ids and layouts of the GENERAL and ANIMA/VIZ
// tabs). Shared by app.cpp and app_settings.cpp; nothing here is part of the program's interface.
#include <string>
#include <vector>

namespace muisc {

struct RefHotkeyRow { const char* header; const char* action; const char* label; };
inline const RefHotkeyRow kRefRows[] = {
    // --- Playback ---
    {"PLAYBACK", "HKeyPlay", "Play Selected"},
    {nullptr, "HKeyTogglePlayPause", "Toggle Play / Pause"}, // was missing from this tab entirely
    {nullptr, "HKeyPlayNextSong", "Next Track"},
    {nullptr, "HKeyPlayPreviousSong", "Prev Track"},
    {nullptr, "HKeyShuffleNext", "Shuffle Next"},
    {nullptr, "HKeyCyclePlayMode", "Cycle Play Mode"},
    {nullptr, "HKeySeekForward", "Seek Forward"},
    {nullptr, "HKeySeekBackward", "Seek Backward"},
    {nullptr, "HKeyIncreaseVolume", "Volume Up"},
    {nullptr, "HKeyDecreaseVolume", "Volume Down"},
    {nullptr, "HKeyToggleMute", "Toggle Mute"},
    {nullptr, "HKeyToggleNormalize", "Toggle Normalize"},
    {nullptr, "HKeyNormMenu", "Normalization Tuning"}, // on/off, target level, max boost, live
    {nullptr, "HKeySleepTimer", "Sleep Timer"},       // 15/30/60/90/120 min or stop after the current song
    {nullptr, "HKeyCreator", "Creator"},              // text / picture -> oscilloscope music or spectrogram sound
    // --- Navigation & View ---
    {"NAVIGATION & VIEW", "HKeyNavigateUp", "Navigate Up"},
    {nullptr, "HKeyNavigateDown", "Navigate Down"},
    {nullptr, "HKeySwitchBetweenCards", "Switch Cards"},
    {nullptr, "HKeyFilterForFolder", "Filter By Folder"},
    {nullptr, "HKeyClearFilter", "Clear Filter"},
    {nullptr, "HKeyCycleSortMode", "Cycle Sort Mode"},
    {nullptr, "HKeyToggleMetaOnly", "Show Metadata Only"}, // list rows: metadata instead of filename
    {nullptr, "HKeyTranslit", "Latin Letters On/Off"},     // Japanese / Korean / Chinese text in Latin letters
    {nullptr, "HKeyToggleWaveform", "Toggle Waveform"},
    {nullptr, "HKeyFolderOverlay", "Big Folder Overlay"}, // folder tree of the library, `f` shows a folder in the list
    {nullptr, "HKeyConvertOverlay", "File Conversion Overlay"}, // convert high resolution files to MP3 / AAC / Opus
    {nullptr, "HKeyListOverlay", "Big List Overlay"}, // larger LOCAL AUDIO FILES pane floated over the main UI
    {nullptr, "HKeyQueueOverlay", "Big Queue Overlay"}, // larger QUEUE pane floated over the main UI
    {nullptr, "HKeyRefreshUi", "Refresh UI"},
    // --- Lyrics & karaoke ---
    {"VIZ, LYRICS & KARAOKE", "HKeyToggleLyrics", "Viz./Lyrics Cycle Forward"},
    {nullptr, "HKeyToggleLyricsBack", "Viz./Lyrics Cycle Backward"},
    {nullptr, "HKeyRetryLyrics", "Retry Lyrics"},
    {nullptr, "HKeyKaraoke", "Karaoke Overlay"},
    {nullptr, "HKeyLyricsTiming", "Lyrics Timing Overlay"},   // shift synced lyrics earlier / later
    {nullptr, "HKeyReadLyrics", "Read Lyrics Overlay"},       // saved lyrics as text, files + translations
    {nullptr, "HKeyKaraokeBigger", "Karaoke Lyrics Bigger"},  // inside the karaoke overlay (size 1-5)
    {nullptr, "HKeyKaraokeSmaller", "Karaoke Lyrics Smaller"},
    // --- Visualizations ---
    {"VISUALIZATIONS", "HKeyOscMenu", "Oscilloscope Tuning"},  // the SHIFT+o overlay, live
    {nullptr, "HKeySpectroMenu", "Spectrogram Options"},
    {nullptr, "HKeySpectroFull", "Spectrogram Full Screen"},
    {nullptr, "HKeyScopeBig", "Oscilloscope Full Screen"},   // YT left, XY right, grid
    {nullptr, "HKeyScopeWindow", "Scope Window"},     // the oscilloscope in its own (graphics card) window
    {nullptr, "HKeySpectroWindow", "Spectrogram Window"}, // the spectrogram in its own (graphics card) window
    // --- Search ---
    {"SEARCH", "HKeySearch", "Search Local"},   // the prefixes typed in the box (s: sc: b: p: f: r: a:) are fixed, see the cheat sheet
    // --- Queue ---
    {"QUEUE", "HKeyAddHoveringSongToQueue", "Add To Queue (Next)"},
    {nullptr, "HKeyRemoveHoveringSongFromQueue", "Remove From Queue"},
    {nullptr, "HKeyQueueMoveUp", "Queue Move Up"},
    {nullptr, "HKeyQueueMoveDown", "Queue Move Down"},
    {nullptr, "HKeyClearQueue", "Clear Queue"},
    {nullptr, "HKeyQueueAddEnd", "Add To End Of Queue"},
    {nullptr, "HKeyQueueLock", "Lock Queue"},
    {nullptr, "HKeyQueueMoveTop", "Queue Move To Top"},
    {nullptr, "HKeyQueueMoveBottom", "Queue Move To Bottom"},
    {nullptr, "HKeyQueueUndoClear", "Undo Clear Queue"},
    {nullptr, "HKeyQueueToPlaylist", "Save Queue As Playlist"},
    {nullptr, "HKeyBulkAdd", "Bulk Add (Playlist Link)"},
    {nullptr, "HKeyAddToPlaylist", "Add Track To Playlist"},
    // --- Playlists ---
    {"PLAYLISTS", "HKeyPlaylist", "Open Playlist Editor"}, // opens the playlist create/manage screen
    // --- Meta editor ---
    {"META EDITOR", "HKeyMetaEditor", "Open Meta Editor"}, // edit file names/tags, AcoustID fetch
    {nullptr, "HKeyCoverFetch", "Open Cover Fetch"},
    {nullptr, "HKeyStyleOptions", "Open Style Options"},   // the cover look of the playing track       // fetch album covers (MusicBrainz / Cover Art Archive)
    {nullptr, "HKeyFetchTags", "Fetch Missing Tags"}, // the hovered title; also inside the meta editor
    // --- Listening history ---
    {"HISTORY", "HKeyHistory", "Open Listening History"}, // HISTORY / TOP TRACKS / TOP PLAYLISTS / HABITS overlay
    // --- Equalizer ---
    {"EQUALIZER", "HKeyEqualizer", "Open Equalizer"}, // 10-band EQ with presets
    // --- Downloads ---
    {"DOWNLOADS", "HKeyDownloadStream", "Download Stream"},
    // --- System ---
    {"LIBRARY", "HKeyRescan", "Rescan Library"},   // pick up files that were added while the app runs
    {"SYSTEM", "HKeySetting", "Open Settings"},
    {nullptr, "HKeyModeNext", "Next Mode"},      // music player -> radio -> audio books (SHIFT and + on a German keyboard)
    {nullptr, "HKeyModePrev", "Previous Mode"},
    {nullptr, "HKeyCoverPrev", "Cover Look Back"},      // plain disk -> the cover modes (audio books: default picture -> cover)
    {nullptr, "HKeyCoverNext", "Cover Look Forward"},
    {nullptr, "HKeyConsole", "Console / Logs"},
    {nullptr, "HKeyAudioDevice", "Choose Audio Device"},
    {nullptr, "HKeyWizard", "Help Wizard"},
    {nullptr, "HKeyCheatsheet", "Cheatsheet"},
    {nullptr, "HKeyMenus", "Menus / Overlays Index"},
    {nullptr, "HKeyQuit", "Quit Application"},
};
inline constexpr int kRefRowCount = sizeof(kRefRows) / sizeof(kRefRows[0]);

// The one-line, read-only note drawn first on the tab (with a blank spacer
// line above it, same as a header) and before the hotkeys start. Not
// selectable -- it costs a display line, same as a header, but no entry in
// the row-index space below.
inline const char* const kRefNote = "KEYS NOT LISTED HERE ARE FIXED. SEE CHEAT SHEET FOR THE FULL LIST OF COMMANDS, `?`";

// Selectable row range of this tab, in order:
//
//     kRefStart .. kRefEnd-1          rebindable hotkeys (kRefRows)
//     kRefEnd ..                      font-map rows (read-only)
//
// Every row-index translation on this tab (ref_display_row(), the scroll
// window, settings_max_row(), the Enter-to-edit guard) has to go through
// these two constants.
inline constexpr int kRefStart = 1;   // selectable row 0 is the "Reset All Keys To Default" line, right under the note
inline constexpr int kRefEnd = kRefStart + kRefRowCount;

// The GENERAL tab (tab 1, formerly ON/OFF) and the ANIMA/VIZ tab (tab 2) are laid out as categories: a header line, then its rows. Every
// selectable row has an id (SetId, the same numbers on both tabs); settings_row_ counts the selectable rows of the tab
// only, so settings_item_id() turns it into the id that settings_get_value() / settings_options_for() /
// settings_commit_edit() / settings_cycle() and the renderer work with. Adding a row = one id + one line in a layout.
enum SetId {
    kSidVizField, kSidDisk, kSidDummy, kSidQueue, kSidWave, kSidLyricsEngine, kSidLyricViz, kSidVisualizer, kSidNoLyricsViz,
    kSidStereo, kSidNormalize, kSidMetaOnly, kSidEmoji, kSidMarquee, kSidUseLyrics, kSidUseOsci, kSidUseSpectro, kSidUseSphere,
    kSidOrder1, kSidOrder2, kSidOrder3, kSidOrder4,
    kSidPlayback, kSidStartMode, kSidBlackBg, kSidShareProfiles, kSidRemoteApp, kSidUpdateCheck, kSidMediaKeys, kSidUseTermSettings, kSidTermSize, kSidFullScreen, kSidLatinLookup, kSidLatinMacrons, kSidLatinWo, kSidLatinHanzi, kSidLyricsVariants, kSidRateCap, kSidTrackBuffer,
    kSidVisFluidity, kSidWaveStyle, kSidDiskSpeed, kSidVisDegr, kSidVisVisc, kSidLyrAlign, kSidLyrAnim,
    kSidVolIndicator,
    kSidUseModePlayer, kSidUseModeRadio, kSidUseModeBook, kSidModeOrder1, kSidModeOrder2, kSidModeOrder3, kSidBookAsk, kSidBookCover, kSidBookCoverWave,
    kSidAlbumCover, kSidCoverMode, kSidCoverContrast, kSidCoverSearch, kSidCoverBrightness, kSidCoverColors,
    kSidTermDefaultFont, // TEXT AND TRANSLITERATION: leave the terminal's own font alone (terminal_font.h)
    kSidCheckUpdate,   // [ENTER] checks for a newer release now (an action row, no value to switch)
    kSidBookSpeed,     // audio book player: the playback speed (0.5x - 3.0x, the pitch stays)
    kSidAudioDevice,   // the sound output: shows the device in use, [ENTER] opens the CHOOSE AUDIO DEVICE overlay
};
// True while the audio book player is the mode that runs (set by App::run()): its GENERAL and ANIMA/VIZ tabs leave out what only
// belongs to the music player and name the lyrics view "notes".
inline bool g_settings_book_layout = false;
struct SetLine { bool header; int id; const char* text; };   // a category header, or a row (id + label)
inline const std::vector<SetLine>& settings_layout(int tab) {
    static const std::vector<SetLine> on_off = {
        {true, -1, "MODES"},
        {false, kSidPlayback, "Playback"}, {false, kSidStartMode, "Start Mode"}, {false, kSidQueue, "Display queue"},
        {true, -1, ""},
        {true, -1, "MODES ON/OFF AND ORDER"},   // SHIFT and + / SHIFT and - walk through the modes that are on, in this order
        {false, kSidUseModePlayer, "Use Music Player"}, {false, kSidUseModeRadio, "Use Radio"}, {false, kSidUseModeBook, "Use Audio Books"},
        {false, kSidModeOrder1, "Mode Order 1"}, {false, kSidModeOrder2, "Mode Order 2"}, {false, kSidModeOrder3, "Mode Order 3"},
        {true, -1, ""},
        {true, -1, "AUDIO"},
        {false, kSidAudioDevice, "Choose Audio Device"},
        {false, kSidStereo, "Stereo Sound"}, {false, kSidNormalize, "Normalize Volume"}, {false, kSidTrackBuffer, "Track Buffer (MB)"},
        {false, kSidRateCap, "Rate Cap at 96 kHz"},
        {true, -1, ""},
        {true, -1, "TERMINAL"},
        {false, kSidBlackBg, "Black Background"}, {false, kSidUseTermSettings, "Use Terminal Settings"}, {false, kSidTermSize, "Terminal Size"},
        {false, kSidFullScreen, "Full Screen"},
        {true, -1, ""},
        {true, -1, "REMOTE / UPDATES"},
        {false, kSidRemoteApp, "Remote App"}, {false, kSidMediaKeys, "Media Keys"},
        {false, kSidUpdateCheck, "Update Notice"}, {false, kSidCheckUpdate, "Check Update"},
        {true, -1, ""},
        {true, -1, "TEXT AND TRANSLITERATION (JAP./KOR./CHIN.)"},
        {false, kSidTermDefaultFont, "Terminal Default Font"},
        {false, kSidEmoji, "Replace Emoji"}, {false, kSidMarquee, "Marquee Scrolling"},
        {false, kSidLatinMacrons, "Latin Macrons"}, {false, kSidLatinWo, "Latin Wo"}, {false, kSidLatinHanzi, "Translit. Kanji"}, {false, kSidLatinLookup, "Latin Name Lookup"}, {false, kSidLyricsVariants, "Lyrics Name Variants"},
    };
    // A header with an empty text is the blank line before the next header (every header but the first has one).
    static const std::vector<SetLine> anima = {
        {true, -1, "LYRICS/VIZ FIELD"},
        {false, kSidLyricViz, "Lyrics/Viz Default"}, {false, kSidNoLyricsViz, "When No Lyrics Show"},
        {true, -1, ""},
        {true, -1, "LYRICS"},
        {false, kSidLyrAlign, "Lyrics Alignment"}, {false, kSidLyrAnim, "Lyrics Animation"},
        {true, -1, ""},
        {true, -1, "DISK"},
        {false, kSidDisk, "Eliment Disk / Cover"}, {false, kSidDiskSpeed, "Disk Speed"},
        {true, -1, ""},
        {true, -1, "DISK / COVER OPTIONS"},
        {false, kSidAlbumCover, "Album Cover"}, {false, kSidCoverMode, "Cover Mode"},
        {false, kSidCoverContrast, "Cover Contrast"}, {false, kSidCoverBrightness, "Cover Brightness"},
        {false, kSidCoverColors, "Cover Colors"}, {false, kSidCoverSearch, "Cover Search"},
        {true, -1, ""},
        {true, -1, "SEEK AND VOLUME"},
        {false, kSidDummy, "Dummy Buttons"}, {false, kSidVolIndicator, "Volume Indicator"},
        {true, -1, ""},
        {true, -1, "SPECTRUM BARS"},   // the bars under the meta data
        {false, kSidVisualizer, "On/Off"},
        {false, kSidVisFluidity, "Bars Fluidity"}, {false, kSidVisDegr, "Bars Degradation"}, {false, kSidVisVisc, "Bars Viscosity"},
        {true, -1, ""},
        {true, -1, "PROGRESS WAVEFORM"},   // the braille waveform in the progress bar
        {false, kSidWave, "On/Off"},
        {false, kSidWaveStyle, "Waveform Style"},
        {true, -1, ""},
        {true, -1, "OSCI/SPECTRO SETTINGS"},
        {false, kSidShareProfiles, "Modes Share Settings"},
        {true, -1, ""},
        {true, -1, "VIZ/LYRICS ORDER"},
        {false, kSidOrder1, "Viz Order 1"}, {false, kSidOrder2, "Viz Order 2"},
        {false, kSidOrder3, "Viz Order 3"}, {false, kSidOrder4, "Viz Order 4"},
        {true, -1, ""},
        {true, -1, "MAIN UI VIZ/LYRICS FIELD"},   // the field right of the meta data
        {false, kSidVizField, "Viz/Lyrics Field"}, {false, kSidUseLyrics, "Use Lyrics Engine"},
        {false, kSidUseOsci, "Use Oscilloscope"},                                            // false: left out entirely
        {false, kSidUseSpectro, "Use Spectrogram"}, {false, kSidUseSphere, "Use Sphere"},    // (not in the "." cycle)
    };
    // The audio book player: the same tabs without what only the music player uses (the play mode, the lyrics, the disk's
    // speed, the display queue), the lyrics view called "notes", the disk's place taken by the book cover.
    static const std::vector<SetLine> book_on_off = {
        {true, -1, "MODES"},
        {false, kSidStartMode, "Start Mode"},
        {true, -1, ""},
        {true, -1, "MODES ON/OFF AND ORDER"},
        {false, kSidUseModePlayer, "Use Music Player"}, {false, kSidUseModeRadio, "Use Radio"}, {false, kSidUseModeBook, "Use Audio Books"},
        {false, kSidModeOrder1, "Mode Order 1"}, {false, kSidModeOrder2, "Mode Order 2"}, {false, kSidModeOrder3, "Mode Order 3"},
        {true, -1, ""},
        {true, -1, "BOOKMARKS"},
        {false, kSidBookAsk, "Ask for Bookmark"},
        {true, -1, ""},
        {true, -1, "TERMINAL"},
        {false, kSidBlackBg, "Black Background"}, {false, kSidUseTermSettings, "Use Terminal Settings"}, {false, kSidTermSize, "Terminal Size"},
        {false, kSidFullScreen, "Full Screen"},
        {true, -1, ""},
        {true, -1, "REMOTE / UPDATES"},
        {false, kSidRemoteApp, "Remote App"}, {false, kSidMediaKeys, "Media Keys"},
        {false, kSidUpdateCheck, "Update Notice"}, {false, kSidCheckUpdate, "Check Update"},
        {true, -1, ""},
        {true, -1, "AUDIO"},
        {false, kSidAudioDevice, "Choose Audio Device"}, {false, kSidBookSpeed, "Playback Speed"},
        {false, kSidStereo, "Stereo Sound"}, {false, kSidNormalize, "Normalize Volume"}, {false, kSidTrackBuffer, "Track Buffer (MB)"},
        {false, kSidRateCap, "Rate Cap at 96 kHz"},
        {true, -1, ""},
        {true, -1, "TEXT"},
        {false, kSidTermDefaultFont, "Terminal Default Font"},
        {false, kSidEmoji, "Replace Emoji"}, {false, kSidMarquee, "Marquee Scrolling"},
    };
    static const std::vector<SetLine> book_anima = {
        {true, -1, "NOTES/VIZ FIELD"},
        {false, kSidLyricViz, "Notes/Viz Default"}, {false, kSidNoLyricsViz, "When No Notes Show"},
        {true, -1, ""},
        {true, -1, "BOOK COVER"},
        {false, kSidBookCover, "Book Cover"}, {false, kSidBookCoverWave, "Book Gradient Wave"}, {false, kSidCoverContrast, "Cover Contrast"},
        {false, kSidCoverBrightness, "Cover Brightness"}, {false, kSidCoverColors, "Cover Colors"},
        {true, -1, ""},
        {true, -1, "SEEK AND VOLUME"},
        {false, kSidDummy, "Dummy Buttons"}, {false, kSidVolIndicator, "Volume Indicator"},
        {true, -1, ""},
        {true, -1, "SPECTRUM BARS"},
        {false, kSidVisualizer, "On/Off"},
        {false, kSidVisFluidity, "Bars Fluidity"}, {false, kSidVisDegr, "Bars Degradation"}, {false, kSidVisVisc, "Bars Viscosity"},
        {true, -1, ""},
        {true, -1, "PROGRESS WAVEFORM"},
        {false, kSidWave, "On/Off"},
        {false, kSidWaveStyle, "Waveform Style"},
        {true, -1, ""},
        {true, -1, "OSCI/SPECTRO SETTINGS"},
        {false, kSidShareProfiles, "Modes Share Settings"},
        {true, -1, ""},
        {true, -1, "VIZ/NOTES ORDER"},
        {false, kSidOrder1, "Viz Order 1"}, {false, kSidOrder2, "Viz Order 2"},
        {false, kSidOrder3, "Viz Order 3"}, {false, kSidOrder4, "Viz Order 4"},
        {true, -1, ""},
        {true, -1, "MAIN UI VIZ/NOTES FIELD"},
        {false, kSidVizField, "Viz/Notes Field"}, {false, kSidUseLyrics, "Use Book Notes"},
        {false, kSidUseOsci, "Use Oscilloscope"},
        {false, kSidUseSpectro, "Use Spectrogram"}, {false, kSidUseSphere, "Use Sphere"},
    };
    if (g_settings_book_layout) return tab == 1 ? book_on_off : book_anima;
    return tab == 1 ? on_off : anima;
}
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
inline const char* const kVizNames[4] = {"sphere", "osci", "spectro", "lyrics"};   // viz_order values, row values
// The name of mode `m` (0 music player, 1 radio, 2 audio books) on a row: the words of config.txt.
inline const char* mode_row_name(int m) { return m == 1 ? "radio" : m == 2 ? "audiobook" : "player"; }
// The name of view `i` on a row: the audio book player calls the lyrics view "notes".
inline const char* viz_row_name(int i) { return (g_settings_book_layout && i == 3) ? "notes" : kVizNames[i < 0 ? 0 : i > 3 ? 3 : i]; }

} // namespace muisc
