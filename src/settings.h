// Settings (see settings.cpp): the Settings struct with every option of the music player, and the
// functions that read and write them.
#pragma once
#include "video_fx.h"
#include "bars_presets.h"
#include <filesystem>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "equalizer.h"
#include "scope_metrics.h"
#include "spectrogram.h"
#include "viz_prefs.h"
#include "volume_bar.h"   // the looks of the VOLUME BAR (kVolIndicatorNames, volume_bar_fill)

namespace muisc {

namespace fs = std::filesystem;

// Colors: every field holds a plain decimal ANSI 256-color palette index
// as a string, "1".."255". "0" (or empty) means "no color" -- inherit
// whatever the terminal/panel would otherwise show. There is no named
// preset ("cyan", "white", etc.) anymore; type the number directly in
// the settings panel with Enter. "_end" fields, when non-empty, turn a
// solid color into a gradient between the base color and that end color.
// "Use Lyrics" on a video (Include Lyrics / Use Lyrics on Video): 0 none, 1 lyrics, 2 lyrics + translation, 3 translation only.
inline const char* lyr_mode_name(int m) { return m == 1 ? "lyrics" : m == 2 ? "lyrics+transl." : m == 3 ? "transl." : "none"; }
inline int lyr_mode_parse(const std::string& v) {   // (an old config says true / false)
    std::string t;
    for (char c : v) t += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (t == "true" || t == "on" || t == "1" || t == "lyrics") return 1;
    if (t == "2" || t == "both" || t == "lyrics+transl." || t == "lyrics+translation") return 2;
    if (t == "3" || t == "transl." || t == "translation" || t == "only") return 3;
    return 0;
}

struct Settings : VizPrefs {
    // --- element visibility toggles (config.txt: "Eliment_*") ----------
    bool element_disk = true;
    // Album cover in the disk's place (music player; config.txt AlbumCover / CoverMode / CoverSearch).
    int latin_hanzi = 0;                 // Latin letters: Chinese characters without any hint: 0 leave alone, 1 read as Chinese, 2 read as Japanese
    bool latin_macrons = false;          // Latin letters: long vowels as macrons (ō ū) instead of ou / uu (LatinMacrons=)
    bool latin_wo = false;               // Latin letters: the particle を as "wo" instead of "o" (LatinWo=)
    bool lyrics_name_variants = true;    // a lyrics search that finds nothing tries other spellings of the names (LyricsNameVariants=)
    bool latin_lookup = false;           // artists written in Chinese characters are looked up on MusicBrainz for their Latin name (background; LatinNameLookup=)
    bool latin_letters = false;          // Japanese / Korean / Chinese text is shown in Latin letters (HKeyTranslit, SHIFT+')
    bool album_cover = false;            // the cover of the playing track is shown in the disk's place (the disk where there is none)
    int cover_mode = 2;                  // 0 = just the cover, 1 = cover with the disk's gradient, 2 = cover behind the disk (the disk's dots are pinholes; the dot grid stays, the pattern turns), 3 = v2 (the dots themselves turn), 4 = full animation (dots that come and go), 5 = Cover ASCII (braille dots in the cover's colours), 6 = Cover ASCII with Gradient
    int cover_contrast = 0;              // 0..100 in steps of 5 (%): more contrast for the cover
    // The cover behind the disk (Cover Modes 2-4, and their braille looks): Cover Brightness 0..100 % (steps of 5) -- how far the cover's
    // brightness is stretched up, so the disk's dots shine like the plain disk's (0 = only the full range of the picture, more = a
    // brighter floor for every dot); Cover Colors 0..100 % -- the cover's own colours mixed into the disk's gradient (0 = the
    // gradient only, 100 = the cover's own colours). config.txt: CoverBrightness= / CoverColors=
    int cover_brightness = 50;
    int cover_colors = 0;
    int cover_search = 0;                // 0 default (file, then folder), 1 folder first, 2 within file first, 3 image in folder only, 4 within file only
    bool element_dummy_buttons = true;
    bool element_queue = true;           // replaces the old queue_visible
    bool element_waveform = true;
    // "Volume Indicator" (GENERAL tab): what the VOLUME BAR is drawn with. 0 = "#", 1 = "⠶", 2 = "⣿", 3 = "⡾⣽" (a fixed row of
    // braille characters that is not repeating), 4 = "disk" (the dots of the disk animation, see volume_bar.h).
    // config.txt: VolumeIndicator=. Default: the "disk" look (the animation).
    int volume_indicator = 4;
    // "gradient" (true) / "no gradient" (false), right of the Volume Indicator: the filled part of the bar takes the colours
    // of the VIZ gradient from left to right, like the oscilloscope does. config.txt: VolumeGradient=
    bool volume_gradient = false;
    // "Viz/Lyrics Field" (ANIMA/VIZ tab): the field right of the meta data, where the lyrics and the sphere / oscilloscope /
    // spectrogram are shown. false: no field at all, the meta data (and the spectrum bars) take its width, no lyrics are
    // fetched for it. config.txt: VizLyricsField.
    bool element_vizfield = true;
    // "Lyrics Engine": the field currently shows the lyrics (and they are fetched) instead of the visual lyric_viz picks.
    // The "." key switches it live. It needs "Use Lyrics": turning one on turns the other on, turning Use Lyrics off turns
    // this one off (enforce_visual_switches()).
    bool element_lyrics = true;
    // Which visual fills the lyrics panel whenever there is nothing to
    // show there (no synced lyrics found / fetch still running / lyrics
    // engine off): 0 = the audio-reactive sphere, 1 = the oscilloscope.
    // config.txt: LyricViz=sphere|osci. This replaces the old
    // element_lyrics_placeholder_ball on/off switch ("Lyric Ball") --
    // the panel now always shows one of the two and this row picks
    // which, from the ANIMA/VIZ tab's "Lyrics/Viz" entry.
    int lyric_viz = 1;                  // default: the oscilloscope (0 = sphere, 1 = oscilloscope, 2 = spectrogram)
    // What the lyrics area shows while the LYRICS view is selected but there are no lyrics to draw (nothing found /
    // fetch still running): 0 = sphere, 1 = oscilloscope, 2 = spectrogram. Before this existed the area showed
    // lyric_viz, i.e. whatever view the "." cycle had been on before the lyrics. config.txt: NoLyricsViz=sphere|osci|spectro.
    // 3 = nothing (an empty field). Picking a visual that is switched off turns its "Use ..." switch on; switching a "Use ..."
    // off moves this pick to the next visual that is on (or to "none"): enforce_visual_switches().
    int nolyrics_viz = 1;
    // Settings -> ANIMA/VIZ "Use Lyrics / Use Oscilloscope / Use Spectrogram": false leaves that one out entirely (no
    // lyrics fetching, not in the "." cycle; the sphere is always there). The windows (SHIFT+8 / SHIFT+9) work anyway.
    bool use_lyrics = true;
    bool use_osci = true;
    bool use_spectro = true;
    // "Use Sphere": false leaves the sphere out of the "." cycle as well. config.txt: UseSphere=.
    bool use_sphere = true;
    // The order of the views in the "." cycle (ANIMA/VIZ tab: "Viz Order 1..4"): a permutation of
    // 0 = sphere, 1 = oscilloscope, 2 = spectrogram, 3 = lyrics. config.txt: VizOrder=sphere,osci,spectro,lyrics.
    int viz_order[5] = {0, 1, 2, 3, 4};   // (4 = video, "Viz Order 5"; config.txt: VizOrder=sphere,osci,spectro,lyrics,video)
    // The view the field starts with when Mousiki opens (ANIMA/VIZ tab: "Lyrics/Viz Default"): 0 = sphere, 1 = oscilloscope,
    // 2 = spectrogram, 3 = lyrics. The "." key changes the view live, but that is not remembered: the next start begins with
    // this one again; 4 = "last used": it starts with the view it was left on. config.txt: VizDefault=sphere|osci|spectro|lyrics|last.
    int viz_default = 1;   // (5 = video, see video_mode)
    // Video mode (HKeyVideo, "&" = SHIFT+6 on a German keyboard): video files are listed and the "Video" view joins the "." cycle right
    // after the lyrics (and the Lyrics/Viz Default list). The field then shows the picture of the track's video with the current lyric
    // line under it. video_view = the field is on that view right now (it also sets element_lyrics: the lyrics are fetched).
    // config.txt: VideoMode=, VideoView=.
    bool video_mode = false;
    bool video_view = false;
    // The & overlay (HKeyVideo): "Include Lyrics" = the current lyric line is shown under the picture; "Cover or Video Snapshot": 0 = the
    // disk shows the track's cover as always, 1 = a frame (a thumbnail) of the track's video, with the cover / the disk when the track has no
    // video. config.txt: VideoLyrics=, VideoCover=cover|snapshot.
    int video_lyrics = 1;             // Include Lyrics (VideoLyrics=): 0 none, 1 lyrics, 2 lyrics + translation, 3 translation only (the lines under the picture)
    bool video_auto = true;           // Auto Video: a track that has a video switches the field to the video view, one without it to the Lyrics/Viz Default (VideoAuto=)
    int video_size = 0;               // Picture Size of the small field (VideoSize=): 0 regular, 1 cropped (two columns cut off each side), 2 full field (fills the oscilloscope's space, cropped to fit), 3 framed (like full, one row and one column smaller on every side)
    int video_fx = 0;                 // Effect (VideoFx=): 0 off, 1 scanlines, 2 crt, 3 vhs, 4 glitch, 5 pixel, 6 gradient, 7 quiet glitch
    int video_brightness = 50;        // Video Brightness (VideoBrightness=): 0..100 %, steps of 5 (50 = as it is, lower darker, higher brighter)
    int video_contrast = 0;           // Video Contrast (VideoContrast=): 0..100 %, steps of 5 (0 = as it is)
    int video_colors = 100;           // Video Colors (VideoColors=): 0..100 %, steps of 5 (0 = greyscale, 100 = the video's own colours)
    bool video_mp3 = true;            // yt-dlp mp3 from mp4 (VideoMp3=): a video downloaded in SEARCH YT without a target song also gets its sound as an mp3 song
    int video_clap = 1;               // Show Clap Loading (VideoClap=): what shows while a video loads: 0 text note ("video: starting ..."), 1 clap (a small clapperboard), 2 none
    bool video_round = false;         // Rounded Corners of the video picture (VideoRound=): the picture's corners are cut round, like the corners of the UI
    int video_chroma = 0;             // Video Chromatic Aberration (VideoChroma=): 0 off, 1 subtle, 2 medium, 3 strong
    int cover_chroma = 0;             // Cover Chromatic Aberration (CoverChroma=): the same, for covers that show their own colours
    int video_res = 0;                // Video Resolution (VideoResolution=): 0 standard, 1 high, 2 full (see video_fx.h)
    int video_buffer = 32;            // Video Buffer (VideoBuffer=): MB of decoded frames the decoder keeps ahead of the picture
    // The layout of the full screen video (SHIFT+UP / SHIFT+DOWN there): 0 video, 1 video + oscilloscope, 2 video + oscilloscope + spectrogram
    // (spectrogram at the bottom). The picture styles and versions of the two visuals are the layout's own (-1 = as in the oscilloscope /
    // spectrogram options); every other option is shared with them.
    int video_layout = 0;                  // full screen video: 0 video | 1 + oscilloscope | 2 + spectrogram | 3 + both
    int vl_osci_style = -1;           // VideoLayoutOsciStyle=: -1 as in the options, 0 braille, 1 image
    int vl_osci_mode = -1;            // VideoLayoutOsciMode=: -1 as in the options, 0 XY, 1 YT
    int vl_spec_style = -1;           // VideoLayoutSpectroStyle=
    int vl_spec_view = -1;            // VideoLayoutSpectroView=: -1 as in the options, 0 spectrogram, 1 waterfall
    bool vl_scales = true;            // VideoLayoutScales=: grid / scales of the oscilloscope and the spectrogram in the layouts
    int video_cover = 0;              // Disk/Cover/Thumbnail (VideoCover=): 0 cover, 1 thumbnail (a frame of the video, else the cover), 2 disk (plain disk for a track with a video), 3 thumbnail fallback (the cover, a frame of the video only when there is no cover)
    // Where downloaded videos go (Search YT): <download folder>/video_down; and the quality cap (height in pixels) of such a download.
    int video_down_height = 720;   // config.txt: VideoDownHeight=
    // "When No Video Shows" (ANIMA/VIZ tab): what the field shows on the video view while the track has no video: 0 sphere, 1 oscilloscope,
    // 2 spectrogram, 3 nothing, 4 the lyrics (the default). config.txt: NoVideoViz=lyrics|sphere|osci|spectro|none.
    int novideo_viz = 4;
    // The field shows lyrics (and they are fetched) right now: field on, lyrics allowed, lyrics view selected.
    bool lyrics_on() const { return element_vizfield && use_lyrics && element_lyrics; }
    // Is that view (0..3) switched on at all?
    bool viz_usable(int v) const { return v == 0 ? use_sphere : v == 1 ? use_osci : v == 2 ? use_spectro : v == 3 ? use_lyrics : video_mode; }
    // The next / previous switched-on view after `cur` in viz_order (dir > 0 forward). `only_visuals` leaves the lyrics
    // (3) out. The video view (4) is never returned: it is not a default. Returns cur when there is no other one.
    int viz_step(int cur, int dir, bool only_visuals) const {
        int pos = 0;
        for (int i = 0; i < 5; ++i) if (viz_order[i] == cur) pos = i;
        for (int k = 1; k <= 5; ++k) {
            const int c = viz_order[((pos + (dir > 0 ? k : -k)) % 5 + 5) % 5];
            if (c == cur) break;
            if (viz_usable(c) && c != 4 && !(only_visuals && c == 3)) return c;
        }
        return cur;
    }
    // The oscilloscope / spectrogram settings (SHIFT+o / SHIFT+i) are the VizPrefs base (src/viz_prefs.h), shared in structure with the radio.
    int lyrics_alignment = 0; // 0=center (default), 1=left, 2=right
    int lyrics_animation = 0; // 0=full (default), 1=word by word, 2=letter by letter, 3=only active line, 4=only active word
    bool element_visualizer = true;

