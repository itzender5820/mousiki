// The console log (see console_log.cpp): ConsoleLog keeps recent lines and writes them to a file at a
// basic or verbose level.
#pragma once
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// Two verbosity tiers:
//   Basic   -- every external command mousiki actually ran (yt-dlp,
//              ffprobe/ffmpeg, lyrics-fetch, etc), each as "==> cmd"
//              followed by its raw captured output. This is the tier
//              most people want: "what did the player just do".
//   Verbose -- everything Basic shows, PLUS internal/OS-level events:
//              terminal resizes, audio-device init results, spawn
//              failures with the OS-provided strerror(), and anything
//              else that isn't itself an external command.
enum class LogVerbosity { Basic, Verbose };

class ConsoleLog {
public:
    static ConsoleLog& instance();

    // Called once, as the very first thing App::run() does, before
    // anything else can log a single line. Starts a fresh console.log at
    // $HOME/.cache/mousiki/logs/console.log; what the last session left
    // there is kept as console_previous.log (the one before that is
    // gone), and its last lines are shown above this session's in the
    // console (lines()).
    void init(LogVerbosity level);

    // true once init() has run in this process (the radio, which can run first, starts the log only when the player has not).
    bool initialized() const;
    void set_level(LogVerbosity level);
    LogVerbosity level() const;
    // [v] in the console: Basic <-> Verbose for this session only (nothing is saved). Returns the new level.
    LogVerbosity toggle_level();

    // Tier 1: an external command mousiki ran. Always recorded (both
    // Basic and Verbose show these) in exactly the "==> cmd" + raw
    // output shape that was asked for. `output` is used verbatim
    // (already however run_capture captured it -- may include stderr if
    // the call merged it, may not).
    void log_command(const std::string& cmd, const std::string& output, int exit_code);

    // Tier 2: Verbose-only internal/OS-level events (resize, device
    // init, spawn failures, etc). No-op at Basic level.
    void log_verbose(const std::string& line);

    // A short, friendly one-line note about something mousiki itself
    // did (played a track, added to queue, saved a snapshot, ...).
    // Always recorded at both tiers -- this is the "basic" narration
    // level, distinct from raw subprocess dumps.
    void log_basic(const std::string& line);

    // Snapshot copy of the in-memory buffer for rendering the Console
    // overlay. Safe to call every frame.
    std::vector<std::string> lines() const;

private:
    ConsoleLog() = default;
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::vector<std::string> prev_lines_;   // the end of the last session's log
    LogVerbosity level_ = LogVerbosity::Basic;
    bool initialized_ = false;
    // fs::path, not std::string: std::ofstream(std::string) opens the
    // file through the ANSI code page on Windows, so a user profile
    // containing a non-ASCII character (C:\\Users\\Jürgen\\...) could not
    // be opened at all. The fs::path overload uses the native wide path.
    fs::path log_path_;
    static constexpr size_t kMaxLines = 1500;   // this session's lines kept in memory (the last session's tail: the same number)

    void push_line_locked(const std::string& line);
    void append_to_file(const std::string& text);
};

} // namespace muisc
