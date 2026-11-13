// Terminal handling (see terminal_ui.cpp): key codes, the screen interface, text width helpers and the
// terminal size limits.
#pragma once
#include <algorithm>
#include <string>
#include <vector>

namespace muisc {

// Non-ASCII-range sentinel for the Home key (VT/xterm "\x1b[H", or
// VK_HOME on Windows) -- returned by poll_key() same as any other key.
// Deliberately outside 0-255 so it can never collide with a raw
// (possibly UTF-8 continuation) byte value coming through the same
// channel, unlike 'A'/'B'/'C'/'D' (the arrow keys), which *do* sit
// inside the printable-ASCII range and so still have to be explicitly
// filtered out of any text-entry field that doesn't want them typed
// literally (see e.g. Mode::Search's and Mode::BulkAdd's key handling).
constexpr int kKeyHome = 300;

// Same idea, for the forward-Delete key (VK_DELETE on Windows, xterm's
// "\x1b[3~" elsewhere) -- distinct from Backspace (127), which is a
// different physical key that some overlays additionally accept as a
// delete/remove action.
constexpr int kKeyDelete = 301;

// Ctrl+Shift+S and Ctrl+Shift+X -- the meta editor's "apply this editing
// session to the files" and "throw this editing session away" commands.
// These are modifier COMBINATIONS, so unlike a plain letter they can't be
// expressed as a hotkey string (hotkey_string_to_key() has no representation
// for them, and every letter code it could produce would be ambiguous with
// the arrow-key letters anyway), which is why they arrive as their own
// sentinel values rather than through settings_.hotkeys. On Windows they
// come from win_poll_key() reading dwControlKeyState; on POSIX from the
// xterm modifyOtherKeys/CSI-u encoding of a modified key.
// Ctrl+Shift+D is not used: Windows Terminal takes it for "duplicate tab" before the program
// sees it. The discard command is Ctrl+Shift+X, which Windows Terminal does not bind.
constexpr int kKeyCtrlShiftS = 302;
constexpr int kKeyCtrlShiftX = 303;

// Text-editing keys, the same idea again: Shift+Left/Right and Ctrl+C/X/V
// are modifier combinations, so they can't be hotkey strings and they can't
// be the bare 'A'-'D'/'c'/'x'/'v' values either (an arrow already collapsed
// to those letters, and a typed capital must still be typable into a field).
// They exist so the single-line editors (the meta editor's field editor and
// the Settings ColorEdit buffer) can move a caret, mark a selection and use
// the clipboard. They are ONLY ever meaningful inside those fields -- every
// other handler simply ignores a value it doesn't know, exactly like
// kKeyHome/kKeyDelete already do.
constexpr int kKeyShiftLeft = 304;  // Shift+Left  -- extend the selection left
constexpr int kKeyShiftRight = 305; // Shift+Right -- extend the selection right
constexpr int kKeyCtrlC = 306;      // Ctrl+C      -- copy
constexpr int kKeyCtrlX = 307;      // Ctrl+X      -- cut
constexpr int kKeyCtrlV = 308;      // Ctrl+V      -- paste
constexpr int kKeyEnd = 309;        // End (VK_END / xterm "ESC [ F")

// Alt+Left / Alt+Right -- the playlist editor's "switch tab" command.
// Plain Left/Right had to stay the caret keys in every one of that
// screen's text fields (name field, both search boxes) and its track
// list uses them for nothing at all, which meant a bare arrow could only
// ever switch tabs from the one pane that doesn't own a caret -- tabs
// were unreachable from the name field entirely. This was originally
// Ctrl+Arrow, but several terminals (Windows Terminal among them) claim
// Ctrl+Left/Right for their own word-navigation/tab shortcuts and never
// pass the keystroke through at all; Alt+Arrow isn't commonly bound that
// way, so it's used instead. Still a modifier combination no field
// claims for anything, same reasoning as kKeyCtrlShiftS/X above.
constexpr int kKeyAltLeft = 310;
constexpr int kKeyAltRight = 311;

// Ctrl+Shift+U / Ctrl+Shift+Z -- the queue's "save the queue as a playlist"
// and "undo the last clear" commands (main UI, incl. the big queue overlay).
// Modifier combinations again, so sentinels like kKeyCtrlShiftS/X above.
// (U rather than P: Windows Terminal binds Ctrl+Shift+P to its command palette.)
constexpr int kKeyCtrlShiftU = 312;
constexpr int kKeyCtrlShiftZ = 313;
// Alt+L -- the lyrics timing overlay (main UI). Another modifier combination,
// so another sentinel (see kKeyCtrlShiftS/X above); not rebindable.
// (Was 314 -- the value of the former Ctrl+Shift+M mode-switch sentinel, so
// Alt+L / Ctrl+L switched to the radio instead of opening the overlay. The
// mode switch is SHIFT and + (the '*' character) only now.)
constexpr int kKeyAltL = 315;

// Ctrl+S -- "save" in the playlist editor, the meta editor and the radio's
// station lists menu. Arrives as the control byte 0x13, which only reaches the
// app because raw mode clears IXON (otherwise the terminal swallows it as
// XOFF and freezes the output until Ctrl+Q).
constexpr int kKeyCtrlS = 316;

// Ctrl+P -- "play" the highlighted file in the big folder overlay (control
// byte 0x10). Not rebindable.
constexpr int kKeyCtrlP = 317;

// Ctrl+L -- opens / closes the READ LYRICS overlay (control byte 0x0c). Not rebindable. The lyrics
// timing overlay is on Alt+L (SHIFT+j on macOS).
constexpr int kKeyCtrlL = 318;

// Ctrl+N -- audio book mode: the small note field and the big notes overlay swap the note that is being written (control byte 0x0e).
// Not rebindable.
constexpr int kKeyCtrlN = 319;

// Ctrl+T / Ctrl+Z -- main screen: the cover look one step back / forward (HKeyCoverPrev / HKeyCoverNext, rebindable; control bytes
// 0x14 / 0x1a). On macOS / Linux Ctrl+Z would suspend the program: raw mode switches that off (VSUSP), so it arrives as a key.
constexpr int kKeyCtrlT = 326;
constexpr int kKeyCtrlZ = 327;
// Ctrl+F (control byte 0x06): the "no lyrics" filter of the add-lyrics-manually overlay.
constexpr int kKeyCtrlF = 328;
// Ctrl+O (control byte 0x0f): the add-lyrics overlay shows the original name of the lyrics file / the name of the copy.
constexpr int kKeyCtrlO = 329;
// A paste by the terminal (bracketed paste, ESC [ 200 ~ ... ESC [ 201 ~): the whole text arrives as this ONE key (see set_paste_aware()).
constexpr int kKeyPaste = 332;
// Ctrl+2 (control byte 0x00 on most terminals, also Ctrl+@ / Ctrl+Space): the Latin-letters switch inside the translation editor.
constexpr int kKeyCtrl2 = 330;
// Ctrl+3: reported only by terminals that send modified keys ("ESC [ 51 ; 5 u" / "ESC [ 27 ; 5 ; 51 ~"); a classic terminal sends ESC for it.
constexpr int kKeyCtrl3 = 331;

// Ctrl+Left / Ctrl+Right (a word to the side) and Ctrl+Shift+Left / Right (mark a word). Only reported while a text field has the
// keyboard (set_text_entry); everywhere else the keys stay what they were, so no screen loses a key.
constexpr int kKeyCtrlLeft = 320;
constexpr int kKeyCtrlRight = 321;
constexpr int kKeyCtrlShiftLeft = 322;
constexpr int kKeyCtrlShiftRight = 323;

// Alt+C -- the cover fetch overlay (main UI). A modifier combination, so a sentinel like kKeyAltL; bindable (HKeyCoverFetch).
constexpr int kKeyAltC = 324;
// Alt+V -- the FETCH VIDEOS overlay (main UI); bindable (HKeyVideoFetch).
constexpr int kKeyAltV = 331;

// The character '°' (SHIFT+^ on a German / Austrian / Swiss keyboard): outside text fields poll_key() joins its two UTF-8 bytes
// (C2 B0) into this one key, so it can be a hotkey (the CHOOSE AUDIO DEVICE overlay, HKeyAudioDevice). In a text field the bytes
// arrive as they are and '°' can be typed.
constexpr int kKeyDegree = 325;

// Name of the Alt key as shown in legends / the cheat sheet. The Mac keyboard
// has no key called Alt -- the same key (the modifier the terminal reports as
// "Alt/Meta") is labelled Option there. Chosen at compile time, so Windows and
// Linux builds are byte-for-byte unaffected. Two spellings, because the
// legends use both: mixed case for hint lines ("Option+") and upper case for
// the cheat sheet's key column ("OPTION+L"). String literals, so they
// concatenate:
//   "[" MUISC_ALT_NAME "+L]"
// Note: only the LABELS change -- the key handling itself is identical (the
// terminal sends ESC-prefixed / modifier-flagged sequences either way).
#if defined(__APPLE__)
#  define MUISC_ALT_NAME      "Option"
#  define MUISC_ALT_NAME_UC   "OPTION"
#else
#  define MUISC_ALT_NAME      "Alt"
#  define MUISC_ALT_NAME_UC   "ALT"
#endif

// Label of the lyrics timing overlay key (Ctrl+L belongs to the READ LYRICS overlay).
#define MUISC_LYRICS_KEY_UC MUISC_ALT_NAME_UC "+L"

// Raw, non-canonical, no-echo terminal mode + non-blocking key reads.
// Panel/box drawing lives in app.cpp; this is just the terminal plumbing.
// Mode switch (player <-> radio): the next TerminalIO() takes over the alternate screen instead of the shell being shown
// for a moment. terminal_hold_alt_screen() is called by the mode that is ending, right before its TerminalIO is
// destroyed; terminal_release_alt_screen() (main(), when no mode took the screen over) puts the shell back.
void terminal_hold_alt_screen();
void terminal_release_alt_screen();
// Black terminal background (Settings -> GENERAL -> TERMINAL -> Black Background): on = OSC 11 sets the terminal's default
// background to black while Mousiki runs; the first time, the terminal is asked for its own colour (Linux / macOS), and
// off -- or leaving Mousiki in any way -- gives it back (OSC 111, plus that colour). Cheap to call every frame: it only
// writes something when the state changes. Terminals that do not know OSC 11 simply ignore it.
void terminal_black_background(bool on);
void terminal_background_restore();
// While the black background is on: the terminal's default text colour (OSC 10) becomes `palette` (16-255; "" or 0-15 = the
// terminal's own). Text that has no colour of its own then looks the same in every terminal, whatever its colour scheme
// says. Only writes when it changes; given back with the background on exit.
void terminal_default_text(const std::string& palette);

class TerminalIO {
public:
    TerminalIO();
    ~TerminalIO();

