#include "test_framework.h"
#include "bg_tasks.h"

#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(_WIN32)
#include <stdlib.h>
#endif

namespace mt {

int g_failures = 0;

std::vector<Case>& registry() { static std::vector<Case> r; return r; }

void fail(const char* file, int line, const std::string& what) {
    ++g_failures;
    std::cerr << "    CHECK WRONG at " << file << ":" << line << "  " << what << std::endl;   // (flushed: a CI log shows it next to the test it belongs to)
}

TempHome::TempHome() {
    muisc::bg_idle_wait(20000);   // (a helper task of the test before still runs ffmpeg: the environment must not change under it)
    const char* h = std::getenv("HOME");
    had_home_ = h != nullptr;
    if (h) old_home_ = h;
    std::mt19937_64 rng(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
    dir_ = std::filesystem::temp_directory_path() / ("mousiki-test-" + std::to_string(rng()));
    std::filesystem::create_directories(dir_);
    std::error_code ec;
    old_cwd_ = std::filesystem::current_path(ec);
    std::filesystem::current_path(dir_, ec);
#if defined(_WIN32)
    _putenv_s("HOME", dir_.string().c_str());
#else
    setenv("HOME", dir_.string().c_str(), 1);
#endif
}

TempHome::~TempHome() {
    muisc::bg_idle_wait(20000);
#if defined(_WIN32)
    _putenv_s("HOME", had_home_ ? old_home_.c_str() : "");
#else
    if (had_home_) setenv("HOME", old_home_.c_str(), 1); else unsetenv("HOME");
#endif
    std::error_code ec;
    if (!old_cwd_.empty()) std::filesystem::current_path(old_cwd_, ec);
    std::filesystem::remove_all(dir_, ec);
}

} // namespace mt

int main(int argc, char** argv) {
    // like the program: UTF-8 text, so display widths of box and braille characters are counted as on screen
    if (!std::setlocale(LC_ALL, "C.UTF-8")) std::setlocale(LC_ALL, "");
    std::setlocale(LC_NUMERIC, "C");
    // the tests never register a media session on the developer's desktop or the CI machine's session bus
#ifdef _WIN32
    _putenv_s("MOUSIKI_NO_MEDIA", "1");
#else
    setenv("MOUSIKI_NO_MEDIA", "1", 1);
#endif
    const std::string filter = argc > 1 ? argv[1] : "";
    int ran = 0, bad = 0;
    std::vector<std::string> bad_names;   // listed again at the very end, where a CI log is read first
    for (const mt::Case& c : mt::registry()) {
        if (!filter.empty() && std::string(c.name).find(filter) == std::string::npos) continue;
        const int before = mt::g_failures;
        std::cerr << "[ RUN  ] " << c.name << "\n";
        try { c.fn(); }
        catch (const std::exception& e) { mt::fail(__FILE__, __LINE__, std::string("exception: ") + e.what()); }
        catch (...) { mt::fail(__FILE__, __LINE__, "unknown exception"); }
        ++ran;
        if (mt::g_failures != before) { ++bad; bad_names.push_back(c.name); std::cerr << "[ NOT OK ] " << c.name << std::endl; }
        else std::cerr << "[  OK  ] " << c.name << std::endl;
    }
    std::cerr << "\n" << ran << " tests, " << bad << " failed" << std::endl;
    for (const auto& n : bad_names) std::cerr << "  NOT OK: " << n << std::endl;
    if (ran == 0) { std::cerr << "no test matches '" << filter << "'\n"; return 2; }
    return bad == 0 ? 0 : 1;
}
