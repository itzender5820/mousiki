// Running external programs (ffmpeg, ffprobe, yt-dlp, the helper scripts): quoting of arguments,
// capturing output, and child processes that can be read and killed.
#include "process_util.h"
#include "console_log.h"

#include <array>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include "win_compat.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace muisc {

// Quotes one argument for display only: plain words are left alone, anything else gets single quotes.
static std::string quote_for_log(const std::string& s) {
    if (!s.empty() && s.find_first_of(" \t\"'\\$`;&|<>()*?[]{}!#~") == std::string::npos) return s;
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string command_text(const Args& args) {
    std::string out;
    for (const std::string& a : args) {
        if (!out.empty()) out += ' ';
        out += quote_for_log(a);
    }
    return out;
}

Args python_command() {
#if defined(_WIN32)
    const std::string& p = win_python_command();
    if (p.empty()) return {};
    if (p == "py -3") return {"py", "-3"};
    return {p};
#else
    return {"python3"};
#endif
}

// An argument for a .cmd / .bat shim, which cmd.exe reads before the program behind it does ("BatBadBut"): cmd.exe does not know
// \" -- a quote in the text would end the quoted argument and let & | < > run as commands -- and expands %NAME% even inside
// quotes. So: always quoted; a quote is written "" (cmd.exe stays inside the quotes, and the program behind the shim -- Python,
// yt-dlp: the Microsoft C runtime -- reads "" inside quotes as one quote); % is written %%cd:~,% (an expansion that yields a
// plain %, the trick Rust's standard library uses); backslashes before the closing quote are doubled. Line breaks cannot be
// passed through cmd.exe at all: false (the program is not started).
bool quote_arg_for_cmd(const std::string& arg, std::string& out) {
    out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\r' || c == '\n') return false;
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') { out.append(backslashes * 2, '\\'); out += "\"\""; backslashes = 0; continue; }
        out.append(backslashes, '\\');
        backslashes = 0;
        if (c == '%') { out += "%%cd:~,%"; continue; }
        out += c;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return true;
}


// =====================================================================
// POSIX implementation
// =====================================================================
#if !defined(_WIN32)

struct ChildProcess::Impl {
    int   out_fd = -1;
    pid_t pid    = -1;
    int   exit_code = -1;
    bool  reaped = false;
};

ChildProcess::ChildProcess() : impl_(new Impl) {}

ChildProcess::~ChildProcess() {
    if (impl_->out_fd >= 0) ::close(impl_->out_fd);
    wait();
}

void ChildProcess::kill() {
    if (!impl_->reaped && impl_->pid > 0) ::kill(impl_->pid, SIGKILL);
}

void ChildProcess::lower_priority() {
    if (!impl_->reaped && impl_->pid > 0) setpriority(PRIO_PROCESS, static_cast<id_t>(impl_->pid), 5);
}

std::ptrdiff_t ChildProcess::read(char* buf, std::size_t n) {
    if (impl_->out_fd < 0) return -1;
    return static_cast<std::ptrdiff_t>(::read(impl_->out_fd, buf, n));
}

int ChildProcess::wait() {
    if (impl_->reaped) return impl_->exit_code;
    impl_->reaped = true;
    int status = 0;
    if (impl_->pid > 0 && waitpid(impl_->pid, &status, 0) == impl_->pid && WIFEXITED(status)) {
        impl_->exit_code = WEXITSTATUS(status);
    }
    return impl_->exit_code;
}

// Deliberately NOT popen()/fork()+exec() -- popen() forks, and fork()ing a
// multithreaded process is unsafe: if another thread holds a libc lock
// (malloc's arena lock, etc.) at the instant of fork(), the child inherits
// that lock permanently held with no thread left alive to release it, and can
// hang forever the next time it needs that lock -- intermittently, depending
// on timing. posix_spawn() is specified to be safe to call from a
// multithreaded process, which is why this uses it instead.
//
// The program is started directly from the argument list, with no shell in
// between: arguments reach it byte for byte, so no quoting is needed or
// interpreted.
std::unique_ptr<ChildProcess> spawn_capture(const Args& args, bool merge_stderr) {
    if (args.empty() || args[0].empty()) return nullptr;
    int out_pipe[2];
    if (pipe(out_pipe) != 0) return nullptr;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // Redirecting every child's stdin to /dev/null means no subprocess we
    // spawn can ever see or touch our terminal, regardless of what that
    // program's stdin behavior is.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    if (merge_stderr) {
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    // posix_spawnp resolves a bare program name through PATH (Termux keeps its
    // programs under its own prefix, not the standard FHS layout).
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);

    if (rc != 0) {
        close(out_pipe[0]);
        ConsoleLog::instance().log_command(command_text(args), std::string("spawn failed: ") + std::strerror(rc), -1);
        return nullptr;
    }

    std::unique_ptr<ChildProcess> child(new ChildProcess());
    child->impl_->out_fd = out_pipe[0];
    child->impl_->pid    = pid;
    return child;
}