    // --- what a track row in the (search-)lists shows (config.txt: MetaDataOnly) ---
    // false = filename stem plus the metadata columns (the long-standing
    //         behavior: filename AND meta data).
    // true  = metadata only: the embedded title tag is shown instead of
    //         the filename wherever a real tag is known (files without a
    //         title tag still fall back to their filename, so an untagged
    //         library doesn't turn into a blank list).
    // Toggled from the GENERAL tab ("Show meta data only") and with the
    // HKeyToggleMetaOnly hotkey (Shift+N by default).
    bool meta_only = false;
    // Marquee scrolling of too-long names (list row under the cursor, Name/Artist/Album of the metadata block, playlist
    // editor lists). false: cut with "..." instead. GENERAL tab "Marquee Scrolling".
    bool marquee = true;

    // --- appearance / tuning -------------------------------------------
    int visualizer_fluidity = 10;       // 1-10, higher = smoother/slower rise
    int visualizer_degradation_speed = 8; // 1 (slow fade) - 10 (near-instant), how fast bars fall after a drop
    int visualizer_viscosity = 3;       // 0 (bars move independently) - 10 (heavy neighbor blending)
    // the Hi-Fi spectrum bars (hifi_bars.h): a style of the bars under the meta data, with three rows of its own
    int hifi_fluidity = 10;             // the Hi-Fi style keeps its own three bar values (visualizer_* above are the Classic ones)
    int hifi_degradation = 8;
    int hifi_viscosity = 3;
    int bars_style = 1;                 // 0 = classic, 1 = Hi-Fi (a braille column per frequency step, stereo)
    int hifi_resolution = 0;            // 0 = fine (a band per dot column), 1 = rough (a band per two dot columns)
    int hifi_channels = 0;              // 0 = stereo, 1 = mono
    int hifi_fscale = 2;                // Hi-Fi frequency scale: 0 linear, 1 logarithmic, 2 mel, 3 bark, 4 erb
    int hifi_gain = -2;                 // Hi-Fi gain in dB (-20 .. 20)
    int hifi_range = 68;                // Hi-Fi range in dB from the bottom to the top (40 .. 100)
    // The three bar values of the style in use (Classic: visualizer_*, Hi-Fi: hifi_*)
    int& bar_fluidity() { return bars_style == 1 ? hifi_fluidity : visualizer_fluidity; }
    int& bar_degradation() { return bars_style == 1 ? hifi_degradation : visualizer_degradation_speed; }
    int& bar_viscosity() { return bars_style == 1 ? hifi_viscosity : visualizer_viscosity; }
    int bar_fluidity() const { return bars_style == 1 ? hifi_fluidity : visualizer_fluidity; }
    int bar_degradation() const { return bars_style == 1 ? hifi_degradation : visualizer_degradation_speed; }
    int bar_viscosity() const { return bars_style == 1 ? hifi_viscosity : visualizer_viscosity; }
    int hifi_adapt = 1;                 // Hi-Fi: the height adapts to the song (dense music is lowered more)
    int hifi_split = 1;                 // Hi-Fi: an empty dot column between the two channels
    int hifi_scale = 2;                 // Hi-Fi height: 0 full, 1 medium, 2 low, 3 very low
    int hifi_color = 0;                 // 0 = gradient (left to right, the bar colours), 1 = corresponding (the spectrogram's colours by height)
    bool colors_palette = false;        // config.txt says ColorCodes=palette (colour numbers are palette numbers); false = an older file, converted on load
    std::string disk_color = "170";
    std::string disk_color_end = "30"; // empty = solid disk_color, no gradient
    bool disk_color_gradient = true;    // true = use disk_color_end as a gradient stop, false = solid/"generic" disk_color only
    double disk_rotation_speed = 0.1;   // multiplier on the reference angular velocity, range 0.01x-1.00x

