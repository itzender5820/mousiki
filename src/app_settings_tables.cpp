// The row layouts of the GENERAL and ANIMA/VIZ tabs of the Settings screen (declared in app_settings_tables.h).
#include "app_settings_tables.h"

#include <utility>

namespace muisc {

const std::vector<SetLine>& settings_layout(int tab) {
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
        {false, kSidRateCap, "Rate Cap at 96 kHz"}, {false, kSidPauseFade, "Pause Fade"},
        {true, -1, ""},
        {true, -1, "TERMINAL"},
        {false, kSidBlackBg, "Black Background"}, {false, kSidUseTermSettings, "Use Terminal Settings"}, {false, kSidTermSize, "Terminal Size"},
        {false, kSidFullScreen, "Full Screen"}, {false, kSidReduceMotion, "Reduce Motion When Paused"},
        {true, -1, ""},
        {true, -1, "CLI / REMOTE / UPDATES"},
        {false, kSidCliScripts, "CLI Scripts"}, {false, kSidRemoteApp, "Remote App"}, {false, kSidMediaKeys, "Media Keys"},
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
        {false, kSidNoVideoViz, "When No Video Shows"},
        {true, -1, ""},
        {true, -1, "LYRICS"},
        {false, kSidLyrAlign, "Lyrics Alignment"}, {false, kSidLyrAnim, "Lyrics Animation"},
        {true, -1, ""},
        {true, -1, "VIDEO"},
        {false, kSidVideoLyrics, "Include Lyrics"}, {false, kSidVideoSize, "Video Size"}, {false, kSidVideoFx, "Video Effect"}, {false, kSidVideoBase, "Video Base Layer"}, {false, kSidVideoContrast, "Video Contrast"}, {false, kSidVideoBrightness, "Video Brightness"}, {false, kSidVideoColors, "Video Colors"}, {false, kSidVideoChroma, "Video Chromatic Aberration"}, {false, kSidVideoDither, "Video Dither Pattern"}, {false, kSidVideoGradSat, "Video Gradient Saturation"}, {false, kSidVideoRound, "Video Rounded Corners"}, {false, kSidVideoClap, "Show Clap Loading"}, {false, kSidVideoCover, "Disk/Cover/Thumbnail"}, {false, kSidVideoRes, "Video Resolution"}, {false, kSidVideoBuffer, "Video Buffer"}, {false, kSidVideoMp3, "yt-dlp mp3 from mp4"},
        {true, -1, ""},
        {true, -1, "DISK"},
        {false, kSidDisk, "Eliment Disk / Cover"}, {false, kSidDiskSpeed, "Disk Speed"}, {false, kSidDiskBorder, "Disk Border"},
        {true, -1, ""},
        {true, -1, "DISK / COVER OPTIONS"},
        {false, kSidAlbumCover, "Album Cover"}, {false, kSidCoverMode, "Cover Mode"},
        {false, kSidCoverContrast, "Cover Contrast"}, {false, kSidCoverBrightness, "Cover Brightness"},
        {false, kSidCoverColors, "Cover Colors"}, {false, kSidCoverGradSat, "Cover Gradient Saturation"}, {false, kSidCoverChroma, "Cover Chromatic Aberration"}, {false, kSidCoverSearch, "Cover Search"},
        {true, -1, ""},
        {true, -1, "SEEK AND VOLUME"},
        {false, kSidDummy, "Dummy Buttons"}, {false, kSidVolIndicator, "Volume Indicator"},
        {true, -1, ""},
        {true, -1, "SPECTRUM BARS"},   // the bars under the meta data
        {false, kSidVisualizer, "On/Off"},
        {false, kSidBarStyle, "Bars Style"},
        {false, kSidHifiRes, "Hi-Fi Resolution"}, {false, kSidHifiChan, "Hi-Fi Channels"}, {false, kSidHifiColor, "Hi-Fi Color"}, {false, kSidHifiScale, "Hi-Fi Height"}, {false, kSidHifiAdapt, "Hi-Fi Adapt Height"}, {false, kSidHifiSplit, "Hi-Fi Channel Gap"}, {false, kSidHifiFScale, "Hi-Fi Frequency Scale"}, {false, kSidHifiGain, "Hi-Fi Gain (dB)"}, {false, kSidHifiRange, "Hi-Fi Range (dB)"},
        {false, kSidBarBehavior, "Bars Behavior"}, {false, kSidVisFluidity, "Bars Fluidity"}, {false, kSidVisDegr, "Bars Degradation"}, {false, kSidVisVisc, "Bars Viscosity"},
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
        {false, kSidOrder3, "Viz Order 3"}, {false, kSidOrder4, "Viz Order 4"}, {false, kSidOrder5, "Viz Order 5"},
        {true, -1, ""},
        {true, -1, "MAIN UI VIZ/LYRICS FIELD"},   // the field right of the meta data
        {false, kSidVizField, "Viz/Lyrics Field"}, {false, kSidUseLyrics, "Use Lyrics Engine"}, {false, kSidUseVideo, "Use Video"},
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
        {false, kSidFullScreen, "Full Screen"}, {false, kSidReduceMotion, "Reduce Motion When Paused"},
        {true, -1, ""},
        {true, -1, "CLI / REMOTE / UPDATES"},
        {false, kSidCliScripts, "CLI Scripts"}, {false, kSidRemoteApp, "Remote App"}, {false, kSidMediaKeys, "Media Keys"},
        {false, kSidUpdateCheck, "Update Notice"}, {false, kSidCheckUpdate, "Check Update"},
        {true, -1, ""},
        {true, -1, "AUDIO"},
        {false, kSidAudioDevice, "Choose Audio Device"}, {false, kSidBookSpeed, "Playback Speed"},
        {false, kSidStereo, "Stereo Sound"}, {false, kSidNormalize, "Normalize Volume"}, {false, kSidTrackBuffer, "Track Buffer (MB)"},
        {false, kSidRateCap, "Rate Cap at 96 kHz"}, {false, kSidPauseFade, "Pause Fade"},
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
        {false, kSidBookCover, "Book Cover"}, {false, kSidDiskBorder, "Disk Border"}, {false, kSidBookCoverWave, "Book Gradient Wave"}, {false, kSidCoverContrast, "Cover Contrast"},
        {false, kSidCoverBrightness, "Cover Brightness"}, {false, kSidCoverColors, "Cover Colors"},
        {true, -1, ""},
        {true, -1, "SEEK AND VOLUME"},
        {false, kSidDummy, "Dummy Buttons"}, {false, kSidVolIndicator, "Volume Indicator"},
        {true, -1, ""},
        {true, -1, "SPECTRUM BARS"},
        {false, kSidVisualizer, "On/Off"},
        {false, kSidBarStyle, "Bars Style"},
        {false, kSidHifiRes, "Hi-Fi Resolution"}, {false, kSidHifiChan, "Hi-Fi Channels"}, {false, kSidHifiColor, "Hi-Fi Color"}, {false, kSidHifiScale, "Hi-Fi Height"}, {false, kSidHifiAdapt, "Hi-Fi Adapt Height"}, {false, kSidHifiSplit, "Hi-Fi Channel Gap"}, {false, kSidHifiFScale, "Hi-Fi Frequency Scale"}, {false, kSidHifiGain, "Hi-Fi Gain (dB)"}, {false, kSidHifiRange, "Hi-Fi Range (dB)"},
        {false, kSidBarBehavior, "Bars Behavior"}, {false, kSidVisFluidity, "Bars Fluidity"}, {false, kSidVisDegr, "Bars Degradation"}, {false, kSidVisVisc, "Bars Viscosity"},
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
    const std::vector<SetLine>& base = g_settings_book_layout ? (tab == 1 ? book_on_off : book_anima) : (tab == 1 ? on_off : anima);
    if (g_settings_hifi_rows) return base;
    // without Hi-Fi the three Hi-Fi rows are left out (one filtered copy per layout, kept)
    static std::vector<std::pair<const std::vector<SetLine>*, std::vector<SetLine>>> cache;
    for (const auto& c : cache) if (c.first == &base) return c.second;
    std::vector<SetLine> f;
    for (const auto& l : base) if (l.header || (l.id != kSidHifiRes && l.id != kSidHifiChan && l.id != kSidHifiColor && l.id != kSidHifiScale && l.id != kSidHifiAdapt && l.id != kSidHifiSplit && l.id != kSidHifiFScale && l.id != kSidHifiGain && l.id != kSidHifiRange)) f.push_back(l);
    cache.emplace_back(&base, std::move(f));
    return cache.back().second;
}

} // namespace muisc
