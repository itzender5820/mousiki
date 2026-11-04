// Keyboard layout detection: finds the layout of this machine (Linux XKB and locale, macOS input
// source, Windows keyboard layout) and names the mode-switch keys accordingly.
#include "keyboard_layout.h"
#include "utf8_util.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#include <windows.h>
#else
#include "process_util.h"
#endif

namespace muisc {
namespace {


// "de,us" / "de(nodeadkeys)" / "de_DE.UTF-8" -> "de"
std::string first_code(const std::string& v) {
    std::string s = ascii_lower_str(v);
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    std::string out;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i])) && out.size() < 3) out += s[i++];
    return out;
}

#if !defined(_WIN32)
#if !defined(__APPLE__)   // (only the Linux branch below reads "key: value" lines: on macOS it would be unused)
std::string value_after(const std::string& text, const std::string& key) {   // "key: value" line
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const size_t p = line.find(key);
        if (p == std::string::npos) continue;
        std::string v = line.substr(p + key.size());
        while (!v.empty() && (v[0] == ' ' || v[0] == ':' || v[0] == '\t' || v[0] == '=')) v.erase(0, 1);
        return v;
    }
    return "";
}
#endif

#if defined(__APPLE__)
// macOS: the layout NAME of the selected input source ("German", "U.S.", "British", "Austrian", ...)
std::string mac_code(const std::string& text) {
    const std::string t = ascii_lower_str(text);
    struct M { const char* name; const char* code; };
    static const M m[] = {{"german", "de"}, {"austrian", "at"}, {"swiss", "ch"}, {"spanish", "es"}, {"italian", "it"},
                          {"french", "fr"}, {"british", "gb"}, {"irish", "ie"}, {"australian", "au"}, {"canadian", "ca"},
                          {"u.s.", "us"}, {"abc", "us"}, {"us", "us"}};
    for (const auto& e : m) if (t.find(e.name) != std::string::npos) return e.code;
    return "";
}
#endif
#endif

std::string detect() {
#if defined(_WIN32)
    const LANGID lang = LOWORD(reinterpret_cast<uintptr_t>(GetKeyboardLayout(0)));
    switch (PRIMARYLANGID(lang)) {
        case LANG_GERMAN: return SUBLANGID(lang) == 0x03 ? "at" : SUBLANGID(lang) == 0x02 ? "ch" : "de";
        case LANG_SPANISH: return "es";
        case LANG_ITALIAN: return "it";
        case LANG_FRENCH: return "fr";
        case LANG_ENGLISH:
            switch (SUBLANGID(lang)) {
                case 0x02: return "gb";   // SUBLANG_ENGLISH_UK (numeric: not every SDK header defines every constant)
                case 0x03: return "au";   // SUBLANG_ENGLISH_AUS
                case 0x04: return "ca";   // SUBLANG_ENGLISH_CAN
                default: return "us";
            }
        default: return "";
    }
#else
    if (const char* e = std::getenv("MOUSIKI_KEYBOARD")) if (*e) return first_code(e);   // manual override, e.g. MOUSIKI_KEYBOARD=de
    if (const char* e = std::getenv("XKB_DEFAULT_LAYOUT")) if (*e) return first_code(e);
#if defined(__APPLE__)
    const ProcResult r = run_capture({"defaults", "read", "com.apple.HIToolbox", "AppleSelectedInputSources"});  // stderr is discarded
    if (r.exit_code == 0) { const std::string c = mac_code(r.out); if (!c.empty()) return c; }
#else
    {
        const ProcResult r = run_capture({"setxkbmap", "-query"});
        if (r.exit_code == 0) { const std::string v = value_after(r.out, "layout:"); if (!first_code(v).empty()) return first_code(v); }
    }
    {
        const ProcResult r = run_capture({"localectl", "status"});
        if (r.exit_code == 0) { const std::string v = value_after(r.out, "X11 Layout:"); if (!first_code(v).empty()) return first_code(v); }
    }
    {
        std::ifstream in("/etc/default/keyboard");
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("XKBLAYOUT=", 0) != 0) continue;
            std::string v = line.substr(10);
            v.erase(std::remove(v.begin(), v.end(), '"'), v.end());
            if (!first_code(v).empty()) return first_code(v);
        }
    }
#endif
    // last resort: the region of the locale (de_DE.UTF-8 -> de)
    for (const char* var : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* e = std::getenv(var);
        if (e && *e && std::string(e) != "C" && std::string(e) != "POSIX") return first_code(e);
    }
    return "";
#endif
}

} // namespace

std::string keyboard_layout_code() {
    static const std::string code = detect();
    return code;
}

std::string mode_switch_key_label() {
    const std::string c = keyboard_layout_code();
    if (c == "de" || c == "at" || c == "es" || c == "it") return "SHIFT and +";
    if (c == "us" || c == "gb" || c == "ie" || c == "au" || c == "ca" || c == "en" || c == "nz") return "SHIFT+8";
    return "*";
}

std::string mode_switch_back_label() {
    const std::string up = mode_switch_key_label();
    return up == "*" ? "_" : up == "SHIFT and +" ? "SHIFT and -" : "SHIFT+-";
}

std::string default_size_mode_key(bool next) {
    const std::string c = keyboard_layout_code();
    if (c == "us" || c == "gb" || c == "ie" || c == "au" || c == "ca" || c == "en" || c == "nz") return next ? "'" : ";";   // ] [ { } are the radio's timeshift keys; ; and ' sit side by side on a US keyboard
    return next ? "*" : "_";
}

std::string default_audio_device_key() {
    const std::string c = keyboard_layout_code();
    return (c == "de" || c == "at" || c == "ch") ? "°" : "&";
}

std::string default_video_key() {
    const std::string c = keyboard_layout_code();
    return (c == "de" || c == "at" || c == "ch") ? "&" : "Y";
}

std::string mode_key_label(int ch) {
    if (ch == 0) return "-";
    if (ch == '*') return mode_switch_key_label();
    if (ch == '_') return mode_switch_back_label();
    if (ch >= 'A' && ch <= 'Z') return std::string("SHIFT+") + static_cast<char>(ch + 32);
    return std::string(1, static_cast<char>(ch));
}

std::string symbol_key_label(const std::string& key) {
    if (key != "(" && key != ")" && key != "=" && key != "<" && key != "/") return key;
    const std::string c = keyboard_layout_code();
    if (key == "/") {   // the search key: SHIFT+7 on a German, Austrian, Swiss, Spanish or Italian keyboard
        if (c == "de" || c == "at" || c == "ch" || c == "es" || c == "it") return "SHIFT+7";
        return key;
    }
    if (key == "<") {   // the menus index: its own key on German-style keyboards, SHIFT+, on US-style ones
        if (c == "us" || c == "gb" || c == "ie" || c == "au" || c == "ca" || c == "en" || c == "nz") return "SHIFT+,";
        return key;
    }
    if (key == "=") {   // the big scope overlay: SHIFT+0 on a German keyboard
        if (c == "de" || c == "at" || c == "ch" || c == "es" || c == "it") return "SHIFT+0";
        return key;
    }
    if (c == "de" || c == "at" || c == "ch" || c == "es" || c == "it") return key == "(" ? "SHIFT+8" : "SHIFT+9";
    if (c == "us" || c == "gb" || c == "ie" || c == "au" || c == "ca" || c == "en" || c == "nz") return key == "(" ? "SHIFT+9" : "SHIFT+0";
    return key;
}

} // namespace muisc
