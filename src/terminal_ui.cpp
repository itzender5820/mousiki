// Terminal handling: raw input and key decoding, screen output, terminal size and colour support, text
// width, and the clipboard. Everything the screen code sends to the terminal goes through here.
#include "terminal_ui.h"
#include "indic_handler.h"
#include "utf8_util.h"
#include <algorithm>
#include <vector>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <thread>
#include <cstdio>
#include <iostream>
#include <cerrno>
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <system_error>
#include <cwchar>
#include <filesystem>
#include "process_util.h"
#endif

namespace muisc {

#if !defined(_WIN32)
static struct termios g_orig_termios;
// Text-entry mode (see set_text_entry()): while true, ISIG stays cleared so
// Ctrl+C reaches poll_key() as a keystroke instead of a SIGINT.
static bool g_text_entry = false;
// In-process fallback for machines with no clipboard tool installed (a bare
// SSH session, a minimal container). When a system clipboard is reachable
// (see the clipboard section below) it is used instead, so copy/paste also
// works across applications -- the same behaviour the Windows build has.
static std::string g_clipboard;

// ---------------------------------------------------------------------------
// Terminal restore on fatal signals
//
// The Windows build installs a console control handler for exactly this
// reason (see win_console_init()): Ctrl+C / closing the terminal /
// `kill` would otherwise terminate the process while it is still inside the
// alternate screen, with the cursor hidden and ECHO/ICANON switched off --
// leaving the user's shell unusable until they type `reset` blind. POSIX had
// no equivalent. Only async-signal-safe calls are allowed in here, hence the
// raw write() and tcsetattr() and nothing else.
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_signals_installed = 0;
// The black background's way back, ready for the signal handler (which may only call write()): see terminal_black_background().
static char g_bg_restore[128] = {};
static size_t g_bg_restore_len = 0;
static volatile sig_atomic_t g_bg_restore_armed = 0;

static void fatal_signal_handler(int sig) {
    if (g_signals_installed) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        static const char kLeave[] = "\x1b[?25h\x1b[?1049l";
        if (g_bg_restore_armed) { ssize_t r = write(STDOUT_FILENO, g_bg_restore, g_bg_restore_len); (void)r; }
        ssize_t ignored = write(STDOUT_FILENO, kLeave, sizeof(kLeave) - 1);
        (void)ignored;
    }
    // Hand the signal back to the default disposition so the shell still sees
    // "terminated by SIGINT" (exit status 130) rather than a normal exit.
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_fatal_signal_handlers() {
    struct sigaction sa;
    sa.sa_handler = fatal_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
    g_signals_installed = 1;
}

static void remove_fatal_signal_handlers() {
    if (!g_signals_installed) return;
    g_signals_installed = 0;
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
}
#endif

// ---------------------------------------------------------------------------
// Black terminal background
// ---------------------------------------------------------------------------
static bool g_bg_black = false;     // OSC 11 black is in force
static bool g_bg_asked = false;     // the terminal's own background was asked for (only the first time)
static std::string g_fg_set;          // the text colour set with OSC 10 ("" = the terminal's own)
#if defined(_WIN32)
static char g_bg_restore[128] = {};
static size_t g_bg_restore_len = 0;
static int g_bg_restore_armed = 0;
#endif

// The terminal's own default background as it reports it ("rgb:1e1e/1e1e/1e1e"), or "" when it does not answer. The query
// is followed by a device attributes request, which every terminal answers, so the wait ends at once (the same exchange
// as the graphics probe); 250 ms at the most.
static std::string bg_query() {
#if defined(_WIN32)
    return "";
#else
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return "";
    std::cout << std::flush;
    const char q[] = "\x1b]11;?\x1b\\\x1b[c";
    if (::write(STDOUT_FILENO, q, sizeof q - 1) < 0) return "";
    std::string reply;
    for (int waited = 0; waited < 250;) {
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (::poll(&pfd, 1, 25) <= 0) { waited += 25; continue; }
        char buf[256];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) break;
        reply.append(buf, static_cast<size_t>(n));
        const size_t da = reply.find("\x1b[?");
        if (da != std::string::npos && reply.find('c', da) != std::string::npos) break;
    }
    const size_t at = reply.find("]11;");
    if (at == std::string::npos) return "";
    std::string col;
    for (size_t i = at + 4; i < reply.size(); ++i) {
        const char c = reply[i];
        if (c == '\x07' || c == '\x1b') break;
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '/' || c == ':' || c == '#')) return "";
        col += c;
    }
    return col.size() >= 4 && col.size() <= 40 ? col : std::string();
#endif
}

void terminal_black_background(bool on) {
    if (on == g_bg_black) return;
    if (on) {
        if (!g_bg_asked) {
            g_bg_asked = true;
            // the way back: OSC 111 (the terminal's configured default) and, when the terminal told us, its exact colour
            std::string back = "\x1b]110\x1b\\\x1b]111\x1b\\";   // (OSC 110: the text colour too, see terminal_default_text())
            const std::string own = bg_query();
            if (!own.empty() && own != "rgb:0000/0000/0000") back += "\x1b]11;" + own + "\x1b\\";
            g_bg_restore_len = std::min(back.size(), sizeof g_bg_restore);
            std::copy(back.begin(), back.begin() + static_cast<std::ptrdiff_t>(g_bg_restore_len), g_bg_restore);
        }
        std::cout << "\x1b]11;rgb:00/00/00\x1b\\" << std::flush;
        g_bg_black = true;
        g_bg_restore_armed = 1;
    } else {
        g_bg_restore_armed = 0;
        std::cout.write(g_bg_restore, static_cast<std::streamsize>(g_bg_restore_len));
        std::cout << std::flush;
        g_bg_black = false;
        g_fg_set.clear();
    }
}

// The 256-colour palette as RGB, for 16-255 (the same in every terminal); "" for 0-15 / empty / anything else.
static std::string palette_rgb(const std::string& v) {
    if (v.empty() || v.size() > 3) return "";
    for (char c : v) if (c < '0' || c > '9') return "";
    const int n = std::atoi(v.c_str());
    int r, g, b;
    if (n >= 16 && n <= 231) {
        static const int lv[6] = {0, 95, 135, 175, 215, 255};
        r = lv[(n - 16) / 36]; g = lv[(n - 16) / 6 % 6]; b = lv[(n - 16) % 6];
    } else if (n >= 232 && n <= 255) {
        r = g = b = 8 + (n - 232) * 10;
    } else return "";
    char buf[32];
    std::snprintf(buf, sizeof buf, "rgb:%02x/%02x/%02x", r, g, b);
    return buf;
}

void terminal_default_text(const std::string& palette) {
    const std::string want = g_bg_black ? palette_rgb(palette) : std::string();
    if (want == g_fg_set) return;
    if (want.empty()) std::cout << "\x1b]110\x1b\\";              // the terminal's own text colour again
    else std::cout << "\x1b]10;" << want << "\x1b\\";
    std::cout << std::flush;
    g_fg_set = want;
}
void terminal_background_restore() { terminal_black_background(false); }

// --- the terminal's size at start (see terminal_ui.h) ---
static int g_screen_cols = 0, g_screen_rows = 0;   // the screen in characters, as the terminal / the system tells (0: unknown)
static bool g_fullscreen_applied = false;

