// Settings > GENERAL and ANIMA/VIZ: the preview box on the right of the rows. It shows a live example of the option that
// is highlighted (never several at once): the volume bar at 85 %, the lyrics, the disk or a cover, the spectrum bars, the
// progress waveform, the oscilloscope / spectrogram / sphere, the transliteration examples, the emoji, the marquee.
//
// The oscilloscope, spectrogram, sphere and bars are driven by their own instances (not the player's). Bars, oscilloscope
// and sphere are fed with the signal of the radio's "TEST Stereo Chords" station, made here in the program; the spectrogram
// with a spectrogram the Creator makes from the text "Mousiki Plus". No file, no network and no ffmpeg are needed.
#include "app.h"
#include "coverfetch_state.h"
#include "hifi_bars.h"
#include "app_internal.h"
#include "app_settings_tables.h"
#include "cover_art.h"
#include "creator.h"
#include "sample_cover_data.h"
#include "miniz.h"
#include "fft_visualizer.h"
#include "oscilloscope_visualizer.h"
#include "sphere_visualizer.h"
#include "spectrogram.h"
#include "waveform.h"
#include "translit.h"
#include "terminal_ui.h"
#include "volume_bar.h"
#include "terminal_gfx.h"
#include "video_player.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>

namespace muisc {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kPreviewX = 71;      // the column of the box's left border
constexpr int kPreviewMinW = 107;  // narrower terminals have no room: no preview

enum PvKind { kPvNone, kPvLyrics, kPvDisk, kPvCover, kPvVolume, kPvBars, kPvWave, kPvScope, kPvSpectro, kPvSphere,
              kPvTranslit, kPvEmoji, kPvMarquee, kPvVideo, kPvClap };

}  // namespace

struct SettingsPreviewState {
    std::chrono::steady_clock::time_point last{};
    bool have_last = false;
    double dt = 0.033;
    double t = 0.0;               // seconds since the preview started (the lyrics' clock, the progress, the marquee)
    double sig = 0.0;             // the chord signal's own clock, in seconds
    double angle = 0.0;           // the disk
    FftVisualizer fft;
    HifiBars hifi;   // the Hi-Fi bars' own analysis of the test signal
    OscilloscopeVisualizer osc;
    SphereVisualizer sph;
    SpectroAnalyzer spec;
    std::vector<float> env_raw, env_smooth;
    CoverImage cover;
    int translit_key = -1;
    double translit_at = -10.0;
    std::vector<std::string> translit_rows;
    std::vector<float> text_audio;   // the spectrogram and spectrum bars preview's sound: a created spectrogram that reads "Mousiki Plus" (interleaved stereo, 48 kHz)
    size_t text_pos = 0;             // in frames
    bool text_made = false;
    std::shared_ptr<VideoPlayer> vid;   // the sample video of the Use Video preview (started when that row is shown)
    bool vid_tried = false, vid_ok = false;
    std::string vid_key;
};

namespace {

// The chord test signal of the radio (aevalsrc in src_radio/radio_engine.cpp), 48 kHz stereo.
void chord_frames(double& t, size_t n, std::vector<float>& lr) {
    constexpr double rate = 48000.0;
    lr.resize(n * 2);
    for (size_t i = 0; i < n; ++i, t += 1.0 / rate) {
        const double l = 0.30 * std::sin(kTwoPi * 220.0 * t + 2.0 * std::sin(kTwoPi * 0.20 * t)) + 0.20 * std::sin(kTwoPi * 277.18 * t);
        const double r = 0.30 * std::sin(kTwoPi * 329.63 * t + 2.0 * std::sin(kTwoPi * 0.13 * t)) + 0.20 * std::sin(kTwoPi * 220.0 * t);
        lr[2 * i] = static_cast<float>(l);
        lr[2 * i + 1] = static_cast<float>(r);
    }
}

// A made-up song for the progress waveform: swells, a quiet part, a loud finish.
std::vector<float> sample_pcm() {
    const size_t n = 4096u * 48u;
    std::vector<float> pcm(n);
    unsigned long long seed = 12345;
    for (size_t i = 0; i < n; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(n);
        double amp = 0.30 + 0.45 * std::fabs(std::sin(x * kTwoPi * 3.0)) * (0.5 + 0.5 * x);
        if (x > 0.38 && x < 0.50) amp *= 0.45;
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        const double noise = (static_cast<double>((seed >> 33) & 0xFFFF) / 32768.0 - 1.0);
        pcm[i] = static_cast<float>(amp * (0.7 * std::sin(static_cast<double>(i) * 0.05) + 0.5 * noise));
    }
    return pcm;
}

// A small made-up cover (a sunset over water) for the cover looks.
void make_sample_cover(CoverImage& c) {
    // The colour version of the audio book picture (see sample_cover_data.h) as the sample cover, so every cover look (picture and
    // braille) is shown on the same familiar, good-looking image.
    const size_t n = static_cast<size_t>(kSampleCoverW) * static_cast<size_t>(kSampleCoverH);
    std::vector<unsigned char> idx(n);
    mz_ulong len = static_cast<mz_ulong>(n);
    c.ok = false;
    if (mz_uncompress(idx.data(), &len, kSampleCoverZ, static_cast<mz_ulong>(kSampleCoverZLen)) != MZ_OK || len != n) return;
    c.key = "settings-preview-sample-book-colour";
    c.w = kSampleCoverW; c.h = kSampleCoverH;
    c.level.assign(n, 1);
    c.rgb.assign(n * 3, 0);
    c.alpha.assign(n, 255);
    for (size_t i = 0; i < n; ++i) {
        const unsigned char* p = &kSampleCoverPal[static_cast<size_t>(idx[i] & 63) * 3];
        c.rgb[3 * i] = p[0]; c.rgb[3 * i + 1] = p[1]; c.rgb[3 * i + 2] = p[2];
        c.level[i] = static_cast<unsigned char>(std::clamp(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2], 1.0, 255.0));
    }
    c.ok = true;
}

