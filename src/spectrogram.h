#pragma once
// Spectrogram: the third visual of the lyrics area (player) and of the scope block (radio), next to the sphere and the
// oscilloscope, plus a full-screen view (SHIFT+u) and an options overlay (SHIFT+i).
//
// It is made the way Audacity makes its spectrogram view, with Audacity's defaults (Audacity 3.x,
// SpectrogramSettings.cpp): FFT window 2048 samples, Hann window scaled to 2 / sum(window) so a full-scale sine reads
// 0 dB, zero padding factor 2, power per bin in dB (10 * log10(re^2 + im^2)), Mel frequency scale from 0 to 20 000 Hz,
// gain 20 dB and range 80 dB (colour = (dB + gain + range) / range, 0..1), frequency gain 0 dB/decade, each pixel row
// takes the strongest bin it covers (Audacity's "findValue"), and the "Color (Roseus)" colour map. Left and right are
// drawn as two stacked channels, like an Audacity stereo track.
//
// The signal is the decoded audio itself (before equalizer, normalization and volume), as Audacity analyses the file.
// The audio callback copies it into a lock-free ring (spectro_feed_push: one atomic load while no spectrogram is shown);
// the UI thread turns it into columns (spectro().update()) and draws them as braille or as a terminal picture.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace muisc {

struct SpectroSettings {
    int style = 1;          // 0 = braille, 1 = image (Kitty graphics / Sixel; braille where the terminal has neither)
    int motion = 0;         // 0 = sweep (the newest column is written left to right, a page at a time, like Audacity
                            //     paging along during playback), 1 = scroll (the newest column always on the right)
    int scale = 2;          // 0 linear, 1 logarithmic, 2 mel, 3 bark, 4 erb, 5 period (Audacity's scales; default mel)
    int min_freq = 0;       // Hz
    int max_freq = 20000;   // Hz (limited to half the sample rate)
    int gain = 20;          // dB
    int range = 80;         // dB
    int freq_gain = 0;      // dB per decade (0 dB at 1 kHz)
    int window_log2 = 11;   // 2^11 = 2048 samples (256 .. 8192)
    int window_type = 1;    // always hann (Audacity's default; the other window shapes made no visible difference)
    int zero_pad = 2;       // 1, 2, 4, 8
    int scheme = 0;         // 0 Roseus (Audacity's default), 1 classic (on black), 2 grayscale (black to white),
                            // 3 gradient (the VIZ / OSCI colours of the settings, like the oscilloscope's "gradient")
    int channels = 0;       // 0 = left + right stacked (like Audacity), 1 = one mix of both
    int span = 30;          // seconds across the width (5 .. 60, steps of 5)
    bool labels = true;     // frequency labels in the full-screen view
    // View 1 = waterfall: the same analysis drawn as a stack of spectra (a ridge per moment, the newest in front, each one filled
    // and outlined in black, the older ones behind it and shifted up and to the right) instead of frequency over time.
    int view = 0;           // 0 = spectrogram, 1 = waterfall
    int wf_graphs = 40;     // waterfall: graphs in the stack (8 .. 100)
    int wf_spacing = 120;   // waterfall: milliseconds between two graphs (20 .. 500)
    int wf_slant = 25;      // waterfall: how far the back graph is shifted to the right, % of the width (0 .. 40)
    int wf_height = 30;     // waterfall: height of the loudest peak, % of the picture (10 .. 60)
    bool wf_smooth = false; // waterfall (not stored): the graphs move back continuously with the clock-driven position (the spectrogram window), not one slot at a time
    int wf_detail = 0;      // waterfall: 0 = normal, 1 = high (drawn at twice the resolution and scaled down: fine lines keep their place and shade)
};

// Overlay rows (SHIFT+i); the first row ("Display") belongs to the caller (it switches the visual itself).
enum SpectroRow { kSrDisplay, kSrView, kSrStyle, kSrMotion, kSrScale, kSrMinFreq, kSrMaxFreq, kSrGain, kSrRange, kSrFreqGain,
                  kSrWindow, kSrZeroPad, kSrScheme, kSrChannels, kSrSpan, kSrLabels, kSrWfGraphs, kSrWfSpacing, kSrWfSlant, kSrWfHeight, kSrWfDetail, kSpectroRowCount };
// The rows the overlay shows for the view in use (after "Display"): the waterfall has no motion / channels / time span, the
// spectrogram no waterfall rows.
bool spectro_row_visible(const SpectroSettings& s, int row);
std::vector<int> spectro_visible_rows(const SpectroSettings& s);
const char* spectro_row_label(int row);
std::string spectro_row_value(const SpectroSettings& s, int row);   // kSrDisplay is the caller's
void spectro_adjust(SpectroSettings& s, int row, int dir);
void spectro_reset(SpectroSettings& s);   // Audacity's defaults (the style and motion stay)
// config keys Spectro<Name>=; true = the key was one of them
bool spectro_config_key(SpectroSettings& s, const std::string& key, const std::string& value);
void spectro_config_write(std::ostream& out, const SpectroSettings& s, const char* comment_prefix, const char* key_prefix = "Spectro");

