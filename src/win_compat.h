#pragma once
// ---------------------------------------------------------------------------
// Windows compatibility layer.
//
// Everything in here is a no-op / not compiled on Linux, Android and macOS,
// so the original POSIX code paths are untouched on those platforms.
//
// Deliberately does NOT include <windows.h>. Pulling that into a header that
// app.cpp / terminal_ui.cpp include would drag in the min/max macros, the
// `near`/`far` legacy junk and a `GetMessage`-style API surface that collides
// with ordinary identifiers. All of that stays confined to win_compat.cpp.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

#if defined(_MSC_VER)
// MSVC has no <unistd.h> and therefore no ssize_t. MinGW-w64 already
// provides it, so this is MSVC-only to avoid a conflicting typedef.
#include <basetsd.h>
using ssize_t = SSIZE_T;
#endif

namespace muisc {

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

// Called once from main() before anything prints. Switches the console to
// UTF-8 (both code pages) and turns on ENABLE_VIRTUAL_TERMINAL_PROCESSING so
// the ANSI escapes the whole UI is built out of are actually interpreted
// instead of being echoed as literal "<-[38;2;..m" garbage.
//
// Also installs a console control handler, because Ctrl+C on Windows is not
// deliverable to _getch(): the default handler kills the process outright,
// which would leave the user sitting in the alternate screen buffer with a
// hidden cursor and no way back. The handler undoes both before letting the
// termination proceed.
//
// Returns false when VT processing could not be enabled (Windows 10 builds
// older than 1511, or a third-party console host). The app still runs; it
// just looks wrong, and main() prints a hint.
bool win_console_init();

// Restores whatever console modes and code pages were in effect at startup.
// Safe to call more than once and safe to call from the Ctrl+C handler.
void win_console_restore();

// Raw-ish input mode, the counterpart of the termios ECHO/ICANON clearing on
// POSIX. Input is actually read through _getch(), which is already
// unbuffered and non-echoing, so this only has to deal with the output side
// and with remembering what to put back.
void win_raw_mode_enter();
void win_raw_mode_exit();

// Non-blocking single logical key read, with exactly the return contract
// terminal_ui.h documents: arrows collapse to 'A'/'B'/'C'/'D', a lone Escape
// is 27, Backspace is 127, nothing waiting is 0. A non-ASCII keystroke (an
// umlaut, any other accented or non-Latin character typed directly off the
// keyboard) is UTF-8 encoded and dispensed one byte per call, exactly like
// a UTF-8 POSIX terminal already delivers such a keystroke one byte per
// read() -- callers that only care about single-byte hotkeys are unaffected
// (multi-byte sequences only ever start with a byte >= 0x80, never a plain
// ASCII hotkey value), and callers that accept free text (search box, etc.)
// already handle exactly this shape of input from the POSIX side.
int win_poll_key();
// Waits up to `ms` milliseconds for something in the console input queue (a key press or release, a resize or focus event); true when
// there is something (or a byte of a half-delivered character is waiting). The console input handle is signalled while its queue is
// not empty, and win_poll_key() empties the queue, so the wait ends at once on input and sleeps otherwise.
bool win_wait_input(int ms);

// True while the key win_poll_key() just returned was one of the four
// arrow keys. Because arrows deliberately collapse to the letters
// 'A'-'D' (the contract documented above), a value of 'A' on its own can
// be either an Up press or a real capital A -- and a path/text field that
// wants to accept uppercase letters needs to tell those apart.
// see last_key_was_arrow() in terminal_ui.h for the platform-neutral view.
bool win_last_key_was_arrow();

// True while the key win_poll_key() just returned was Up/Down with Shift
// held (the value is still 'A'/'B'). See last_key_was_shifted().
bool win_last_key_was_shifted();
bool win_last_key_was_page();
bool win_last_key_was_ctrl();   // Up / Down with CTRL held

// Text-entry mode. While `on`, ENABLE_PROCESSED_INPUT is cleared from the
// input handle, which is the one console setting that decides whether Ctrl+C
// is swallowed by the system (CTRL_C_EVENT -> the ctrl_handler above, i.e.
// "quit") or handed to win_poll_key() as an ordinary key event. A text field
// needs the latter so Ctrl+C can mean "copy"; everywhere else it stays off so
// Ctrl+C keeps meaning "quit", exactly as it always has. win_raw_mode_enter()
// re-derives the mode from this flag on every poll, so the state survives the
// per-frame raw-mode reassert.
void win_set_text_entry(bool on);
void win_set_paste_aware(bool on);   // a burst of typed characters (a paste into a text field) is returned as kKeyPaste
std::string win_take_paste();

// The Windows clipboard as UTF-8 (CF_UNICODETEXT -- never CP_ACP, which would
// mangle exactly the paths/titles this UI is full of). get() returns "" when
// the clipboard holds no text or cannot be opened.
std::string win_clipboard_get();
void win_clipboard_set(const std::string& utf8);

// The screen in characters, a rough estimate from its pixels (8 x 16 per character) for the largest size the terminal may be asked for.
bool win_screen_chars_estimate(int& cols, int& rows);
// Full screen on / off for the terminal window this console belongs to: presses its F11 (Windows Terminal) -- only while that window is
// in front, and only when it is not already in the wanted state. False: nothing was done.
bool win_set_fullscreen(bool want);
int win_term_rows();
int win_term_cols();

// ---------------------------------------------------------------------------
// Environment / paths
// ---------------------------------------------------------------------------

// Seven different translation units do getenv("HOME") to find the config,
// cache, snapshot and log directories. HOME simply does not exist on Windows,
// so every one of them would silently fall back to "." and scatter dot-folders
// into whatever directory the exe happened to be launched from.
//
// Rather than rewrite all seven call sites, this sets HOME (for this process
// only -- it is not persisted to the user's environment) to %USERPROFILE%, so
// the existing code finds C:\Users\you\.config\mousiki\config.txt and friends.
// An explicitly pre-set HOME is respected and left alone.
void win_bootstrap_env();

// Replacement for readlink("/proc/self/exe"). Empty string on failure.
std::string win_executable_path();
// The full path of a program in the Windows system folder ("cmd.exe" -> "C:\\Windows\\System32\\cmd.exe"; UTF-8), so it is never
// a copy lying in the folder Mousiki was started in. Falls back to the bare name if the system folder cannot be read.
std::string win_system_program(const std::string& name);

// Resolves a bare program name against PATH and PATHEXT, returning a full
// path. CreateProcess only ever appends ".exe" on its own, so without this a
// yt-dlp installed by scoop or pipx as a .cmd shim is invisible to us.
// Returns an empty string when nothing matches.
std::string win_find_executable(const std::string& name);

// Windows has no "python3" binary. Depending on how Python was installed the
// working invocation is "python", "python3", or the launcher "py -3". This
// probes them once and caches the answer. Returns an empty string if none of
// them work, which lyrics_fetcher.cpp reports as PythonMissing.
const std::string& win_python_command();

// ---------------------------------------------------------------------------
// Small POSIX shims
// ---------------------------------------------------------------------------

// Converts a path's native (UTF-16) representation to a UTF-8 std::string.
// Deliberately NOT the same thing as fs::path::string(): on Windows that
// converts through the process's ANSI code page and throws
// std::system_error ("No mapping for the Unicode character exists in the
// target multi-byte code page") for any character that code page can't
// represent -- which ordinary Windows filenames routinely contain (accents,
// umlauts, em dashes, non-Latin scripts, ...). Converting to UTF-8 instead
// always succeeds for any valid path, matching what .string() already does
// for free on every other platform (where UTF-8 is the native encoding).
std::string win_path_to_utf8(const std::wstring& native);

// The inverse, and just as necessary: fs::path(std::string) converts *from*
// the ANSI code page too, so building a path out of a UTF-8 string (one read
// from config.txt, the session snapshot, or the folder filter) without this
// silently yields a mangled path that then appears not to exist. Prefer
// path_from_utf8() in path_utf8.h over calling this directly -- that header
// is the platform-agnostic front door for both directions.
std::wstring win_utf8_to_wide(const std::string& utf8);

// Positional pread() over a file descriptor from _wopen(). Returns bytes
// read, 0 at EOF, -1 on error. Unlike a seek+read pair it leaves the
// descriptor's own file pointer untouched.
long long win_pread(int fd, void* buf, std::size_t count, long long offset);

// Column width of a Unicode scalar value. Note this takes a uint32_t rather
// than a wchar_t: wchar_t is 16 bits on Windows, so the obvious
// wcwidth(static_cast<wchar_t>(cp)) would truncate every astral-plane
// codepoint (all emoji, among other things) to a meaningless BMP value.
int win_codepoint_width(uint32_t cp);

// localtime_r equivalent. MSVC has localtime_s with reversed arguments;
// MinGW-w64's localtime_r is only visible under certain feature-test macros.
std::tm win_localtime(std::time_t t);

} // namespace muisc

#endif // _WIN32