    // --- border / separator characters (from config.txt) ---------------
    std::string box_upper_left = "\u256d";   // ╭
    std::string box_upper_right = "\u256e";  // ╮
    std::string box_lower_left = "\u2570";   // ╰
    std::string box_lower_right = "\u256f";  // ╯
    std::string box_vertical = "\u2502";     // │
    std::string box_horizontal = "\u2500";   // ─
    bool disk_border = false;                // the ":  :" column between the disk and the meta data (false: the disk is centred in the space instead)
    std::string meta_separator = ":  :";     // separator between disk|meta|lyrics columns
    std::string list_separator = "|";        // separator in list panel between columns

    // --- font map (config.txt font_en block) ---------------------------
    // Maps ASCII letter → {uppercase_display, lowercase_display}.
    // Empty map = use default ASCII glyphs.
    std::unordered_map<char, std::pair<std::string, std::string>> font_map;

    // --- colors --------------------------------------------------------
    // Defaults here match the "g_defaults" fallback map from the redesigned
    // config: config.txt intentionally ships every color field blank
    // except these seven, which fall back to a real color even when the
    // config value is empty. Everything else genuinely defaults to "no
    // color" (plain terminal default) until the user sets it.
    std::string border_color = "170";
    std::string border_color_bottom = "104";   // top uses border_color; box_top()/box_bottom() use these two independently
    bool border_smooth = true;                // BorderGradient=smooth|rough: smooth fades the frame line by line from top to bottom colour; rough = only the bottom border line takes the bottom colour

