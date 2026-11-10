// The scope window process (`mousiki --scope-window`): reads samples and settings from its standard input (sent by
// scope_window.cpp) and draws them as an analog XY oscilloscope with the graphics card (SDL2's renderer: Direct3D 11 on
// Windows, OpenGL on Linux, Metal on macOS -- integrated graphics are plenty).
//
// How it draws, every frame (at the monitor's refresh rate):
//   1. Phosphor: an off-screen picture keeps the beam. It is darkened in steps of 1/60 s by the Decay setting, plus one
//      level per step so faint rests vanish instead of staying stuck in 8-bit colour.
//   2. Beam: every sample that arrived since the last frame is drawn once, as a soft line from the previous sample
//      (a quad with a Gaussian cross-section, added to what is there) -- dense parts of the trace add up and glow.
//   3. Bloom: the picture is shrunk to 1/2, 1/4 and 1/8 with smooth filtering and the two smallest are added on top.
// With the braille style set in the oscilloscope overlay (ScopeWinConfig::braille) it draws the terminal scope instead: the same
// OscilloscopeVisualizer renders the 2 x 4 dot grid of braille cells, and every lit dot becomes a square on the screen --
// so the braille look can be seen as large as the window (or the whole screen) goes.
// The playhead runs on the window's own clock at the sample rate, ~35 ms behind the newest sample, so bursts in the pipe
// never show as jerks.
#include "scope_window.h"
#include "scope_sdl.h"
#include "oscilloscope_visualizer.h"
#include "string_util.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace muisc {

namespace {

constexpr size_t kRing = 1u << 17;   // samples kept (~2.7 s at 48 kHz)

struct Shared {
    std::mutex m;
    std::vector<float> ring = std::vector<float>(2 * kRing, 0.0f);
    uint64_t total = 0;              // samples received so far
    ScopeWinConfig cfg;
    unsigned cfg_gen = 0;
    std::atomic<bool> eof{false};
};

std::string home_dir() {
    const char* h = std::getenv("HOME");
#if defined(_WIN32)
    if (!h || !*h) h = std::getenv("USERPROFILE");
#endif
    return h && *h ? h : ".";
}

void write_error(const std::string& msg) {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::path(home_dir()) / ".cache" / "mousiki";
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "scope_window_error.txt") << msg << "\n";
}