#if !defined(_WIN32)
// ESC [ 19 t: "report the size of the screen in characters", answered as ESC [ 9 ; rows ; cols t by xterm, VTE, iTerm2 ...
// Terminals that do not know it say nothing: the read gives up after a tenth of a second.
static bool query_screen_chars(int& cols, int& rows) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return false;
    struct termios orig;
    if (tcgetattr(STDIN_FILENO, &orig) != 0) return false;
    struct termios raw = orig;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;   // one read waits at most 0.1 s
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    const char q[] = "\x1b[19t";
    ssize_t w = ::write(STDOUT_FILENO, q, sizeof q - 1);
    (void)w;
    std::string buf;
    while (buf.size() < 40) {
        char ch = 0;
        if (::read(STDIN_FILENO, &ch, 1) != 1) break;
        buf.push_back(ch);
        if (ch == 't') break;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    const size_t at = buf.find("\x1b[9;");
    int r = 0, c = 0;
    if (at == std::string::npos || std::sscanf(buf.c_str() + at, "\x1b[9;%d;%dt", &r, &c) != 2) return false;
    if (r < 10 || c < 20 || r > 2000 || c > 2000) return false;
    rows = r; cols = c;
    return true;
}
#endif

void terminal_probe_screen() {
    static bool done = false;
    if (done) return;
    done = true;
#if defined(_WIN32)
    int c = 0, r = 0;
    if (win_screen_chars_estimate(c, r)) { g_screen_cols = c; g_screen_rows = r; }
#else
    int c = 0, r = 0;
    if (query_screen_chars(c, r)) { g_screen_cols = c; g_screen_rows = r; }
#endif
}

TerminalLimits terminal_size_limits() {
    TerminalLimits L;
    if (g_screen_cols > 0 && g_screen_rows > 0) { L.max_cols = g_screen_cols; L.max_rows = g_screen_rows; L.screen_known = true; }
    L.max_cols = std::max(L.max_cols, L.min_cols);
    L.max_rows = std::max(L.max_rows, L.min_rows);
    return L;
}

void terminal_apply_size(int cols, int rows, bool fullscreen) {
#if defined(_WIN32)
    const bool tty = true;
#else
    const bool tty = isatty(STDOUT_FILENO) != 0;
#endif
    if (!tty) return;
    if (fullscreen) {
        if (g_fullscreen_applied) return;
        std::cout << "\x1b[10;1t" << std::flush;   // xterm: full screen
#if defined(_WIN32)
        win_set_fullscreen(true);                   // Windows Terminal: its F11
#endif
        g_fullscreen_applied = true;
        return;
    }
    if (g_fullscreen_applied) {
        std::cout << "\x1b[10;0t" << std::flush;
#if defined(_WIN32)
        win_set_fullscreen(false);
#endif
        g_fullscreen_applied = false;
    }
    if (cols <= 0 || rows <= 0) return;
    const TerminalLimits L = terminal_size_limits();
    cols = std::clamp(cols, L.min_cols, L.max_cols);
    rows = std::clamp(rows, L.min_rows, L.max_rows);
#if defined(_WIN32)
    if (win_term_cols() == cols && win_term_rows() == rows) return;   // already that size: leave the window alone
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col == cols && ws.ws_row == rows) return;
#endif
    std::cout << "\x1b[8;" << rows << ";" << cols << "t" << std::flush;
}

static bool g_alt_screen_held = false;
void terminal_hold_alt_screen() { g_alt_screen_held = true; }
void terminal_release_alt_screen() {
    if (g_alt_screen_held) { terminal_background_restore(); std::cout << "\x1b[?25h" << "\x1b[?1049l" << std::flush; g_alt_screen_held = false; }
}

TerminalIO::TerminalIO() {
#if defined(_WIN32)
    // The console mode/code-page save already happened in main() via
    // win_console_init(); this only flips input into the no-echo,
    // no-line-editing state that the termios branch below sets up.
    win_raw_mode_enter();
#else
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);   // (IEXTEN off: macOS' terminal driver eats Ctrl+V (literal-next) and Ctrl+O (discard) otherwise, so they never reached the app as keys)
    raw.c_iflag &= ~(IXON | IXOFF);   // Ctrl+S / Ctrl+Q reach the app as keys (Ctrl+S = save), not as flow control
#ifdef _POSIX_VDISABLE
    raw.c_cc[VSUSP] = _POSIX_VDISABLE;   // Ctrl+Z is a key (the cover look), it does not stop the program
#endif
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    install_fatal_signal_handlers();
#endif
    raw_mode_active_ = true;
    // Alternate screen buffer: the terminal keeps a second, separate
    // grid (same dimensions as the visible one) while this is active.
    // "\x1b[H" homing the cursor and any accidental scroll it causes
    // both happen *within* that private grid, never touching the user's
    // actual shell scrollback -- and switching back with "\x1b[?1049l"
    // on exit restores exactly whatever was on screen before mousiki
    // started, cleanly, rather than leaving a trail of overwritten
    // frames behind in their scrollback history.
    //
    // To be clear about what this does and doesn't fix: it does NOT by
    // itself prevent the frame-height-exceeds-terminal-rows scrolling
    // bug (the alt-screen grid is still exactly term.rows() tall, and
    // still scrolls internally if you print past its bottom with no
    // clear) -- that's what term_rows_ and clamp_output_rows() in
    // app.cpp actually fix. This is a separate, complementary
    // improvement: even if some future change reintroduces a height
    // miscalculation, the damage is contained to mousiki's own private
    // buffer instead of polluting the terminal the person is actually
    // going to keep using afterwards.
    if (g_alt_screen_held) {   // taken over from the mode that just ended: stay on the alternate screen, just blank it
        g_alt_screen_held = false;
        std::cout << "\x1b[2J\x1b[H\x1b[?25l" << std::flush;
    } else {
        std::cout << "\x1b[?1049h" << "\x1b[?25l" << std::flush; // enter alt-screen, hide cursor
    }
}

TerminalIO::~TerminalIO() { restore(); }

void TerminalIO::restore() {
    if (raw_mode_active_) {
#if defined(_WIN32)
        win_raw_mode_exit();
#else
        remove_fatal_signal_handlers();
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
#endif
        if (!g_alt_screen_held) {   // show cursor, leave alt-screen and give the terminal its background back (not when the other mode takes it over)
            terminal_background_restore();
            std::cout << "\x1b[?25h" << "\x1b[?1049l" << std::flush;
        }
        raw_mode_active_ = false;
    }
}

// Subprocesses we spawn are supposed to never touch our stdin at all
// (see process_util.cpp's run_capture() and waveform.cpp's ffmpeg
// fallback — both redirect the child's stdin to /dev/null specifically
// because of this). But that fix lives in the spawn call sites, and
// this is the one place that actually NEEDS raw+non-blocking mode to
// keep working no matter what: re-applying our own termios settings on
// every poll is cheap (one syscall, ~25x/sec) and means that even if
// something unexpected resets the terminal to canonical/line-buffered
// mode, we're never more than one frame away from correcting it,
// instead of the read() call silently becoming blocking and stalling
// the entire render loop until a keypress+Enter happens to satisfy it.
void TerminalIO::reassert_raw_mode() {
    if (!raw_mode_active_) return;
#if defined(_WIN32)
    // Same reasoning, different mechanism: ffmpeg and yt-dlp are spawned with
    // their own console handles (see process_util.cpp), but re-asserting the
    // input mode every frame is one cheap call and makes the render loop
    // self-healing if anything else resets it.
    win_raw_mode_enter();
#else
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);   // (IEXTEN off: macOS' terminal driver eats Ctrl+V (literal-next) and Ctrl+O (discard) otherwise, so they never reached the app as keys)
    raw.c_iflag &= ~(IXON | IXOFF);   // see the constructor: Ctrl+S is a key here
#ifdef _POSIX_VDISABLE
    raw.c_cc[VSUSP] = _POSIX_VDISABLE;   // see the constructor: Ctrl+Z is a key here
#endif
    // Text-entry mode: keep SIGINT off the keyboard so Ctrl+C reaches the
    // editor as a key (copy) rather than as a signal (quit). Re-derived here
    // rather than applied once, because this runs on every poll.
    if (g_text_entry) raw.c_lflag &= ~ISIG;
    else raw.c_lflag |= ISIG;
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
}