    std::string active_line_color = "182";
    std::string active_line_bg_color;
    std::string active_word_color = "75";
    std::string active_word_bg_color;
    std::string inactive_line_color = "75";
    std::string inactive_line_bg_color;

    std::string progress_remaining_color = "182";
    std::string progress_played_color = "213";
    std::string progress_timestamp_color;    // empty = falls back to border_color

    // Search bar input (the "/" box, the meta editor's and the playlist
    // manager's "Search:" rows). Empty = follow LIST INACTIVE FG/BG, which is
    // what these bars always looked like; "0" = explicit terminal default.
    std::string search_text_color = "231";           // typed query: FG / BG, drawn behind the query text only
    std::string search_bg_color = "199";
    std::string search_inactive_text_color = "75";  // the rest of the bar (prefix, placeholder, empty field)
    std::string search_inactive_bg_color;

    std::string list_color = "75";
    std::string list_inactive_bg_color;
    std::string list_playing_color = "16";
    std::string list_cursor_color = "227";

    std::string queue_color = "75";
    std::string queue_inactive_bg_color;
    std::string queue_playing_color = "16";
    std::string queue_playing_bg_color = "182";
    std::string queue_cursor_color = "75";     // the "hovering" row once you Tab into the queue
    std::string queue_cursor_bg_color = "240";     // "0" = no background; default matches list_cursor_bg_color so the hovering row is always visible

    // Row background colors. Value "0" (or empty) = no background color.
    std::string list_playing_bg_color = "182";
    std::string list_cursor_bg_color = "238"; // one of the seven config-level defaults (ColorListCursorBg)

    std::string button_color;

    // Section headers drawn by the Settings panel itself: the category
    // titles on the REFERENCE tab and the "LOCAL PATH" / "DOWNLOAD PATH" /
    // "PLAYLIST PATH" titles on the PATHS tab. Default "10" -- palette index 10 of 256,
    // exactly the color the Reference tab headers have always been drawn
    // in -- changeable from the Colors tab's HEADER row, or ColorHeader=
    // in config.txt. "0"/empty = plain bold, no color.
    std::string header_color = "227";

    // Key command legends: the grey "[ESC] close | [ENTER] confirm" hint lines
    // of the Settings panel, the big list / queue overlays, the playlist
    // editor, the meta editor and the listening history. Default "8" --
    // palette colour 8 (bright black), the grey they have always been
    // drawn in -- changeable from the Colors tab's LEGEND row, or ColorLegend=
    // in config.txt. "0"/empty = the terminal's own text color.
    std::string legend_color = "199";
    // The descriptions behind the keys of those legends ("[ESC] Close": the keys use legend_color, "Close" this one);
    // the Colors tab's LEGEND row, second cell (COMMAND), or ColorLegendCommand=.
    std::string legend_cmd_color = "213";
    std::string tab_current_color = "199";   // settings / playlist / history tab strips: the current tab "[NAME]" and the "< \u2194 >" hint
    std::string tab_other_color = "227";     // ... the other tab names

    // "This field is actively being typed into" -- every in-place text edit
    // across the Settings panel (a PATHS row, a hex colour value, a hotkey)
    // and the meta editor's tag fields. Defaults "7"/"1" -- palette
    // 7 (white text) / 1 (red background), the white-on-red this
    // was always hardcoded as before it had a name of its own -- changeable
    // from the Colors tab's CHANGE row, or ColorChangeFg=/ColorChangeBg= in
    // config.txt. "0"/empty = the terminal's own text/background.
    std::string change_text_color = "231";
    std::string change_bg_color = "199";

    // The pane focus indicator "\u25c0": the little triangle after a pane's
    // title that marks which pane has the keys (main screen: the search box,
    // the file list or the queue; also the meta editor and the playlist
    // editor). Colors tab FOCUS row, or ColorFocus= in config.txt. Default
    // "10" = the default header color; a config.txt without the key follows
    // whatever ColorHeader is, and every theme sets it to its header color.
    std::string focus_color = "183";

    std::string visualizer_color = "170";
    std::string visualizer_color_end = "30";