bool read_exact(void* buf, size_t n) {
    char* p = static_cast<char*>(buf);
    while (n > 0) {
        const size_t got = std::fread(p, 1, n, stdin);
        if (got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

// Reads packets until the input ends (the player closed the window or quit).
void reader_main(Shared* S) {
    std::vector<char> payload;
    for (;;) {
        uint8_t head[5];
        if (!read_exact(head, sizeof head)) break;
        const uint32_t len = static_cast<uint32_t>(head[1]) | (static_cast<uint32_t>(head[2]) << 8) |
                             (static_cast<uint32_t>(head[3]) << 16) | (static_cast<uint32_t>(head[4]) << 24);
        if (len > (64u << 20)) break;   // nonsense: stop rather than allocate
        payload.resize(len);
        if (len > 0 && !read_exact(payload.data(), len)) break;
        if (head[0] == 1 && len == sizeof(ScopeWinConfig)) {
            ScopeWinConfig c;
            std::memcpy(&c, payload.data(), sizeof c);
            if (c.magic != ScopeWinConfig{}.magic) continue;
            std::lock_guard<std::mutex> lk(S->m);
            S->cfg = c;
            ++S->cfg_gen;
        } else if (head[0] == 2) {
            const size_t n = len / (2 * sizeof(float));
            std::vector<float> f(2 * n);   // (copied: the byte buffer is not guaranteed to be aligned for floats)
            if (n > 0) std::memcpy(f.data(), payload.data(), f.size() * sizeof(float));
            std::lock_guard<std::mutex> lk(S->m);
            for (size_t i = 0; i < n; ++i) {
                const size_t at = static_cast<size_t>((S->total + i) & (kRing - 1));
                S->ring[2 * at] = f[2 * i];
                S->ring[2 * at + 1] = f[2 * i + 1];
            }
            S->total += n;
        }
    }
    S->eof.store(true);
}

// ---- window position and size, kept between runs ----------------------------------------------------------------------
struct Geometry { int x = sdl::kPosCentered, y = sdl::kPosCentered, w = 720, h = 720; bool full = false, top = false; };

std::filesystem::path geometry_file() { return std::filesystem::path(home_dir()) / ".config" / "mousiki" / "scope_window.txt"; }

Geometry load_geometry(const sdl::Api& A) {
    Geometry g;
    std::ifstream in(geometry_file());
    int x, y, w, h, f, t;
    if (in >> x >> y >> w >> h >> f >> t) {
        g.w = std::clamp(w, 160, 8192);
        g.h = std::clamp(h, 120, 8192);
        g.full = f != 0;
        g.top = t != 0;
        // only where a screen still is (a monitor may have gone since)
        const int n = A.GetNumVideoDisplays();
        for (int i = 0; i < n; ++i) {
            sdl::Rect r{};
            if (A.GetDisplayBounds(i, &r) != 0) continue;
            const int mx = x + g.w / 2, my = y + 16;
            if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h) { g.x = x; g.y = y; break; }
        }
    }
    return g;
}

void save_geometry(const Geometry& g) {
    std::error_code ec;
    std::filesystem::create_directories(geometry_file().parent_path(), ec);
    std::ofstream(geometry_file()) << g.x << " " << g.y << " " << g.w << " " << g.h << " " << (g.full ? 1 : 0) << " " << (g.top ? 1 : 0) << "\n";
}

// ---- drawing -------------------------------------------------------------------------------------------------------
struct Gpu {
    sdl::Texture* accum = nullptr;      // the phosphor
    sdl::Texture* half = nullptr;
    sdl::Texture* quarter = nullptr;
    sdl::Texture* eighth = nullptr;
    sdl::Texture* beam = nullptr;       // 64 x 64 soft dot: the cross-section of the beam
    sdl::Texture* pic = nullptr;        // the YT / combined modes in the image style: a finished picture made on the CPU
    int pic_w = 0, pic_h = 0;
    int w = 0, h = 0;
};

void destroy(const sdl::Api& A, Gpu& g) {
    for (sdl::Texture** t : {&g.accum, &g.half, &g.quarter, &g.eighth, &g.beam, &g.pic})
        if (*t) { A.DestroyTexture(*t); *t = nullptr; }
    g.pic_w = g.pic_h = 0;
}

bool create(const sdl::Api& A, sdl::Renderer* R, Gpu& g, int w, int h) {
    destroy(A, g);
    g.w = std::max(1, w);
    g.h = std::max(1, h);
    auto target = [&](int tw, int th) {
        sdl::Texture* t = A.CreateTexture(R, sdl::kFmtARGB8888, sdl::kTexTarget, std::max(1, tw), std::max(1, th));
        if (t && A.SetTextureScaleMode) A.SetTextureScaleMode(t, sdl::kScaleLinear);
        return t;
    };
    g.accum = target(g.w, g.h);
    g.half = target(g.w / 2, g.h / 2);
    g.quarter = target(g.w / 4, g.h / 4);
    g.eighth = target(g.w / 8, g.h / 8);
    std::vector<uint32_t> px(64 * 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const float dx = (x + 0.5f - 32.0f) / 32.0f, dy = (y + 0.5f - 32.0f) / 32.0f;
            const float d2 = dx * dx + dy * dy;
            const float a = d2 >= 1.0f ? 0.0f : std::exp(-d2 * 4.0f) * (1.0f - d2);   // Gaussian, reaching 0 at the rim
            px[static_cast<size_t>(y) * 64 + x] = (static_cast<uint32_t>(std::lround(a * 255.0f)) << 24) | 0x00FFFFFFu;
        }
    g.beam = A.CreateTexture(R, sdl::kFmtARGB8888, sdl::kTexStatic, 64, 64);
    if (g.beam) {
        A.UpdateTexture(g.beam, nullptr, px.data(), 64 * 4);
        A.SetTextureBlendMode(g.beam, sdl::kBlendAdd);
        if (A.SetTextureScaleMode) A.SetTextureScaleMode(g.beam, sdl::kScaleLinear);
    }
    if (!g.accum || !g.half || !g.quarter || !g.eighth || !g.beam) return false;
    A.SetRenderTarget(R, g.accum);
    A.SetRenderDrawColor(R, 0, 0, 0, 255);
    A.RenderClear(R);
    A.SetRenderTarget(R, nullptr);
    return true;
}

void write_ppm(const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
    std::ofstream o(path, std::ios::binary);
    o << "P6\n" << w << " " << h << "\n255\n";
    for (uint32_t v : px) { const char c[3] = {static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)}; o.write(c, 3); }
}

// Mono detection, as in the terminal scope: side / mid energy below kMonoFull = phase portrait, above kMonoNone = XY.
constexpr float kMonoFull = 0.003f, kMonoNone = 0.020f;
constexpr int kDeriv = 3;
constexpr int kLz = 3;   // Lanczos-3: 6 samples per interpolated point
// Beam of the scope window (the lines you see). Half width of the beam in pixels = the larger of kBeamMinHalfWidth and
// kRelHalfWidth * window height, then widened by up to kBeamBigWidth (x 1 + that) in big windows. The brightness factor goes
// from kBeamGainSmall (small window) to kBeamGainSmall + kBeamGainBig (1100 px and more). Bigger = thicker / brighter.
constexpr float kBeamMinHalfWidth = 2.5f;
constexpr float kBeamRelHalfWidth = 0.0020f;
constexpr float kBeamBigWidth = 0.4f;
constexpr float kBeamGainSmall = 2.0f;
constexpr float kBeamGainBig = 1.0f;
// Extra light: the finished picture is added to itself this many times (0 = off, 1 = twice as bright, 2 = three times ...).
// Unlike the gain above it also lifts what a single pass of the beam cannot make brighter, so thin lines look bright, not wide.
constexpr float kBeamAddLight = 2.6f;
// The YT / overlap / row / col pictures (image style) are painted by the terminal scope's renderer, not by the GPU beam above, so
// they get their own brightness lift: level -> (1 - e^(-level * g)) / (1 - e^-g). 0 = as the terminal draws it, 3 = clearly brighter
// (faint parts most), 5 = very bright. The XY mode is not affected.
constexpr float kImageBrightness = 3.0f;
constexpr float kDwell = 1.0f;   // music mode: segments shorter than this many beam half widths get the full beam
constexpr float kBlankSpeed = 20000.0f;   // music mode below 96 kHz: beam speed (full scale widths per second) of a jump

} // namespace

int scope_window_main() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    // On the heap and never freed: the reader thread may still sit in fread() while the window shuts down.
    Shared& S = *new Shared;
    std::thread(reader_main, &S).detach();   // blocked in fread until the input ends; the process exit ends it

    sdl::Api A;
    std::string err;
    if (!sdl::load(A, &err)) { write_error(err); return 2; }
    A.SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    A.SetHint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");
    A.SetHint("SDL_RENDER_SCALE_QUALITY", "1");
    A.SetHint("SDL_RENDER_BATCHING", "1");
#if defined(_WIN32)
    A.SetHint("SDL_RENDER_DRIVER", "direct3d11");
