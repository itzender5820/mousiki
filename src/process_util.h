// Running external programs (see process_util.cpp): run_capture() for a short command, ChildProcess
// for a long-running one. Programs are always started from an argument list (argv[0] is the program,
// the rest are its arguments, passed as-is). No shell is involved on any platform, so file names and
// URLs need no quoting and cannot be interpreted as shell syntax.
#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace muisc {

struct ProcResult {
    std::string out;   // combined stdout (stderr redirected away unless merge_stderr=true)
    int exit_code = -1;
    bool ok() const { return exit_code == 0; }
};

// The program and its arguments: args[0] is the program (a name on PATH or a path).
using Args = std::vector<std::string>;

// Runs the program and captures its stdout. stderr is discarded unless merge_stderr is true.
ProcResult run_capture(const Args& args, bool merge_stderr = false);

// Text for logs and the console overlay only (arguments with spaces or special characters are quoted
// so the line can be read back). It is never run.
std::string command_text(const Args& args);

// Windows only uses it, but it is plain text work (tested on every platform): `arg` quoted for a .cmd / .bat program, which
// cmd.exe reads first ("BatBadBut": no breaking out with a quote, no %NAME% expansion). false: a line break, which cannot be
// passed through cmd.exe at all.
bool quote_arg_for_cmd(const std::string& arg, std::string& out);

// The Python 3 interpreter as an argument list ("python3", or on Windows "py -3" or a full path). Empty when Python
// is not installed (Windows only; elsewhere the name is always returned and a missing binary shows up at spawn time).
Args python_command();

// ---------------------------------------------------------------------------
// Streaming variant
//
// run_capture() reads the child to completion before returning, which is fine
// for ffprobe and yt-dlp but wrong for the ffmpeg decode fallback, where the
// whole point is to consume PCM as it is produced. This exposes the same spawn
// semantics with an incremental read instead.
//
// The child is always started with stdin detached from the terminal. That is
// not cosmetic: ffmpeg reads keystrokes for interactive control whenever its
// stdin is a real console, and manipulates the terminal to do so, which used
// to leave the parent's input back in line-buffered mode and stall the entire
// render loop.
// ---------------------------------------------------------------------------

// Ends every child process that is still running (SIGKILL / TerminateProcess), for the end of the program: a task that waits for a
// download, a translation or a lyrics lookup then returns at once, and no child is left running on its own after the player has quit.
void kill_all_children();

class ChildProcess {
public:
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Reads up to `n` bytes of the child's stdout. Returns the byte count,
    // 0 at EOF, or -1 on error. Blocks until at least one byte is available.
    std::ptrdiff_t read(char* buf, std::size_t n);

    // Waits for exit and returns the exit code, or -1 if it could not be
    // determined. Idempotent.
    int wait();

    // Ends the child right away (SIGKILL / TerminateProcess); wait() then returns. Safe to call from another thread than
    // the one reading, and after the child has exited.
    void kill();

    // Makes the child yield the processor to the program itself (nice 5 / BELOW_NORMAL): the video decoder fills its buffer as fast as
    // it can after every start and would otherwise take the processor away from the picture being drawn. Best effort.
    void lower_priority();

private:
    ChildProcess();
    friend std::unique_ptr<ChildProcess> spawn_capture(const Args&, bool);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns nullptr if the process could not be started at all.
std::unique_ptr<ChildProcess> spawn_capture(const Args& args, bool merge_stderr);

#if !defined(_WIN32)
// Runs the program with its stdin read from the file `stdin_path`, its stdout and stderr discarded, and waits for it.
// Returns the exit code, or -1 if it could not be started. Used for the clipboard tools (POSIX only).
int run_with_stdin_file(const Args& args, const std::string& stdin_path);

// True when `name` is an executable found on PATH.
bool program_on_path(const std::string& name);
#endif

} // namespace muisc
