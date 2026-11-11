// The spectrogram window process (`mousiki --spectro-window`): reads the decoded signal and the spectrogram options from
// its standard input (sent by scope_window.cpp, like the scope window) and draws the spectrogram with the graphics card.
//
// The analysis is the very same as in the terminal (SpectroAnalyzer: Audacity's way and defaults), done here in the window
// process. The columns go into a texture used as a ring (one texel column per spectrogram column, as high as the picture
// on screen), and every frame -- at the monitor's refresh rate -- that texture is drawn with a sub-pixel offset, so the
// picture scrolls perfectly smoothly: in a terminal a picture can only move in whole pixels or cells. The view follows
// a clock (not the bursts in which audio arrives), slightly behind the newest column.
#include "app_dirs.h"
#include "scope_window.h"
#include "scope_sdl.h"
#include "spectrogram.h"
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

struct Shared {
    std::mutex m;
    SpectroWinConfig cfg;
    unsigned cfg_gen = 0;
    std::atomic<int> rate{48000};
    std::atomic<bool> eof{false};
};

void write_error(const std::string& msg) {
    std::error_code ec;
    const std::filesystem::path dir = muisc::app_dirs::cache();
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "spectro_window_error.txt") << msg << "\n";
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

// Reads packets until the input ends (the player closed the window or quit). Samples go straight into the analyzer's
// lock-free feed (this thread is its only producer; the drawing loop is its consumer).
void reader_main(Shared* S) {
    std::vector<char> payload;
    for (;;) {
        uint8_t head[5];
        if (!read_exact(head, sizeof head)) break;
        const uint32_t len = static_cast<uint32_t>(head[1]) | (static_cast<uint32_t>(head[2]) << 8) |
                             (static_cast<uint32_t>(head[3]) << 16) | (static_cast<uint32_t>(head[4]) << 24);
        if (len > (64u << 20)) break;
        payload.resize(len);
        if (len > 0 && !read_exact(payload.data(), len)) break;
        if (head[0] == 1 && len == sizeof(SpectroWinConfig)) {
            SpectroWinConfig c;
            std::memcpy(&c, payload.data(), sizeof c);
            if (c.magic != SpectroWinConfig{}.magic) continue;
            S->rate.store(std::max(8000, static_cast<int>(c.rate)));
            std::lock_guard<std::mutex> lk(S->m);
            S->cfg = c;
            ++S->cfg_gen;
        } else if (head[0] == 2) {
            const size_t n = len / (2 * sizeof(float));
            std::vector<float> f(2 * n);   // (copied: the byte buffer is not guaranteed to be aligned for floats)
            if (n > 0) std::memcpy(f.data(), payload.data(), f.size() * sizeof(float));
            spectro().push(f.data(), n, S->rate.load());
        }
    }
    S->eof.store(true);
}

// ---- window position and size, kept between runs ----------------------------------------------------------------------
struct Geometry { int x = sdl::kPosCentered, y = sdl::kPosCentered, w = 1100, h = 520; bool full = false, top = false; };

std::filesystem::path geometry_file() { return muisc::app_dirs::config() / "spectro_window.txt"; }

Geometry load_geometry(const sdl::Api& A) {
    Geometry g;
    std::ifstream in(geometry_file());
    int x, y, w, h, f, t;
    if (in >> x >> y >> w >> h >> f >> t) {
        g.w = std::clamp(w, 200, 8192);
        g.h = std::clamp(h, 120, 8192);
        g.full = f != 0;
        g.top = t != 0;
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

void write_ppm(const std::string& path, const std::vector<uint32_t>& px, int w, int h) {
    std::ofstream o(path, std::ios::binary);
    o << "P6\n" << w << " " << h << "\n255\n";
    for (uint32_t v : px) { const char c[3] = {static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)}; o.write(c, 3); }
}

// Digits 3 x 5 (the frequency labels are plain numbers), one row per byte, bit 2 = left.
const uint8_t kDigits[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};

// "k" (thousand) for the waterfall's frequency axis
const uint8_t kGlyphK[5] = {5, 5, 6, 5, 5};

} // namespace

