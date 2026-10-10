#include "console_log.h"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace muisc {

namespace fs = std::filesystem;

static fs::path log_dir() {
    const char* home = std::getenv("HOME");
#if defined(_WIN32)
    if (!home) home = std::getenv("USERPROFILE");
#endif
    fs::path base = home ? fs::path(home) : fs::path(".");
    return base / ".cache" / "mousiki" / "logs";
}

static std::string now_hms() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
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
    lines_.clear();

    std::error_code ec;
    fs::path dir = log_dir();
    fs::create_directories(dir, ec);
    log_path_ = (dir / "console.log").string();

    // Truncate (std::ios::trunc, not append) -- this is exactly the
    // "previous session's log gets cleaned, a new one starts" behavior:
    // whatever the last session wrote here is gone the instant a new
    // session launches.
    std::ofstream out(log_path_, std::ios::trunc);
    if (out.is_open()) {
        out << "===== mousiki session started " << now_hms()
            << " (level=" << (level_ == LogVerbosity::Verbose ? "verbose" : "basic") << ") =====\n";
    }
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
    return lines_;
}

} // namespace muisc