// The colour scheme as 256 RGB entries (index 0 = quietest).
const std::array<std::array<uint8_t, 3>, 256>& spectro_palette(int scheme);
// The palette a picture of this view is drawn with: the same, but the waterfall's background (0) and outline (1) are pure black.
const std::array<std::array<uint8_t, 3>, 256>& spectro_palette_view(const SpectroSettings& s);
// Waterfall: the width in pixels of one graph in a picture `w_px` wide (the rest is the slant), and the labels for the
// frequency axis under the front graph: (text column, label) for a picture of `w_px` pixels, one text column = `unit_px`.
int spectro_waterfall_plot_width(const SpectroSettings& s, int w_px);
std::vector<std::pair<int, std::string>> spectro_axis_x(const SpectroSettings& s, int rate, int w_px, int unit_px);
// The two colours of the "gradient" scheme (the caller's VIZ / OSCI gradient); cheap to call every frame.
void spectro_set_gradient(const std::array<uint8_t, 3>& from, const std::array<uint8_t, 3>& to);
// Frequency at a height 0 (bottom) .. 1 (top) of one channel, and back.
double spectro_freq_at(const SpectroSettings& s, int rate, double y01);
double spectro_pos_of(const SpectroSettings& s, int rate, double hz);
// Frequency labels for the axis next to a picture `h_px` pixels high drawn by render() (both channels when stereo),
// one text row = `unit_px` pixels: (text row, label) with Audacity-like round numbers that fit.
std::vector<std::pair<int, std::string>> spectro_axis(const SpectroSettings& s, int rate, int h_px, int unit_px);

class SpectroAnalyzer {
public:
    // Audio thread: the decoded signal, interleaved L/R. Does nothing while no spectrogram is shown.
    void push(const float* lr, size_t frames, int rate);
    // UI thread, once per frame while a spectrogram is visible (it also switches the feed on). Turns the new samples
    // into columns. Returns true when something changed since the last call.
    bool update(const SpectroSettings& s);
    void set_visible(bool on);          // false: the feed is off and nothing is computed
    bool visible() const { return on_.load(std::memory_order_relaxed); }
    // Palette indices (0..255) of a w x h picture of the current view (`x0` .. `x0 + cols` of it, for tiles). The two
    // channels are stacked with a one-pixel dark line between them. `playhead` draws the sweep position as a line.
    void render(const SpectroSettings& s, int w, int h, int x0, int cols, std::vector<uint8_t>& idx, bool playhead = true);
    // Braille: `cols` x `rows` text cells, coloured.
    std::vector<std::string> braille(const SpectroSettings& s, int cols, int rows);
    // The pixel columns (of a view `w` wide) that changed since the last call with the same `w` (sweep: a narrow
    // strip; scroll: everything). [first, last) -- empty when nothing changed.
    void changed_columns(const SpectroSettings& s, int w, int& first, int& last);
    int rate() const { return rate_; }
    void reset();
    // scroll: the picture moves on `px` pixels at a time (SpectroGfx makes the steps even when a terminal picture
    // cannot be sent for every pixel)
    void set_scroll_step(int px) { scroll_step_ = std::max(1, px); }
    // scroll: newest pixel position of a view `w` wide (whole steps)
    uint64_t scroll_offset(int w) const;
    double columns_per_second() const { return hop_ > 0 ? static_cast<double>(rate_) / hop_ : 0.0; }
    // For the spectrogram window (spectro_window_app.cpp): columns made so far, how many of the newest are kept, the
    // clock-driven position of the scrolling view (fractional), and columns [c0, c1) as images `h` pixels high (palette
    // indices, top to bottom, column after column; both channels with the dark line between them, as render() draws).
    uint64_t columns() const { return ncols_; }
    size_t columns_kept() const { return cap_; }
    double display_columns() const { return disp_cols_; }
    unsigned generation() const { return gen_; }
    void column_images(const SpectroSettings& s, uint64_t c0, uint64_t c1, int h, std::vector<uint8_t>& out);
    int columns_per_page() const { return cols_per_page_; }
    // Waterfall picture (palette indices, w x h, row after row): 0 background, 1 black outline, 48..255 the fill by height.
    // `lines_only` (braille): only the outlines, in the colour of their height, the fill just hides what is behind it.
    void waterfall(const SpectroSettings& s, int w, int h, bool lines_only, std::vector<uint8_t>& img);
    void waterfall_raw(const SpectroSettings& s, int w, int h, int R, bool lines_only, std::vector<uint8_t>& img);
    // The same graphs as numbers, for a window that draws them on the graphics card: back to front, each with the row of its
    // baseline, the shift to the right, the row it is filled down to and the top edge of every column (and the loudness there).
    struct WfGraph { int baseline = 0, ox = 0, bottom = 0; std::vector<int> top; std::vector<uint8_t> v; };
    void waterfall_layout(const SpectroSettings& s, int w, int h, std::vector<WfGraph>& out, int& plot_w, int& amp);
    // (with the resolution "high" the picture uses the 64 colour registers of spectro_palette_view(): index = register * 4)

private:
    struct Shape { int rate = 0, fft = 0, win = 0, span = 0, ch = 0; };
    void configure(const SpectroSettings& s, int rate);
    void column(uint64_t c);                         // FFT column c (its window ends at sample (c + 1) * hop_)
    void prepare(const SpectroSettings& s, int h);   // rows -> bins for a channel `h` pixels high (once per render)
    int value_at(uint64_t col, int ch, int y);       // 0..255 of row y (0 = top) of a channel, cached per column