    void restore();

    // Non-blocking single "logical" key read. Arrow keys (3-byte escape
    // sequences) collapse to 'A'/'B'/'C'/'D' (up/down/right/left); Home
    // (2- or 3-byte, terminal-dependent) collapses to kKeyHome. A lone
    // Escape key returns 27. Backspace returns 127. Returns 0 if nothing
    // is waiting. A non-ASCII keystroke (an umlaut, any other accented or
    // non-Latin character) arrives as its UTF-8 encoding, one byte per
    // call -- the same shape a plain read() off a UTF-8 terminal already
    // produces, so text-entry fields that accept it need no special
    // casing per platform.
    int poll_key();
    // Waits up to `ms` milliseconds for something to arrive on the terminal input (a key, a paste, a mouse report); true when there is
    // something to read. It replaces the sleeping in short slices of the main loop (poll() on stdin; on Windows the console input handle).
    bool wait_input(int ms) const;

    int rows() const;
    int cols() const;

private:
    bool raw_mode_active_ = false;
    void reassert_raw_mode(); // see poll_key()'s definition for why this exists
    int poll_key_raw();       // one key as the platform reads it (poll_key() joins the bytes of '°' on top)
};

// True while the key most recently returned by poll_key() was one of the
// four arrow keys. Arrows collapse to the letters 'A'-'D' (see kKeyHome's
// comment above), so a bare value of 'A' can be either an Up press or a
// real capital A -- indistinguishable by value alone. A text field that
// wants to accept those four capitals (a Windows path has to be able to
// type "C:\...") checks this to accept a typed letter while still
// ignoring arrow presses; navigation code that keys off 'A'-'D' can keep
// using the value as-is, since every return path updates this flag in
// step with what it returned.
bool last_key_was_arrow();

// ---------------------------------------------------------------------------
// Terminal size at start (Settings -> GENERAL -> TERMINAL -> Terminal Size / Full Screen)
// ---------------------------------------------------------------------------
// Mousiki ASKS the terminal to resize itself (the xterm window sequence ESC [ 8 ; rows ; cols t, ESC [ 10 ; 1 t for full
// screen); whether the window really changes is the terminal's decision. xterm, GNOME Terminal (VTE), iTerm2, Terminal.app and
// recent Windows Terminal versions usually obey; Konsole, kitty, Alacritty, tmux and tiling window managers ignore it, and
// nothing happens then (the program works with whatever size it gets, as before). On Windows full screen is the F11 key
// of the terminal window, pressed for you while the window is in front.
struct TerminalLimits {
    int min_cols = 100, min_rows = 28;   // below this the screens do not fit
    int max_cols = 300, max_rows = 100;  // what the screen holds, in characters (the terminal tells; a rough estimate otherwise)
    bool screen_known = false;           // max_* come from the terminal / the system, not from the fallback
};
// Asks the terminal how big the screen is (once; ESC [ 19 t, which only some terminals answer). Call before the first
// terminal_size_limits().
void terminal_probe_screen();
TerminalLimits terminal_size_limits();
// Brings the terminal to cols x rows (clamped to the limits; 0 in either: leave it alone) or, with fullscreen, to full screen.
// Switching full screen off again puts the window back (and then the size). Safe to call when output is not a terminal.
void terminal_apply_size(int cols, int rows, bool fullscreen);

// True while the key most recently returned by poll_key() was an Up/Down
// arrow pressed WITH Shift held. The value itself stays a plain 'A'/'B'
// (and last_key_was_arrow() stays true), so every screen that doesn't care
// about the modifier keeps treating SHIFT+Up/Down exactly like Up/Down --
// only the list overlay (SHIFT+l) asks this, to turn them into page jumps.
// (SHIFT+Left/Right have their own sentinels, see kKeyShiftLeft/Right.)
bool last_key_was_shifted();
// True when the key just returned was PgUp ('A') or PgDn ('B'). Reported like SHIFT+Up / SHIFT+Down (arrow + shift flag), so
// every page that turns on SHIFT+arrow turns on these too; a text editor, where SHIFT+arrow marks text, asks this to tell them apart.
bool last_key_was_page();
// True when the key just returned was Up / Down pressed with CTRL held (still reported as the plain arrow, like SHIFT+arrow):
// the lists use it to jump to the first / last row.
bool last_key_was_ctrl();

// ---------------------------------------------------------------------------
// Text-entry mode + clipboard
// ---------------------------------------------------------------------------
//
// While a single-line text field is being edited the console must deliver
// Ctrl+C to the app instead of raising a CTRL_C_EVENT: on Windows
// ENABLE_PROCESSED_INPUT swallows it (see win_raw_mode_enter()), on POSIX
// ISIG turns it into SIGINT, and in both cases the keystroke never reaches
// poll_key() -- so "copy" would be impossible. This flips exactly that one
// switch off (and nothing else: echo/line-buffering stay as raw mode left
// them), and the caller restores it by calling with false again. Safe to
// call every frame with the same value.
void set_text_entry(bool on);
// What the last set_text_entry() call said: a text field has the keyboard right now (so a typed capital is text, not a command).
bool text_entry_active();

// The system clipboard as UTF-8, and the setter for it. Used by Ctrl+C/X/V
// in the text fields, so a copied path or title can also be pasted OUTSIDE
// the app. get() returns "" when the clipboard holds no text (or reading it
// fails); control characters are stripped by the caller-side paste, not here.
// On POSIX there is no portable system clipboard, so an in-process buffer is
// used instead -- copy/paste still works within one run of the app.
std::string clipboard_get();
// Bracketed paste: the terminal marks a text that the person pastes (CTRL+SHIFT+V, the right mouse button, the menu), so it is not mistaken for
// hundreds of typed keys. A program that handles kKeyPaste says so with set_paste_aware(true); poll_key() then returns kKeyPaste once and
// take_paste_text() gives the text (line breaks as '\n'). Everywhere else the pasted characters come one by one as before.
void set_paste_aware(bool on);
std::string take_paste_text();
// While set, clipboard_get() returns this text (a paste by the terminal is handled like CTRL+v with it); "" = the real clipboard again.
void set_paste_override(const std::string& text);
void clipboard_set(const std::string& utf8);

// Sends one finished frame to the terminal. On POSIX the whole frame goes out
// in a SINGLE write() wrapped in a synchronized-update bracket (DEC private
// mode 2026): `std::cout << frame` on a line-buffered tty gets chopped into many
// small write()s, and terminals that repaint in between (Windows Terminal /
// ConPTY under WSL in particular) then show half-drawn frames -- the
// menus/overlays flicker. Terminals that do not know mode 2026 ignore it.
// Windows keeps the existing std::cout path (its streambuf already emits one
// WriteConsoleW per flush).
void write_frame(const std::string& frame);
// For a frame with a big picture: written by a writer thread while the next one is built. Returns the ms spent waiting for the write of the
// frame before. Every other write (write_frame() too) waits for the writer first; anything that writes to the terminal on its own calls
// write_frame_drain() before.
double write_frame_async(std::string frame);
void write_frame_drain();
// A frame with the hidden cursor parked at the top left ("ESC[H" added), unless its last cursor escape is "show" (a text caret to be seen).
std::string park_cursor(const std::string& frame);
bool write_frame_async_allowed();   // false with MOUSIKI_SYNC_WRITE=1
double write_frame_last_ms();       // how long the terminal took for the last frame the writer wrote
bool write_frame_picture_busy();    // the writer is writing a frame with a picture, or one waits: no new picture is made or sent until it is done

// 24-bit colours (ESC[38;2;r;g;bm / 48;2) need a terminal that knows them. macOS Terminal before macOS 26 does not and shows
// wrong colours for them, so there write_frame() turns every one into the nearest of the 256 fixed colours (to_256_colours()).
// MOUSIKI_COLORS=256 forces that anywhere, MOUSIKI_COLORS=truecolor switches it off.
bool truecolor_ok();
std::string to_256_colours(const std::string& text);
int nearest_256(int r, int g, int b);   // the 256-colour palette entry (16..255) closest to an RGB colour

// Truncates/right-pads (by byte length — good enough for the mostly-ASCII
// UI text here; multi-byte titles may render slightly short) to exactly
// `width` visible columns.
std::string pad_right(const std::string& s, int width);
std::string pad_left(const std::string& s, int width);
std::string truncate_str(const std::string& s, int width);
std::string utf8_take(const std::string& s, int width);

// The characters (UTF-8 sequences) of a string; a cut-off sequence at the end is returned as it is.
inline std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 1;
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        if (i + n > s.size()) n = s.size() - i;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

// The braille block of a bar level 0..4 (the spectrum bars of the main screen and of the radio).
inline const char* fft_glyph(int level) {
    switch (std::clamp(level, 0, 4)) {
        case 0: return " ";
        case 1: return "\u28C0";
        case 2: return "\u28E4";
        case 3: return "\u28F6";
        default: return "\u28FF";
    }
}

// Word-wraps `s` to `width` display columns, across at most `max_lines`
// lines. A word wider than `width` on its own (a long filename, or any
// title with no spaces at all -- CJK, Thai, etc.) is chunked across
// successive lines rather than left to overflow or being flattened to one
// line. If the text still doesn't fit in `max_lines`, the last line ends
// in "..." exactly like truncate_str(). Returns fewer than `max_lines`
// entries when the text is shorter; never more.
std::vector<std::string> wrap_lines(const std::string& s, int width, int max_lines);

// Skips `skip_cols` display columns from the start of `s`, then returns up
// to the next `take_cols` display columns. A window into the middle of a
// string, rather than a truncation from its front -- what the local list's
// marquee scroll needs to show a moving slice of an over-wide title.
std::string utf8_skip_take(const std::string& s, int skip_cols, int take_cols);

// Computes terminal display width of a UTF-8 string based on wcwidth.
int display_width(const std::string& s);

// ---- key legends as a small tree table -------------------------------------------------------------------------------
// One row per group of commands: a tree line, the group's name, a bar and the keys, e.g.
//   ┌─ Move    │ [←→] Tab   [TAB] Focus   [↑↓] Navi.
//   ├─ Fetch   │ [a] To fetch list   [SHIFT+f] Add full list
//   └─ Session │ [CTRL+s] Save   [ESC] Exit
// A command is "[key] Long label" or "[key] Long label\x01Short label": when a row does not fit the width, the keys are first
// spaced tighter, then the short labels are used, and only then the last commands of the row are replaced by "…".
struct LegendGroup {
    std::string label;
    std::vector<std::string> cmds;
    int max_lines = 1;   // 2: when the commands do not fit one row, they continue on a second row (broken between two commands, never inside one)
};
struct LegendRow {
    std::string tree;    // "┌─ ", "├─ " or "└─ " (three columns)
    std::string label;   // padded to the widest label
    std::string cmds;    // the keys, already fitted
};
// `width` = columns available for the whole row (tree + label + bar + keys). `max_rows` > 0: when there are more groups the
// last ones are merged (their keys follow each other in one row) -- for terminals too short for the full legend.
std::vector<LegendRow> layout_legend_tree(std::vector<LegendGroup> groups, int width, int max_rows = 0);
// The row in colour: tree and bar in `border`, the label in `header`, the keys in `legend` (all complete SGR strings).
// `pad_to` > 0 fills the row with spaces up to that many columns (to wipe what is underneath).
// `legend` colours the keys (everything in [square brackets]); `cmd`, when given, colours their descriptions.
std::string paint_legend_row(const LegendRow& row, const std::string& border, const std::string& header, const std::string& legend, int pad_to = 0,
                             const std::string& cmd = std::string());

// High frame rates: instead of the whole screen, only the lines that changed since the last frame. `body` is a frame
// written top to bottom from row 1 with '\n' (or "\r\n") between the lines; each changed line is sent with its own
// cursor position and a colour reset in front. `prev` holds the last frame's lines and is updated. Returns false when
// the number of lines changed -- then the caller sends the whole frame (and `prev` is set from it).
// Of a changed line only the middle part is sent when the glyphs at its start (and end) are the same as on screen and are all of one column
// (ASCII, box drawing, braille): the cursor goes to the column where the difference starts, the colours in force there are set again, the
// differing glyphs follow. A line with another escape sequence or a wide glyph in the shared parts is sent whole, as before.
// `whole_lines`: every changed line is sent whole (the caller does that now and then, so that stray text on screen is written over again).
bool diff_frame_lines(const std::string& body, std::vector<std::string>& prev, std::string& out, bool whole_lines = false);

// Emoji handling (set_emoji_mode(): 0 off, 1 safe = keep plain one-codepoint emoji, 2 all; see the long comment above replace_emoji() in
// terminal_ui.cpp). On (the default): every emoji cluster is measured and
// drawn as a single "?" by display_width()/pad_right()/truncate_str()/
// wrap_lines()/utf8_take()/utf8_skip_take(), so a title containing an emoji
// can never push the box borders out of line, whatever the terminal thinks the
// emoji's width is. Off: emoji pass through and are measured by the width
// table. Driven by config.txt's ReplaceEmoji.
void set_emoji_replacement(bool on);
void set_emoji_mode(int mode);   // 0 off, 1 safe, 2 all

// Marquee scrolling of too-long titles (config.txt / radio_config.txt: MarqueeScrolling). Off: such a text is cut
// with "..." like every other over-wide text. Each mode sets it from its own settings.
void set_marquee_enabled(bool on);
bool marquee_enabled();

} // namespace muisc
