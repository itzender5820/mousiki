// The console log behind the t console: keeps the last lines in memory, writes them to a log file, and
// records commands with their output at the chosen verbosity.
#include "app_dirs.h"
#include "console_log.h"
#include "path_utf8.h"
#include <chrono>
#include <cstdlib>
#if defined(_WIN32)
#include "win_compat.h"
#endif
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace muisc {

static fs::path log_dir() {
    const char* home = std::getenv("HOME");
    // HOME is stored as UTF-8 by win_bootstrap_env() (it comes out of
    // GetEnvironmentVariableW, which is UTF-16). fs::path(std::string)
    // would re-read those bytes as ANSI, so a user profile with a
    // non-ASCII name produced a garbled base directory.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return muisc::app_dirs::cache() / "logs";
}

static std::string now_hms() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#if defined(_WIN32)
    tmv = win_localtime(t);   // MSVC has localtime_s, with the arguments swapped
#else
    localtime_r(&t, &tmv);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

ConsoleLog& ConsoleLog::instance() {
    static ConsoleLog inst;
    return inst;
}

void ConsoleLog::init(LogVerbosity level) {
    std::lock_guard<std::mutex> lk(mutex_);
    level_ = level;
    initialized_ = true;
    lines_.clear();
    prev_lines_.clear();

    std::error_code ec;
    fs::path dir = log_dir();
    fs::create_directories(dir, ec);
    log_path_ = dir / "console.log";
    const fs::path prev_path = dir / "console_previous.log";

    // The last session's log is kept (console_previous.log) and its end is shown above this session's lines in the console.
    if (fs::exists(log_path_, ec)) {
        {
            std::ifstream in(log_path_);
            std::string ln;
            while (std::getline(in, ln)) {
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                prev_lines_.push_back(ln);
                if (prev_lines_.size() > kMaxLines) prev_lines_.erase(prev_lines_.begin(), prev_lines_.begin() + (prev_lines_.size() - kMaxLines));
            }
        }
        fs::remove(prev_path, ec);
        fs::rename(log_path_, prev_path, ec);
    }

    // A fresh file for this session.
    std::ofstream out(log_path_, std::ios::trunc);
    const std::string head = "===== mousiki session started " + now_hms() + " (level=" +
                             std::string(level_ == LogVerbosity::Verbose ? "verbose" : "basic") + ") =====";
    if (out.is_open()) out << head << "\n";
    lines_.push_back(head);
}

bool ConsoleLog::initialized() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return initialized_;
}

LogVerbosity ConsoleLog::toggle_level() {
    LogVerbosity now;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        level_ = level_ == LogVerbosity::Verbose ? LogVerbosity::Basic : LogVerbosity::Verbose;
        now = level_;
    }
    log_basic(now == LogVerbosity::Verbose ? "verbose log: on for this session (internal and OS-level events are logged too)"
                                           : "verbose log: off");
    return now;
}

void ConsoleLog::set_level(LogVerbosity level) {
    std::lock_guard<std::mutex> lk(mutex_);
    level_ = level;
}

LogVerbosity ConsoleLog::level() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return level_;
}

void ConsoleLog::push_line_locked(const std::string& line) {
    lines_.push_back(line);
    if (lines_.size() > kMaxLines) {
        lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kMaxLines));
    }
}

void ConsoleLog::append_to_file(const std::string& text) {
    if (log_path_.empty()) return;
    std::ofstream out(log_path_, std::ios::app);
    if (out.is_open()) out << text;
}

void ConsoleLog::log_command(const std::string& cmd, const std::string& output, int exit_code) {
    std::ostringstream block;
    block << "[" << now_hms() << "] ==> " << cmd << "\n";

    std::lock_guard<std::mutex> lk(mutex_);
    push_line_locked("[" + now_hms() + "] ==> " + cmd);

    // Raw output, verbatim, one buffer line per vector entry so the
    // Console overlay can scroll it like any other log line.
    std::istringstream iss(output);
    std::string ln;
    bool any = false;
    while (std::getline(iss, ln)) {
        // strip a trailing \r some tools (yt-dlp progress) leave behind
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        push_line_locked(ln);
        block << ln << "\n";
        any = true;
    }
    if (!any) block << "(no output)\n";
    block << "(exit " << exit_code << ")\n\n";
    push_line_locked("(exit " + std::to_string(exit_code) + ")");
    push_line_locked("");

    append_to_file(block.str());
}

void ConsoleLog::log_verbose(const std::string& line) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (level_ != LogVerbosity::Verbose) return;
    std::string full = "[" + now_hms() + "] " + line;
    push_line_locked(full);
    append_to_file(full + "\n");
}

void ConsoleLog::log_basic(const std::string& line) {
    std::lock_guard<std::mutex> lk(mutex_);
    std::string full = "[" + now_hms() + "] " + line;
    push_line_locked(full);
    append_to_file(full + "\n");
}

std::vector<std::string> ConsoleLog::lines() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (prev_lines_.empty()) return lines_;
    std::vector<std::string> all;
    all.reserve(prev_lines_.size() + lines_.size() + 2);
    all.push_back("===== the last session (console_previous.log) =====");
    all.insert(all.end(), prev_lines_.begin(), prev_lines_.end());
    all.push_back("");
    all.insert(all.end(), lines_.begin(), lines_.end());
    return all;
}

} // namespace muisc