    // feed (lock-free single producer / single consumer)
    static constexpr size_t kFeed = 1u << 17;
    std::vector<float> feed_ = std::vector<float>(2 * kFeed, 0.0f);
    std::atomic<size_t> fw_{0}, fr_{0};
    std::atomic<bool> on_{false};
    std::atomic<int> feed_rate_{44100};

    // analysis (UI thread)
    Shape shape_;
    int rate_ = 44100;
    int hop_ = 441;                     // samples per column
    int fft_ = 4096, bins_ = 2048, win_ = 2048;
    std::vector<float> window_;
    // The signal itself (16 bit stereo) for the last minute: a changed window size, zero padding, time span or channel
    // setting is computed again from it at once, instead of starting empty.
    std::vector<int16_t> raw_;
    size_t raw_cap_ = 0;                // frames
    uint64_t raw_total_ = 0;            // frames received since the last reset
    void* fft_cfg_ = nullptr;
    std::vector<float> fft_in_;
    std::vector<float> fft_out_;        // complex, interleaved
    // history: per column and channel `bins_` dB values, quantized to 8 bit (-140 .. +10 dB)
    size_t cap_ = 0;                    // columns kept
    uint64_t ncols_ = 0;                // columns made so far
    std::vector<uint8_t> hist_;
    int cols_per_page_ = 1024;
    // per-column cache of the row values for the last height / settings
    std::vector<uint8_t> cache_;
    std::vector<uint64_t> cache_tag_;
    int cache_h_ = 0;
    unsigned gen_ = 1;
    std::string cache_key_;
    std::vector<int> row_lo_, row_hi_;  // bin range per row (cache_h_ rows per channel)
    SpectroSettings prep_;
    // changed_columns()
    int cw_last_w_ = 0;
    uint64_t cw_last_cols_ = 0;
    uint64_t cw_last_off_ = 0;
    int scroll_step_ = 1;
    double disp_cols_ = 0;              // scroll: the column shown as the newest (clock-driven, see advance_display)
    std::chrono::steady_clock::time_point disp_t_{};
    void advance_display();
    bool rebuilt_ = false;              // the columns were computed again: everything has changed
    // waterfall: the finished picture, kept while the columns, the size and the settings stay as they were (the tiles of a terminal
    // picture are slices of it)
    std::vector<uint8_t> wf_img_;
    std::string wf_key_;
};

// The one analyzer of the program (player and radio share it; only one of them plays at a time).
SpectroAnalyzer& spectro();
// Audio thread: forwards to spectro().push().
void spectro_feed_push(const float* lr, size_t frames, int rate);
bool spectro_feed_active();

// Does the terminal move a Sixel picture when a rectangle of the screen is copied (DECCRA)? Windows Terminal does;
// MOUSIKI_SIXEL_SHIFT=0/1 overrides it.
bool spectro_rect_copy();

// The picture in the terminal, in vertical tiles: in sweep mode only the strip being written is sent again.
class SpectroGfx {
public:
    // proto: 0 none, 1 kitty, 2 sixel. Area in cells (0-based row/col) and the cell size in pixels. `skip_*`: a cell
    // rectangle an overlay covers (tiles touching it are left out). Returns the escape codes for this frame.
    std::string emit(int proto, const SpectroSettings& s, SpectroAnalyzer& a, int row, int col, int cols, int rows,
                     int cell_w, int cell_h, bool due, int skip_row, int skip_col, int skip_rows, int skip_cols);
    std::string clear(int proto);       // take the picture away (Kitty deletes its images; Sixel needs nothing)
    void invalidate() { all_dirty_ = true; urgent_ = true; }   // the screen was cleared / the picture must be drawn again completely
    bool shown() const { return shown_; }
private:
    static constexpr int kTileCols = 4;
    int row_ = -1, col_ = -1, cols_ = 0, rows_ = 0, cw_ = 0, ch_ = 0, proto_ = 0;
    std::string key_;
    std::vector<char> dirty_, on_screen_;
    std::vector<int> layout_;           // per tile: which rows an overlay covers (0 = none), to notice a change
    uint64_t last_off_ = 0;             // Sixel scroll by copying (see emit): the view position on screen
    bool have_off_ = false;
    bool all_dirty_ = true, shown_ = false;
    bool urgent_ = false;               // invalidate() asked for the whole picture now (no pacing wait)
};

} // namespace muisc