PvKind kind_of(int id, const Settings& s) {
    switch (id) {
        case kSidLyrAlign: case kSidLyrAnim: case kSidUseLyrics: return kPvLyrics;
        case kSidUseVideo: case kSidVideoLyrics: case kSidVideoSize: case kSidVideoFx: case kSidVideoRes: case kSidVideoBuffer: case kSidVideoMp3: case kSidVideoContrast: case kSidVideoBrightness: case kSidVideoColors: case kSidVideoChroma: case kSidVideoRound: return kPvVideo;   // (a sample video; the lyrics preview when it is not there)
        case kSidVideoCover: return kPvNone;
        case kSidVideoClap: return kPvClap;
        case kSidDisk: case kSidDiskSpeed: case kSidDiskBorder: return kPvDisk;
        case kSidAlbumCover: case kSidCoverMode: case kSidCoverContrast: case kSidCoverBrightness: case kSidCoverColors: case kSidCoverGradSat: case kSidCoverChroma:
        case kSidCoverSearch: return kPvCover;
        case kSidVolIndicator: case kSidDummy: return kPvVolume;
        case kSidVisualizer: case kSidVisFluidity: case kSidVisDegr: case kSidVisVisc: case kSidBarStyle: case kSidHifiRes: case kSidHifiChan: case kSidHifiColor: case kSidHifiScale: case kSidHifiAdapt: case kSidHifiSplit: case kSidHifiFScale: case kSidHifiGain: case kSidHifiRange: case kSidBarBehavior: return kPvBars;
        case kSidWave: case kSidWaveStyle: return kPvWave;
        case kSidUseOsci: return kPvScope;   // (Modes Share Settings: no preview)
        case kSidUseSpectro: return kPvSpectro;
        case kSidUseSphere: return kPvSphere;
        case kSidLyricViz: case kSidNoLyricsViz: case kSidNoVideoViz: {
            // Lyrics/Viz Defaults is edited in viz_default (0 sphere, 1 osci, 2 spectro, 3 lyrics, 4 last used: the view in use, lyric_viz);
            // lyric_viz alone is only what the field shows now, so the preview showed the wrong picture until the row was changed twice.
            int v = id == kSidLyricViz ? s.viz_default : id == kSidNoVideoViz ? s.novideo_viz : s.nolyrics_viz;
            if (id == kSidNoVideoViz && v == 4) v = 3;   // the lyrics (3 = lyrics here); "none" has nothing to preview, shown as the lyrics too
            if (id == kSidLyricViz && v == 4) v = (s.element_lyrics || s.video_view) ? 3 : s.lyric_viz;   // the view in use right now
            if (id == kSidLyricViz && v == 5) v = 3;   // video: previewed as the lyrics
            return v == 0 ? kPvSphere : v == 1 ? kPvScope : v == 2 ? kPvSpectro : kPvLyrics;
        }
        case kSidLatinMacrons: case kSidLatinWo: case kSidLatinHanzi: case kSidLatinLookup: return kPvTranslit;
        case kSidEmoji: return kPvEmoji;
        case kSidMarquee: return kPvMarquee;
        default: return kPvNone;
    }
}

std::string braille_utf8(int bits) {
    const unsigned cp = 0x2800u + static_cast<unsigned>(bits);
    std::string s;
    s += static_cast<char>(0xE0 | (cp >> 12));
    s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
    return s;
}

// What a renderer hands back: rows (ANSI allowed), their visible width, a caption, and where to start (centred if -1).
struct PvOut {
    std::vector<std::string> rows;
    int w = 0;
    std::string caption;
    bool picture = false;   // the rows are placeholders for a spectrogram picture the terminal draws (rows x w cells)
    int cover_mode = -1;    // >= 0: the rows are placeholders for the sample cover as a picture (Kitty / Sixel) in that Cover Mode
    bool clap = false;      // Show Clap Loading: the rows are made in the box (the loading picture of a video as it would show)
    bool video = false;     // the rows are placeholders for the sample video as a picture (vw x vh pixels shown on vcols x vrows cells)
    int vcols = 0, vrows = 0, vw = 0, vh = 0, cw0 = 10, ch0 = 20;
    bool vfresh = false;
    const VideoFrame* vframe = nullptr;
};

}  // namespace

