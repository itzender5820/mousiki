#pragma once
// Terminal graphics for the oscilloscope's "image" style: the picture is a real pixel image drawn by the terminal itself
// (the way yazi shows previews). Two protocols are spoken:
//   Kitty graphics  Kitty, WezTerm, Ghostty, Konsole, ...   RGBA with alpha, drawn UNDER the text, replaced in place
//   Sixel           Windows Terminal (1.22+), foot, xterm -ti vt340, mlterm, WezTerm, ...   palette image over the cells
// Without either, the UI keeps drawing the braille scope.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace muisc {

enum class GfxProto { None, Kitty, Sixel };

// The finished picture of one frame, filled by the UI (app.cpp) and turned into escape codes by gfx_emit().
struct GfxFrame {
    bool active = false;            // an image belongs on screen this frame (false: remove it)
    bool plain = false;             // a picture, not a beam: the colour as it is (no white-hot core), nothing painted behind it (Sixel)
    bool direct = false;            // with `plain`: `hue` is an index into `pal` (at most 240 colours) taken as it is, `level` only the opacity
    bool fresh = false;             // `level` / `hue` hold new pixels (false: the picture of the last frame stays)
    int col = 0, row = 0;           // top-left cell, 0-based
    int cols = 0, rows = 0;         // size in cells
    int crop = 0;                   // cells at the left that an overlay covers and that stay free
    int w = 0, h = 0;               // size of the picture in pixels (with scale > 1 smaller than the screen area)
    int scale = 1;                  // the picture has 1/scale of the screen resolution and is enlarged on the way out:
                                    // Kitty scales it itself (c= / r=), Sixel repeats the pixels (vertically via the pixel
                                    // aspect ratio where the terminal honours it, i.e. Windows Terminal)
    int out_cw = 0, out_ch = 0;     // Sixel: screen pixels of one cell (0 = w / cols, h / rows)
    bool truecolor = false;         // with `plain` + `direct` (Kitty only): `rgb` holds the picture's own colours (w * h * 3), `level` only the
                                    // opacity (0 = transparent: the cut rectangle); `hue` / `pal` are not used
    std::vector<uint8_t> rgb;
    std::vector<uint8_t> level;     // brightness 0..255, w * h
    std::vector<uint8_t> hue;       // colour index 0..255, w * h (see `pal`)
    std::array<std::array<uint8_t, 3>, 256> pal{};   // hue -> colour at full brightness
    // A second, third ... picture at the same time (the big oscilloscope overlay shows two): Kitty image id offset (0, 1, ...).
    int id = 0;
    // Optional grid layer, w * h (empty = none): non-zero pixels are drawn in `grid_rgb` wherever the trace is faint, so the grid
    // sits under the beam. (The braille style puts the grid in text cells; a picture covers its cells, so it carries its own.)
    std::vector<uint8_t> grid;
    std::array<uint8_t, 3> grid_rgb{128, 128, 128};
    // A rectangle of cells, relative to col / row, that something else covers (a floating panel): the caller has cleared its
    // pixels; Sixel must not erase its cells either. cut_w = 0: none.
    int cut_c = 0, cut_r = 0, cut_w = 0, cut_h = 0;
    int cut2_c = 0, cut2_r = 0, cut2_w = 0, cut2_h = 0;   // a second one (the video: the lyric box beside a floating panel); cut2_w = 0: none
    // With `plain` (a picture, no beam), Sixel only: what happens to the pixels that are not part of the picture.
    //   opaque: they are painted black (the terminal is black: Settings -> Black Background), so the picture replaces what was there
    //           -- the last picture of an animation, the text under it. Cells in the cut rectangle stay as they are.
    //   erase:  the terminal keeps its own background colour, so the picture's cells are blanked with spaces first (in the same
    //           write; the cut rectangle is left out) and the picture is drawn on top of the empty cells.
    // Neither: the pixels stay transparent (a still picture such as the audio book's).
    bool opaque = false;
    bool erase = false;
};

// Which protocol does this terminal speak? `pref` = "auto" | "kitty" | "sixel" | "off" (the environment variable
// MOUSIKI_GFX overrides it). "auto" asks the terminal (a Kitty graphics query plus the device attributes, 250 ms at
// most) and looks at the environment. Call once at start, with the terminal already in raw mode.
GfxProto gfx_probe(const std::string& pref);
// Pixels per cell: the terminal's report, `override_wxh` ("10x20"), or a guess.
void gfx_cell_pixels(const std::string& override_wxh, int& w, int& h);
// Escape codes that put the picture on screen (cursor moves included; the cursor position is not kept) / take it away.
std::string gfx_emit(GfxProto proto, const GfxFrame& f);
std::string gfx_clear(GfxProto proto, int id = 0);   // id: the picture's GfxFrame::id
const char* gfx_name(GfxProto proto);
bool gfx_compressed();
// Placeholder cells for the picture in a text frame (n columns) and their replacement before the frame is written:
// skip = true moves the cursor over them (Sixel: the picture on screen stays), false writes spaces.
std::string gfx_hole(int n);
void gfx_fill_holes(std::string& s, bool skip);
// The most pictures per second sent to the terminal (Kitty 60; Sixel 30 at full resolution, 60 at half / third).
int gfx_picture_cap(GfxProto proto, int scale);   // Kitty pictures are always zlib-compressed (miniz): cheap enough to send every frame

} // namespace muisc