// See last_key_was_arrow() in terminal_ui.h. Only ever written by
// poll_key() below, and only read by whatever handles the key that same
// call just returned -- single-threaded, since the input loop is the only
// thing that polls.
static bool g_last_key_was_arrow = false;

bool last_key_was_arrow() { return g_last_key_was_arrow; }

// See last_key_was_shifted() in terminal_ui.h. Cleared at the top of every
// poll_key() call, so it can only ever be true for the key just returned.
static bool g_last_key_was_page = false;
bool last_key_was_page() { return g_last_key_was_page; }
static bool g_last_key_was_shifted = false;
bool last_key_was_shifted() { return g_last_key_was_shifted; }
static bool g_last_key_was_ctrl = false;
bool last_key_was_ctrl() { return g_last_key_was_ctrl; }

// ---------------------------------------------------------------------------
// Text-entry mode + clipboard (see terminal_ui.h)
//
// The flag is what survives the per-poll reassert below: every call to
// poll_key() re-applies raw mode, so toggling ISIG/ENABLE_PROCESSED_INPUT
// once and walking away would silently undo itself on the very next key.
// Both platforms therefore keep the requested state and re-derive the
// console's mode from it every time.
// ---------------------------------------------------------------------------
static bool g_text_entry_requested = false;
bool text_entry_active() { return g_text_entry_requested; }
void set_text_entry(bool on) {
    g_text_entry_requested = on;
#if defined(_WIN32)
    win_set_text_entry(on);
#else
    g_text_entry = on;
#endif
}

#if !defined(_WIN32)
namespace {

// POSIX has no clipboard API of its own: it belongs to the desktop session, and
// the portable way to reach it is the command-line tool that session ships.
// macOS always has pbcopy/pbpaste; on Linux it is wl-clipboard (Wayland), xclip
// or xsel (X11), and on Termux termux-clipboard-*. Which one exists is probed
// once. spawn_capture() is used rather than run_capture() on purpose:
// run_capture() writes every command's complete output to the console log,
// which for a paste would put whatever is on the clipboard (a password, say)
// into a log file on disk.
enum class ClipTool { Unknown, None, Pasteboard, Wayland, Xclip, Xsel, Termux };
ClipTool g_clip_tool = ClipTool::Unknown;

bool have_command(const char* name) {
    return program_on_path(name);
}

ClipTool detect_clip_tool() {
#if defined(__APPLE__)
    if (have_command("pbcopy") && have_command("pbpaste")) return ClipTool::Pasteboard;
#else
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* x11 = std::getenv("DISPLAY");
    if (wayland && *wayland && have_command("wl-copy") && have_command("wl-paste")) return ClipTool::Wayland;
    if (x11 && *x11) {
        if (have_command("xclip")) return ClipTool::Xclip;
        if (have_command("xsel")) return ClipTool::Xsel;
    }
    if (have_command("termux-clipboard-set") && have_command("termux-clipboard-get")) return ClipTool::Termux;
#endif
    return ClipTool::None;
}

ClipTool clip_tool() {
    if (g_clip_tool == ClipTool::Unknown) g_clip_tool = detect_clip_tool();
    return g_clip_tool;
}

// The tool's program and arguments; empty when there is no clipboard tool.
Args clip_paste_command(ClipTool t) {
    switch (t) {
        case ClipTool::Pasteboard: return {"pbpaste"};
        case ClipTool::Wayland:    return {"wl-paste", "--no-newline"};
        case ClipTool::Xclip:      return {"xclip", "-selection", "clipboard", "-o"};
        case ClipTool::Xsel:       return {"xsel", "--clipboard", "--output"};
        case ClipTool::Termux:     return {"termux-clipboard-get"};
        default:                   return {};
    }
}

Args clip_copy_command(ClipTool t) {
    switch (t) {
        case ClipTool::Pasteboard: return {"pbcopy"};
        case ClipTool::Wayland:    return {"wl-copy"};
        case ClipTool::Xclip:      return {"xclip", "-selection", "clipboard", "-i"};
        case ClipTool::Xsel:       return {"xsel", "--clipboard", "--input"};
        case ClipTool::Termux:     return {"termux-clipboard-set"};
        default:                   return {};
    }
}

} // namespace
#endif

