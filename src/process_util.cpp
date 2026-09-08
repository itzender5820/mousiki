#include "process_util.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace muisc {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// Deliberately NOT popen()/fork()+exec() — popen() forks, and fork()ing a
// multithreaded process is unsafe: if another thread holds a libc lock
// (malloc's arena lock, etc.) at the instant of fork(), the child
// inherits that lock permanently held with no thread left alive to
// release it, and can hang forever the next time it needs that lock —
// intermittently, depending on timing. That's invisible in a
// single-threaded program but became a real, hard-to-reproduce hang the
// moment loading/search/lyrics started running on background threads
// concurrently. posix_spawn() is specified to be safe to call from a
// multithreaded process (no full fork() semantics), which is why this
// swaps to it instead of trying to patch around popen().
ProcResult run_capture(const std::string& cmd, bool merge_stderr) {
    ProcResult result;

    int out_pipe[2];
    if (pipe(out_pipe) != 0) {
        result.exit_code = -1;
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // Every subprocess we spawn was inheriting our own stdin -- i.e. the
    // terminal, in raw mode, which is exactly what poll_key() reads
    // from. That's harmless for something like ffprobe that never
    // touches stdin, but ffmpeg specifically reads keystrokes for
    // interactive control (pause/quit/etc) whenever stdin is a real TTY,
    // and manipulates termios to do it -- when it exits it can leave the
    // terminal back in canonical/line-buffered mode instead of raw,
    // which turns our normally non-blocking key read into a blocking
    // one and stalls the whole render loop until a keypress+Enter
    // happens to satisfy it. Redirecting every child's stdin to
    // /dev/null here means no subprocess we spawn can ever see or touch
    // our terminal, regardless of what that program's stdin behavior is.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);           // child doesn't read
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    if (merge_stderr) {
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);           // close the now-duped fd

    // posix_spawnp (not posix_spawn) + a bare "sh" resolves through PATH
    // instead of assuming /bin/sh exists — Termux's filesystem lives
    // under its own prefix, not the standard FHS layout, so a hardcoded
    // /bin/sh here silently failed every subprocess call (ffprobe,
    // yt-dlp, everything routed through this function) with no
    // diagnostic visible to the user.
    const char* argv[] = {"sh", "-c", cmd.c_str(), nullptr};
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "sh", &actions, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]); // parent never writes

    if (rc != 0) {
        close(out_pipe[0]);
        result.exit_code = -1;
        return result;
    }

    std::array<char, 4096> buf{};
    std::ostringstream oss;
    ssize_t n;
    while ((n = read(out_pipe[0], buf.data(), buf.size())) > 0) {
        oss.write(buf.data(), static_cast<std::streamsize>(n));
    }
    close(out_pipe[0]);
    result.out = oss.str();

    int status = 0;
    if (waitpid(pid, &status, 0) == pid && WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = -1;
    }
    return result;
}

} // namespace muisc
