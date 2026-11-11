// The terminal side of the scope window: starts `mousiki --scope-window`, feeds it samples and settings through a pipe.
// See scope_window.h for the overall picture; the window itself is scope_window_app.cpp.
#include "app_dirs.h"
#include "scope_window.h"
#include "scope_sdl.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include "win_compat.h"
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace muisc {

namespace {

// Two windows run the same way: the oscilloscope (`--scope-window`) and the spectrogram (`--spectro-window`). Each has
// its own feed, link and process.
constexpr size_t kFeedFrames = 1u << 16;   // ~1.4 s at 48 kHz; far more than the sender's 4 ms rhythm needs

struct Link {
    // ---- lock-free single-producer / single-consumer ring: audio thread -> sender thread ---------------------------
    std::vector<float> feed = std::vector<float>(2 * kFeedFrames);
    std::atomic<size_t> feed_w{0}, feed_r{0};
    std::atomic<bool> feed_on{false};
    std::atomic<int> feed_rate{48000};
    // ---- the link to the window process ---------------------------------------------------------------------------
    std::mutex mtx;                    // guards cfg / cfg_gen and the process handles below (not the pipe writes)
    std::vector<uint8_t> cfg;          // the settings packet (ScopeWinConfig / SpectroWinConfig), raw bytes
    size_t rate_at = 0;                // offset of the int32 sample rate inside it
    unsigned cfg_gen = 0;
    std::thread sender;
    std::atomic<bool> stop{false};
    std::atomic<bool> alive{false};    // process running and pipe open
    std::string error;                 // reported once after an abnormal end
    const char* arg = "";              // the command line switch of the window process
    const char* what = "";             // for messages
    const char* err_name = "";         // its error file
#if defined(_WIN32)
    HANDLE proc = nullptr;
    HANDLE pipe_w = nullptr;
#else
    pid_t pid = -1;
    int pipe_w = -1;
#endif
};

Link& link_of(int k) {
    static Link links[2];
    static const bool init = [] {
        links[0].arg = "--scope-window"; links[0].what = "scope window"; links[0].err_name = "scope_window_error.txt";
        links[0].cfg.resize(sizeof(ScopeWinConfig));
        { ScopeWinConfig c; std::memcpy(links[0].cfg.data(), &c, sizeof c); }
        links[0].rate_at = offsetof(ScopeWinConfig, rate);
        links[1].arg = "--spectro-window"; links[1].what = "spectrogram window"; links[1].err_name = "spectro_window_error.txt";
        links[1].cfg.resize(sizeof(SpectroWinConfig));
        { SpectroWinConfig c; std::memcpy(links[1].cfg.data(), &c, sizeof c); }
        links[1].rate_at = offsetof(SpectroWinConfig, rate);
        return true;
    }();
    (void)init;
    return links[k];
}

std::string error_file(const Link& L) {
    return path_utf8(muisc::app_dirs::cache() / L.err_name);
}

std::string self_exe() {
#if defined(_WIN32)
    return win_executable_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return buf;
    return "";
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    return buf;
#endif
}

// Writes all of `n` bytes; false when the window is gone (pipe closed).
bool write_all(Link& L, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n > 0) {
#if defined(_WIN32)
        DWORD done = 0;
        if (!WriteFile(L.pipe_w, p, static_cast<DWORD>(std::min<size_t>(n, 1u << 20)), &done, nullptr) || done == 0) return false;
#else
        const ssize_t done = ::write(L.pipe_w, p, n);
        if (done < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE) {   // the SIGPIPE this raised is blocked in this thread: take it off the pending set
                sigset_t pend;
                sigemptyset(&pend);
                if (sigpending(&pend) == 0 && sigismember(&pend, SIGPIPE)) {
                    sigset_t only;
                    sigemptyset(&only);
                    sigaddset(&only, SIGPIPE);
                    int sig = 0;
                    sigwait(&only, &sig);
                }
            }
            return false;
        }
        if (done == 0) return false;
#endif
        p += done;
        n -= static_cast<size_t>(done);
    }
    return true;
}

bool send_packet(Link& L, uint8_t type, const void* payload, uint32_t len) {
    uint8_t head[5] = {type, static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 24)};
    return write_all(L, head, sizeof head) && (len == 0 || write_all(L, payload, len));
}

bool process_exited(Link& L, int* code) {
#if defined(_WIN32)
    if (!L.proc) return true;
    if (WaitForSingleObject(L.proc, 0) != WAIT_OBJECT_0) return false;
    DWORD c = 0;
    GetExitCodeProcess(L.proc, &c);
    if (code) *code = static_cast<int>(c);
    return true;
#else
    if (L.pid <= 0) return true;
    int st = 0;
    const pid_t r = waitpid(L.pid, &st, WNOHANG);
    if (r == 0) return false;
    if (r == L.pid) {
        L.pid = -1;
        if (code) *code = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    }
    return true;
#endif
}