std::string clipboard_get() {
#if defined(_WIN32)
    return win_clipboard_get();
#else
    const Args cmd = clip_paste_command(clip_tool());
    if (cmd.empty()) return g_clipboard;
    std::unique_ptr<ChildProcess> child = spawn_capture(cmd, false);
    if (!child) return g_clipboard;
    std::string out;
    char buf[4096];
    std::ptrdiff_t n;
    while ((n = child->read(buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    // A non-zero exit is how these tools report "nothing usable on the
    // clipboard" (empty, or an image) as well as "no display reachable"; either
    // way the text last copied inside this app is the best answer left.
    if (child->wait() != 0) return g_clipboard;
    if (out.empty() && !g_clipboard.empty()) return g_clipboard;   // (a tool that took the copy without error but gives nothing back, as on a session without a desktop)
    return out;
#endif
}

void clipboard_set(const std::string& utf8) {
#if defined(_WIN32)
    win_clipboard_set(utf8);
#else
    g_clipboard = utf8;
    const Args cmd = clip_copy_command(clip_tool());
    if (cmd.empty()) return;
    // The text goes in through a file the tool reads on stdin: building it into
    // the argument list would pass arbitrary bytes on a command line. mkstemp()
    // creates the file 0600, so the clipboard text is never readable by other
    // users while it exists.
    std::error_code ec;
    std::string tmpl = (std::filesystem::temp_directory_path(ec) / "mousiki-clip-XXXXXX").string();
    if (ec) return;
    int fd = mkstemp(&tmpl[0]);
    if (fd < 0) return;
    size_t off = 0;
    while (off < utf8.size()) {
        ssize_t w = write(fd, utf8.data() + off, utf8.size() - off);
        if (w <= 0) break;
        off += static_cast<size_t>(w);
    }
    close(fd);
    if (off == utf8.size()) {
        // Output goes to /dev/null: xclip, xsel and wl-copy fork into the
        // background to keep serving the selection, and would otherwise hold
        // an output pipe open.
        run_with_stdin_file(cmd, tmpl);
    }
    unlink(tmpl.c_str());
#endif
}

// '°' as one key (kKeyDegree) outside text fields: its first byte (C2) is held back until the second one shows whether it is '°';
// any other character that starts with C2 (§, ´, µ ...) comes out unchanged, one byte per call as before.
int TerminalIO::poll_key() {
    static int held = -1;   // a byte read ahead that was not part of '°'
    if (held >= 0) { const int k = held; held = -1; g_last_key_was_arrow = false; return k; }
    const int key = poll_key_raw();
    if (key != 0xC2 || g_text_entry_requested) return key;
    int next = 0;
    for (int tries = 0; tries < 20 && next == 0; ++tries) {   // the second byte is normally there at once (a few ms at most)
        next = poll_key_raw();
        if (next == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (next == 0xB0) { g_last_key_was_arrow = false; return kKeyDegree; }
    if (next != 0) held = next;
    g_last_key_was_arrow = false;
    return key;
}

int TerminalIO::poll_key_raw() {
    reassert_raw_mode();
    g_last_key_was_shifted = false;
    g_last_key_was_page = false;
    g_last_key_was_ctrl = false;
#if defined(_WIN32)
    int key = win_poll_key();
    g_last_key_was_ctrl = win_last_key_was_ctrl();
    g_last_key_was_arrow = win_last_key_was_arrow();
    g_last_key_was_shifted = win_last_key_was_shifted();
    g_last_key_was_page = win_last_key_was_page();
    return key;
#else
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) { g_last_key_was_arrow = false; return 0; }

    if (c == '\x1b') {
        unsigned char seq[2] = {0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) != 1) { g_last_key_was_arrow = false; return 27; }
        // Alt+L: terminals send ESC followed by the letter (a lone byte, so it
        // has to be recognised before the two-byte read below).
        if (seq[0] == 'l' || seq[0] == 'L') { g_last_key_was_arrow = false; return kKeyAltL; }
        if (seq[0] == 'c' || seq[0] == 'C') { g_last_key_was_arrow = false; return kKeyAltC; }   // Alt+C: the cover fetch overlay
        // Option/Alt+Left and +Right as macOS Terminal.app and iTerm2 send them
        // out of the box: the readline word-movement pair ESC b / ESC f rather
        // than xterm's "ESC [ 1 ; 3 D" / "ESC [ 1 ; 3 C" (still handled below).
        // Without this the playlist editor's tab switch was unreachable there.
        if (seq[0] == 'b') { g_last_key_was_arrow = false; return kKeyAltLeft; }
        if (seq[0] == 'f') { g_last_key_was_arrow = false; return kKeyAltRight; }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) { g_last_key_was_arrow = false; return 27; }
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': g_last_key_was_arrow = true; return 'A';
                case 'B': g_last_key_was_arrow = true; return 'B';
                case 'C': g_last_key_was_arrow = true; return 'C';
                case 'D': g_last_key_was_arrow = true; return 'D';
                case 'H': g_last_key_was_arrow = false; return kKeyHome; // most xterm-likes send ESC [ H for Home
                case 'F': g_last_key_was_arrow = false; return kKeyEnd;  // ...and ESC [ F for End
                case '3': { // ESC [ 3 ~ -- Delete key; ESC [ 3 ; <mod> ~ -- with modifiers (Shift+Delete: mod 2)
                    unsigned char tail = 0;
                    g_last_key_was_arrow = false;
                    if (read(STDIN_FILENO, &tail, 1) != 1) return 27;
                    if (tail == '~') return kKeyDelete;
                    if (tail == ';') {
                        int mod = 0;
                        unsigned char c = 0;
                        while (read(STDIN_FILENO, &c, 1) == 1 && c >= '0' && c <= '9') mod = mod * 10 + (c - '0');
                        if (c == '~') {
                            g_last_key_was_shifted = mod >= 2 && (((mod - 1) & 1) != 0);
                            return kKeyDelete;
                        }
                    }
                    return 27;
                }
            }
            // Modified-key sequences (a digit after ESC [): xterm's
            // modifyOtherKeys form "ESC [ 27 ; <mod> ; <code> ~" and the
            // kitty/CSI-u form "ESC [ <code> ; <mod> u", which is how a
            // terminal reports a key pressed WITH modifiers -- notably the
            // Ctrl+Shift+S / Ctrl+Shift+X sentinels the meta editor uses
            // (mod = 1 + shift*1 + alt*2 + ctrl*4, so Ctrl+Shift = 6).
            // Only ever reached for a sequence whose final byte hasn't been
            // consumed by the arrow/Home/Delete cases above.
            if (seq[1] >= '0' && seq[1] <= '9') {
                std::string body(1, static_cast<char>(seq[1]));
                unsigned char tail = 0;
                for (int i = 0; i < 24; ++i) {
                    if (read(STDIN_FILENO, &tail, 1) != 1) break;
                    if (tail >= 0x40 && tail <= 0x7E) break; // final byte of the sequence
                    body.push_back(static_cast<char>(tail));
                }
                // PgUp / PgDn ("ESC [ 5 ~" / "ESC [ 6 ~", with a modifier "ESC [ 5 ; <mod> ~"): reported exactly like SHIFT+Up /
                // SHIFT+Down (a plain 'A' / 'B' arrow with the shift flag), so every page that turns a page on SHIFT+arrow
                // turns it on PgUp / PgDn too. (SHIFT+arrow stays as the key for keyboards that have no PgUp / PgDn.)
                if (tail == '~') {
                    int pg = 0;
                    if (std::sscanf(body.c_str(), "%d", &pg) == 1 && (pg == 5 || pg == 6)) {
                        g_last_key_was_arrow = true; g_last_key_was_shifted = true; g_last_key_was_page = true;
                        return pg == 5 ? 'A' : 'B';
                    }
                }
                // "27;<mod>;<code>~" or "<code>;<mod>u"
                int code = 0, mod = 0;
                if (body.rfind("27;", 0) == 0 && tail == '~') {
                    int m = 0, c = 0;
                    if (std::sscanf(body.c_str(), "27;%d;%d~", &m, &c) == 2) { mod = m; code = c; }
                } else if (tail == 'u') {
                    int c = 0, m = 0;
                    if (std::sscanf(body.c_str(), "%d;%d", &c, &m) >= 1) { code = c; mod = m; }
                }
                if (mod == 6) { // Ctrl+Shift
                    if (code == 'S' || code == 's') { g_last_key_was_arrow = false; return kKeyCtrlShiftS; }
                    if (code == 'X' || code == 'x') { g_last_key_was_arrow = false; return kKeyCtrlShiftX; }
                    if (code == 'U' || code == 'u') { g_last_key_was_arrow = false; return kKeyCtrlShiftU; }
                    if (code == 'Z' || code == 'z') { g_last_key_was_arrow = false; return kKeyCtrlShiftZ; }
                }
                if (mod == 2 && code == 13) { g_last_key_was_arrow = false; g_last_key_was_shifted = true; return 13; }   // SHIFT+ENTER (terminals that report it): Enter, with the shift flag
                if (mod == 5 && (code == 's' || code == 'S')) { g_last_key_was_arrow = false; return kKeyCtrlS; } // Ctrl+S
                if (mod == 5 && (code == 'p' || code == 'P')) { g_last_key_was_arrow = false; return kKeyCtrlP; } // Ctrl+P
                if (mod == 5 && (code == 'l' || code == 'L')) { g_last_key_was_arrow = false; return kKeyCtrlL; } // Ctrl+L
                if (mod == 5 && (code == 'n' || code == 'N')) { g_last_key_was_arrow = false; return kKeyCtrlN; } // Ctrl+N
                if (mod == 5 && (code == 't' || code == 'T')) { g_last_key_was_arrow = false; return kKeyCtrlT; } // Ctrl+T
                if (mod == 5 && (code == 'z' || code == 'Z')) { g_last_key_was_arrow = false; return kKeyCtrlZ; } // Ctrl+Z
                if (mod == 5 && (code == 'y' || code == 'Y')) { g_last_key_was_arrow = false; return 25; } // Ctrl+Y (undo in a text field)
                if (mod == 5 && (code == 'f' || code == 'F')) { g_last_key_was_arrow = false; return kKeyCtrlF; } // Ctrl+F
                if (mod == 5 && (code == 'o' || code == 'O')) { g_last_key_was_arrow = false; return kKeyCtrlO; } // Ctrl+O
                if (mod == 5 && code == '2') { g_last_key_was_arrow = false; return kKeyCtrl2; } // Ctrl+2
                // "1;<mod> A" -- an arrow (or Home/End) pressed WITH a
                // modifier, which is how xterm-style terminals report
                // Shift+Left/Right: "ESC [ 1 ; 2 D". mod is
                // 1 + shift*1 + alt*2 + ctrl*4, so the shift bit is bit 0
                // of (mod - 1). Only the shift variants are consumed here;
                // every other modified arrow keeps today's behaviour of
                // falling through as a plain Escape rather than starting to
                // navigate on Ctrl/Alt+Arrow.
                if (tail == 'A' || tail == 'B' || tail == 'C' || tail == 'D' ||
                    tail == 'H' || tail == 'F') {
                    int csi = 0, m = 1;
                    if (std::sscanf(body.c_str(), "%d;%d", &csi, &m) == 2 && m >= 1) {
                        bool shift = (((m - 1) & 1) != 0);
                        // Alt bit is bit 1 of (mod - 1) (mod = 1 + shift*1 +
                        // alt*2 + ctrl*4); the playlist editor's tab switch
                        // (see kKeyAltLeft/Right). Only the plain Alt (no
                        // Shift) combination is consumed -- Alt+Shift+Arrow
                        // isn't bound to anything and falls through as today.
                        bool alt = (((m - 1) & 2) != 0);
                        // SHIFT+Up/Down: still reported as a plain arrow (so
                        // nothing that ignores the modifier changes), with the
                        // shift flag set for the list overlay's page jumps.
                        // Used to fall through as a bare Escape.
                        if (((m - 1) & 4) != 0 && !shift && !alt && (tail == 'A' || tail == 'B')) {   // CTRL+Up / Down
                            g_last_key_was_arrow = true; g_last_key_was_ctrl = true; return tail;
                        }
                        if (((m - 1) & 4) != 0 && !alt && g_text_entry_requested && (tail == 'C' || tail == 'D')) {   // a word to the side / mark a word
                            g_last_key_was_arrow = false;
                            return tail == 'D' ? (shift ? kKeyCtrlShiftLeft : kKeyCtrlLeft) : (shift ? kKeyCtrlShiftRight : kKeyCtrlRight);
                        }
                        if (shift && !alt && tail == 'A') { g_last_key_was_arrow = true; g_last_key_was_shifted = true; return 'A'; }
                        if (shift && !alt && tail == 'B') { g_last_key_was_arrow = true; g_last_key_was_shifted = true; return 'B'; }
                        if (shift && tail == 'D') { g_last_key_was_arrow = false; return kKeyShiftLeft; }
                        if (shift && tail == 'C') { g_last_key_was_arrow = false; return kKeyShiftRight; }
                        if (shift && tail == 'H') { g_last_key_was_arrow = false; return kKeyHome; }
                        if (shift && tail == 'F') { g_last_key_was_arrow = false; return kKeyEnd; }
                        if (alt && !shift && tail == 'D') { g_last_key_was_arrow = false; return kKeyAltLeft; }
                        if (alt && !shift && tail == 'C') { g_last_key_was_arrow = false; return kKeyAltRight; }
                    }
                }
            }
        }
        g_last_key_was_arrow = false;
        return 27;
    }
    // Ctrl+C / Ctrl+X / Ctrl+V arrive as control bytes in raw mode. Ctrl+C
    // only ever gets here while text-entry mode has ISIG cleared (otherwise
    // the terminal itself turns it into SIGINT before we see a byte) --
    // see set_text_entry().
    if (c == 0x03) { g_last_key_was_arrow = false; return kKeyCtrlC; }
    if (c == 0x18) { g_last_key_was_arrow = false; return kKeyCtrlX; }
    if (c == 0x16) { g_last_key_was_arrow = false; return kKeyCtrlV; }
    if (c == 0x13) { g_last_key_was_arrow = false; return kKeyCtrlS; }   // Ctrl+S (IXON is off, see the constructor)
    if (c == 0x10) { g_last_key_was_arrow = false; return kKeyCtrlP; }   // Ctrl+P
    // Ctrl+L = the READ LYRICS overlay (the lyrics timing overlay is Alt+L).
    if (c == 0x0c) { g_last_key_was_arrow = false; return kKeyCtrlL; }
    if (c == 0x0e) { g_last_key_was_arrow = false; return kKeyCtrlN; }   // Ctrl+N (audio book mode: small note field <-> big notes overlay)
    if (c == 0x14) { g_last_key_was_arrow = false; return kKeyCtrlT; }   // Ctrl+T: the cover look one step back
    if (c == 0x06) { g_last_key_was_arrow = false; return kKeyCtrlF; }   // Ctrl+F
    if (c == 0x0f) { g_last_key_was_arrow = false; return kKeyCtrlO; }   // Ctrl+O
    if (c == 0x00) { g_last_key_was_arrow = false; return kKeyCtrl2; }   // Ctrl+2 (a terminal sends the NUL byte for it)
    if (c == 0x1a) { g_last_key_was_arrow = false; return kKeyCtrlZ; }   // Ctrl+Z: one step forward (VSUSP is off, see below)
    g_last_key_was_arrow = false;
    return c;
#endif
}