    // New colors from config.txt
    std::string meta_key_color = "140";          // label text ("Name", "Artist") — empty = inherit list_color
    std::string meta_val_color = "182";          // value text — empty = inherit list_color
    std::string status_color;            // status line — empty = default
    std::string pl_active_bg_color;      // playlist active row background — empty = none

    // Multi-stop visualizer gradient (config.txt viz_left/center/right)
    std::string viz_left_color;          // treble edge color
    std::string viz_center_color;        // bass center color
    std::string viz_right_color;         // treble edge color (right side)

    // Lyrics-specific (config.txt lyr_*)
    std::string lyr_inactive_color;      // empty = falls back to inactive_line_color
    std::string lyr_active_line_color;   // empty = falls back to active_line_color
    std::string lyr_active_word_color;   // empty = falls back to active_word_color

    // --- visualizer tuning ---------------------------------------------
    int viz_style = 0;
    int viz_bands = 32;
    int viz_density = 1;

    // --- playback ------------------------------------------------------
    bool waveform_smooth = true;
    // Which mode Mousiki starts in: "player" (the music player), "radio" or "audiobook". Settings -> GENERAL -> MODES -> Start
    // Mode (the radio's settings have the same row). config.txt: StartMode=player|radio|audiobook. main() reads it with
    // read_start_mode().
    std::string start_mode = "player";
    // The three modes (0 = music player, 1 = radio, 2 = audio book player), which of them are switched on and the order the
    // SHIFT and + key (forward) / SHIFT and - key (back) walks through them. One mode is always on (mode_normalize()).
    // config.txt: UseModePlayer= / UseModeRadio= / UseModeAudiobook=, ModeOrder=player,radio,audiobook. Both programs share them.
    bool use_mode[3] = {true, true, true};
    int mode_order[3] = {0, 1, 2};
    // The folder of the audio books (Settings -> PATHS -> AUDIO BOOK PATH, config.txt: AudioBookPath=). Every folder below it
    // that holds audio files is one book; Mousiki keeps a .cue file beside each book that records where you stopped listening.
    std::string audiobook_path;
    bool mode_usable(int m) const { return m >= 0 && m < 3 && use_mode[m]; }
    // The next switched-on mode after `cur` in mode_order (dir > 0 forward, < 0 back); `cur` itself when no other is on.
    int next_mode(int cur, int dir) const {
        int pos = 0;
        for (int i = 0; i < 3; ++i) if (mode_order[i] == cur) pos = i;
        for (int k = 1; k <= 3; ++k) {
            const int c = mode_order[((pos + (dir >= 0 ? k : -k)) % 3 + 3) % 3];
            if (c != cur && use_mode[c]) return c;
        }
        return cur;
    }
    // Makes the three values consistent: a permutation for the order, at least one mode on, a start mode that is on.
    void mode_normalize();
    // Black Background (GENERAL -> TERMINAL): the terminal's background is black while Mousiki runs (OSC 11, restored on exit).
    // One value for the player and the radio, in config.txt (BlackBackground=true|false); the radio reads / writes that line.
    bool black_background = true;
    // Settings -> ANIMA/VIZ -> OSCI/SPECTRO SETTINGS \"Osci/Spectro Share Profile\": the radio uses the player's oscilloscope / spectrogram settings (the VizPrefs in
    // this config.txt) instead of its own; its own values wait in radio_config.txt and come back when this is turned off.
    // One value for both programs (config.txt: ShareProfiles=true|false); the radio reads / writes that line. Colours are never shared.
    bool share_profiles = false;
    // Audio book mode: leaving a chapter, the mode or the program asks whether to set a bookmark (config.txt: BookmarkAsk=true|false).
    bool book_ask_bookmark = true;
    // Audio book mode: false = the picture of the woman reading in the disk's place, true = the cover image found in the book's folder
    // (the standard picture when the book has none); config.txt: BookCover=true|false. Where the terminal speaks neither Kitty graphics
    // nor Sixel the standard picture is drawn as braille dots instead.
    bool book_cover = false;
    // With Book Cover on: true = the colour wave runs over the cover (grey), false = the cover in its own colours
    // (config.txt: BookCoverWave=true|false). The standard picture always has the wave.
    bool book_cover_wave = true;
    // Audio book mode: the playback speed, 0.5 - 3.0 in steps of 0.05, the pitch stays (Settings -> GENERAL -> AUDIO "Playback Speed",
    // SHIFT+LEFT / SHIFT+RIGHT in the audio book player; config.txt: BookSpeed=1.25). The music player and the radio always play at 1.0.
    double book_speed = 1.0;
    // The sound output for all modes (Settings -> GENERAL -> AUDIO "Choose Audio Device"; config.txt AudioDevice=<name as the system
    // names it>; empty = the system's default device). A device that is not there plays on the default one.
    std::string audio_device;
    // The terminal's font in Mousiki's window (terminal_font.h): normally Cascadia Mono. Settings -> GENERAL -> TEXT AND TRANSLITERATION
    // -> "Terminal Default Font" (TerminalDefaultFont=): true = Mousiki leaves the terminal's own font alone. TerminalFont= (config.txt
    // only, no settings row) names another font than Cascadia Mono.
    bool terminal_default_font = false;
    std::string terminal_font = "Cascadia Mono";
    static constexpr double kBookSpeedStep = 0.05;
    // A quiet check for a newer release at start-up (once a day, shown once per version); config.txt: UpdateCheck=true|false.
    bool update_check = true;
    // Decoded audio above this many MB is not held whole in RAM but in a moving 2-minute window (see streaming_pcm.h);
    // config.txt: TrackBufferMB. Default 256.
    static constexpr int kTrackBufferDefaultMB = 256, kTrackBufferMinMB = 64, kTrackBufferMaxMB = 16384;
    int track_buffer_mb = kTrackBufferDefaultMB;
    // The terminal is resized to this many columns x rows when Mousiki starts (and when it is changed in the settings); 0 in either:
    // the terminal is left as it is. FullScreen asks the terminal for full screen and overrides the size. Terminals decide whether
    // they obey (see terminal_ui.h); config.txt: TerminalCols, TerminalRows, FullScreen.
    static constexpr int kTermColsDefault = 120, kTermRowsDefault = 32;
    int terminal_cols = kTermColsDefault;
    int terminal_rows = kTermRowsDefault;
    bool full_screen = false;
    bool reduce_motion_paused = true;   // ReduceMotionWhenPaused=: paused (or nothing playing) and untouched for a few seconds, the screen is redrawn at a low rate (POSIX terminals)
    // true: Mousiki leaves the terminal alone whatever Terminal Size / Full Screen say (they stay as they are); UseTerminalSettings.
    bool use_terminal_settings = false;
    // The desktop's media controls (Linux: MPRIS; macOS: Now Playing + media keys); config.txt: MediaKeys=true|false.
    bool media_keys = true;
    // true (the default): a track above 96 kHz is brought down to 96 kHz; false: it plays at its own rate (up to 768 kHz). The next track
    // that is loaded uses it. config.txt: RateCap96=true|false.
    bool rate_cap_96 = true;
    // The remote app (phone): a small web server inside Mousiki, off by default. One value for both programs (config.txt:
    // RemoteApp=, RemoteAppPort=, RemoteAppPin=); see remote_server.h.
    bool remote_app = false;
    int remote_port = 4747;
    std::string remote_pin;      // made (6 random digits) the first time the remote app is switched on
    int play_mode = 0; // 0=list 1=loop(repeat) 2=shuffle 3=stop 4=queue then stop (play the queue through once, then stop)