void App::settings_preview_draw(std::ostringstream& frame, int W, int MAX_Y, int tab,
                                const std::function<std::string(int)>& B) const {
    if (W < kPreviewMinW) return;
    const int id = settings_item_id(tab, settings_row_);
    const PvKind kind = kind_of(id, settings_);
    if (!pv_) pv_ = std::make_shared<SettingsPreviewState>();
    SettingsPreviewState& P = *pv_;

    // the clock
    const auto now = std::chrono::steady_clock::now();
    P.dt = P.have_last ? std::clamp(std::chrono::duration<double>(now - P.last).count(), 0.004, 0.1) : 0.033;
    P.last = now;
    P.have_last = true;
    P.t += P.dt;

    const bool want_spec = kind == kPvSpectro;
    if (!want_spec) P.spec.set_visible(false);
    if (kind != kPvVideo && P.vid) P.vid->stop();   // (the decoder only runs while its row is shown)
    PvKind shown = kind;
    if (kind == kPvVideo) {
        if (!P.vid_tried) {   // the sample video: scripts/preview_video.mp4 (8 seconds of a Pixabay cat video, see THIRD_PARTY_LICENSES.txt)
            P.vid_tried = true;
            const fs::path f = find_preview_video();
            std::error_code ec;
            if (!f.empty() && fs::exists(f, ec)) {
                P.vid = std::make_shared<VideoPlayer>();
                P.vid_ok = P.vid->set_source(path_utf8(f));
            }
        }
        if (!P.vid_ok) shown = kPvLyrics;   // no sample (or no ffmpeg): the lyrics stand in for it, as before
    }
    if (kind == kPvNone) return;

    const std::string R = "\x1b[0m";
    const int iw = W - 77;                                        // inside the box
    const int visible = std::max(1, MAX_Y - 3);
    const int bh = std::min(visible - 1, 19);                         // the box, borders included
    const int ih = bh - 2;                                        // rows inside
    if (ih < 6 || iw < 30) return;
    const int body_h = ih - 1;                                    // the last row is the caption

    // ---- the signal, only for what is shown ------------------------------------------------------------------
    std::vector<float> lr;
    const bool wants_signal = kind == kPvBars || kind == kPvScope || kind == kPvSpectro || kind == kPvSphere;
    if (wants_signal) {
        const size_t n = static_cast<size_t>(std::lround(P.dt * 48000.0));
        double t = P.sig;
        if (kind == kPvSpectro || kind == kPvBars) {   // (the spectrum bars listen to the same lively sound)
            // a spectrogram the Creator makes from the text "Mousiki Plus" (made once, played in a loop)
            if (!P.text_made) {
                P.text_made = true;
                const creator::Gray g = creator::gray_from_text("Mousiki Plus", 6);
                creator::SpecOptions so;
                so.rate = 48000;
                so.seconds = 8.0;
                so.octaves = 6;
                creator::Stereo st;
                if (!g.empty() && creator::render_spectrogram_sound(g, so, st) && !st.l.empty()) {
                    const size_t gap = 48000 * 3 / 2;   // a pause between two rounds
                    P.text_audio.assign((st.l.size() + gap) * 2, 0.0f);
                    for (size_t i = 0; i < st.l.size(); ++i) { P.text_audio[2 * i] = st.l[i]; P.text_audio[2 * i + 1] = st.r[i]; }
                }
            }
            if (!P.text_audio.empty()) {
                const size_t total = P.text_audio.size() / 2;
                lr.resize(n * 2);
                for (size_t i = 0; i < n; ++i) {
                    const size_t k = (P.text_pos + i) % total;
                    lr[2 * i] = P.text_audio[2 * k]; lr[2 * i + 1] = P.text_audio[2 * k + 1];
                }
                P.text_pos = (P.text_pos + n) % total;
            } else chord_frames(t, n, lr);
        } else chord_frames(t, n, lr);
        P.sig = t;
        std::vector<float> mono(n);
        for (size_t i = 0; i < n; ++i) mono[i] = 0.5f * (lr[2 * i] + lr[2 * i + 1]);
        if (kind == kPvBars || kind == kPvSphere) {
            P.fft.set_fluidity(settings_.visualizer_fluidity);
            P.fft.set_degradation_speed(settings_.visualizer_degradation_speed);
            P.fft.set_viscosity(settings_.visualizer_viscosity);
            P.fft.push_samples(mono.data(), mono.size(), 48000);
        }
        if (kind == kPvScope) P.osc.push_frames(lr.data(), n);
        if (kind == kPvBars && settings_.bars_style == 1) P.hifi.push(lr.data(), n, 48000);
        if (kind == kPvSpectro) {
            P.spec.set_visible(true);
            P.spec.push(lr.data(), n, 48000);
        }
    }

    PvOut out;
    VideoFrame vfx;       // (the sample video's frame with the retro effect on it)
    VideoFrame vscaled;   // (the sample video's frame scaled to the picture's size while the decoder catches up)
    const int cw = iw - 2;   // content width (one blank column each side)
    switch (shown) {
    case kPvLyrics: {
        struct S { double at; const char* text; };
        static const S kLines[] = {{0.0, "Meow, Meooooow"}, {3.0, "Meow meow Meeeeow meow"},
                                   {6.0, "Mrrrow, meooow, mew mew"}, {9.0, "Meow... Meeeoooow!"}};
        std::vector<LyricLine> lines;
        for (const S& s : kLines) {
            LyricLine l;
            l.start_time = s.at;
            l.full_text = s.text;
            std::istringstream iss(s.text);
            std::string w;
            double at = s.at;
            while (iss >> w) { l.words.push_back({at, w}); at += 0.6; }
            lines.push_back(l);
        }
        const double el = std::fmod(P.t, 12.0);
        int active = 0;
        for (size_t i = 0; i < lines.size(); ++i) if (lines[i].start_time <= el) active = static_cast<int>(i);
        std::vector<std::string> rows(static_cast<size_t>(body_h), std::string(static_cast<size_t>(cw), ' '));
        if (settings_.lyrics_animation == 4) {
            std::string word = lines[static_cast<size_t>(active)].full_text;
            for (const auto& wt : lines[static_cast<size_t>(active)].words) if (wt.first <= el) word = wt.second;
            const int pad = std::max(0, (cw - display_width(word)) / 2);
            rows[static_cast<size_t>(body_h / 2)] = std::string(static_cast<size_t>(pad), ' ') + ansi_for(settings_.active_word_color) + word + R;
        } else if (settings_.lyrics_animation == 3) {
            auto wr = render_lyric_line_wrapped(lines[static_cast<size_t>(active)], el, cw, true, settings_);
            const int st = std::max(0, (body_h - static_cast<int>(wr.size())) / 2);
            for (size_t i = 0; i < wr.size() && st + static_cast<int>(i) < body_h; ++i) rows[static_cast<size_t>(st) + i] = wr[i];
        } else {
            std::vector<std::string> flat;
            int a_start = 0, a_count = 1;
            for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
                auto wr = render_lyric_line_wrapped(lines[static_cast<size_t>(li)], el, cw, li == active, settings_);
                if (li == active) { a_start = static_cast<int>(flat.size()); a_count = static_cast<int>(wr.size()); }
                for (auto& r : wr) flat.push_back(std::move(r));
            }
            const int start = a_start + a_count / 2 - body_h / 2;
            for (int row = 0; row < body_h; ++row) {
                const int idx = start + row;
                if (idx >= 0 && idx < static_cast<int>(flat.size())) rows[static_cast<size_t>(row)] = flat[static_cast<size_t>(idx)];
            }
        }
        out.rows = rows;
        out.w = cw;
        const char* al = settings_.lyrics_alignment == 1 ? "left" : settings_.lyrics_alignment == 2 ? "right" : "center";
        out.caption = std::string("alignment: ") + al;
        break;
    }
    case kPvDisk:
    case kPvCover: {
        if (!settings_.element_disk) {   // the field is removed: no disk and no cover
            out.caption = "Disk / Cover field removed";
            break;
        }
        const int dc = std::min(30, cw), dr = std::min(15, body_h);
        std::vector<std::string> rows;
        P.angle = std::fmod(P.angle + kAngularVelocity * settings_.disk_rotation_speed * P.dt, kTwoPi);
        const bool cover_look = kind == kPvCover;
        if (cover_look) {
            if (!P.cover.ok) make_sample_cover(P.cover);
            if (cf_->full_res && gfxs_.proto != GfxProto::None && settings_.cover_mode <= 4) {
                // a real picture (like the Style Option overlay): the box gets placeholders, the picture is set up below
                for (int i = 0; i < dr; ++i) rows.push_back(gfx_hole(dc));
                out.cover_mode = settings_.cover_mode;
                out.caption = settings_.album_cover ? "sample cover (image)" : "sample cover (image; Album Cover is off)";
            } else {
                rows = album_cover_lines_for(P.cover, settings_.cover_mode, settings_.cover_contrast, settings_.cover_brightness,
                                             settings_.cover_colors, P.angle, dc, dr, settings_.cover_chroma, settings_.cover_grad_sat);
                out.caption = std::string(gfxs_.proto != GfxProto::None && cf_->full_res ? "sample cover (braille)" : "sample cover")
                              + (settings_.album_cover ? "" : " (Album Cover is off)");
            }
        }
        if (rows.empty()) {
            rows = disk_.frame(P.angle);
            if (static_cast<int>(rows.size()) > dr) rows.resize(static_cast<size_t>(dr));
            for (size_t i = 0; i < rows.size(); ++i) {
                const float t = rows.size() > 1 ? static_cast<float>(i) / static_cast<float>(rows.size() - 1) : 0.0f;
                rows[i] = gradient_ansi(settings_.disk_color, settings_.disk_color_end, t) + rows[i] + R;
            }
            out.caption = "disk speed " + std::to_string(settings_.disk_rotation_speed).substr(0, 4);
        }
        out.rows = rows;
        out.w = dc;
        break;
    }
    case kPvVolume: {
        const int vol = 85;
        const int hashes = (vol * 20) / 100;
        const std::string played = ansi_for(settings_.progress_played_color), unplayed = ansi_for(settings_.progress_remaining_color);
        const std::string border = ansi_for(settings_.border_color, false);
        std::string fill;
        const double ang = volume_clock_angle();
        if (settings_.volume_gradient) {
            const auto cells = volume_bar_cells(settings_.volume_indicator, hashes, ang);
            for (size_t i = 0; i < cells.size(); ++i)
                fill += gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, static_cast<float>(i) / (kVolBarCells - 1)) + cells[i];
        } else {
            fill = played + volume_bar_fill(settings_.volume_indicator, hashes, ang);
        }
        const std::string bar = border + "VOLUME BAR:[" + R + fill + R + unplayed + std::string(static_cast<size_t>(20 - hashes), '-') + R + border + "]" + R + " 85%";
        const std::string bar_short = border + "[" + R + fill + R + unplayed + std::string(static_cast<size_t>(20 - hashes), '-') + R + border + "]" + R + " 85%";
        const int vw = 12 + 20 + 1 + 1 + 3;
        std::vector<std::string> rows;
        int w = vw;
        if (id == kSidDummy) {
            // The real buttons (as in the PROGRESS BAR panel): three framed 13-column boxes; the middle one shows PAUSE while
            // playing and PLAY while paused (alternating here), and << / >> flash in the bottom border colour while seeking.
            if (settings_.element_dummy_buttons) {
                const int bw = 13;
                const std::string ba = ansi_for(settings_.border_color, false), bb = ansi_for(settings_.border_color_bottom, false);
                const bool paused = (static_cast<int>(P.t / 2.0) % 2) == 1;
                const bool flash = std::fmod(P.t, 4.0) < 0.5;
                const bool back = static_cast<int>(P.t / 4.0) % 2 == 0;
                auto mid = [&](const std::string& text, const std::string& tx) {
                    const std::string bar = ba + settings_.box_vertical + R;
                    return bar + " " + tx + center_pad(text, 9) + R + " " + bar;
                };
                const std::string lb = (flash && back) ? bb : ba, rb = (flash && !back) ? bb : ba;
                const std::string pl = paused ? "PLAY" : "PAUSE";
                const bool all = cw >= 3 * bw;
                std::string t, m, bt;
                if (all) {
                    t = box_top("", bw, ba) + box_top("", bw, ba) + box_top("", bw, ba);
                    m = mid("<<<", lb) + mid(pl, ba) + mid(">>>", rb);
                    bt = box_bottom(bw, "", bb) + box_bottom(bw, "", bb) + box_bottom(bw, "", bb);
                    w = 3 * bw;
                } else {   // a narrow box: the play / pause button alone
                    t = box_top("", bw, ba); m = mid(pl, ba); bt = box_bottom(bw, "", bb);
                    w = bw;
                }
                rows = {t, m, bt};
            } else { rows = {"(Dummy Buttons are off)"}; w = 23; }
            out.caption = settings_.element_dummy_buttons ? "Dummy Buttons: on" : "Dummy Buttons: off";
        } else {
            rows = {cw < vw ? bar_short : bar};   // (a narrow box: without the "VOLUME BAR:" label)
            if (cw < vw) w = vw - 12;
            out.caption = std::string("Volume Indicator: ") + (settings_.volume_gradient ? "gradient" : "no gradient");
        }
        out.rows = rows;
        out.w = w;
        break;
    }
    case kPvBars: {
        std::vector<std::string> rows;
        const int n = std::min(cw, 48);
        const auto bars = P.fft.compute_bars(n, P.dt);
        std::string top, bot;
        for (int i = 0; i < n; ++i) {
            const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
            const std::string a = !settings_.viz_center_color.empty()
                ? multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t)
                : gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t);
            top += a + fft_glyph(std::max(0, bars[static_cast<size_t>(i)] - 4));
            bot += a + fft_glyph(std::min(bars[static_cast<size_t>(i)], 4));
        }
        rows = {ansi_for(settings_.border_color, false) + "Title : Chords" + R, ansi_for(settings_.border_color, false) + "Artist: Test signal" + R, "", top + R, bot + R};
        if (settings_.bars_style == 1) {   // Hi-Fi: the same drawing as under the meta data (four rows high, the test signal in stereo)
            HifiView hv;
            hv.cells = n; hv.rows = 4; hv.resolution = settings_.hifi_resolution; hv.channels = settings_.hifi_channels; hv.color = settings_.hifi_color; hv.scale = settings_.hifi_scale; hv.adapt = settings_.hifi_adapt; hv.fscale = settings_.hifi_fscale; hv.gain = settings_.hifi_gain; hv.range = settings_.hifi_range; hv.split = settings_.hifi_split;
            hv.fluidity = settings_.hifi_fluidity; hv.degradation = settings_.hifi_degradation; hv.viscosity = settings_.hifi_viscosity; hv.dt = P.dt;
            spectro_gradient_from(settings_);
            const auto& pal = spectro_palette(settings_.spectro.scheme);
            const std::vector<std::string> hr = hifi_rows(P.hifi, hv,
                [&](float t) {
                    return !settings_.viz_center_color.empty() ? multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t)
                                                              : gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t);
                },
                [&](float f) {
                    const auto& c = pal[static_cast<size_t>(std::clamp(48 + static_cast<int>(f * 207.0f), 0, 255))];
                    return "\x1b[38;2;" + std::to_string(c[0]) + ";" + std::to_string(c[1]) + ";" + std::to_string(c[2]) + "m";
                });
            rows = {ansi_for(settings_.border_color, false) + "Title : Chords" + R, ansi_for(settings_.border_color, false) + "Artist: Test signal" + R, ""};
            for (const auto& l : hr) rows.push_back(l + R);
        }
        out.rows = rows;
        out.w = n;
        out.caption = std::string(bars_preset_name(bars_preset_of(settings_.bar_fluidity(), settings_.bar_degradation(), settings_.bar_viscosity()))) + ": fluidity " + std::to_string(settings_.bar_fluidity()) + "  degradation " + std::to_string(settings_.bar_degradation()) +
                      "  viscosity " + std::to_string(settings_.bar_viscosity());
        break;
    }
    case kPvWave: {
        if (P.env_raw.empty()) {
            const auto pcm = sample_pcm();
            P.env_raw = WaveformQuantizer::generate_high_res_envelope(pcm, 4096, false, 1);
            P.env_smooth = WaveformQuantizer::generate_high_res_envelope(pcm, 4096, true, 1);
        }
        const int n = cw;
        const double frac = std::fmod(P.t / 20.0, 1.0);
        const int active = static_cast<int>(frac * n);
        const std::string cp = ansi_for(settings_.progress_played_color), cu = ansi_for(settings_.progress_remaining_color);
        if (settings_.element_waveform) {
            const auto levels = WaveformQuantizer::resample_for_ui(settings_.waveform_smooth ? P.env_smooth : P.env_raw, n);
            std::string top, mid, bot;
            for (int i = 0; i < n; ++i) {
                const BrailleColumn col = WaveformQuantizer::get_column(i < static_cast<int>(levels.size()) ? levels[static_cast<size_t>(i)] : 0);
                const std::string& c = i < active ? cp : cu;
                if (i == 0 || i == active) { top += c; mid += c; bot += c; }
                top += col.top; mid += col.mid; bot += col.bot;
            }
            out.rows = {top + R, mid + R, bot + R};
            out.caption = std::string("Waveform Style: ") + (settings_.waveform_smooth ? "smooth" : "raw");
        } else {
            const int inner = std::max(0, n - 2);
            const int filled = std::clamp(static_cast<int>(frac * inner), 0, inner);
            out.rows = {"", "[" + cp + std::string(static_cast<size_t>(filled), '#') + R + cu + std::string(static_cast<size_t>(inner - filled), '-') + R + "]", ""};
            out.caption = "waveform off: the plain bar";
        }
        out.w = n;
        break;
    }
    case kPvScope: {
        const int rows_n = std::min(body_h, 12);
        const OsciSet& os = settings_.osci();
        const auto cells = P.osc.render(cw, rows_n, osci_scope_params(settings_, P.dt, 0));
        for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
            std::string line, last;
            for (int cell = 0; cell < cw; ++cell) {
                const auto& c = cells[static_cast<size_t>(i)][static_cast<size_t>(cell)];
                if (c.braille == 0) { line += ' '; continue; }
                const float t = cw > 1 ? static_cast<float>(cell) / static_cast<float>(cw - 1) : 0.0f;
                const float lvl = std::round(c.level / 255.0f * 7.0f) / 7.0f;
                const float k = 0.30f + 0.70f * lvl;
                std::string ansi;
                if (os.palette == 0) ansi = osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t), k);
                else { int r = 255, g = 255, b = 255; osci_palette_rgb(settings_, os.palette, c.hue / 255.0f, r, g, b); ansi = osci_rgb_seq(r, g, b, k); }
                if (ansi != last) { line += ansi; last = ansi; }
                line += braille_utf8(c.braille);
            }
            out.rows.push_back(line + R);
        }
        out.w = cw;
        out.caption = "oscilloscope";
        break;
    }
    case kPvSpectro: {
        // always the plain defaults (the regular spectrogram as an image, mel scale, Roseus colours), whatever the spectrogram is set to:
        // the preview only shows what a spectrogram is, it is not connected to the SHIFT+i values (nor to the waterfall)
        SpectroSettings sp;
        sp.motion = 1;   // scrolling, so the small picture is full at once; span: a short stretch of time across the box
        sp.span = 10;
        P.spec.update(sp);
        const int srows = std::min(body_h, 12);
        if (gfxs_.proto != GfxProto::None) {   // the picture wherever the terminal can show one, braille only without Kitty / Sixel
            // the image style (the default): the terminal draws the picture, the box holds placeholders (main loop: pv_spectro_gfx_)
            pv_spectro_set_ = sp;
            for (int r = 0; r < srows; ++r) out.rows.push_back(gfx_hole(cw));
            out.picture = true;
            out.caption = "spectrogram (image; default look)";
        } else {
            out.rows = P.spec.braille(sp, cw, srows);
            out.caption = "spectrogram (braille; default look)";
        }
        out.w = cw;
        break;
    }
    case kPvSphere: {
        const auto bars = P.fft.compute_bars(48, P.dt);
        const auto rows = P.sph.render(std::min(cw, 40), std::min(body_h, 12), bars, P.dt);
        const std::string a = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, 0.5f);
        for (const std::string& r : rows) out.rows.push_back(a + r + R);
        out.w = std::min(cw, 40);
        out.caption = "sphere";
        break;
    }
    case kPvTranslit: {
        // What the option changes, for the highlighted row: the examples of the manual with the result for each value of the setting; the value in
        // use is lit. Under it a few examples that never change. Rows that are wider than the box scroll (marquee).
        struct PRow { std::string plain, ansi; };
        std::vector<PRow> rows;
        const std::string lit = "\x1b[7m", off = R;
        // one example: the source, then the results with the index of the one in use
        // (a row that does not fit the box is split: the source on one row, every result on a row of its own, indented and
        // named by `labels` -- so nothing has to scroll)
        auto example = [&](const std::string& src, const std::vector<std::string>& results, int use,
                           const std::vector<std::string>& labels = {}) {
            PRow r;
            const int pad = std::max(1, 14 - display_width(src));
            r.plain = src + std::string(static_cast<size_t>(pad), ' ') + "\u2192 ";
            r.ansi = r.plain;
            for (size_t k = 0; k < results.size(); ++k) {
                if (k) { r.plain += " | "; r.ansi += " | "; }
                r.plain += results[k];
                r.ansi += (static_cast<int>(k) == use ? lit : std::string()) + results[k] + off;
            }
            if (display_width(r.plain) <= cw || results.size() < 1) { rows.push_back(r); return; }
            rows.push_back({src, src});
            for (size_t k = 0; k < results.size(); ++k) {
                const std::string lab = k < labels.size() ? labels[k] + ": " : std::string();
                const bool u = static_cast<int>(k) == use && results.size() > 1;
                rows.push_back({"  " + lab + results[k], "  " + lab + (u ? lit : std::string()) + results[k] + (u ? off : std::string())});
            }
        };
        auto line = [&](const std::string& t) {   // wrapped to the box (no scrolling)
            if (display_width(t) <= cw) { rows.push_back({t, t}); return; }
            for (const std::string& w : wrap_lines(t, cw, 6)) rows.push_back({w, w});
        };
        const int m = settings_.latin_macrons ? 1 : 0, wo = settings_.latin_wo ? 1 : 0, hz = settings_.latin_hanzi;
        std::string cap;
        if (id == kSidLatinMacrons) {
            line("Long vowels: off | on");
            example("\u30e9\u30fc\u30e1\u30f3", {"raamen", "r\u0101men"}, m, {"off", "on"});
            example("\u30b3\u30fc\u30d2\u30fc", {"koohii", "k\u014dh\u012b"}, m, {"off", "on"});
            example("\u304d\u3048\u306c\u3088\u3046", {"kienuyou", "kienuy\u014d"}, m, {"off", "on"});
            cap = std::string("Latin Macrons: ") + (m ? "on" : "off");
        } else if (id == kSidLatinWo) {
            line("The particle \u3092: wo | o");
            example("\u3053\u3048\u3092\u304d\u304b\u305b\u3066", {"koe wo", "koe o"}, wo ? 0 : 1, {"wo", "o"});
            example("\u3042\u306a\u305f\u3092", {"anata wo", "anata o"}, wo ? 0 : 1, {"wo", "o"});
            example("\u3059\u304d\u3092", {"suki wo", "suki o"}, wo ? 0 : 1, {"wo", "o"});
            cap = std::string("Latin Wo: ") + (wo ? "wo" : "o");
        } else if (id == kSidLatinHanzi) {
            line("Chinese characters without a hint (Leave | Chin. | Jap.):");
            example("\u6771\u4eac", {"\u6771\u4eac", "d\u014dng j\u012bng", m ? "t\u014dky\u014d" : "toukyou"}, hz, {"Leave", "Chin.", "Jap."});
            example("\u5f20\u60ac", {"zh\u0101ng xu\u00e1n"}, 0);   // 张悬 (Deserts Chang), a Taiwanese independent singer-songwriter
            cap = std::string("Translit. Kanji: ") + (hz == 1 ? "Chin." : hz == 2 ? "Jap." : "Leave");
        } else {   // kSidLatinLookup
            line("Names in Chinese characters / kana:");
            if (P.translit_key < 0 || P.t - P.translit_at > 1.5) {
                P.translit_key = 0;
                P.translit_at = P.t;
                P.translit_rows.assign(1, transliterate("\u30ab\u30cd\u30b3\u30a2\u30e4\u30ce"));   // what the rules alone give
            }
            example("\u30ab\u30cd\u30b3\u30a2\u30e4\u30ce", {P.translit_rows.empty() ? std::string("kanekoayano") : P.translit_rows[0], "Kaneko Ayano"}, settings_.latin_lookup ? 1 : 0, {"off", "on"});
            line("on: the Latin spelling is looked up online at MusicBrainz (the artist name is sent there).");
            cap = std::string("Latin Name Lookup: ") + (settings_.latin_lookup ? "on" : "off");
        }
        line("");
        line("Always the same:");
        example("\u3055\u304f\u3089", {"sakura"}, 0);
        example("\u732b\u306e\u624b\u3082\u501f\u308a\u305f\u3044", {"neko no te mo karitai"}, 0);
        example("\u3057\u3093\u3042\u3044", {"shin'ai"}, 0);
        example("\uc548\ub155\ud558\uc138\uc694", {"annyeonghaseyo"}, 0);
        line("A short poem, phrase by phrase:");
        example("\u8349\u306e\u540d\u3082", {"kusa no na mo"}, 0);
        example("\u5bc2\u3057\u3046\u306a\u308a\u3051\u308a", {"sabishiu narikeri"}, 0);
        example("\u6625\u306e\u96e8", {"haru no ame"}, 0);
        for (const PRow& r : rows) {
            if (display_width(r.plain) <= cw) { out.rows.push_back(r.ansi); continue; }
            // too wide for the box: the text scrolls
            const std::string loop = r.plain + "    ";
            const int period = std::max(1, display_width(loop));
            const int at = static_cast<int>(P.t * 5.0) % period;
            out.rows.push_back(utf8_skip_take(loop + loop, at, cw));
        }
        if (static_cast<int>(out.rows.size()) > body_h) out.rows.resize(static_cast<size_t>(body_h));
        out.w = 0;   // left aligned
        out.caption = cap;
        break;
    }
    case kPvEmoji: {
        const int em = settings_.emoji_mode;
        out.rows.push_back(std::string("now: Replace Emoji ") + (em == 2 ? "All" : em == 1 ? "Safe" : "Off"));
        out.rows.push_back("");
        out.rows.push_back("            All   Safe  Off");
        static const char* const kName[] = {"note", "radio", "alien", "book", "red heart", "thumbs up"};
        static const char* const kEmoji[] = {"\U0001f3b5", "\U0001f4fb", "\U0001f47e", "\U0001f4d6",
                                             "\u2764\ufe0f", "\U0001f44d\U0001f3fd"};
        // Safe keeps a plain one-codepoint emoji and turns every joined / flag / skin-tone / VS16 form into "?".
        static const bool kSafeKeeps[] = {true, true, true, true, false, false};
        for (int i = 0; i < 6; ++i) {
            std::string n = kName[i];
            n.resize(12, ' ');
            // (the real emoji are last on their row so a different width cannot push anything; 26 columns in all)
            out.rows.push_back(n + "?     " + (kSafeKeeps[i] ? std::string("kept  ") : std::string("?     ")) + kEmoji[i]);
        }
        out.rows.push_back("");
        for (const std::string& w : wrap_lines("Safe: plain emoji stay, risky ones become \"?\"", cw, 3)) out.rows.push_back(w);
        out.w = 0;
        out.caption = "";
        break;
    }
    case kPvVideo: {   // the sample video, looping, drawn like the video view of the main screen
        VideoPlayer& V = *P.vid;
        int cw0 = 10, ch0 = 20;
        gfx_cell_pixels(settings_.cell_pixels, cw0, ch0);
        cw0 = std::max(4, cw0); ch0 = std::max(8, ch0);
        const bool picture = gfxs_.proto != GfxProto::None;
        const int avail_w = std::max(8, cw), avail_h = std::max(3, body_h);
        int vcols = 0, vrows = 0, dw = 0, dh = 0, fw = 0, fh = 0;
        if (picture) {
            V.fit(avail_w * cw0, avail_h * ch0, 1.0, fw, fh);
            vcols = std::clamp((fw + cw0 / 2) / cw0, 1, avail_w);
            vrows = std::clamp((fh + ch0 / 2) / ch0, 1, avail_h);
            const int maxw = gfxs_.proto == GfxProto::Sixel ? 480 : 640;
            const double f = std::min(1.0, static_cast<double>(maxw) / (vcols * cw0));
            dw = std::max(2, static_cast<int>(vcols * cw0 * f) & ~1);
            dh = std::max(2, static_cast<int>(vrows * ch0 * f) & ~1);
        } else {
            V.fit(avail_w * 2, avail_h * 4, 2.0 * cw0 / ch0, fw, fh);
            vcols = std::clamp((fw + 1) / 2, 1, avail_w);
            vrows = std::clamp((fh + 3) / 4, 1, avail_h);
            dw = vcols * 2; dh = vrows * 4;
        }
        double vt = P.t;
        const double vdur = V.duration();
        if (vdur > 1.0) vt = std::fmod(vt, vdur);
        bool fresh = false;
        const VideoFrame* fr = V.frame_for(vt, dw, dh, true, &fresh);
        if (fr && (fr->w != dw || fr->h != dh)) { vscaled = VideoPlayer::resample(*fr, dw, dh); fr = &vscaled; }
        {
            VideoLook look;
            look.fx = settings_.video_fx; look.contrast = settings_.video_contrast; look.brightness = settings_.video_brightness; look.colors = settings_.video_colors; look.chroma = settings_.video_chroma;
            if (video_fx_uses_gradient(look.fx)) look.grad = video_fx_gradient(settings_);
            if (fr && look.active()) { vfx = *fr; VideoPlayer::apply_look(vfx, look, vt, !picture, V.previous_frame()); fr = &vfx; }
        }
        if (!fr) {   // the decoder is starting (or failed): say it in the middle
            std::string msg = V.status();
            if (msg.empty()) msg = "video: ...";
            out.rows = {truncate_str(msg, std::max(1, cw))};
            out.w = display_width(out.rows[0]);
            out.caption = "sample video";
            break;
        }
        if (picture) {
            for (int r = 0; r < vrows; ++r) out.rows.push_back(gfx_hole(vcols));
            out.video = true; out.vframe = fr; out.vfresh = fresh;
            out.vcols = vcols; out.vrows = vrows; out.vw = dw; out.vh = dh; out.cw0 = cw0; out.ch0 = ch0;
            out.caption = "sample video (image)";
        } else {
            const auto cells = VideoPlayer::braille(*fr, vcols, vrows);
            for (int r = 0; r < vrows; ++r) {
                std::string line, last_ansi;
                for (int c = 0; c < vcols; ++c) {
                    const auto& cell = cells[static_cast<size_t>(r)][static_cast<size_t>(c)];
                    if (cell.mask == 0 || (settings_.video_round && vrows >= 3 && vcols >= 4 && (r == 0 || r == vrows - 1) && (c == 0 || c == vcols - 1))) { line += ' '; continue; }   // (rounded: the four corner cells stay empty)
                    const float tt = vcols > 1 ? static_cast<float>(c) / static_cast<float>(vcols - 1) : 0.0f;
                    const float lvl = std::round(cell.level / 255.0f * 7.0f) / 7.0f;
                    const std::string ansi = osci_dim_ansi(gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, tt), 0.30f + 0.70f * lvl);
                    if (ansi != last_ansi) { line += ansi; last_ansi = ansi; }
                    const int cp = 0x2800 + cell.mask;
                    line += static_cast<char>(0xE0 | (cp >> 12));
                    line += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    line += static_cast<char>(0x80 | (cp & 0x3F));
                }
                out.rows.push_back(line + "\x1b[0m");
            }
            out.caption = "sample video (braille)";
        }
        out.w = vcols;
        break;
    }
    case kPvMarquee: {
        const std::string text = "Meowsome Moew Orchestra And The Best-Off-All-Meows feat. Bozai   ";
        const int w = std::max(8, std::min(cw - 8, 24));
        std::string shown;
        if (settings_.marquee) {
            const std::string loop = text + text;
            const size_t off = static_cast<size_t>(P.t * 6.0) % text.size();
            shown = loop.substr(off, static_cast<size_t>(w));
        } else {
            shown = text.substr(0, static_cast<size_t>(w - 3)) + "...";
        }
        out.rows = {"Title : " + shown};
        out.w = 8 + w;
        out.caption = settings_.marquee ? "Marquee on: the title scrolls" : "Marquee off: cut with ...";
        break;
    }
    case kPvClap: {   // what shows while a video loads: the text note, the small clapperboard (a picture or braille) or nothing
        out.clap = true;
        out.caption = settings_.video_clap == 1 ? "loading: clap" : settings_.video_clap == 0 ? "loading: text note" : "loading: none";
        break;
    }
    default: return;
    }

    // ---- the box ------------------------------------------------------------------------------------------------
    auto pos = [&](int y, int x, const std::string& s) { frame << "\x1b[" << y << ";" << x << "H" << s; };
    std::string label = "Preview";
    {
        std::string head;
        for (const SetLine& L : settings_layout(tab)) {
            if (L.header) { if (*L.text) head = L.text; continue; }
            if (L.id != id) continue;
            label = std::string("Preview: ") + (std::string(L.text) == "On/Off" ? head : std::string(L.text));
            break;
        }
    }
    if (display_width(label) > iw - 2) label = "Preview";
    const int top = 4;                                            // one row below the first option row
    const int bottom = top + bh - 1;
    const int x0 = kPreviewX, x1 = kPreviewX + iw + 1;
    pos(top, x0, B(top) + "┌─ " + R + ansi_for(settings_.tab_current_color, false) + label + R + B(top) + " ");
    {
        const int used = 3 + display_width(label) + 1;   // "┌─ " + label + " "
        std::string dash;
        for (int i = used; i < iw + 1; ++i) dash += "─";
        frame << dash << R;
    }
    pos(top, x1, B(top) + "┐" + R);
    for (int y = top + 1; y < bottom; ++y) {
        pos(y, x0, B(y) + "│" + R + std::string(static_cast<size_t>(iw), ' '));
        pos(y, x1, B(y) + "│" + R);
    }
    pos(bottom, x0, B(bottom) + "└");
    {
        std::string dash;
        for (int i = 0; i < iw; ++i) dash += "─";
        frame << dash << "┘" << R;
    }
    const int inner_x = x0 + 1;
    if (out.clap) {   // the loading picture of a video, made by the same code as the video view (the box inside is iw x body_h cells)
        out.rows.assign(static_cast<size_t>(body_h), std::string(static_cast<size_t>(iw), ' '));
        out.w = iw;
        if (settings_.video_clap == 1) {
            clap_loading_fill(out.rows, iw, body_h, inner_x - 1, top, gfx_, P.vid_key, 0);
            if (gfxs_.proto != GfxProto::None && body_h >= 4) pv_cover_pic_ = true;   // (the picture uses the picture slot gfx_, like the sample cover)
        } else if (settings_.video_clap == 0) {
            const std::string msg = "video: starting ...";
            const int pad = std::max(0, (iw - display_width(msg)) / 2);
            out.rows[static_cast<size_t>(body_h / 2)] = std::string(static_cast<size_t>(pad), ' ') + msg + std::string(static_cast<size_t>(std::max(0, iw - pad - display_width(msg))), ' ');
        }
    }
    const int nrows = std::min(static_cast<int>(out.rows.size()), body_h);
    const int vtop = std::max(0, (body_h - nrows) / 2);
    const int xo = out.w > 0 ? std::max(1, (iw - out.w) / 2 + 1) : 2;
    if (out.cover_mode >= 0 && nrows > 0) {   // the sample cover as a picture (it uses the picture slot gfx_)
        pv_cover_pic_ = true;
        meta_sty_gfx(pv_->cover, out.cover_mode, settings_.cover_contrast, settings_.cover_brightness, settings_.cover_colors, settings_.cover_chroma,
                     pv_->angle, inner_x + xo - 2, top + vtop, out.w, nrows, settings_.cover_grad_sat);
    }
    if (out.video && nrows > 0 && out.vframe) {   // the sample video as a picture (it uses the picture slot gfx_, like the sample cover)
        pv_cover_pic_ = true;
        GfxFrame& g = gfx_;
        const std::string key = std::to_string(out.vw) + "x" + std::to_string(out.vh) + "|" + std::to_string(out.vcols) + "x" + std::to_string(out.vrows) + "|" +
                                std::to_string(inner_x + xo - 2) + "," + std::to_string(top + vtop) + (settings_.video_round ? "|r" : "|-");
        const bool resized = key != P.vid_key || !g.plain || g.w != out.vw || g.h != out.vh || g.level.size() != static_cast<size_t>(out.vw) * out.vh || g.id != 0;
        g.plain = true; g.truecolor = false; g.direct = true; g.scale = 1;
        g.out_cw = out.cw0; g.out_ch = out.ch0;
        g.cols = out.vcols; g.rows = out.vrows; g.w = out.vw; g.h = out.vh;
        g.col = inner_x + xo - 2; g.row = top + vtop;
        g.id = 0; g.grid.clear(); g.cut_w = 0; g.crop = 0;
        g.opaque = settings_.black_background;
        g.erase = !settings_.black_background;
        g.active = true;
        g.fresh = out.vfresh || resized;
        if (g.fresh) {
            VideoPlayer::quantize(*out.vframe, g.hue, g.level);
            if (settings_.video_round) {   // Video Rounded Corners: the corners cut round, like in the main screen (a quarter circle of about half a cell)
                const int rp = std::clamp(g.w / std::max(1, out.vcols) * 11 / 20, 2, std::min(g.w, g.h) / 2);
                const float rf = static_cast<float>(rp);
                for (int y = 0; y < rp; ++y)
                    for (int x = 0; x < rp; ++x) {
                        const float dx = rf - static_cast<float>(x) - 0.5f, dy = rf - static_cast<float>(y) - 0.5f;
                        if (dx * dx + dy * dy <= rf * rf) continue;
                        const int xs[2] = {x, g.w - 1 - x}, ys[2] = {y, g.h - 1 - y};
                        for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) g.level[static_cast<size_t>(ys[b]) * g.w + xs[a]] = 0;
                    }
            }
            const auto& pal = VideoPlayer::palette();
            for (size_t i = 0; i < pal.size() && i < g.pal.size(); ++i) g.pal[i] = pal[i];
            P.vid_key = key;
        }
    }
    if (out.picture && nrows > 0) {   // where the picture goes (0-based row and column)
        pv_spectro_area_[0] = top + vtop;
        pv_spectro_area_[1] = inner_x + xo - 2;
        pv_spectro_area_[2] = out.w;
        pv_spectro_area_[3] = nrows;
    }
    for (int i = 0; i < nrows; ++i)
        if (!out.rows[static_cast<size_t>(i)].empty()) pos(top + 1 + vtop + i, inner_x + xo - 1, out.rows[static_cast<size_t>(i)] + R);
    if (!out.caption.empty()) {
        std::string cap = out.caption;
        if (display_width(cap) > iw - 2) cap = truncate_str(cap, iw - 2);
        const int cx = inner_x + std::max(1, (iw - display_width(cap)) / 2);
        pos(bottom - 1, cx, ansi_for(settings_.tab_current_color, false) + cap + R);
    }
}

SpectroAnalyzer* App::settings_preview_spectro() const { return pv_ ? &pv_->spec : nullptr; }

}  // namespace muisc