int nearest_256(int r, int g, int b) {
    static const int kLevels[6] = {0, 95, 135, 175, 215, 255};   // the steps of the 6 x 6 x 6 colour cube (16..231)
    auto step = [](int v) { int best = 0; for (int i = 1; i < 6; ++i) if (std::abs(kLevels[i] - v) < std::abs(kLevels[best] - v)) best = i; return best; };
    r = std::clamp(r, 0, 255); g = std::clamp(g, 0, 255); b = std::clamp(b, 0, 255);
    const int ri = step(r), gi = step(g), bi = step(b);
    auto dist = [&](int R, int G, int B) { return (R - r) * (R - r) * 3 + (G - g) * (G - g) * 4 + (B - b) * (B - b) * 2; };
    const int cube = 16 + 36 * ri + 6 * gi + bi;
    const int dc = dist(kLevels[ri], kLevels[gi], kLevels[bi]);
    const int avg = (r + g + b) / 3;
    const int gi2 = std::clamp((avg - 8 + 5) / 10, 0, 23);   // the grey ramp 232..255: 8, 18, ... 238
    const int gv = 8 + 10 * gi2;
    return dist(gv, gv, gv) < dc ? 232 + gi2 : cube;
}

std::string to_256_colours(const std::string& in) {
    if (in.find(";2;") == std::string::npos) return in;
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '\x1b' || i + 1 >= in.size() || in[i + 1] != '[') { out += in[i++]; continue; }
        // a CSI sequence: parameters (digits and ';') up to the final byte
        size_t j = i + 2;
        while (j < in.size() && ((in[j] >= '0' && in[j] <= '9') || in[j] == ';')) ++j;
        if (j >= in.size() || in[j] != 'm') { out.append(in, i, (j < in.size() ? j + 1 : j) - i); i = j < in.size() ? j + 1 : j; continue; }
        std::vector<int> p;
        {
            int cur = 0;
            bool any = false;
            for (size_t k = i + 2; k < j; ++k) {
                if (in[k] == ';') { p.push_back(any ? cur : 0); cur = 0; any = false; }
                else { cur = cur * 10 + (in[k] - '0'); any = true; if (cur > 100000) cur = 100000; }
            }
            p.push_back(any ? cur : 0);
        }
        std::string sgr = "\x1b[";
        for (size_t k = 0; k < p.size(); ++k) {
            if ((p[k] == 38 || p[k] == 48 || p[k] == 58) && k + 4 < p.size() && p[k + 1] == 2) {
                sgr += std::to_string(p[k]) + ";5;" + std::to_string(nearest_256(p[k + 2], p[k + 3], p[k + 4]));
                k += 4;
            } else if ((p[k] == 38 || p[k] == 48 || p[k] == 58) && k + 2 < p.size() && p[k + 1] == 5) {
                sgr += std::to_string(p[k]) + ";5;" + std::to_string(p[k + 2]);   // a 256-colour one stays (its number must not be read as a code)
                k += 2;
            } else sgr += std::to_string(p[k]);
            if (k + 1 < p.size()) sgr += ';';
        }
        out += sgr + "m";
        i = j + 1;
    }
    return out;
}