    // --- console logging (config.txt: ConsoleVerbosity) -----------------
    // "basic" = every external command mousiki ran (yt-dlp/ffprobe/
    // ffmpeg/lyrics-fetch) + raw output. "verbose" = that, plus internal/
    // OS-level events (resizes, audio device init, spawn failures, ...).
    int console_verbosity = 0; // 0=basic, 1=verbose

    // --- emoji in titles (config.txt: ReplaceEmoji) ----------------------
    // 2 (true, default) = every emoji is drawn as a single "?" so box borders can never
    //         be shifted by a terminal that measures emoji differently.
    // 1 (safe) = plain one-codepoint emoji (note, radio, book ...) are kept; the risky ones (joined
    //         sequences like the black cat, flags, skin tones, VS16 forms, keycaps) become one "?".
    // 0 (false) = draw real emoji (alignment then depends on the terminal).
    int emoji_mode = 2;

    // --- stereo playback (config.txt: StereoPlayback) -----------------------
    // true  = tracks are decoded and played in stereo (2x the memory per
    //         loaded track: about 106 MB for a 5-minute track instead of 53 MB).
    // false = mono, as before: left and right are folded together.
    bool stereo = true;

    // --- loudness normalisation (config.txt: NormalizeVolume & co.) --------
    // Every track is measured in LUFS while it decodes and played back with
    // a gain that brings it to normalize_target_lufs, so quiet and
    // heavily-compressed recordings sit at the same perceived level.
    bool   normalize = true;
    double normalize_target_lufs = -16.0;   // -14 = YouTube/Spotify reference, -16 = a bit more headroom
    double normalize_max_boost_db = 9.0;    // never amplify a quiet track by more than this

    // --- equaliser (config.txt: EqualizerEnabled / EqualizerBands / EqualizerPreset) ---
    // Ten band gains in dB (31 Hz .. 16 kHz, see equalizer.h), edited in the
    // Shift+E overlay. Which preset is "selected" is not stored: it is derived
    // by matching the gains against the preset table and the custom presets
    // below (no match = Custom).
    bool   eq_enabled = false;
    EqGains eq_gains = {};
    // The user's own presets (S in the overlay), one "EqualizerPreset=" line
    // each. They come after the built-in presets in the preset cycle.
    std::vector<EqCustomPreset> eq_custom_presets;

    // Sleep timer: glide the volume down over the last 10 % of the time before pausing (Shift+Z overlay).
    bool menu_index_classic = false;   // the "<" menus overlay shows the classic category boxes instead of the tree (MenuIndexClassic=, SHIFT+t there)
    bool sleep_fade = true;
    // Help wizard (SHIFT+w): false means it has never been shown, so the
    // first launch after an install opens it by itself. It is set to true and
    // written to config.txt the moment the wizard is first closed, so it only
    // ever greets a user once -- and deleting the line (or the whole
    // config.txt) brings the welcome back.
    bool wizard_seen = false;
    // Karaoke overlay (k): size of the lyrics, 1 = normal text, 2..5 = drawn
    // with the built-in pixel font in braille, each step one font pixel
    // bigger. Changed in the overlay with SHIFT and + / SHIFT and -.
    int karaoke_lyrics_size = 1;
    // READ LYRICS (CTRL+L) translation, chosen in its translate overlay ([t]): the service (google | mymemory), the
    // language to translate INTO and the language of the lyrics ("auto" = the service detects it).
    std::string translate_service = "google";
    std::string translate_to = "en";
    std::string translate_from = "auto";
    // SID tunes (C64) are rendered by sidplayfp for this many seconds (config.txt only: SidPlayLength=, 10..1800).
    int sid_play_length = 180;

    // --- autosave / session snapshot (config.txt: AutoSave*) -----------
    bool autosave_enabled = true;
    bool autosave_indicator = true;
    int autosave_delay_sec = 30;
    std::string autosave_chr = "\u2022"; // single glyph -- only the first UTF-8 character of the config value is kept
    int autosave_indicator_type = 0;     // 0=blink (appear/disappear), 1=color (heartbeat C1->C2->C1)
    std::string autosave_c1;             // empty = falls back to border_color
    std::string autosave_c2;             // empty = falls back to visualizer_color

    std::string theme_name = "default";
    // The audio book mode keeps its own look (colours, theme, elements, visualizations, borders: copy_look() below) in
    // audiobook_config.txt; this flag (set by main(), never written to a file) makes save_settings() route a save that way.
    bool book_profile = false;