void sender_main(Link* Lp) {
    Link& L = *Lp;
#if !defined(_WIN32)
    {   // a write to a closed pipe must not kill the program: block SIGPIPE in this thread only (write() returns EPIPE)
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &s, nullptr);
    }
#endif
    unsigned sent_gen = ~0u;
    int sent_rate = 0;
    std::vector<float> chunk;
    auto last_check = std::chrono::steady_clock::now();
    while (!L.stop.load()) {
        // settings (and the sample rate) first, so samples are never drawn with stale values
        std::vector<uint8_t> cfg;
        unsigned gen;
        {
            std::lock_guard<std::mutex> lk(L.mtx);
            cfg = L.cfg;
            gen = L.cfg_gen;
        }
        const int rate = L.feed_rate.load();
        if (gen != sent_gen || rate != sent_rate) {
            const int32_t r32 = rate;
            std::memcpy(cfg.data() + L.rate_at, &r32, sizeof r32);
            if (!send_packet(L, 1, cfg.data(), static_cast<uint32_t>(cfg.size()))) break;
            sent_gen = gen;
            sent_rate = rate;
        }
        // samples
        const size_t w = L.feed_w.load(std::memory_order_acquire);
        size_t r = L.feed_r.load(std::memory_order_relaxed);
        if (w != r) {
            const size_t n = w - r;
            chunk.resize(2 * n);
            for (size_t i = 0; i < n; ++i) {
                const size_t at = (r + i) & (kFeedFrames - 1);
                chunk[2 * i] = L.feed[2 * at];
                chunk[2 * i + 1] = L.feed[2 * at + 1];
            }
            L.feed_r.store(w, std::memory_order_release);
            if (!send_packet(L, 2, chunk.data(), static_cast<uint32_t>(chunk.size() * sizeof(float)))) break;
        }
        // a window closed by the user: nothing may be written for a while (paused), so look at the process itself
        const auto now = std::chrono::steady_clock::now();
        if (now - last_check > std::chrono::milliseconds(250)) {
            last_check = now;
            std::lock_guard<std::mutex> lk(L.mtx);
            if (process_exited(L, nullptr)) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    L.feed_on.store(false);
    L.alive.store(false);
}

void close_handles(Link& L) {
#if defined(_WIN32)
    if (L.pipe_w) { CloseHandle(L.pipe_w); L.pipe_w = nullptr; }
#else
    if (L.pipe_w >= 0) { ::close(L.pipe_w); L.pipe_w = -1; }
#endif
}

// Waits a moment for the process to end (it does as soon as its input closes), then makes sure it is gone.
void reap(Link& L, bool force) {
    int code = 0;
    bool ended = false;
    for (int i = 0; i < (force ? 40 : 1); ++i) {
        if (process_exited(L, &code)) { ended = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
#if defined(_WIN32)
    if (!ended && L.proc) { TerminateProcess(L.proc, 1); WaitForSingleObject(L.proc, 2000); }
    if (L.proc) { CloseHandle(L.proc); L.proc = nullptr; }
#else
    if (!ended && L.pid > 0) { kill(L.pid, SIGTERM); int st = 0; waitpid(L.pid, &st, 0); L.pid = -1; }
#endif
    if (ended && code != 0) {
        std::ifstream in(error_file(L));
        std::string line, last;
        while (std::getline(in, line)) if (!line.empty()) last = line;
        L.error = last.empty() ? std::string("the ") + L.what + " ended with an error (code " + std::to_string(code) + ")" : last;
    }
}

bool win_running(Link& L) {
    if (L.alive.load()) return true;
    // the sender has stopped (window closed by the user or crashed): tidy up once
    if (L.sender.joinable()) {
        L.stop.store(true);
        L.sender.join();
        std::lock_guard<std::mutex> lk(L.mtx);
        close_handles(L);
        reap(L, true);
    }
    return false;
}

bool win_open(Link& L, std::string* err) {
    if (win_running(L)) return true;
    std::string why;
    if (!scope_window_available(&why)) { if (err) *err = why; return false; }
    const std::string exe = self_exe();
    if (exe.empty()) { if (err) *err = std::string("cannot find the mousiki executable to start the ") + L.what; return false; }
    std::remove(error_file(L).c_str());
    L.error.clear();
    L.stop.store(false);
#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 20)) { if (err) *err = std::string("cannot create a pipe for the ") + L.what; return false; }
    SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);   // only the read end goes to the child
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = rd;
    si.hStdOutput = nul;
    si.hStdError = nul;
    PROCESS_INFORMATION pi{};
    const std::wstring wexe = win_utf8_to_wide(exe);
    std::wstring cmd = L"\"" + wexe + L"\" " + win_utf8_to_wide(L.arg);
    const BOOL ok = CreateProcessW(wexe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(rd);
    if (nul && nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(wr); if (err) *err = std::string("cannot start the ") + L.what + " process"; return false; }
    CloseHandle(pi.hThread);
    L.proc = pi.hProcess;
    L.pipe_w = wr;
#else
    int fds[2];
    if (pipe(fds) != 0) { if (err) *err = std::string("cannot create a pipe for the ") + L.what; return false; }
    // the write end must not leak into other children (ffmpeg, yt-dlp ...): the window would never see its input end
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    const pid_t pid = fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); if (err) *err = std::string("cannot start the ") + L.what + " process"; return false; }
    if (pid == 0) {
        // child: only async-signal-safe calls until exec
        dup2(fds[0], 0);
        const int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        setsid();   // its own session: CTRL+C in the terminal goes to the player only, which then closes the window
        execl(exe.c_str(), exe.c_str(), L.arg, static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(fds[0]);
    L.pid = pid;
    L.pipe_w = fds[1];
#endif
    {   // a fresh start of the feed: nothing old in the ring
        L.feed_r.store(L.feed_w.load());
        std::lock_guard<std::mutex> lk(L.mtx);
        ++L.cfg_gen;
    }
    L.alive.store(true);
    L.feed_on.store(true);
    L.sender = std::thread(sender_main, &L);
    return true;
}

void win_close(Link& L) {
    L.feed_on.store(false);
    if (L.sender.joinable()) {
        L.stop.store(true);
        L.sender.join();
    }
    std::lock_guard<std::mutex> lk(L.mtx);
    close_handles(L);   // the window sees the end of its input and closes itself (saving its position)
    reap(L, true);
    L.error.clear();   // closed on purpose: nothing to report
    L.alive.store(false);
}

std::string win_take_error(Link& L) {
    win_running(L);
    std::string e;
    e.swap(L.error);
    return e;
}

void win_set_config(Link& L, const void* c, size_t n) {
    std::lock_guard<std::mutex> lk(L.mtx);
    if (L.cfg.size() == n && std::memcmp(L.cfg.data(), c, n) == 0) return;
    L.cfg.assign(static_cast<const uint8_t*>(c), static_cast<const uint8_t*>(c) + n);
    ++L.cfg_gen;
}

void win_feed_push(Link& L, const float* lr, size_t frames, int rate) {
    if (!L.feed_on.load(std::memory_order_relaxed) || !lr || frames == 0) return;
    if (rate > 0) L.feed_rate.store(rate, std::memory_order_relaxed);
    const size_t w = L.feed_w.load(std::memory_order_relaxed);
    const size_t r = L.feed_r.load(std::memory_order_acquire);
    const size_t room = kFeedFrames - (w - r);
    const size_t n = std::min(frames, room);   // the window fell behind: drop what does not fit (never block the audio)
    for (size_t i = 0; i < n; ++i) {
        const size_t at = (w + i) & (kFeedFrames - 1);
        L.feed[2 * at] = lr[2 * i];
        L.feed[2 * at + 1] = lr[2 * i + 1];
    }
    L.feed_w.store(w + n, std::memory_order_release);
}

} // namespace

bool scope_window_available(std::string* why) {
    void* lib = sdl::lib_open();
    if (!lib) { if (why) *why = sdl::missing_hint(); return false; }
    const bool geo = sdl::lib_sym(lib, "SDL_RenderGeometry") != nullptr;
    sdl::lib_close(lib);
    if (!geo && why) *why = std::string("SDL2 is too old (SDL_RenderGeometry is missing); ") + sdl::missing_hint();
    return geo;
}

// ---- the oscilloscope window ----------------------------------------------------------------------------------------
bool scope_window_running() { return win_running(link_of(0)); }
bool scope_window_open(std::string* err) { return win_open(link_of(0), err); }
void scope_window_close() { win_close(link_of(0)); }
std::string scope_window_take_error() { return win_take_error(link_of(0)); }
void scope_window_set_config(const ScopeWinConfig& c) { win_set_config(link_of(0), &c, sizeof c); }
void scope_feed_push(const float* lr, size_t frames, int rate) { win_feed_push(link_of(0), lr, frames, rate); }

// ---- the spectrogram window -----------------------------------------------------------------------------------------
bool spectro_window_running() { return win_running(link_of(1)); }
bool spectro_window_open(std::string* err) { return win_open(link_of(1), err); }
void spectro_window_close() { win_close(link_of(1)); }
std::string spectro_window_take_error() { return win_take_error(link_of(1)); }
void spectro_window_set_config(const SpectroWinConfig& c) { win_set_config(link_of(1), &c, sizeof c); }
void spectro_window_feed_push(const float* lr, size_t frames, int rate) { win_feed_push(link_of(1), lr, frames, rate); }
bool spectro_window_feed_active() { return link_of(1).feed_on.load(std::memory_order_relaxed); }

} // namespace muisc