int spectro_window_main() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    SpectroAnalyzer& an = spectro();
    an.set_visible(true);   // the feed is on from the start
    Shared& S = *new Shared;   // never freed: the reader thread may still sit in fread() while the window shuts down
    std::thread(reader_main, &S).detach();

    sdl::Api A;
    std::string err;
    if (!sdl::load(A, &err)) { write_error(err); return 2; }
    A.SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    A.SetHint("SDL_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR", "0");
    A.SetHint("SDL_RENDER_SCALE_QUALITY", "1");
#if defined(_WIN32)
    A.SetHint("SDL_RENDER_DRIVER", "direct3d11");
#endif
    if (A.Init(sdl::kInitVideo) != 0) { write_error(std::string("spectrogram window: SDL could not start the video system: ") + A.GetError()); return 3; }

    Geometry geo = load_geometry(A);
    sdl::Window* win = A.CreateWin("Mousiki spectrogram", geo.x, geo.y, geo.w, geo.h, sdl::kWinResizable | sdl::kWinHighDpi | sdl::kWinShown);
    if (!win) { write_error(std::string("spectrogram window: cannot open a window: ") + A.GetError()); A.Quit(); return 4; }
    sdl::Renderer* R = A.CreateRenderer(win, -1, sdl::kRenAccelerated | sdl::kRenVsync);
    if (!R) { A.SetHint("SDL_RENDER_DRIVER", ""); R = A.CreateRenderer(win, -1, sdl::kRenAccelerated | sdl::kRenVsync); }
    if (!R) R = A.CreateRenderer(win, -1, sdl::kRenVsync);
    if (!R) { write_error(std::string("spectrogram window: no usable renderer: ") + A.GetError()); A.DestroyWindow(win); A.Quit(); return 5; }
    if (geo.full) A.SetWindowFullscreen(win, sdl::kWinFullscreenDesktop);
    if (geo.top && A.SetWindowAlwaysOnTop) A.SetWindowAlwaysOnTop(win, 1);

    const char* shot_path = std::getenv("MOUSIKI_SPECTRO_SHOT");   // tests: save frame N as a PPM picture
    const int shot_frame = std::getenv("MOUSIKI_SPECTRO_SHOT_FRAME") ? muisc::atoi_safe(std::getenv("MOUSIKI_SPECTRO_SHOT_FRAME")) : 240;
    const bool shot_exit = std::getenv("MOUSIKI_SPECTRO_SHOT_EXIT") != nullptr;

    constexpr int kTexW = 2048;          // texel columns: more than one page (1024 columns) plus what arrives meanwhile
    sdl::Texture* tex = nullptr;
    int tex_h = 0;
    uint64_t tex_upto = 0;               // the next column to put into the texture
    std::string tex_key;                 // what the texture's content depends on (settings, colours, height)
    std::vector<uint8_t> vals;
    std::vector<uint32_t> px;
    std::vector<sdl::Vertex> verts;
    std::vector<int> idx;
    sdl::Texture* ptex = nullptr;        // the waterfall's palette: one row of 256 colours
    std::string ptex_key;
    sdl::Texture* btex = nullptr; int btex_w = 0, btex_h = 0;
    std::vector<uint8_t> bimg;
    std::vector<uint32_t> bpx;
    std::vector<SpectroAnalyzer::WfGraph> wgraphs;
    std::vector<sdl::Vertex> wverts;
    std::vector<int> widx;
    std::vector<sdl::Point> wpts;
    SpectroWinConfig cfg;
    unsigned cfg_seen = ~0u;
    std::string title_set;
    const double freq = static_cast<double>(A.GetPerformanceFrequency());
    long frame_no = 0;
    bool quit = false;
    alignas(8) unsigned char ev[128];

    while (!quit) {
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
                // 8 / 9: the key of "(" (German SHIFT+8, English SHIFT+9) closes it as in the terminal
                if (sym == sdl::kKeyEsc || sym == 'q' || sym == '8' || sym == '9') quit = true;
                else if (sym == 'f' || sym == sdl::kKeyF11) {
                    const bool full = (A.GetWindowFlags(win) & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
                    A.SetWindowFullscreen(win, full ? 0 : sdl::kWinFullscreenDesktop);
                } else if (sym == 't' && A.SetWindowAlwaysOnTop) {
                    geo.top = !geo.top;
                    A.SetWindowAlwaysOnTop(win, geo.top ? 1 : 0);
                }
            } else if (type == sdl::kEvMouseDown) {
                if (ev[18] >= 2) {
                    const bool full = (A.GetWindowFlags(win) & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
                    A.SetWindowFullscreen(win, full ? 0 : sdl::kWinFullscreenDesktop);
                }
            } else if (type == sdl::kEvTargetsReset || type == sdl::kEvDeviceReset) { resized = true; tex_key.clear(); }
        }
        if (S.eof.load()) quit = true;
        if (quit) break;
        {
            const uint32_t fl = A.GetWindowFlags(win);
            geo.full = (fl & sdl::kWinFullscreenDesktop) == sdl::kWinFullscreenDesktop;
            if (!geo.full && !(fl & sdl::kWinMinimized)) { A.GetWindowPosition(win, &geo.x, &geo.y); A.GetWindowSize(win, &geo.w, &geo.h); }
            if (fl & (sdl::kWinMinimized | sdl::kWinHidden)) {   // keep analysing (the feed must not overflow), draw nothing
                std::lock_guard<std::mutex> lk(S.m);
                an.update(spectro_win_unpack(S.cfg));
                A.Delay(30);
                continue;
            }
        }
        (void)resized;
        const uint64_t t_now = A.GetPerformanceCounter();

        // ---- settings, analysis ---------------------------------------------------------------------------------------
        {
            std::lock_guard<std::mutex> lk(S.m);
            if (S.cfg_gen != cfg_seen) { cfg = S.cfg; cfg_seen = S.cfg_gen; }
        }
        const SpectroSettings s = spectro_win_unpack(cfg);
        an.update(s);
        {
            std::string t = std::string(cfg.title, strnlen(cfg.title, sizeof cfg.title));
            t = t.empty() ? "Mousiki spectrogram" : t + "  -  Mousiki spectrogram";
            if (t != title_set) { A.SetWindowTitle(win, t.c_str()); title_set = t; }
        }

        // ---- layout: frequency labels on the left, the picture beside them -------------------------------------------
        int ow = 0, oh = 0;
        A.GetRendererOutputSize(R, &ow, &oh);
        const int sc = std::max(2, oh / 260);                       // pixel size of the label font
        const int lw = s.labels ? 5 * 4 * sc + 3 * sc : 0;          // "20000" + a margin
        const int pw = std::max(1, ow - lw), ph = std::max(1, oh);
        const int rate = an.rate();
        const uint64_t cpp = static_cast<uint64_t>(an.columns_per_page());
        const uint64_t ncols = an.columns();

        // ---- the waterfall: a picture of its own, made from the analysis for every frame --------------------------------
        if (s.view == 1) {
            const int strip = s.labels ? 8 * sc : 0;                       // frequencies under the picture
            const int wpw = std::max(16, ow), wph = std::max(16, oh - strip);
            const bool braille = s.style == 0;      // SHIFT+i style braille: coloured lines of dots on black, image: the filled graphs with a black outline
            // The graphs are drawn by the graphics card (filled triangles coloured from a one-row palette texture, the outline as lines):
            // the processor only works out where the corners are, which is a few thousand points, so a full-screen window costs the
            // same as a small one and even an integrated chip keeps up with the monitor's refresh rate.
            if (!ptex) {
                ptex = A.CreateTexture(R, sdl::kFmtARGB8888, sdl::kTexStatic, 256, 1);
                if (!ptex) { write_error(std::string("spectrogram window: cannot create a texture: ") + A.GetError()); break; }
                A.SetTextureBlendMode(ptex, sdl::kBlendNone);
                if (A.SetTextureScaleMode) A.SetTextureScaleMode(ptex, sdl::kScaleLinear);
                ptex_key.clear();
            }
            {
                const std::string pk(reinterpret_cast<const char*>(cfg.pal), sizeof cfg.pal);
                if (pk != ptex_key) {
                    uint32_t row[256];
                    for (int i = 0; i < 256; ++i) row[i] = 0xFF000000u | (static_cast<uint32_t>(cfg.pal[i][0]) << 16) | (static_cast<uint32_t>(cfg.pal[i][1]) << 8) | cfg.pal[i][2];
                    A.UpdateTexture(ptex, nullptr, row, 256 * 4);
                    ptex_key = pk;
                }
            }
            if (braille) {
                // The braille look, the same picture as in the terminal (outlines only, a line of dots in the colour of its height): the
                // dot grid of braille_cells (2 x 4 dots to a cell, every dot a square of `dot` pixels), one colour for each cell.
                const int dot = std::max(3, wph / 270);
                const int bw = std::max(2, wpw / dot / 2 * 2), bh = std::max(4, wph / dot / 4 * 4);
                if (!btex || btex_w != bw || btex_h != bh) {
                    if (btex) A.DestroyTexture(btex);
                    btex = A.CreateTexture(R, sdl::kFmtARGB8888, sdl::kTexStatic, bw, bh);
                    if (!btex) { write_error(std::string("spectrogram window: cannot create a texture: ") + A.GetError()); break; }
                    A.SetTextureBlendMode(btex, sdl::kBlendNone);
                    if (A.SetTextureScaleMode) A.SetTextureScaleMode(btex, sdl::kScaleNearest);
                    btex_w = bw; btex_h = bh;
                }
                SpectroSettings sb = s;
                sb.wf_smooth = false;   // the dot grid moves a graph back one place at a time, as in the terminal (the glide, rounded to dots, made the rows shake back and forth)
                an.waterfall(sb, bw, bh, true, bimg);
                bpx.assign(static_cast<size_t>(bw) * bh, 0xFF000000u);
                for (int cy = 0; cy < bh; cy += 4)
                    for (int cx = 0; cx < bw; cx += 2) {
                        int mx = 0;
                        for (int dy = 0; dy < 4; ++dy)
                            for (int dx = 0; dx < 2; ++dx) mx = std::max<int>(mx, bimg[static_cast<size_t>(cy + dy) * bw + cx + dx]);
                        if (mx == 0) continue;
                        const uint32_t col = 0xFF000000u | (static_cast<uint32_t>(cfg.pal[mx][0]) << 16) | (static_cast<uint32_t>(cfg.pal[mx][1]) << 8) | cfg.pal[mx][2];
                        for (int dy = 0; dy < 4; ++dy)
                            for (int dx = 0; dx < 2; ++dx)
                                if (bimg[static_cast<size_t>(cy + dy) * bw + cx + dx]) bpx[static_cast<size_t>(cy + dy) * bw + cx + dx] = col;
                    }
                A.UpdateTexture(btex, nullptr, bpx.data(), bw * 4);
                A.SetRenderDrawColor(R, 0, 0, 0, 255);
                A.RenderClear(R);
                const sdl::Rect dst{0, 0, bw * dot, bh * dot};
                A.RenderCopy(R, btex, nullptr, &dst);
            }
            int fwp = 0, amp = 1;
            if (!braille) {
            an.waterfall_layout(s, wpw, wph, wgraphs, fwp, amp);
            A.SetRenderDrawColor(R, 0, 0, 0, 255);
            A.RenderClear(R);
            const float u0 = (48 + 0.5f) / 256.0f;
            const int thick = wph >= 1300 ? 2 : 1;   // a fine line stays fine on a big screen
            for (const auto& G : wgraphs) {
                wverts.clear(); widx.clear(); wpts.clear();
                const int n = static_cast<int>(G.top.size());
                for (int x = 0; x < n; ++x) {
                    const float px = static_cast<float>(G.ox + x);
                    const int top = G.top[static_cast<size_t>(x)];
                    const float fr = std::clamp(static_cast<float>(G.baseline - top) / amp, 0.0f, 1.0f);
                    const float ut = (48.0f + fr * 207.0f + 0.5f) / 256.0f;
                    const int vi = static_cast<int>(wverts.size());
                    const uint8_t cc = 255;
                    wverts.push_back({px, static_cast<float>(top), cc, cc, cc, 255, ut, 0.5f});
                    wverts.push_back({px, static_cast<float>(G.baseline), cc, cc, cc, 255, u0, 0.5f});
                    wverts.push_back({px, static_cast<float>(G.bottom + 1), cc, cc, cc, 255, u0, 0.5f});
                    if (x > 0) {
                        const int p = vi - 3;
                        const int q[12] = {p, p + 1, vi, p + 1, vi + 1, vi, p + 1, p + 2, vi + 1, p + 2, vi + 2, vi + 1};
                        widx.insert(widx.end(), q, q + 12);
                    }
                }
                if (!widx.empty()) A.RenderGeometry(R, ptex, wverts.data(), static_cast<int>(wverts.size()), widx.data(), static_cast<int>(widx.size()));
                // the outline: along the curve, down both ends and along the lower edge
                wpts.push_back({G.ox, G.bottom});
                for (int x = 0; x < n; ++x) wpts.push_back({G.ox + x, G.top[static_cast<size_t>(x)]});
                wpts.push_back({G.ox + n - 1, G.bottom});
                wpts.push_back({G.ox, G.bottom});
                A.SetRenderDrawColor(R, 0, 0, 0, 255);
                for (int k = 0; k < thick; ++k) {
                    A.RenderDrawLines(R, wpts.data(), static_cast<int>(wpts.size()));
                    if (k + 1 < thick) for (auto& pt : wpts) pt.y += 1;
                }
            }
            }
            if (s.labels && rate > 0) {   // the frequencies of the front graph, under it
                A.SetRenderDrawColor(R, 190, 190, 190, 255);
                for (const auto& [col, text] : spectro_axis_x(s, rate, wpw, 4 * sc)) {
                    int x = col * 4 * sc;
                    for (char ch : text) {
                        const uint8_t* g = nullptr;
                        if (ch >= '0' && ch <= '9') g = kDigits[ch - '0']; else if (ch == 'k') g = kGlyphK;
                        if (g)
                            for (int gy = 0; gy < 5; ++gy)
                                for (int gx = 0; gx < 3; ++gx)
                                    if (g[gy] & (4 >> gx)) { const sdl::Rect r{x + gx * sc, wph + sc + gy * sc, sc, sc}; A.RenderFillRect(R, &r); }
                        x += 4 * sc;
                    }
                }
            }
            ++frame_no;
            if (shot_path && frame_no == shot_frame) {
                std::vector<uint32_t> shot(static_cast<size_t>(ow) * oh);
                if (A.RenderReadPixels(R, nullptr, sdl::kFmtARGB8888, shot.data(), ow * 4) == 0) write_ppm(shot_path, shot, ow, oh);
                if (shot_exit) quit = true;
            }
            A.RenderPresent(R);
            const double spent_wf = static_cast<double>(A.GetPerformanceCounter() - t_now) / freq;
            if (spent_wf < 1.0 / 240.0) A.Delay(static_cast<uint32_t>((1.0 / 240.0 - spent_wf) * 1000.0));
            continue;
        }

        // ---- the texture: new columns in, everything again when the look changed -------------------------------------
        if (!tex || tex_h != ph) {
            if (tex) A.DestroyTexture(tex);
            tex = A.CreateTexture(R, sdl::kFmtARGB8888, sdl::kTexStatic, kTexW, ph);
            if (!tex) { write_error(std::string("spectrogram window: cannot create a texture: ") + A.GetError()); break; }
            if (A.SetTextureScaleMode) A.SetTextureScaleMode(tex, sdl::kScaleLinear);
            A.SetTextureBlendMode(tex, sdl::kBlendNone);
            tex_h = ph;
            tex_key.clear();
        }
        std::string key;
        {
            char b[256];
            std::snprintf(b, sizeof b, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d", s.scale, s.min_freq, s.max_freq, s.gain, s.range,
                          s.freq_gain, s.window_log2, s.zero_pad, s.scheme, s.channels, s.span, an.generation(), ph);
            key = b;
            key.append(reinterpret_cast<const char*>(cfg.pal), sizeof cfg.pal);
        }
        if (key != tex_key || ncols < tex_upto) {
            tex_key = key;
            // all that is kept and was made with these settings; the rest of the ring black
            const uint64_t keep = std::min<uint64_t>(ncols, cpp + 32);
            tex_upto = ncols - keep;
            px.assign(static_cast<size_t>(kTexW) * static_cast<size_t>(ph), 0xFF000000u);
            A.UpdateTexture(tex, nullptr, px.data(), kTexW * 4);
        }
        if (ncols > tex_upto) {
            if (ncols - tex_upto > static_cast<uint64_t>(kTexW)) tex_upto = ncols - kTexW;
            uint64_t c = tex_upto;
            while (c < ncols) {   // in pieces that do not cross the end of the ring
                const uint64_t at = c % kTexW;
                const uint64_t e = std::min<uint64_t>(ncols, c + (kTexW - at));
                const int w = static_cast<int>(e - c);
                an.column_images(s, c, e, ph, vals);
                px.resize(static_cast<size_t>(w) * static_cast<size_t>(ph));
                for (int y = 0; y < ph; ++y)
                    for (int i = 0; i < w; ++i) {
                        const uint8_t* p = cfg.pal[vals[static_cast<size_t>(i) * static_cast<size_t>(ph) + static_cast<size_t>(y)]];
                        px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(i)] =
                            0xFF000000u | (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
                    }
                const sdl::Rect r{static_cast<int>(at), 0, w, ph};
                A.UpdateTexture(tex, &r, px.data(), w * 4);
                c = e;
            }
            tex_upto = ncols;
        }
        const uint64_t oldest = ncols > static_cast<uint64_t>(kTexW) ? ncols - kTexW + 1 : 0;   // still in the ring

        // ---- draw ---------------------------------------------------------------------------------------------------
        A.SetRenderDrawColor(R, 0, 0, 0, 255);
        A.RenderClear(R);
        verts.clear();
        idx.clear();
        const double px_per_col = static_cast<double>(pw) / static_cast<double>(cpp);
        // columns [c0, c1) of the history at screen x = lw + (column - origin) * px_per_col
        auto span = [&](double c0, double c1, double origin) {
            c0 = std::max(c0, static_cast<double>(oldest));
            c1 = std::min(c1, static_cast<double>(ncols));
            while (c1 - c0 > 1e-6) {
                const double ring0 = std::floor(c0 / kTexW) * kTexW;
                const double e = std::min(c1, ring0 + kTexW);
                const float x0 = static_cast<float>(lw + (c0 - origin) * px_per_col), x1 = static_cast<float>(lw + (e - origin) * px_per_col);
                const float u0 = static_cast<float>((c0 - ring0) / kTexW), u1 = static_cast<float>((e - ring0) / kTexW);
                const int b = static_cast<int>(verts.size());
                verts.push_back({x0, 0.0f, 255, 255, 255, 255, u0, 0.0f});
                verts.push_back({x1, 0.0f, 255, 255, 255, 255, u1, 0.0f});
                verts.push_back({x1, static_cast<float>(ph), 255, 255, 255, 255, u1, 1.0f});
                verts.push_back({x0, static_cast<float>(ph), 255, 255, 255, 255, u0, 1.0f});
                for (int k : {0, 1, 2, 0, 2, 3}) idx.push_back(b + k);
                c0 = e;
            }
        };
        const double disp = an.display_columns();
        double head_x = -1;
        if (s.motion == 1) {
            span(disp - static_cast<double>(cpp), disp, disp - static_cast<double>(cpp));   // scroll: the newest on the right
        } else {
            // sweep: the current page from the left up to the playhead, the rest of the previous page after a short gap
            const double head = std::fmod(disp, static_cast<double>(cpp));
            const double page = disp - head;
            const double gap = std::max(2.0, pw / 80.0) / px_per_col;
            span(page, disp, page);
            span(page - static_cast<double>(cpp) + head + gap, page, page - static_cast<double>(cpp));
            head_x = lw + head * px_per_col;
        }
        if (!idx.empty()) A.RenderGeometry(R, tex, verts.data(), static_cast<int>(verts.size()), idx.data(), static_cast<int>(idx.size()));
        if (head_x >= 0) {
            const uint8_t* p = cfg.pal[200];
            A.SetRenderDrawColor(R, p[0], p[1], p[2], 255);
            const sdl::Rect r{static_cast<int>(head_x), 0, std::max(1, sc / 2), ph};
            A.RenderFillRect(R, &r);
        }
        if (s.labels && rate > 0) {   // frequency labels, Audacity's round numbers, right-aligned
            const int unit = 8 * sc;
            A.SetRenderDrawColor(R, 0, 0, 0, 255);
            const sdl::Rect bg{0, 0, lw, oh};
            A.RenderFillRect(R, &bg);
            A.SetRenderDrawColor(R, 190, 190, 190, 255);
            for (const auto& [row, text] : spectro_axis(s, rate, ph, unit)) {
                const int y = std::clamp(row * unit + (unit - 5 * sc) / 2, 0, std::max(0, oh - 5 * sc));
                int x = lw - 3 * sc - static_cast<int>(text.size()) * 4 * sc + sc;
                for (char ch : text) {
                    if (ch >= '0' && ch <= '9')
                        for (int gy = 0; gy < 5; ++gy)
                            for (int gx = 0; gx < 3; ++gx)
                                if (kDigits[ch - '0'][gy] & (4 >> gx)) {
                                    const sdl::Rect r{x + gx * sc, y + gy * sc, sc, sc};
                                    A.RenderFillRect(R, &r);
                                }
                    x += 4 * sc;
                }
                const sdl::Rect tick{lw - 2 * sc, std::clamp(row * unit + unit / 2, 0, oh - 1), sc, 1};
                A.RenderFillRect(R, &tick);
            }
        }
        ++frame_no;
        if (shot_path && frame_no == shot_frame) {
            std::vector<uint32_t> shot(static_cast<size_t>(ow) * oh);
            if (A.RenderReadPixels(R, nullptr, sdl::kFmtARGB8888, shot.data(), ow * 4) == 0) write_ppm(shot_path, shot, ow, oh);
            if (shot_exit) quit = true;
        }
        A.RenderPresent(R);
        const double spent = static_cast<double>(A.GetPerformanceCounter() - t_now) / freq;
        if (spent < 1.0 / 240.0) A.Delay(static_cast<uint32_t>((1.0 / 240.0 - spent) * 1000.0));
    }

    save_geometry(geo);
    if (tex) A.DestroyTexture(tex);
    if (ptex) A.DestroyTexture(ptex);
    if (btex) A.DestroyTexture(btex);
    A.DestroyRenderer(R);
    A.DestroyWindow(win);
    A.Quit();
    return 0;
}

} // namespace muisc