// =====================================================================
// Windows implementation
// =====================================================================
#else

namespace {

// The inverse: CommandLineToArgvW's quoting rules, which are what
// CreateProcessW hands the child. Backslashes are only special immediately
// before a quote, where they must be doubled.
std::string quote_arg(const std::string& arg, bool force) {
    bool needs = force || arg.empty() ||
                 arg.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needs) return arg;

    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');   // trailing run precedes the closing quote
    out += '"';
    return out;
}

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

bool ends_with_ci(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    return _stricmp(s.c_str() + (s.size() - n), suffix) == 0;
}

HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa, OPEN_EXISTING, 0, nullptr);
}

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST \
    ProcThreadAttributeValue(9, FALSE, TRUE, FALSE)
#endif

// RAII wrapper around a PROC_THREAD_ATTRIBUTE_LIST restricted to an explicit
// set of handles.
//
// Plain `bInheritHandles=TRUE` (what the first version of this file used)
// inherits *every* inheritable handle open anywhere in the process at the
// instant CreateProcess snapshots the handle table -- not just the ones this
// call put in STARTUPINFO. mousiki spawns subprocesses from several threads
// concurrently by design (the background per-track metadata sweep in
// launch_row_meta_resolver(), a track's own ffprobe/ffmpeg calls, yt-dlp
// resolution, the lyrics helper), so two CreateProcess calls landing close
// together is routine, not an edge case. When that happens, a handle one
// thread just created (or is about to close) for its own spawn can get
// duplicated into a completely different, concurrently-starting child --
// there's no synchronization protecting the process-wide handle table
// between "open the pipe" and "call CreateProcess". Whichever spawn ends up
// with an extra, unexpected write handle to its stdout pipe never sees EOF
// on that pipe, because something it doesn't know about is still holding it
// open; ReadFile() in ChildProcess::read() then blocks forever. If that
// happens to the metadata probe inside App::launch_load_async() -- which
// playback genuinely waits on before calling Player::play() -- the result
// looks exactly like "the track just never starts", intermittently, with no
// error anywhere.
//
// An explicit handle list closes this off completely: CreateProcess is told
// precisely which handles this one call may inherit, so what any other
// thread is doing to the process-wide handle table at the same moment
// becomes irrelevant.
class HandleInheritList {
public:
    explicit HandleInheritList(std::vector<HANDLE> handles) : handles_(std::move(handles)) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        buf_.resize(size);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) {
            list_ = nullptr;
            return;
        }
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles_.data(), handles_.size() * sizeof(HANDLE),
                                       nullptr, nullptr)) {
            DeleteProcThreadAttributeList(list_);
            list_ = nullptr;
        }
    }
    ~HandleInheritList() { if (list_) DeleteProcThreadAttributeList(list_); }
    HandleInheritList(const HandleInheritList&) = delete;
    HandleInheritList& operator=(const HandleInheritList&) = delete;

    bool valid() const { return list_ != nullptr; }
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

private:
    std::vector<HANDLE> handles_;   // must outlive list_ -- UpdateProcThreadAttribute stores the pointer, not a copy
    std::vector<char> buf_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

} // namespace

struct ChildProcess::Impl {
    HANDLE out_read = INVALID_HANDLE_VALUE;
    HANDLE process  = INVALID_HANDLE_VALUE;
    int    exit_code = -1;
    bool   reaped = false;
};