bool truecolor_ok() {
    static const bool ok = [] {
        if (const char* e = std::getenv("MOUSIKI_COLORS")) {
            const std::string v = e;
            if (v == "256") return false;
            if (v == "truecolor" || v == "24bit") return true;
        }
#if defined(__APPLE__)
        const char* prog = std::getenv("TERM_PROGRAM");
        if (prog && std::string(prog) == "Apple_Terminal") {
            // macOS Terminal knows 24-bit colours from macOS 26 (Darwin 25) on
            struct utsname u {};
            if (uname(&u) == 0) return std::atoi(u.release) >= 25;
            return false;
        }
#endif
        return true;
    }();
    return ok;
}

// Where the cursor is left after a frame. A terminal that is made lower moves everything up by the rows it loses when the cursor
// sits below the new bottom (which a frame ending on its last line always does): the first row is thrown away, and our repaint then
// fits it back in -- the visible "jump". With the (hidden) cursor parked in the top-left corner nothing is pushed out.
static std::string park_cursor(const std::string& frame) {
    const size_t show = frame.rfind("\x1b[?25h"), hide = frame.rfind("\x1b[?25l");
    if (show != std::string::npos && (hide == std::string::npos || show > hide)) return frame;   // a text caret is meant to be seen
    return frame + "\x1b[H";
}

void write_frame(const std::string& frame_in) {
    const std::string frame = park_cursor(frame_in);
#if defined(_WIN32)
    // Synchronized update (ConPTY / Windows Terminal honour it; older consoles ignore it): one finished frame is painted, never a half-drawn one.
    std::cout << "\x1b[?2026h" << frame << "\x1b[?2026l" << std::flush;
#else
    std::cout.flush(); // anything already buffered must precede the frame
    std::string buf;
    buf.reserve(frame.size() + 16);
    buf += "\x1b[?2026h"; // begin synchronized update
    buf += truecolor_ok() ? frame : to_256_colours(frame);
    buf += "\x1b[?2026l"; // end synchronized update -> terminal paints the finished frame
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = ::write(STDOUT_FILENO, buf.data() + off, buf.size() - off);
        if (n > 0) { off += static_cast<size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) { // non-blocking tty: wait until it can take more
            struct pollfd pfd = {STDOUT_FILENO, POLLOUT, 0};
            ::poll(&pfd, 1, 50);
            continue;
        }
        break; // real error (terminal gone) -- nothing sensible left to do
    }
#endif
}

int TerminalIO::rows() const {
#if defined(_WIN32)
    return win_term_rows();
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 40;
#endif
}

int TerminalIO::cols() const {
#if defined(_WIN32)
    return win_term_cols();
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 155;
#endif
}

// ---------------------------------------------------------------------------
// Emoji replacement
//
// Whether an emoji occupies one cell or two is decided by the *terminal*, not
// by this program, and terminals disagree: conhost, Windows Terminal (which
// changed its rules between releases), the VS Code terminal and ConPTY
// each ship their own width tables, and multi-codepoint emoji (ZWJ sequences,
// flags, skin tones, VS16 "emoji style" forms, keycaps) are measured
// differently again. Every column calculation in this file is only as good as
// our guess at that answer, and a wrong guess by a single cell shifts every
// "|" to the right of the title on that row.
//
// So by default an emoji (a whole cluster, however many codepoints it has) is
// drawn as ONE plain "?" -- a character every terminal agrees is exactly one
// cell wide -- and display_width()/pad_right()/truncate_str()/... all measure
// that "?". The layout is then correct no matter what the terminal thinks of
// emoji. Set  ReplaceEmoji=false  in config.txt to draw the real emoji again
// (alignment then depends on the terminal agreeing with the width table).
//
// Deliberately narrow: only genuine emoji are replaced. Text symbols that
// music titles use all the time (music notes, stars, arrows, dingbat checks)
// are untouched, as are all CJK / Indic / Latin characters.
// ---------------------------------------------------------------------------
static int g_emoji_mode = 2;   // 0 off, 1 safe (keep plain one-codepoint emoji), 2 all

void set_emoji_mode(int m) { g_emoji_mode = m < 0 ? 0 : m > 2 ? 2 : m; }
void set_emoji_replacement(bool on) { g_emoji_mode = on ? 2 : 0; }

static bool g_marquee = true;
void set_marquee_enabled(bool on) { g_marquee = on; }
bool marquee_enabled() { return g_marquee; }

namespace {

struct CpRange { uint32_t lo, hi; };

// Codepoints below U+1F000 with Emoji_Presentation=Yes, i.e. ones that are
// emoji even without a variation selector (sorted, binary-searched).
constexpr CpRange kBmpEmoji[] = {
    {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
    {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
    {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
};

bool cp_in_table(uint32_t cp, const CpRange* t, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < t[mid].lo)      hi = mid;
        else if (cp > t[mid].hi) lo = mid + 1;
        else                     return true;
    }
    return false;
}

// A codepoint that is an emoji on its own.
bool is_emoji_base(uint32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true; // pictographs, flags' regional indicators, emoticons, ...
    return cp_in_table(cp, kBmpEmoji, sizeof(kBmpEmoji) / sizeof(kBmpEmoji[0]));
}

bool is_regional_indicator(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
bool is_emoji_modifier(uint32_t cp)     { return cp >= 0x1F3FB && cp <= 0x1F3FF; } // skin tones
bool is_emoji_tag(uint32_t cp)          { return cp >= 0xE0020 && cp <= 0xE007F; } // flag subdivisions

std::string replace_emoji_valid(const std::string& s) {
    if (g_emoji_mode == 0) return s;
    // Fast path: every emoji, VS16, ZWJ and keycap is encoded with a lead byte
    // >= 0xE2, so nearly every ordinary title bails out here after one scan.
    bool maybe = false;
    for (unsigned char c : s) { if (c >= 0xE2) { maybe = true; break; } }
    if (!maybe) return s;

    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);

        // A plain character turned into an emoji by a following VS16 (U+FE0F)
        // or keycap (U+20E3): "\u2764\uFE0F", "1\uFE0F\u20E3", ...
        bool forms_emoji = false;
        if (!is_emoji_base(cp) && i < s.size()) {
            size_t j = i;
            uint32_t next = utf8_decode(s, j);
            forms_emoji = (next == 0xFE0F || next == 0x20E3);
        }
        if (!is_emoji_base(cp) && !forms_emoji) {
            out.append(s, start, i - start);
            continue;
        }

        // Swallow the rest of the cluster: modifiers, variation selectors,
        // keycaps, tag sequences, a second regional indicator (flags), and
        // ZWJ-joined follow-up emoji.
        const size_t base_end = i;
        bool first_is_ri = is_regional_indicator(cp);
        while (i < s.size()) {
            size_t j = i;
            uint32_t next = utf8_decode(s, j);
            if (next == 0xFE0F || next == 0xFE0E || next == 0x20E3
                || is_emoji_modifier(next) || is_emoji_tag(next)) {
                i = j;
            } else if (first_is_ri && is_regional_indicator(next)) {
                i = j;
                first_is_ri = false; // a flag is exactly two indicators
            } else if (next == 0x200D && j < s.size()) {
                size_t k = j;
                uint32_t after = utf8_decode(s, k);
                if (is_emoji_base(after)) { i = k; } else { break; }
            } else {
                break;
            }
        }
        // "Safe": a plain emoji of one codepoint (and an old enough Unicode version) is kept; only the clusters
        // that terminals and fonts draw differently become "?".
        if (g_emoji_mode == 1 && is_emoji_base(cp) && !is_regional_indicator(cp) && cp <= 0x1F9FF && i == base_end) {
            out.append(s, start, i - start);
            continue;
        }
        out += '?';
    }
    return out;
}

// The first step of every width helper below (display_width, utf8_take, pad_right, truncate_str, ...): malformed
// UTF-8 is repaired first (utf8_repair(): each broken byte becomes one visible U+FFFD), so the text these helpers
// measure is exactly the text they hand back to be drawn -- then the emoji replacement as before.
std::string replace_emoji(const std::string& s) {
    if (utf8_is_valid(s)) return replace_emoji_valid(s);
    return replace_emoji_valid(utf8_repair(s));
}

} // namespace