    // --- local music library paths -------------------------------------
    // Each entry is an absolute path scanned for audio files.
    // Configured via one or more LocalMusicPath= lines in config.txt.
    // Example:
    //   LocalMusicPath=/home/user/Music
    //   LocalMusicPath=/mnt/nas/albums
    // When empty the scanner falls back to the built-in defaults
    // (~/Music and ~/disk/Music).
    std::vector<std::string> local_music_paths;

    // --- download folder ------------------------------------------------
    // Where yt-dlp writes what it downloads (one folder -- there is only
    // ever one "where downloads go"). Empty (the default) means the
    // built-in cache folder, $HOME/.cache/mousiki, which is where every
    // download landed before this setting existed; it is NOT written into
    // config.txt until it is actually changed, so an untouched install
    // keeps the same config.txt byte for byte. Whichever folder it resolves
    // to is also injected into local_music_paths (App::load_library() /
    // rescan_library()), so downloads show up in the library without a
    // second LocalMusicPath line. Editable from the PATHS tab's DOWNLOAD
    // FOLDER row.
    std::string download_folder;

    // Folder that holds the listening history (history.json). Empty = ~/.cache/mousiki/history. ONE folder
    // (Settings -> PATHS -> HISTORY PATH, config.txt: HistoryPath=).
    std::string history_path;

    // Folder the playlist editor exports M3U8 / M3U files to ('e' on the
    // Saved Playlists tab). Empty = the playlist folder itself. ONE folder
    // (Settings -> PATHS -> PLAYLIST EXPORT PATH, config.txt: PlaylistExportPath=).
    std::string playlist_export_path;
    // Folder 'i' on the Saved Playlists tab reads M3U8 / M3U files from (Settings -> PATHS -> PLAYLIST IMPORT PATH, PlaylistImportPath=).
    // Empty = the "import" folder inside the playlist folder. The files stay; a copy of each list goes to imported_lists.
    std::string playlist_import_path;

    // Folder the file conversion overlay (SHIFT+c) writes its converted files to. Empty = "conversion-export"
    // inside the download folder. ONE folder (Settings -> PATHS -> CONVERSION EXPORT PATH, config.txt:
    // ConversionExportPath=).
    std::string conversion_export_path;

    // --- playlists folder -----------------------------------------------
    // Where saved playlists (App::playlists_dir()) live. Empty (the
    // default) means "use local_music_paths[0]/playlists", which falls
    // back further to the cache folder if no LocalMusicPath is
    // configured at all. Set PlaylistsPath= in config.txt -- one line per
    // folder -- to pin it somewhere specific regardless of
    // local_music_paths. Several folders may be listed (each additional
    // PlaylistsPath= line adds one): playlists are *listed* and *loaded*
    // from all of them, while saving/deleting always uses the first one,
    // which is also the one App::playlists_dir() returns. Editable from
    // the PATHS tab's PLAYLIST PATH list.
    std::vector<std::string> playlists_paths;

    // --- hotkey mapping ------------------------------------------------
    // Action name → key string (e.g. "ARROW_KEY_UP", "s", "ENTER")
    std::unordered_map<std::string, std::string> hotkeys;