#endif
    if (A.Init(sdl::kInitVideo) != 0) { write_error(std::string("scope window: SDL could not start the video system: ") + A.GetError()); return 3; }

    Geometry geo = load_geometry(A);
    sdl::Window* win = A.CreateWin("Mousiki scope", geo.x, geo.y, geo.w, geo.h, sdl::kWinResizable | sdl::kWinHighDpi | sdl::kWinShown);
    if (!win) { write_error(std::string("scope window: cannot open a window: ") + A.GetError()); A.Quit(); return 4; }
    sdl::Renderer* R = A.CreateRenderer(win, -1, sdl::kRenAccelerated | sdl::kRenVsync | sdl::kRenTarget);
    if (!R) {   // the preferred driver did not work: let SDL pick any
        A.SetHint("SDL_RENDER_DRIVER", "");
        R = A.CreateRenderer(win, -1, sdl::kRenAccelerated | sdl::kRenVsync | sdl::kRenTarget);
    }
    if (!R) R = A.CreateRenderer(win, -1, sdl::kRenVsync | sdl::kRenTarget);   // software, slow but working
    if (!R) { write_error(std::string("scope window: no usable renderer: ") + A.GetError()); A.DestroyWindow(win); A.Quit(); return 5; }
    if (geo.full) A.SetWindowFullscreen(win, sdl::kWinFullscreenDesktop);
    if (geo.top && A.SetWindowAlwaysOnTop) A.SetWindowAlwaysOnTop(win, 1);

    // "dst - src": takes the faint rests off the phosphor (not every renderer has it; then only the decay works)
    const int sub_blend = A.ComposeCustomBlendMode(sdl::kFactorOne, sdl::kFactorOne, sdl::kOpRevSubtract,
                                                   sdl::kFactorOne, sdl::kFactorOne, sdl::kOpRevSubtract);
    const bool sub_ok = A.SetRenderDrawBlendMode(R, sub_blend) == 0;
    A.SetRenderDrawBlendMode(R, sdl::kBlendNone);

    Gpu gpu;
    int ow = 0, oh = 0;
    A.GetRendererOutputSize(R, &ow, &oh);
    if (!create(A, R, gpu, ow, oh)) { write_error(std::string("scope window: cannot create textures: ") + A.GetError()); destroy(A, gpu); A.DestroyRenderer(R); A.DestroyWindow(win); A.Quit(); return 6; }

    const char* shot_path = std::getenv("MOUSIKI_SCOPE_SHOT");   // tests: save frame N as a PPM picture
    const int shot_frame = std::getenv("MOUSIKI_SCOPE_SHOT_FRAME") ? muisc::atoi_safe(std::getenv("MOUSIKI_SCOPE_SHOT_FRAME")) : 120;
    const bool shot_exit = std::getenv("MOUSIKI_SCOPE_SHOT_EXIT") != nullptr;

    ScopeWinConfig cfg;
    OscilloscopeVisualizer braille_scope;   // the terminal's scope, for the braille style
    bool braille_was = false;
    unsigned cfg_seen = ~0u;
    std::string title_set;
    std::vector<float> xs, ys, mids, rx, ry, lz_w;
    int lz_ups = 0;
    std::vector<sdl::Vertex> verts;
    std::vector<int> idx;
    std::vector<uint8_t> img_level, img_hue;   // YT / combined modes, image style
    std::vector<uint32_t> img_px;
    float mid_hist[2 * kDeriv] = {};
    double play = 0.0;
    uint64_t drawn = 0;
    bool started = false;
    float peak_prev = 0.05f;
    float peak = 0.05f, mono = 0.0f, pscale = 0.0f, spd_lo = -3.0f, spd_hi = -1.5f;
    float last_x = 0.0f, last_y = 0.0f;
    bool have_last = false;
    double decay_acc = 0.0;
    // Exposure: a coarse copy of the phosphor on the CPU (beam length per cell, decaying like the picture) tells how
    // often the beam crosses the same spot. A tone that retraces its figure hundreds of times per second would burn
    // everything white, sparse music would be faint: the beam strength is set so the busiest places end near full.
    std::vector<float> dens, dens_tmp;
    int gw = 0, gh = 0;
    float gcell = 8.0f, expo = 0.25f;
    const double freq = static_cast<double>(A.GetPerformanceFrequency());
    uint64_t t_last = A.GetPerformanceCounter();
    long frame_no = 0;
    bool quit = false;
    alignas(8) unsigned char ev[128];

    while (!quit) {
        // ---- events -----------------------------------------------------------------------------------------------
        bool resized = false;
        while (A.PollEvent(ev)) {
            uint32_t type;
            std::memcpy(&type, ev, 4);
            if (type == sdl::kEvQuit) quit = true;
            else if (type == sdl::kEvWindow) {
                const uint8_t we = ev[12];
                if (we == sdl::kWinEvClose) quit = true;
                if (we == sdl::kWinEvResized || we == sdl::kWinEvSizeChanged) resized = true;
            } else if (type == sdl::kEvKeyDown) {
                int32_t sym;
                std::memcpy(&sym, ev + 20, 4);
                // 9 / 0: the key of ")" (German SHIFT+9, English SHIFT+0) closes it as in the terminal
                if (sym == sdl::kKeyEsc || sym == 'q' || sym == '9' || sym == '0') quit = true;
                else if (sym == 'f' || sym == sdl::kKeyF11) {
                    const bool full = (A.GetWindowFlags(win) & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
                    A.SetWindowFullscreen(win, full ? 0 : sdl::kWinFullscreenDesktop);
                } else if (sym == 't' && A.SetWindowAlwaysOnTop) {
                    geo.top = !geo.top;
                    A.SetWindowAlwaysOnTop(win, geo.top ? 1 : 0);
                }
            } else if (type == sdl::kEvMouseDown) {
                if (ev[18] >= 2) {   // double click: fullscreen on / off
                    const bool full = (A.GetWindowFlags(win) & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
                    A.SetWindowFullscreen(win, full ? 0 : sdl::kWinFullscreenDesktop);
                }
            } else if (type == sdl::kEvTargetsReset || type == sdl::kEvDeviceReset) resized = true;
        }
        if (S.eof.load()) quit = true;
        if (quit) break;
        {   // remember the windowed position and size (not the fullscreen one)
            const uint32_t fl = A.GetWindowFlags(win);
            geo.full = (fl & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
            if (!geo.full && !(fl & sdl::kWinMinimized)) { A.GetWindowPosition(win, &geo.x, &geo.y); A.GetWindowSize(win, &geo.w, &geo.h); }
            if (fl & (sdl::kWinMinimized | sdl::kWinHidden)) { A.Delay(50); t_last = A.GetPerformanceCounter(); continue; }
        }
        if (resized) {
            A.GetRendererOutputSize(R, &ow, &oh);
            if (!create(A, R, gpu, ow, oh)) { write_error(std::string("scope window: cannot create textures: ") + A.GetError()); break; }
            have_last = false;
        }

        const uint64_t t_now = A.GetPerformanceCounter();
        const double dt = std::clamp(static_cast<double>(t_now - t_last) / freq, 0.0, 0.25);
        t_last = t_now;

        // ---- settings and new samples -------------------------------------------------------------------------------
        uint64_t total;
        {
            std::lock_guard<std::mutex> lk(S.m);
            if (S.cfg_gen != cfg_seen) { cfg = S.cfg; cfg_seen = S.cfg_gen; }
            total = S.total;
        }
        {
            std::string t = std::string(cfg.title, strnlen(cfg.title, sizeof cfg.title));
            t = t.empty() ? "Mousiki scope" : t + "  -  Mousiki scope";
            if (t != title_set) { A.SetWindowTitle(win, t.c_str()); title_set = t; }
        }
        const double rate = std::max(8000, static_cast<int>(cfg.rate));
        const double target_lag = 0.035 * rate;
        if (!started && total > 0) { play = std::max(0.0, static_cast<double>(total) - target_lag); drawn = static_cast<uint64_t>(play); started = true; }
        if (started) {
            play += dt * rate;
            const double backlog = static_cast<double>(total) - play;
            if (backlog > 0.30 * rate) play = static_cast<double>(total) - target_lag;   // far behind (e.g. the window was dragged)
            else play += (backlog - target_lag) * std::min(1.0, dt * 3.0);              // glide to the target lag
            play = std::min(play, static_cast<double>(total));
        }
        // the interpolation below looks kLz samples ahead: never draw closer than that to the newest sample
        uint64_t upto = static_cast<uint64_t>(std::max(0.0, std::min(play, static_cast<double>(total) - kLz)));
        if (upto < drawn) upto = drawn;
        if (upto - drawn > kRing / 2) drawn = upto - kRing / 2;
        const size_t n_raw = static_cast<size_t>(upto - drawn);
        // Smoothing between the samples, like the output filter of a real scope's signal path: Lanczos interpolation
        // to about 192 000 points per second (384 000 in oscilloscope music mode), so fast strokes are curves, not
        // polygons.
        const int ups = !cfg.interp ? 1   // dots: only the real samples
                      : std::clamp(static_cast<int>(std::lround((cfg.music ? 384000.0 : 192000.0) / rate)), 1, cfg.music ? 8 : 4);
        if (ups != lz_ups) {
            lz_ups = ups;
            lz_w.assign(static_cast<size_t>(ups) * 2 * kLz, 0.0f);
            for (int j = 0; j < ups; ++j) {
                const double t = static_cast<double>(j) / ups;
                double sum = 0.0;
                for (int k = 0; k < 2 * kLz; ++k) {
                    const double x = t - (k - kLz + 1);   // distance to tap k (taps m-kLz+1 .. m+kLz)
                    double w;
                    if (std::fabs(x) < 1e-9) w = 1.0;
                    else if (std::fabs(x) >= kLz) w = 0.0;
                    else { const double px = 3.14159265358979 * x; w = kLz * std::sin(px) * std::sin(px / kLz) / (px * px); }
                    lz_w[static_cast<size_t>(j) * 2 * kLz + k] = static_cast<float>(w);
                    sum += w;
                }
                for (int k = 0; k < 2 * kLz; ++k) lz_w[static_cast<size_t>(j) * 2 * kLz + k] = static_cast<float>(lz_w[static_cast<size_t>(j) * 2 * kLz + k] / sum);
            }
        }
        rx.resize(n_raw + 2 * kLz);
        ry.resize(n_raw + 2 * kLz);
        if (n_raw > 0) {
            std::lock_guard<std::mutex> lk(S.m);
            for (size_t i = 0; i < rx.size(); ++i) {   // samples drawn - kLz .. upto + kLz - 1
                const long long idx = static_cast<long long>(drawn) - kLz + static_cast<long long>(i);
                if (idx < 0 || static_cast<uint64_t>(idx) >= total || total - static_cast<uint64_t>(idx) > kRing) { rx[i] = ry[i] = 0.0f; continue; }
                const size_t at = static_cast<size_t>(static_cast<uint64_t>(idx) & (kRing - 1));
                rx[i] = S.ring[2 * at];
                ry[i] = S.ring[2 * at + 1];
            }
        }
        const size_t n = n_raw * static_cast<size_t>(ups);
        xs.resize(n);
        ys.resize(n);
        for (size_t m = 0; m < n_raw; ++m) {
            for (int j = 0; j < ups; ++j) {
                const float* w = lz_w.data() + static_cast<size_t>(j) * 2 * kLz;
                float vx = 0.0f, vy = 0.0f;
                for (int k = 0; k < 2 * kLz; ++k) {   // tap k = sample m - kLz + 1 + k = rx index m + 1 + k
                    vx += w[k] * rx[m + 1 + static_cast<size_t>(k)];
                    vy += w[k] * ry[m + 1 + static_cast<size_t>(k)];
                }
                xs[m * static_cast<size_t>(ups) + static_cast<size_t>(j)] = vx;
                ys[m * static_cast<size_t>(ups) + static_cast<size_t>(j)] = vy;
            }
        }
        drawn = upto;

        // ---- braille style: the terminal scope's dot grid, drawn as squares ------------------------------------------------
        // The YT modes (SHIFT+o "Scope mode") are drawn by the same scope in either style: the smooth image style only knows the
        // XY beam, so there the dots just get finer (and touch each other) instead of changing the picture.
        if (cfg.braille || cfg.mode != 0) {
            if (!braille_was) { braille_scope.reset(); braille_was = true; }
            if (n_raw > 0) {
                std::vector<float> lr(2 * n_raw);
                for (size_t i = 0; i < n_raw; ++i) { lr[2 * i] = rx[kLz + i]; lr[2 * i + 1] = ry[kLz + i]; }   // rx[kLz ..]: the new samples
                braille_scope.push_frames(lr.data(), n_raw);
            }
            OscilloscopeVisualizer::Params p;
            p.decay = std::pow(std::clamp(cfg.decay, 0.0f, 0.99f), static_cast<float>(std::clamp(dt, 0.004, 0.15) * 30.0));   // per frame at 30 fps, as in the terminal
            p.dot_threshold = cfg.dot_threshold;
            p.tail_brightness = cfg.tail;
            p.interpolate = cfg.interp != 0;
            p.z_axis = cfg.z != 0;
            p.z_depth = cfg.z_depth;
            p.z_source = cfg.z_source;
            p.trace = cfg.trace;
            p.rotate = cfg.rotate != 0;
            p.mono_phase = cfg.mono_phase != 0 && !cfg.music;
            p.glow = cfg.glow;
            p.fixed_gain = cfg.music != 0;
            p.crt = cfg.crt != 0;
            p.low_rate_blank = cfg.low_rate != 0;
            p.dwell = cfg.dwell != 0;
            p.rough = cfg.rough != 0;
            p.mode = cfg.mode;
            // XY: a square field in the middle, a dot every `pitch` pixels (about 100 dots across, like the terminal's area).
            // The YT modes use the whole window (the time axis is wide).
            const int side_px = std::min(gpu.w, gpu.h);
            const bool fine = !cfg.braille;
            const int pitch = fine ? std::max(2, static_cast<int>(std::lround(side_px / 300.0))) : std::max(4, static_cast<int>(std::lround(side_px / 100.0)));
            const bool wide = cfg.mode != 0;
            const int rows = std::max(6, (wide ? gpu.h : side_px) / (pitch * 4));
            const int cols = wide ? std::max(12, gpu.w / (pitch * 2)) : rows * 2;
            const auto cells = cfg.braille ? braille_scope.render(cols, rows, p) : std::vector<std::vector<OscilloscopeVisualizer::Cell>>{};   // (image style: no dot grid, see below)
            const int ox = (gpu.w - cols * 2 * pitch) / 2, oy = (gpu.h - rows * 4 * pitch) / 2;
            const int dot = fine ? pitch : std::max(2, static_cast<int>(std::lround(pitch * 0.72)));
            const int pad = (pitch - dot) / 2;
            A.SetRenderTarget(R, nullptr);
            A.SetRenderDrawBlendMode(R, sdl::kBlendNone);
            A.SetRenderDrawColor(R, 0, 0, 0, 255);
            A.RenderClear(R);
            if (!cfg.braille) {
                // The image style: the same picture as the terminal's image mode, made at the window's own resolution (a real
                // pixel picture, not a dot grid), then scaled to the window. Very large windows are rendered a little smaller.
                constexpr long kMaxPixels = 2600000;
                int iw = gpu.w, ih = gpu.h;
                if (static_cast<long>(iw) * ih > kMaxPixels) {
                    const double f = std::sqrt(static_cast<double>(kMaxPixels) / (static_cast<double>(iw) * ih));
                    iw = std::max(16, static_cast<int>(iw * f));
                    ih = std::max(16, static_cast<int>(ih * f));
                }
                braille_scope.render_image(iw, ih, p, img_level, img_hue);
                if (!gpu.pic || gpu.pic_w != iw || gpu.pic_h != ih) {
                    if (gpu.pic) A.DestroyTexture(gpu.pic);
                    gpu.pic = A.CreateTexture(R, sdl::kFmtARGB8888, sdl::kTexStatic, iw, ih);
                    gpu.pic_w = iw;
                    gpu.pic_h = ih;
                    if (gpu.pic) {
                        A.SetTextureBlendMode(gpu.pic, sdl::kBlendNone);
                        if (A.SetTextureScaleMode) A.SetTextureScaleMode(gpu.pic, sdl::kScaleLinear);
                    }
                }
                if (gpu.pic) {
                    float lift[256];
                    for (int i = 0; i < 256; ++i) {
                        const float l = i / 255.0f;
                        lift[i] = kImageBrightness > 0.0f ? (1.0f - std::exp(-l * kImageBrightness)) / (1.0f - std::exp(-kImageBrightness)) : l;
                    }
                    img_px.resize(static_cast<size_t>(iw) * ih);
                    for (int y = 0; y < ih; ++y) {
                        for (int x = 0; x < iw; ++x) {
                            const size_t at = static_cast<size_t>(y) * iw + x;
                            const float lv = lift[img_level[at]];
                            if (lv <= 0.0f) { img_px[at] = 0xFF000000u; continue; }
                            const int h = cfg.color_by_x ? static_cast<int>(std::lround(iw > 1 ? 255.0f * x / (iw - 1) : 0.0f)) : img_hue[at];
                            const uint8_t* pc = cfg.pal[h];
                            const auto ch = [&](int v) { return static_cast<uint32_t>(std::clamp(std::lround((v * 0.90f + 255 * 0.10f) * lv), 0l, 255l)); };
                            img_px[at] = 0xFF000000u | (ch(pc[0]) << 16) | (ch(pc[1]) << 8) | ch(pc[2]);
                        }
                    }
                    A.UpdateTexture(gpu.pic, nullptr, img_px.data(), iw * 4);
                    A.RenderCopy(R, gpu.pic, nullptr, nullptr);
                }
            }
            for (int ty = 0; ty < rows && ty < static_cast<int>(cells.size()); ++ty) {
                for (int tx = 0; tx < cols; ++tx) {
                    const OscilloscopeVisualizer::Cell& c = cells[static_cast<size_t>(ty)][static_cast<size_t>(tx)];
                    if (c.braille == 0) continue;
                    // as in the terminal: 8 brightness steps, the colour across the width (gradient) or by beam speed
                    const float lvl = std::round(c.level / 255.0f * 7.0f) / 7.0f;
                    const float k = 0.30f + 0.70f * lvl;
                    const int h = cfg.color_by_x ? static_cast<int>(std::lround(cols > 1 ? 255.0f * tx / (cols - 1) : 0.0f)) : c.hue;
                    A.SetRenderDrawColor(R, static_cast<uint8_t>(cfg.pal[h][0] * k), static_cast<uint8_t>(cfg.pal[h][1] * k), static_cast<uint8_t>(cfg.pal[h][2] * k), 255);
                    for (int yy = 0; yy < 4; ++yy)
                        for (int xx = 0; xx < 2; ++xx) {
                            if (c.sub[static_cast<size_t>(yy * 2 + xx)] == 0) continue;
                            sdl::Rect r{ox + (tx * 2 + xx) * pitch + pad, oy + (ty * 4 + yy) * pitch + pad, dot, dot};
                            A.RenderFillRect(R, &r);
                        }
                }
            }
            ++frame_no;
            if (shot_path && frame_no == shot_frame) {
                std::vector<uint32_t> px(static_cast<size_t>(gpu.w) * gpu.h);
                if (A.RenderReadPixels(R, nullptr, sdl::kFmtARGB8888, px.data(), gpu.w * 4) == 0) write_ppm(shot_path, px, gpu.w, gpu.h);
                if (shot_exit) quit = true;
            }
            A.RenderPresent(R);
            const double spent_b = static_cast<double>(A.GetPerformanceCounter() - t_now) / freq;
            if (spent_b < 1.0 / 120.0) A.Delay(static_cast<uint32_t>((1.0 / 120.0 - spent_b) * 1000.0));   // the dot grid needs no more than 120 frames/s
            continue;
        }
        braille_was = false;

        // ---- the same signal shaping as the terminal scope ----------------------------------------------------------------
        const float k30 = static_cast<float>(dt * 30.0);   // the terminal values are per 1/30 s
        if (n > 0) {
            double em = 0.0, es = 0.0;
            for (size_t i = 0; i < n; ++i) {
                const double m = xs[i] + ys[i], s = xs[i] - ys[i];
                em += m * m;
                es += s * s;
            }
            const float ratio = static_cast<float>(es / (em + 1e-9));
            const float want = (cfg.mono_phase && !cfg.music) ? std::clamp((kMonoNone - ratio) / (kMonoNone - kMonoFull), 0.0f, 1.0f) : 0.0f;
            mono += (want - mono) * std::min(1.0f, 0.25f * k30);
            // the mid signal and its rate of change (difference over 2 * kDeriv samples, continued across frames)
            mids.resize(n);
            for (size_t i = 0; i < n; ++i) mids[i] = 0.5f * (xs[i] + ys[i]);
            if (mono > 0.001f) {
                std::vector<float> q(n);
                double rs = 0.0, rq = 0.0;
                for (size_t i = 0; i < n; ++i) {
                    const float back = i >= static_cast<size_t>(2 * kDeriv) ? mids[i - 2 * kDeriv] : mid_hist[i];
                    q[i] = mids[i] - back;
                    rs += static_cast<double>(mids[i]) * mids[i];
                    rq += static_cast<double>(q[i]) * q[i];
                }
                const float sc = std::min(400.0f, static_cast<float>(std::sqrt(rs / n) / std::max(std::sqrt(rq / n), 1e-6)));
                pscale = pscale <= 0.0f ? sc : pscale + (sc - pscale) * std::min(1.0f, 0.2f * k30);
                for (size_t i = 0; i < n; ++i) {
                    xs[i] = xs[i] + (mids[i] - xs[i]) * mono;
                    ys[i] = ys[i] + (q[i] * pscale - ys[i]) * mono;
                }
            }
            {   // keep the newest 2 * kDeriv mids (oldest first) for the next frame
                float comb[2 * kDeriv];
                for (int i = 0; i < 2 * kDeriv; ++i) {
                    const long j = static_cast<long>(n) - 2 * kDeriv + i;   // index into this frame's mids
                    comb[i] = j >= 0 ? mids[static_cast<size_t>(j)] : mid_hist[static_cast<size_t>(i) + n];   // older: from the last frame
                }
                std::memcpy(mid_hist, comb, sizeof comb);
            }
            if (cfg.rotate) {
                constexpr float r2 = 0.70710678f;
                for (size_t i = 0; i < n; ++i) {
                    const float a = xs[i], b = ys[i];
                    xs[i] = (a - b) * r2;
                    ys[i] = (a + b) * r2;
                }
            }
            float fp = 0.0f;
            for (size_t i = 0; i < n; ++i) fp = std::max(fp, std::max(std::fabs(xs[i]), std::fabs(ys[i])));
            const float keep = std::pow(0.97f, k30);
            peak_prev = peak;
            peak = fp > peak ? fp : peak * keep + fp * (1.0f - keep);
        }
        // music mode: full scale = the edge; rough mode: the previous frame's peak only, so a sudden louder beat clips along the edges
        const float gain = cfg.music ? 1.0f : 1.0f / std::max(cfg.rough ? peak_prev : peak, 0.05f);

        // ---- phosphor decay, in steps of 1/60 s ---------------------------------------------------------------------------
        A.SetRenderTarget(R, gpu.accum);
        decay_acc += dt;
        int steps = 0;
        while (decay_acc >= 1.0 / 60.0 && steps < 30) { decay_acc -= 1.0 / 60.0; ++steps; }
        if (steps >= 30) decay_acc = 0.0;
        if (steps > 0) {
            const float f = std::pow(std::clamp(cfg.decay, 0.0f, 0.99f), 0.5f * static_cast<float>(steps));
            A.SetRenderDrawBlendMode(R, sdl::kBlendBlend);
            A.SetRenderDrawColor(R, 0, 0, 0, static_cast<uint8_t>(std::lround((1.0f - f) * 255.0f)));
            A.RenderFillRect(R, nullptr);
            if (sub_ok) {
                const uint8_t sub = static_cast<uint8_t>(std::min(steps, 8));
                A.SetRenderDrawBlendMode(R, sub_blend);
                A.SetRenderDrawColor(R, sub, sub, sub, 0);
                A.RenderFillRect(R, nullptr);
            }
            A.SetRenderDrawBlendMode(R, sdl::kBlendNone);
            for (float& d : dens) d *= f;
        }

        // ---- the beam ------------------------------------------------------------------------------------------------
        const float side = static_cast<float>(std::min(gpu.w, gpu.h));
        const float cx = gpu.w * 0.5f, cy = gpu.h * 0.5f;
        const float amp = side * 0.5f * 0.94f;
        // BEAM THICKNESS AND BRIGHTNESS of the scope window -- the numbers to adjust are the kBeam... constants above.
        const float big = std::clamp((side - 500.0f) / 600.0f, 0.0f, 1.0f);   // 0 below a 500 px window, 1 from 1100 px up
        const float beam_gain = kBeamGainSmall + kBeamGainBig * big;
        const float hw = std::max(kBeamMinHalfWidth, side * kBeamRelHalfWidth) * (1.0f + kBeamBigWidth * big) * (cfg.music ? 1.0f : 1.5f);   // half width of the beam
        {
            const float want_cell = std::max(4.0f, 6.0f * hw);
            const int ngw = std::max(1, static_cast<int>(std::ceil(gpu.w / want_cell)));
            const int ngh = std::max(1, static_cast<int>(std::ceil(gpu.h / want_cell)));
            if (ngw != gw || ngh != gh) { gw = ngw; gh = ngh; gcell = want_cell; dens.assign(static_cast<size_t>(gw) * gh, 0.0f); }
        }
        auto splat = [&](float x0, float y0, float x1, float y1, float len, float wgt) {
            const int k = std::max(1, static_cast<int>(std::ceil(len / (gcell * 0.5f))));
            const float add = std::max(len, hw) / k / gcell * wgt;
            for (int j = 0; j < k; ++j) {
                const float t = (j + 0.5f) / k;
                const int ix = std::clamp(static_cast<int>((x0 + (x1 - x0) * t) / gcell), 0, gw - 1);
                const int iy = std::clamp(static_cast<int>((y0 + (y1 - y0) * t) / gcell), 0, gh - 1);
                dens[static_cast<size_t>(iy) * gw + ix] += add;
            }
        };
        verts.clear();
        idx.clear();
        double spd_sum = 0.0;
        int spd_n = 0;
        auto colour = [&](float x, float spd_t, float a, sdl::Vertex& v) {
            int h;
            if (cfg.color_by_x) h = static_cast<int>(std::lround(std::clamp((x - (cx - amp)) / (2.0f * amp), 0.0f, 1.0f) * 255.0f));
            else h = static_cast<int>(std::lround(std::clamp(spd_t, 0.0f, 1.0f) * 255.0f));
            const uint8_t* p = cfg.pal[h];
            v.r = static_cast<uint8_t>(p[0] * 0.90f + 255 * 0.10f);   // a little white: dense parts of the trace burn white
            v.g = static_cast<uint8_t>(p[1] * 0.90f + 255 * 0.10f);
            v.b = static_cast<uint8_t>(p[2] * 0.90f + 255 * 0.10f);
            v.a = static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
        };
        auto quad = [&](float x0, float y0, float x1, float y1, float r, const sdl::Vertex& c0, const sdl::Vertex& c1, bool dot) {
            const int b = static_cast<int>(verts.size());
            sdl::Vertex v[4] = {c0, c0, c1, c1};
            if (dot) {
                v[0].x = x0 - r; v[0].y = y0 - r; v[0].u = 0; v[0].v = 0;
                v[1].x = x0 + r; v[1].y = y0 - r; v[1].u = 1; v[1].v = 0;
                v[2].x = x0 + r; v[2].y = y0 + r; v[2].u = 1; v[2].v = 1;
                v[3].x = x0 - r; v[3].y = y0 + r; v[3].u = 0; v[3].v = 1;
            } else {
                const float dx = x1 - x0, dy = y1 - y0;
                const float len = std::sqrt(dx * dx + dy * dy);
                const float nx = -dy / len * r, ny = dx / len * r;
                v[0].x = x0 + nx; v[0].y = y0 + ny; v[0].u = 0.5f; v[0].v = 0.0f;
                v[1].x = x0 - nx; v[1].y = y0 - ny; v[1].u = 0.5f; v[1].v = 1.0f;
                v[2].x = x1 - nx; v[2].y = y1 - ny; v[2].u = 0.5f; v[2].v = 1.0f;
                v[3].x = x1 + nx; v[3].y = y1 + ny; v[3].u = 0.5f; v[3].v = 0.0f;
            }
            for (auto& q : v) verts.push_back(q);
            for (int k : {0, 1, 2, 0, 2, 3}) idx.push_back(b + k);
        };
        for (size_t i = 0; i < n; ++i) {
            const float x = cx + std::clamp(xs[i] * gain, -1.05f, 1.05f) * amp;
            const float y = cy - std::clamp(ys[i] * gain, -1.05f, 1.05f) * amp;
            if (!have_last) { last_x = x; last_y = y; have_last = true; }
            const float dx = x - last_x, dy = y - last_y;
            const float len = std::sqrt(dx * dx + dy * dy);
            const float lspd = std::log10(len / side + 1e-5f);
            spd_sum += lspd;
            ++spd_n;
            const float spd_t = (lspd - spd_lo) / std::max(0.2f, spd_hi - spd_lo);
            float a = 1.0f;
            // Music mode: every segment stands for the same time, so its light is spread over its length -- slow strokes are
            // bright, the fast jumps between shapes (the extra lines) nearly vanish, as on a real CRT.
            if (cfg.crt) {
                if (cfg.dwell) a *= std::min(1.0f, kDwell * hw / std::max(len, 1e-3f));
                if (cfg.music && cfg.low_rate && rate < 96000.0) {
                    // below 96 kHz the jumps between shapes are rounded off to a few samples and ring a little: moves faster
                    // than strokes ever go (full scale widths per second) fade out, as in the terminal
                    const float v = len / amp * static_cast<float>(ups) * static_cast<float>(rate) / kBlankSpeed;
                    a /= 1.0f + v * v * v;
                }
                // the samples of this frame are not equally old: the earlier ones have already faded (Decay is per 1/30 s),
                // so a figure redrawn several times within one frame leaves one bright newest copy, not a fan of equal ones
                a *= std::pow(std::clamp(cfg.decay, 0.0f, 0.99f), static_cast<float>(dt * 30.0) * static_cast<float>(n - 1 - i) / static_cast<float>(n));
            }
            if (cfg.z) {
                float zf;
                if (cfg.z_source == 0) zf = 1.0f / (1.0f + len / (3.0f * hw));
                else zf = std::clamp(std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / amp, 0.0f, 1.0f);
                a *= (1.0f - cfg.z_depth) + cfg.z_depth * zf;
            }
            splat(last_x, last_y, x, y, len, a);
            a *= expo * beam_gain;
            sdl::Vertex c0{}, c1{};
            colour(last_x, spd_t, a, c0);
            colour(x, spd_t, a, c1);
            if (!cfg.interp) quad(x, y, x, y, hw * 1.6f, c1, c1, true);
            else if (len < 0.35f) quad(x, y, x, y, hw, c1, c1, true);
            else quad(last_x, last_y, x, y, hw, c0, c1, false);
            last_x = x;
            last_y = y;
        }
        if (n > 0) {   // exposure from the busiest 10 % of the places the beam has been
            dens_tmp.clear();
            for (float d : dens) if (d > 0.05f) dens_tmp.push_back(d);
            if (dens_tmp.size() >= 8) {
                // music mode: the busiest 1 % (the parts that stand still) set the exposure, so a figure that moves fast
                // keeps the faint light of its few passes instead of being lifted to full brightness
                const size_t at = dens_tmp.size() - 1 - dens_tmp.size() / (cfg.crt ? 100 : 10);
                std::nth_element(dens_tmp.begin(), dens_tmp.begin() + static_cast<long>(at), dens_tmp.end());
                const float busy = std::max(dens_tmp[at], 1e-3f);
                const float want = std::clamp(1.8f / busy, 0.02f, cfg.crt ? 40.0f : 0.90f);   // CRT: the dwell weight dims every segment
                expo += (want - expo) * std::min(1.0f, static_cast<float>(dt) * 4.0f);
            }
        }
        if (spd_n > 0) {   // the speed palette follows the music: the range spans the recent speeds
            const float m = static_cast<float>(spd_sum / spd_n);
            const float k = std::min(1.0f, static_cast<float>(dt) * 2.0f);
            spd_lo += (m - 0.9f - spd_lo) * k;
            spd_hi += (m + 0.9f - spd_hi) * k;
        }
        if (!idx.empty())
            A.RenderGeometry(R, gpu.beam, verts.data(), static_cast<int>(verts.size()), idx.data(), static_cast<int>(idx.size()));

        // ---- on screen: phosphor + bloom ------------------------------------------------------------------------------
        A.SetTextureBlendMode(gpu.accum, sdl::kBlendNone);
        A.SetTextureBlendMode(gpu.half, sdl::kBlendNone);
        A.SetTextureBlendMode(gpu.quarter, sdl::kBlendNone);
        A.SetRenderTarget(R, gpu.half);    A.RenderCopy(R, gpu.accum, nullptr, nullptr);
        A.SetRenderTarget(R, gpu.quarter); A.RenderCopy(R, gpu.half, nullptr, nullptr);
        A.SetRenderTarget(R, gpu.eighth);  A.RenderCopy(R, gpu.quarter, nullptr, nullptr);
        A.SetRenderTarget(R, nullptr);
        A.SetRenderDrawColor(R, 0, 0, 0, 255);
        A.RenderClear(R);
        A.RenderCopy(R, gpu.accum, nullptr, nullptr);
        if (kBeamAddLight > 0.0f) {
            A.SetTextureBlendMode(gpu.accum, sdl::kBlendAdd);
            for (float left = kBeamAddLight; left > 0.0f; left -= 1.0f) {
                A.SetTextureAlphaMod(gpu.accum, static_cast<uint8_t>(std::lround(std::min(left, 1.0f) * 255.0f)));
                A.RenderCopy(R, gpu.accum, nullptr, nullptr);
            }
            A.SetTextureAlphaMod(gpu.accum, 255);
            A.SetTextureBlendMode(gpu.accum, sdl::kBlendNone);
        }
        const float glow = std::clamp(cfg.glow, 0.0f, 1.0f);
        if (glow > 0.0f) {
            A.SetTextureBlendMode(gpu.quarter, sdl::kBlendAdd);
            A.SetTextureBlendMode(gpu.eighth, sdl::kBlendAdd);
            // the shrunk copies are faint (a thin line is averaged with its dark surroundings): added more than once
            const float g = glow * 2.2f;
            for (int pass = 0; pass < 2; ++pass) {
                const float gq = std::clamp(g * 0.8f - pass, 0.0f, 1.0f), ge = std::clamp(g - pass, 0.0f, 1.0f);
                if (gq > 0.0f) { A.SetTextureAlphaMod(gpu.quarter, static_cast<uint8_t>(std::lround(gq * 255.0f))); A.RenderCopy(R, gpu.quarter, nullptr, nullptr); }
                if (ge > 0.0f) { A.SetTextureAlphaMod(gpu.eighth, static_cast<uint8_t>(std::lround(ge * 255.0f))); A.RenderCopy(R, gpu.eighth, nullptr, nullptr); }
            }
        }
        ++frame_no;
        if (shot_path && frame_no == shot_frame) {
            std::vector<uint32_t> px(static_cast<size_t>(gpu.w) * gpu.h);
            if (A.RenderReadPixels(R, nullptr, sdl::kFmtARGB8888, px.data(), gpu.w * 4) == 0) write_ppm(shot_path, px, gpu.w, gpu.h);
            if (shot_exit) quit = true;
        }
        A.RenderPresent(R);
        // without vsync (some drivers ignore it) do not spin faster than 240 frames per second
        const double spent = static_cast<double>(A.GetPerformanceCounter() - t_now) / freq;
        if (spent < 1.0 / 240.0) A.Delay(static_cast<uint32_t>((1.0 / 240.0 - spent) * 1000.0));
    }

    save_geometry(geo);
    destroy(A, gpu);
    A.DestroyRenderer(R);
    A.DestroyWindow(win);
    A.Quit();
    return 0;
}

} // namespace muisc