static int codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (is_indic_codepoint(cp)) return indic_codepoint_width(cp);
#if defined(_WIN32)
    // Not wcwidth(static_cast<wchar_t>(cp)): wchar_t is 16 bits here, so
    // that cast silently mangles every codepoint above U+FFFF.
    int w = win_codepoint_width(cp);
#else
    int w = wcwidth(static_cast<wchar_t>(cp));
#endif
    return w < 0 ? 0 : w;
}
// w
// int display_width(const std::string& s) {
//     std::string clean = sanitize_lyric_text(s);
// 
//     int cols = 0;
//     size_t i = 0;
//     uint32_t prev_cp = 0;
// 
//     while (i < clean.size()) {
//         uint32_t cp = utf8_decode(clean, i);
//         int w = codepoint_width(cp);

bool diff_frame_lines(const std::string& body, std::vector<std::string>& prev, std::string& out) {
    std::vector<std::string> lines;
    size_t from = 0;
    while (from < body.size()) {
        size_t nl = body.find('\n', from);
        if (nl == std::string::npos) nl = body.size();
        size_t end = nl;
        if (end > from && body[end - 1] == '\r') --end;
        lines.emplace_back(body, from, end - from);
        from = nl + 1;
    }
    const bool same_shape = lines.size() == prev.size();
    if (same_shape) {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i] == prev[i]) continue;
            out += "\x1b[" + std::to_string(i + 1) + ";1H\x1b[0m";
            out += lines[i];
        }
    }
    prev.swap(lines);
    return same_shape;
}

int display_width(const std::string& raw) {
    const std::string s = replace_emoji(raw);
    int cols = 0;
    size_t i = 0;
    uint32_t prev_cp = 0;
    while (i < s.size()) {
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        // Virama conjunct subtraction: if previous char was a Virama and current is a consonant (width 1),
        // they form a conjunct ligature that fits in the same cell. Subtract 1 to compensate.
        if (is_indic_codepoint(cp)) {
            if (prev_cp == 0x094D || prev_cp == 0x09CD || prev_cp == 0x0A4D || 
                prev_cp == 0x0ACD || prev_cp == 0x0B4D || prev_cp == 0x0BCD || 
                prev_cp == 0x0C4D || prev_cp == 0x0CCD || prev_cp == 0x0D4D) {
                if (w == 1) {
                    cols -= 1; // Merge with previous cell
                }
            }
        }
        
        cols += w;
        prev_cp = cp;
    }
    return cols;
}

std::string utf8_take(const std::string& raw, int width) {
    const std::string s = replace_emoji(raw);
    std::string out;
    size_t i = 0;
    int used = 0;
    uint32_t prev_cp = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        
        if (is_indic_codepoint(cp)) {
            if (prev_cp == 0x094D || prev_cp == 0x09CD || prev_cp == 0x0A4D || 
                prev_cp == 0x0ACD || prev_cp == 0x0B4D || prev_cp == 0x0BCD || 
                prev_cp == 0x0C4D || prev_cp == 0x0CCD || prev_cp == 0x0D4D) {
                if (w == 1) {
                    used -= 1; // Merge with previous cell
                }
            }
        }
        
        if (used + w > width) {
            if (w == 0) {
                out += s.substr(start, i - start);
                used += w;
                prev_cp = cp;
                continue;
            }
            break;
        }
        out += s.substr(start, i - start);
        used += w;
        prev_cp = cp;
    }
    return out;
}

std::string utf8_skip_take(const std::string& raw, int skip_cols, int take_cols) {
    const std::string s = replace_emoji(raw);
    std::string out;
    size_t i = 0;
    int skipped = 0;
    uint32_t prev_cp = 0;
    while (i < s.size() && skipped < skip_cols) {
        uint32_t cp = utf8_decode(s, i);
        skipped += codepoint_width(cp);
        prev_cp = cp;
    }
    (void)prev_cp;
    // Zero-width marks right after the skipped part belong to its last character (decomposed "ト" + U+3099 = "ド"):
    // skip them too, or the window would start with a combining mark that has nothing to sit on.
    if (skipped > 0) {
        while (i < s.size()) {
            size_t j = i;
            const uint32_t cp = utf8_decode(s, j);
            if (codepoint_width(cp) != 0 || cp < 0x300) break;   // only real combining marks, not control characters
            i = j;
        }
    }
    int taken = 0;
    bool cut = false;   // a double-width character did not fit at the right edge
    while (i < s.size() && taken < take_cols) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        if (taken + w > take_cols) { cut = true; break; }
        out += s.substr(start, i - start);
        taken += w;
    }
    // A double-width character cut by either edge of the window is left out whole; its lost column is made up
    // with a blank so the slice keeps exactly `take_cols` columns while there is text (a marquee frame that came
    // out one column short moved everything right of it, the border included).
    if (skipped > skip_cols && taken < take_cols) { out.insert(0, " "); ++taken; }
    if (cut && taken < take_cols) out.append(static_cast<size_t>(take_cols - taken), ' ');
    return out;
}

std::string pad_right(const std::string& raw, int width) {
    if (width <= 0) return "";
    const std::string s = replace_emoji(raw);
    int w = display_width(s);
    if (w >= width) {
        // utf8_take() stops BEFORE a double-width character that would only half fit, so the cut can come out one
        // column short (CJK titles like "「レモンドロップ」..."); fill that column, the result is always `width` wide.
        std::string t = utf8_take(s, width);
        const int tw = display_width(t);
        return tw < width ? t + std::string(static_cast<size_t>(width - tw), ' ') : t;
    }
    return s + std::string(width - w, ' ');
}

std::string pad_left(const std::string& raw, int width) {
    if (width <= 0) return "";
    const std::string s = replace_emoji(raw);
    int w = display_width(s);
    if (w >= width) {
        std::string t = utf8_take(s, width);   // may stop one column short before a double-width character, see pad_right()
        const int tw = display_width(t);
        return tw < width ? std::string(static_cast<size_t>(width - tw), ' ') + t : t;
    }
    return std::string(width - w, ' ') + s;
}

std::string truncate_str(const std::string& raw, int width) {
    if (width <= 0) return "";
    const std::string s = replace_emoji(raw);
    int w = display_width(s);
    if (w <= width) return s;
    // A cut string is always exactly `width` columns: utf8_take() can stop one column short before a double-width
    // character, and the blank that makes up for it goes in front of the "..." so the dots still end the column.
    std::string t = utf8_take(s, width <= 3 ? width : width - 3);
    const int miss = (width <= 3 ? width : width - 3) - display_width(t);
    if (miss > 0) t.append(static_cast<size_t>(miss), ' ');
    return width <= 3 ? t : t + "...";
}