ChildProcess::ChildProcess() : impl_(new Impl) {}

ChildProcess::~ChildProcess() {
    if (impl_->out_read != INVALID_HANDLE_VALUE) CloseHandle(impl_->out_read);
    impl_->out_read = INVALID_HANDLE_VALUE;
    wait();
    if (impl_->process != INVALID_HANDLE_VALUE) CloseHandle(impl_->process);
}

void ChildProcess::kill() {
    if (!impl_->reaped && impl_->process != INVALID_HANDLE_VALUE) TerminateProcess(impl_->process, 1);
}

void ChildProcess::lower_priority() {
    if (!impl_->reaped && impl_->process != INVALID_HANDLE_VALUE) SetPriorityClass(impl_->process, BELOW_NORMAL_PRIORITY_CLASS);
}

std::ptrdiff_t ChildProcess::read(char* buf, std::size_t n) {
    if (impl_->out_read == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    if (!ReadFile(impl_->out_read, buf, static_cast<DWORD>(n), &got, nullptr)) {
        // The child closing its end of the pipe surfaces as ERROR_BROKEN_PIPE
        // rather than a zero-byte read; that is a clean EOF, not a failure.
        return (GetLastError() == ERROR_BROKEN_PIPE) ? 0 : -1;
    }
    return static_cast<std::ptrdiff_t>(got);
}

int ChildProcess::wait() {
    if (impl_->reaped) return impl_->exit_code;
    impl_->reaped = true;
    if (impl_->process == INVALID_HANDLE_VALUE) return impl_->exit_code;
    WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(impl_->process, &code)) impl_->exit_code = static_cast<int>(code);
    return impl_->exit_code;
}

