#pragma once
// The REFERENCE tab of the Settings screen: the rebindable hotkey rows and the row range they take. Only the files that draw or
// edit that tab include this (app.cpp, app_keys_browse.cpp, app_settings.cpp, app_settings_screen.cpp).

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
    {nullptr, "HKeyCyclePlayModeBack", "Cycle Play Mode Back"},
    {nullptr, "HKeySeekForward", "Seek Forward"},
    {nullptr, "HKeySeekBackward", "Seek Backward"},
    {nullptr, "HKeyIncreaseVolume", "Volume Up"},
    {nullptr, "HKeyDecreaseVolume", "Volume Down"},
    {nullptr, "HKeyToggleMute", "Toggle Mute"},
    {nullptr, "HKeyToggleNormalize", "Toggle Normalize"},
    {nullptr, "HKeyBarsMenu", "Spectrum Bars Options"}, // style, the Hi-Fi rows, motion; under the upper pane
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
    {nullptr, "HKeyRetryLyrics", "Refetch Lyrics"},
    {nullptr, "HKeyKaraoke", "Karaoke Overlay"},
    {nullptr, "HKeyVideoFetch", "Open Fetch Videos"},
    {nullptr, "HKeyVideoFx", "Video Effect (Next, This Track)"},
    {nullptr, "HKeyVideoFxBack", "Video Effect (Previous, This Track)"},
    {nullptr, "HKeyVideoFull", "Full Screen Video"},
    {nullptr, "HKeyVideo", "Video Overlay"},          // video files in the library + the "video" view (picture with the lyric line)
    {nullptr, "HKeyLyricsTiming", "Lyrics Timing Overlay"},   // shift synced lyrics earlier / later
    {nullptr, "HKeyReadLyrics", "Read and Translate Overlay"},       // saved lyrics as text, files + translations
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
    {"SEARCH", "HKeySearch", "Search Local"},   // the prefixes typed in the box (y: sc: b: p: f: r: a:) are fixed, see the cheat sheet
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
    {nullptr, "HKeyFetchTags", "Fetch Missing Tags (Meta Editor)"}, // the highlighted title, inside the meta data editor
    {nullptr, "HKeyBookmark", "Set Bookmark (Audio Book)"}, // the same key as the spectrum bars overlay, which the audio book mode does not use
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

} // namespace muisc