std::vector<std::string> wrap_lines(const std::string& raw, int width, int max_lines) {
    const std::string s = replace_emoji(raw);
    std::vector<std::string> out;
    if (width <= 0 || max_lines <= 0) return out;

    // Word-wrap on ASCII spaces (the only inputs are track titles and
    // filesystem paths). The hard case this exists for: a "word" that alone
    // is wider than one whole line -- a long unbroken filename, or a CJK/
    // Thai/etc. title, which has no spaces at all and would otherwise be
    // one giant word -- is sliced into successive width-sized chunks
    // instead of being dumped whole onto an overflowing line or silently
    // cut down to a single line.
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : s) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }

    size_t wi = 0;
    while (wi < words.size() && static_cast<int>(out.size()) < max_lines) {
        std::string line;
        int line_w = 0;
        bool line_done = false;
        while (wi < words.size() && !line_done) {
            std::string& w = words[wi];
            int ww = display_width(w);
            if (ww > width) {
                // Doesn't fit on a line by itself. If this line already has
                // something on it, close it out so the oversized word gets
                // its own fresh line(s) to be chunked across.
                if (!line.empty()) { line_done = true; break; }
                std::string piece = utf8_take(w, width);
                if (piece.empty()) { ++wi; continue; } // width too small for even one codepoint; skip rather than loop forever
                line = piece;
                line_w = display_width(piece);
                std::string rest = w.substr(piece.size());
                if (rest.empty()) ++wi; else w = rest;
                line_done = true;
                break;
            }
            int add_w = ww + (line.empty() ? 0 : 1);
            if (line_w + add_w > width) { line_done = true; break; }
            if (!line.empty()) { line += ' '; line_w += 1; }
            line += w;
            line_w += ww;
            ++wi;
        }
        out.push_back(line);
    }

    // Words remain but we're out of lines: mark the truncation on the last
    // line actually produced, the same way truncate_str() does for a
    // single line.
    if (wi < words.size() && !out.empty()) {
        std::string& last = out.back();
        last = (width <= 3) ? utf8_take(last, width) : utf8_take(last, std::max(0, width - 3)) + "...";
    }
    return out;
}

std::vector<LegendRow> layout_legend_tree(std::vector<LegendGroup> groups, int width, int max_rows) {
    std::vector<LegendRow> rows;
    if (groups.empty()) return rows;
    while (max_rows > 0 && static_cast<int>(groups.size()) > max_rows && groups.size() > 1) {
        LegendGroup last = std::move(groups.back());
        groups.pop_back();
        for (auto& c : last.cmds) groups.back().cmds.push_back(std::move(c));
    }
    int label_w = 1;
    for (const auto& g : groups) label_w = std::max(label_w, display_width(g.label));
    const int cmd_w = std::max(10, width - (6 + label_w));
    // "[key] Long\x01Short" -> the long and the short form
    const auto split_cmd = [](const std::string& c, std::string& lng, std::string& shrt) {
        const size_t sep = c.find('\x01');
        if (sep == std::string::npos) { lng = shrt = c; return; }
        lng = c.substr(0, sep);
        const size_t close = lng.find("] ");
        shrt = (close == std::string::npos ? std::string() : lng.substr(0, close + 2)) + c.substr(sep + 1);
    };
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        std::vector<std::string> lng, shrt;
        for (const auto& c : groups[gi].cmds) {
            std::string l, s;
            split_cmd(c, l, s);
            lng.push_back(l);
            shrt.push_back(s);
        }
        std::string cmds;
        bool fitted = false;
        std::string second;   // the second row of a group that may use two (max_lines > 1)
        const auto joined_of = [](const std::vector<std::string>& v, size_t from, size_t to) {
            std::string j;
            for (size_t i = from; i < to; ++i) j += (i > from ? " " : "") + v[i];
            return j;
        };
        const auto try_two_rows = [&](const std::vector<std::string>* list) {   // the break goes between two commands, as even as possible
            int best = -1, best_w = 1 << 30;
            for (size_t k = 1; k < list->size(); ++k) {
                const int w1 = display_width(joined_of(*list, 0, k)), w2 = display_width(joined_of(*list, k, list->size()));
                if (w1 <= cmd_w && w2 <= cmd_w && std::max(w1, w2) < best_w) { best = static_cast<int>(k); best_w = std::max(w1, w2); }
            }
            if (best <= 0) return false;
            cmds = joined_of(*list, 0, static_cast<size_t>(best)); second = joined_of(*list, static_cast<size_t>(best), list->size());
            return true;
        };
        for (const std::vector<std::string>* list : {&lng, &shrt}) {
            if (list == &shrt && groups[gi].max_lines > 1 && try_two_rows(&lng)) { fitted = true; break; }   // two rows with the long labels beat one row of short ones
            for (int gap : {1}) {   // one space between commands, everywhere (a long legend must fit a 120-column line)
                std::string joined;
                for (size_t ci = 0; ci < list->size(); ++ci) joined += (ci ? std::string(static_cast<size_t>(gap), ' ') : std::string()) + (*list)[ci];
                if (display_width(joined) <= cmd_w) { cmds = joined; fitted = true; break; }
            }
            if (fitted) break;
        }
        if (!fitted && groups[gi].max_lines > 1) fitted = try_two_rows(&shrt);
        if (!fitted) {
            for (size_t ci = 0; ci < shrt.size(); ++ci) {
                const std::string next = cmds + (ci ? " " : "") + shrt[ci];
                if (display_width(next) + 2 > cmd_w) break;
                cmds = next;
            }
            cmds += cmds.empty() ? "\u2026" : " \u2026";
        }
        LegendRow r;
        r.tree = groups.size() == 1 ? "\u2500\u2500 " : (gi == 0 ? "\u250c\u2500 " : (gi + 1 == groups.size() ? "\u2514\u2500 " : "\u251c\u2500 "));
        r.label = pad_right(groups[gi].label, label_w);
        r.cmds = cmds;
        rows.push_back(std::move(r));
        if (!second.empty()) {   // the continuation: the tree line goes on (not after the last group), the label stays blank
            LegendRow c;
            c.tree = gi + 1 == groups.size() ? "   " : "\u2502  ";
            c.label = pad_right(std::string(), label_w);
            c.cmds = second;
            rows.push_back(std::move(c));
        }
    }
    return rows;
}

std::string paint_legend_row(const LegendRow& row, const std::string& border, const std::string& header, const std::string& legend, int pad_to,
                             const std::string& cmd) {
    const std::string R = "\x1b[0m";
    std::string body;
    if (cmd.empty()) {
        std::string plain = row.cmds;
        plain.erase(std::remove_if(plain.begin(), plain.end(), [](char c) { return c == '\x02' || c == '\x03'; }), plain.end());
        body = legend + plain + R;
    } else {   // keys in [brackets] in the legend colour, the rest (descriptions, the closing "...") in the command colour
        size_t i = 0;
        while (i < row.cmds.size()) {
            if (row.cmds[i] == '[') {
                const size_t e = row.cmds.find(']', i);
                const size_t stop = e == std::string::npos ? row.cmds.size() : e + 1;
                body += legend + row.cmds.substr(i, stop - i) + R;
                i = stop;
            } else {
                size_t e = row.cmds.find('[', i);
                if (e == std::string::npos) e = row.cmds.size();
                const std::string seg = row.cmds.substr(i, e - i);
                // a sub-heading wrapped in \002...\003 ("SINGLE TRACKS") stands out in the header colour; the markers
                // themselves are not printed (a "…" cut can drop the closing one -- the heading then runs to the segment end)
                size_t k = 0;
                while (k < seg.size()) {
                    const size_t open = seg.find('\x02', k);
                    if (open == std::string::npos) { body += cmd + seg.substr(k) + R; break; }
                    if (open > k) body += cmd + seg.substr(k, open - k) + R;
                    size_t close = seg.find('\x03', open + 1);
                    if (close == std::string::npos) close = seg.size();
                    body += header + seg.substr(open + 1, close - open - 1) + R;
                    k = close + 1;
                }
                i = e;
            }
        }
    }
    // the tree glyph and the bar each follow their border-colour escape directly: that is what the smooth border gradient looks for
    std::string out = border + row.tree + R + header + row.label + R + " " + border + "\u2502" + R + " " + body;
    const int used = display_width(row.tree) + display_width(row.label) + 3 + display_width(row.cmds);
    if (pad_to > used) out += std::string(static_cast<size_t>(pad_to - used), ' ');
    return out;
}

} // namespace muisc