    // Convenience: resolve a hotkey action to the configured key code.
    // Returns the default if not mapped. Key codes returned match what
    // TerminalIO::poll_key() produces (single chars) or special names.
    std::string hotkey(const std::string& action) const {
        auto it = hotkeys.find(action);
        return it != hotkeys.end() ? it->second : "";
    }
};

const char* play_mode_name(int mode);

// Migrates a config value from the old named-preset color scheme
// ("cyan", "white", ...) to the new bare-number scheme.
std::string normalize_color_value(const std::string& value);

// ANSI SGR escape for a color value. The value must be a bare decimal
// string "0".."255" (a 256-color palette index); "0" or empty means no
// color at all (returns ""). Anything unparsable is also treated as "no
// color" rather than erroring.
std::string ansi_for(const std::string& color_name, bool bold = true);

// Background-color counterpart to ansi_for(). Same "0"/empty = no
// background rule.
std::string bg_ansi_for(const std::string& color_name);

// Just the numeric SGR parameters for a color value ("38;5;208"), with
// no leading \x1b[ or trailing m. Empty string if the value is "0"/none.
std::string sgr_params_for(const std::string& color_name);

// Rough brightness 0-100 for display next to a color in the settings panel.
int brightness_percent(const std::string& color_value);

// Best-effort RGB extraction for gradient interpolation.
bool try_parse_rgb(const std::string& color_value, int& r, int& g, int& b);

// Linearly interpolates between `start` and `end` at t in [0,1].
std::string gradient_ansi(const std::string& start, const std::string& end, float t, bool bold = true);

// Multi-stop gradient: interpolates across left → center → right using
// position t in [0,1]. Falls back to gradient_ansi(start, end, t) if
// center is empty.
std::string multi_stop_gradient_ansi(const std::string& left, const std::string& center,
                                      const std::string& right, float t);

// A horizontal strip of filled block characters, smoothly interpolated
// between start and end -- used for settings-panel preview swatches.
std::string gradient_preview_bar(const std::string& start, const std::string& end, int width);

// Parse "ANSI_index,brightness" format from config.txt (e.g. "39,100")
// into an SGR color value usable by ansi_for(). Returns the input
// unchanged if it doesn't match the format.
std::string parse_ansi256_brightness(const std::string& value);

// Apply a font map to a string: each ASCII letter is replaced with its
// mapped glyph (uppercase or lowercase variant as appropriate).
std::string apply_font_map(const std::string& text,
                            const std::unordered_map<char, std::pair<std::string, std::string>>& font_map);

// Prebaked themes.
const std::vector<std::string>& theme_names();
void apply_theme(Settings& s, const std::string& theme_name);

// Default hotkey bindings — used when config.txt doesn't specify them.
void apply_default_hotkeys(Settings& s);
bool shares_shift_b(const std::string& a, const std::string& b);   // commands that may hold the same key (see settings.cpp)

// Config file path and I/O. Uses config.txt format (: and == separators).
// Falls back to reading legacy settings.txt if config.txt doesn't exist.
fs::path config_path();
// The StartMode= line of config.txt ("player" or "radio"; "player" when there is none). Read by main() before anything
// starts, and by the radio, whose Start Mode row is the same setting.
std::string read_start_mode();   // "player", "radio" or "audiobook" -- already corrected to a mode that is switched on
// The modes: names as written in config.txt, and the shared switches (UseMode*= and ModeOrder= lines of config.txt).
inline const char* const kModeNames[3] = {"player", "radio", "audiobook"};
int mode_index(const std::string& name);                      // 0..2, or -1 for an unknown name
struct ModeConfig { bool use[3] = {true, true, true}; int order[3] = {0, 1, 2}; int key_next = '*', key_prev = '_'; };   // (the keys: 0 = none)
// Are `a` and `b` the same folder (written differently, trailing slash, "." parts; not case sensitive on Windows)? Empty never matches.
bool same_folder_path(const std::string& a, const std::string& b);
// Is `book` (the AUDIO BOOK PATH) one of the music folders? The two must not be the same folder: a book folder INSIDE a music folder
// is fine (the music player leaves it out), the very same folder is a conflict.
bool book_path_conflicts(const std::vector<std::string>& music_paths, const std::string& book);
std::string read_audiobook_path();                           // AudioBookPath= of config.txt ("" when missing; the radio's PATHS tab edits it)
ModeConfig read_mode_config();                                // from config.txt (defaults when missing), normalised
std::string mode_order_text(const int order[3]);              // "player,radio,audiobook"
int next_mode_in(const ModeConfig& mc, int cur, int dir);     // like Settings::next_mode
// The BlackBackground= line of config.txt (true when there is none); the radio may have changed it meanwhile.
bool read_black_background();
// The ShareProfiles= line of config.txt (false when there is none).
bool read_share_profiles();
// The UpdateCheck= line of config.txt (true when there is none).
bool read_update_check();
// The MediaKeys= line of config.txt (true when there is none).
bool read_media_keys();
// RemoteApp= / RemoteAppPort= / RemoteAppPin= of config.txt (defaults when missing), and writing them (one line each,
// nothing else in the file changes). write_remote_app makes a PIN when it is switched on without one.
#ifndef MOUSIKI_REMOTE_APP_CONFIG
#define MOUSIKI_REMOTE_APP_CONFIG
struct RemoteAppConfig { bool enabled = false; int port = 4747; std::string pin; };
#endif
RemoteAppConfig read_remote_app();
bool write_remote_app(RemoteAppConfig& rc);
// Share Profiles, the radio's side: the player's oscilloscope / spectrogram settings as they are in config.txt, and writing the
// radio's changes back into config.txt (the rest of the file is kept). False when there is no config.txt (the player owns it).
bool read_player_viz(VizPrefs& out);
bool write_player_viz(const VizPrefs& v);
// Writes that one line into config.txt and leaves the rest of the file alone (the radio's settings screen uses it).
bool write_start_mode(const std::string& mode);
// The radio's Latin letters switch (the quote key): LatinLetters= in config.txt, the rest of the file is kept.
bool write_latin_letters(bool on);
Settings load_settings();
// Upgrade safety: config.txt / radio_config.txt carry "# Written by Mousiki v<version>". Before a version other than the
// one that wrote the file rewrites it, the old file is copied to <name>.bak (one backup, replaced at the next upgrade).
std::string config_stamp_line();
std::string config_written_by(const std::filesystem::path& p);   // "" when the file has no stamp (older than v4.2.0)
bool backup_config_if_older(const std::filesystem::path& p);     // true when a .bak was written
// Writes `text` to "<p>.tmp" and renames it over `p`: a crash or a power cut in the middle never leaves a cut-off file.
bool write_text_atomic(const std::filesystem::path& p, const std::string& text);
void save_settings(const Settings& s);
// One line of config.txt without loading / saving the whole file (the CHOOSE AUDIO DEVICE choice, which every mode writes at once,
// also from a Settings screen with other changes still unsaved): the value of `key` ("" when missing), and rewriting that line
// (appended when missing; false when config.txt cannot be written).
std::string config_value(const std::string& key);
bool set_config_value(const std::string& key, const std::string& value);
// ---- one look per mode ----
// What each mode keeps for itself ("the look"): all colours and the theme, the border characters, which elements and
// visualizations are shown (and their oscilloscope / spectrogram settings), the volume bar look, the disk, the spectrum bar
// tuning, the list display and the play mode, and the sound (loudness normalisation, equaliser, stereo / mono). Everything else (paths, hotkeys, sound, the mode switches, the remote app ...)
// belongs to all modes. The radio has its own file for its look; the audio book mode uses audiobook_config.txt.
void copy_look(const Settings& src, Settings& dst);
// Everything that is NOT the look is taken over from `from` into `to`; the look of `to` stays (main() calls it when the running mode changes).
void sync_shared(const Settings& from, Settings& to);
fs::path book_config_path();   // audiobook_config.txt next to config.txt
// The settings of the audio book mode: those of the music player, with the look read from audiobook_config.txt (when it exists).
// `wants_default_theme` (optional): set when the mode has no look of its own yet, so the caller applies its default theme
// (apply_book_default_theme, Rose Pine) before the mode starts.
Settings load_book_settings(const Settings& player, bool* wants_default_theme = nullptr);
void apply_book_default_theme(Settings& s);   // app_settings.cpp (the player's program only)
// The text save_settings() would write (used to tell whether the settings screen changed anything).
std::string settings_to_text(const Settings& s);

// ---- the SHIFT+o overlay: the shared rows are in viz_prefs.h; the player adds the "Display" row (what the lyrics area shows) ----
std::string osci_row_value(const Settings& s, int row);
void osci_adjust(Settings& s, int row, int dir);                // dir = +1 / -1

} // namespace muisc