std::unique_ptr<ChildProcess> spawn_capture(const Args& args, bool merge_stderr) {
    if (args.empty() || args[0].empty()) return nullptr;

    // CreateProcess only ever appends ".exe" when resolving a bare name, so a
    // yt-dlp installed by pipx or scoop as a .cmd shim would otherwise look
    // like "yt-dlp is not installed".
    std::string exe = win_find_executable(args[0]);
    if (exe.empty()) {
        ConsoleLog::instance().log_command(command_text(args), "spawn failed: '" + args[0] + "' not found on PATH", -1);
        return nullptr;
    }

    // A .cmd/.bat shim is a script, not an image -- it has to be handed to
    // the command interpreter. Everything gets force-quoted in that case so
    // cmd.exe cannot reinterpret '&' in a playlist URL as a separator.
    bool via_cmd = ends_with_ci(exe, ".cmd") || ends_with_ci(exe, ".bat");

    std::string cmdline;
    if (via_cmd) {
        // the system cmd.exe (never one in the current folder); /d: no AutoRun commands, /v:off: no !NAME! expansion. The whole
        // command after /c is in one more pair of quotes, which cmd.exe removes (it drops the first and the last quote of the line).
        std::string inner;
        if (!quote_arg_for_cmd(exe, inner)) return nullptr;
        for (size_t i = 1; i < args.size(); ++i) {
            std::string q;
            if (!quote_arg_for_cmd(args[i], q)) {
                ConsoleLog::instance().log_command(command_text(args), "spawn refused: a line break cannot be passed to a .cmd / .bat program", -1);
                return nullptr;
            }
            inner += ' ';
            inner += q;
        }
        cmdline = quote_arg(win_system_program("cmd.exe"), false) + " /d /e:on /v:off /c \"" + inner + "\"";
    } else {
        cmdline = quote_arg(exe, false);
        for (size_t i = 1; i < args.size(); ++i) {
            cmdline += ' ';
            cmdline += quote_arg(args[i], false);
        }
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_read = INVALID_HANDLE_VALUE, out_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) return nullptr;
    // Our end of the pipe must not leak into the child, or the read loop
    // below never sees EOF: the child would still be holding a write handle
    // open and ReadFile would block forever after the real writer exits.
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE nul_err = merge_stderr ? INVALID_HANDLE_VALUE : open_nul(GENERIC_WRITE);

    STARTUPINFOEXW six{};
    six.StartupInfo.cb = sizeof(six);
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdInput  = nul_in;
    six.StartupInfo.hStdOutput = out_write;
    six.StartupInfo.hStdError  = merge_stderr ? out_write : nul_err;

    // The list passed here must be exactly the set of handles this process
    // needs -- CreateProcess rejects the call if a STARTUPINFO std handle is
    // set but missing from the list. de-duplicated because merge_stderr
    // reuses out_write for both stdout and stderr, and the same HANDLE value
    // must not appear twice.
    std::vector<HANDLE> inherit = {nul_in, out_write};
    if (!merge_stderr) inherit.push_back(nul_err);
    HandleInheritList handle_list(inherit);

    PROCESS_INFORMATION pi{};
    std::wstring wcmdline = widen_utf8(cmdline);
    wcmdline.push_back(L'\0');   // CreateProcessW may modify the buffer in place

    // CREATE_NO_WINDOW is the Windows counterpart of the /dev/null stdin
    // redirect: the child gets no console of its own and no access to ours,
    // so ffmpeg cannot read our keystrokes or leave our console mode altered,
    // and yt-dlp does not flash a black window on every download.
    //
    // EXTENDED_STARTUPINFO_PRESENT + the attribute list is what actually
    // enforces the explicit handle set below -- without this flag Windows
    // ignores lpAttributeList entirely and falls back to inheriting
    // everything, silently reintroducing the race this exists to close.
    // If the attribute list itself failed to build (old SDK / OS too old --
    // shouldn't happen on any supported Windows release, but this is not a
    // path worth hard-failing on), fall back to the old whole-process
    // inheritance rather than not spawning at all.
    DWORD flags = CREATE_NO_WINDOW;
    LPSTARTUPINFOW si_ptr = &six.StartupInfo;
    if (handle_list.valid()) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
        six.lpAttributeList = handle_list.get();
    } else {
        // Not using the extended form after all -- cb must describe the
        // plain STARTUPINFOW Windows will actually interpret it as.
        six.StartupInfo.cb = sizeof(STARTUPINFOW);
    }

    BOOL ok = CreateProcessW(nullptr, &wcmdline[0], nullptr, nullptr,
                             /*bInheritHandles=*/TRUE, flags,
                             nullptr, nullptr, si_ptr, &pi);

    CloseHandle(out_write);
    if (nul_in  != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
    if (nul_err != INVALID_HANDLE_VALUE) CloseHandle(nul_err);

    if (!ok) {
        CloseHandle(out_read);
        ConsoleLog::instance().log_command(
            command_text(args), "spawn failed: CreateProcess error " + std::to_string(GetLastError()), -1);
        return nullptr;
    }
    CloseHandle(pi.hThread);

    std::unique_ptr<ChildProcess> child(new ChildProcess());
    child->impl_->out_read = out_read;
    child->impl_->process  = pi.hProcess;
    return child;
}

#endif // _WIN32

#if !defined(_WIN32)
int run_with_stdin_file(const Args& args, const std::string& stdin_path) {
    if (args.empty() || args[0].empty()) return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, stdin_path.c_str(), O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

bool program_on_path(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0;
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string dirs = path;
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find(':', start);
        if (end == std::string::npos) end = dirs.size();
        const std::string dir = start == end ? std::string(".") : dirs.substr(start, end - start);
        if (access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        start = end + 1;
    }
    return false;
}
#endif

// =====================================================================
// Shared
// =====================================================================

ProcResult run_capture(const Args& args, bool merge_stderr) {
    ProcResult result;

    std::unique_ptr<ChildProcess> child = spawn_capture(args, merge_stderr);
    if (!child) {
        result.exit_code = -1;
        return result;   // spawn_capture already logged the reason
    }

    std::array<char, 4096> buf{};
    std::ostringstream oss;
    std::ptrdiff_t n;
    while ((n = child->read(buf.data(), buf.size())) > 0) {
        oss.write(buf.data(), static_cast<std::streamsize>(n));
    }
    result.out = oss.str();
    result.exit_code = child->wait();

    ConsoleLog::instance().log_command(command_text(args), result.out, result.exit_code);
    return result;
}

} // namespace muisc
