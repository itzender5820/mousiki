#pragma once
// A tiny test framework, no dependencies: TEST(name) { CHECK(...); } registers a test, tests/test_main.cpp runs them all.
//   mousiki_tests              run everything
//   mousiki_tests <substring>  run the tests whose name contains it
// Built with  cmake -DMOUSIKI_BUILD_TESTS=ON  and run with  ctest  (or the executable directly).
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mt {

struct Case { const char* name; void (*fn)(); };
std::vector<Case>& registry();
struct Registrar { Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); } };
void fail(const char* file, int line, const std::string& what);

// A throw-away HOME: settings, themes, remote-control files ... go to a fresh folder that is removed at the end. It is
// also the working directory meanwhile: a config.txt / radio_config.txt next to the test program (the build folder has
// copies) would otherwise be read and written instead of the ones in HOME.
class TempHome {
public:
    TempHome();
    ~TempHome();
    const std::filesystem::path& path() const { return dir_; }
    std::filesystem::path config_dir() const { return dir_ / ".config" / "mousiki"; }
private:
    std::filesystem::path dir_, old_cwd_;
    std::string old_home_, old_xdg_cfg_, old_xdg_cache_;   // (XDG_*: a CI runner sets them; they would lead out of the throw-away HOME)
    bool had_home_ = false, had_xdg_cfg_ = false, had_xdg_cache_ = false;
};

template <class A, class B> std::string show(const A& a, const B& b) {
    std::ostringstream o;
    o << "[" << a << "] != [" << b << "]";
    return o.str();
}

} // namespace mt

#define MT_CAT2(a, b) a##b
#define MT_CAT(a, b) MT_CAT2(a, b)
#define TEST(name) \
    static void name(); \
    static ::mt::Registrar MT_CAT(mt_reg_, name)(#name, name); \
    static void name()

#define CHECK(cond) do { if (!(cond)) ::mt::fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_EQ(a, b) do { const auto& mt_a = (a); const auto& mt_b = (b); \
    if (!(mt_a == mt_b)) ::mt::fail(__FILE__, __LINE__, std::string(#a " == " #b "  ") + ::mt::show(mt_a, mt_b)); } while (0)
#define CHECK_NEAR(a, b, eps) do { const double mt_a = (a), mt_b = (b); \
    if (!(mt_a >= mt_b - (eps) && mt_a <= mt_b + (eps))) ::mt::fail(__FILE__, __LINE__, std::string(#a " ~ " #b "  ") + ::mt::show(mt_a, mt_b)); } while (0)
