// The trees of the "<" menu index for the music player, the audio book and the radio (see organigram.h). The action of a node is the
// hotkey action it presses (the player and the audio book), or the id of the radio's menu entry; an action starting with '#' is a
// fixed key (the fallback), not a bound one.
#pragma once

#include "organigram.h"

namespace muisc {
namespace org {

#if defined(__APPLE__)
#define MUSIKI_ORG_LYRICS_TIMING_KEY "J"       // the default on a Mac (settings.cpp)
#define MUSIKI_ORG_COVER_FETCH_KEY "Q"
#define MUSIKI_ORG_VIDEO_FETCH_KEY "D"
#else
#define MUSIKI_ORG_LYRICS_TIMING_KEY "ALT+L"
#define MUSIKI_ORG_COVER_FETCH_KEY "ALT+C"
#define MUSIKI_ORG_VIDEO_FETCH_KEY "ALT+V"
#endif

inline const Node kPlayerTree[] = {
    {0, "Main", nullptr, nullptr},
    {1, "System", nullptr, nullptr},
    {2, "Settings", "HKeySetting", "s"},
    {2, "Console / logs / status", "HKeyConsole", "t"},
    {2, "Help Wizard", "HKeyWizard", "W"},
    {2, "Cheat Sheet", "HKeyCheatsheet", "?"},
    {1, "Sound", nullptr, nullptr},
    {2, "Equalizer", "HKeyEqualizer", "E"},
    {2, "Loudness Normalization", "HKeyNormMenu", "V"},
    {2, "Sleep Timer", "HKeySleepTimer", "Z"},
    {2, "Audio Device", "HKeyAudioDevice", "\xc2\xb0"},
    {1, "Library", nullptr, nullptr},
    {2, "Big Folder Overlay", "HKeyFolderOverlay", "F", "Folder Overlay"},
    {2, "Big List Overlay", "HKeyListOverlay", "L", "List Overlay"},
    {2, "Big Queue Overlay", "HKeyQueueOverlay", "K", "Queue Overlay"},
    {1, "Other Lists", nullptr, nullptr},
    {2, "Playlists", "HKeyPlaylist", "P"},
    {2, "Listening History", "HKeyHistory", "H"},
    {1, "Meta and Multi Media Data", nullptr, nullptr, "Meta & Multimedia"},
    {2, "Meta Data Menu", "HKeyMetaEditor", "M"},
    {3, "Fetch and Convert", nullptr, nullptr},
    {4, "Fetch Cover", "HKeyCoverFetch", MUSIKI_ORG_COVER_FETCH_KEY},
    {4, "Fetch Video Overlay", "HKeyVideoFetch", MUSIKI_ORG_VIDEO_FETCH_KEY},
    {4, "File Conversion", "HKeyConvertOverlay", "C"},
    {3, "Cover and Video Look", nullptr, nullptr},
    {4, "Style Options (cover look)", "HKeyStyleOptions", "S", "Style Options"},
    {1, "Visualizations", nullptr, nullptr},
    {2, "Spectrum Bars Options", "HKeyBarsMenu", "B", "Spectrum Bars"},
    {2, "Spectrogram", nullptr, nullptr},
    {3, "Spectrogram Options", "HKeySpectroMenu", "I", "Options"},
    {3, "Spectrogram Overlay", "HKeySpectroFull", "U", "Overlay"},
    {3, "Spectrogram Extra Window (Max. Screen Refresh Rate)", "HKeySpectroWindow", "(", "Extra Window"},
    {2, "Oscilloscope", nullptr, nullptr},
    {3, "Oscilloscope Options", "HKeyOscMenu", "O", "Options"},
    {3, "Oscilloscope Overlay", "HKeyScopeBig", "=", "Overlay"},
    {3, "Oscilloscope Extra Window (Max. Screen Refresh Rate)", "HKeyScopeWindow", ")", "Extra Window"},
    {2, "Video", nullptr, nullptr},
    {3, "Video Overlay", "HKeyVideo", "&", "Options"},
    {3, "Full Screen Video", "HKeyVideoFull", "v", "Overlay"},
    {2, "Creator", "HKeyCreator", "^"},
    {1, "Lyrics", nullptr, nullptr, nullptr, 'L'},
    {2, "Refetch Lyrics", "HKeyRetryLyrics", "l"},
    {2, "Read and Translate Overlay", "HKeyReadLyrics", "CTRL+L"},
    {2, "Karaoke Mode", "HKeyKaraoke", "k"},
    {2, "Lyrics Timing", "HKeyLyricsTiming", MUSIKI_ORG_LYRICS_TIMING_KEY},
};

// The audio book: only what it can do (no lyrics, playlists or creator).
inline const Node kBookTree[] = {
    {0, "Main", nullptr, nullptr},
    {1, "System", nullptr, nullptr},
    {2, "Settings", "HKeySetting", "s"},
    {2, "Console / logs / status", "HKeyConsole", "t"},
    {2, "Help Wizard", "HKeyWizard", "W"},
    {2, "Cheat Sheet", "HKeyCheatsheet", "?"},
    {2, "File Conversion", "HKeyConvertOverlay", "C"},
    {1, "Library and Meta Data", nullptr, nullptr},
    {2, "Library Overlay", "#", "S"},
    {2, "Tag Editor", "HKeyMetaEditor", "M"},
    {2, "Reading History", "HKeyHistory", "H"},
    {2, "Chapter Marks", "HKeyLyricsTiming", MUSIKI_ORG_LYRICS_TIMING_KEY},
    {1, "Bookmarks and Notes", nullptr, nullptr},
    {2, "Bookmark List", "HKeyQueueOverlay", "K"},
    {2, "New Note", "HKeyToggleMetaOnly", "N"},
    {2, "Set Bookmark", "HKeyBookmark", "B"},
    {2, "Notes Overlay", "HKeyListOverlay", "L"},
    {1, "Sound", nullptr, nullptr},
    {2, "Equalizer", "HKeyEqualizer", "E"},
    {2, "Loudness Normalization", "HKeyNormMenu", "V"},
    {2, "Sleep Timer", "HKeySleepTimer", "Z"},
    {2, "Audio Device", "HKeyAudioDevice", "\xc2\xb0"},
    {1, "Visualizations", nullptr, nullptr},
    {2, "Spectrum Bars Options", "#bars", "Y", "Spectrum Bars"},   // (SHIFT+b is the bookmark in the audio book: SHIFT+y opens the bars there)
    {2, "Spectrogram", nullptr, nullptr},
    {3, "Spectrogram Options", "HKeySpectroMenu", "I", "Options"},
    {3, "Spectrogram Overlay", "HKeySpectroFull", "U", "Overlay"},
    {2, "Oscilloscope", nullptr, nullptr},
    {3, "Oscilloscope Options", "HKeyOscMenu", "O", "Options"},
    {3, "Oscilloscope Overlay", "HKeyScopeBig", "=", "Overlay"},
};

// The radio: the actions are the ids of its menu entries (radio_ui.h kMenusEntries).
inline const Node kRadioTree[] = {
    {0, "Main", nullptr, nullptr},
    {1, "System", nullptr, nullptr},
    {2, "Settings", "Settings", ""},
    {2, "Help Wizard", "Wizard", ""},
    {2, "Cheat Sheet", "Cheatsheet", ""},
    {1, "Sound", nullptr, nullptr},
    {2, "Equalizer", "EqMenu", ""},
    {2, "Loudness Normalization", "NormMenu", ""},
    {2, "Sleep Timer", "SleepTimer", ""},
    {2, "Record", "Record", ""},
    {1, "Stations and Presets", nullptr, nullptr},
    {2, "Presets Menu", "Presets", ""},
    {2, "Station Lists", "Lists", ""},
    {2, "Radio Browser", "Browser", ""},
    {2, "Big Stations Overlay", "Stations", ""},
    {2, "Listening History", "History", ""},
    {1, "Visualizations", nullptr, nullptr},
    {2, "Spectrum Bars Options", "BarsMenu", "", "Spectrum Bars"},
    {2, "Spectrogram", nullptr, nullptr},
    {3, "Spectrogram Options", "SpectroMenu", "", "Options"},
    {3, "Spectrogram Overlay", "SpectroFull", "", "Overlay"},
    {3, "Spectrogram Extra Window", "SpectroWindow", "", "Extra Window"},
    {2, "Oscilloscope", nullptr, nullptr},
    {3, "Oscilloscope Options", "OsciMenu", "", "Options"},
    {3, "Oscilloscope Overlay", "ScopeBig", "", "Overlay"},
    {3, "Oscilloscope Extra Window", "ScopeWindow", "", "Extra Window"},
};

inline Tree player_tree() { return make_tree(kPlayerTree); }
inline Tree book_tree() { return make_tree(kBookTree); }
inline Tree radio_tree() { return make_tree(kRadioTree); }

}  // namespace org
}  // namespace muisc
