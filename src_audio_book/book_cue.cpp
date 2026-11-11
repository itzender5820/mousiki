// The .cue file of an audio book (see book_cue.h).
#include "book_cue.h"
#include "string_util.h"
#include "path_utf8.h"
#include "settings.h"   // write_text_atomic()
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace muisc {

namespace fs = std::filesystem;

namespace {
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

fs::path BookCue::path_for(const fs::path& book_dir) {
    fs::path d = book_dir;
    if (d.has_filename() == false) d = d.parent_path();   // "dir/" -> "dir"
    return d / path_from_utf8(path_utf8(d.filename()) + ".cue");
}

bool BookCue::load(const fs::path& file) {
    lines_.clear();
    bom_ = crlf_ = false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string all = ss.str();
    if (all.size() >= 3 && static_cast<unsigned char>(all[0]) == 0xEF && static_cast<unsigned char>(all[1]) == 0xBB && static_cast<unsigned char>(all[2]) == 0xBF) {
        bom_ = true;
        all.erase(0, 3);
    }
    crlf_ = all.find("\r\n") != std::string::npos;
    std::istringstream ls(all);
    std::string line;
    while (std::getline(ls, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        lines_.push_back(line);
    }
    while (!lines_.empty() && trim_ws(lines_.back()).empty()) lines_.pop_back();
    return true;
}

bool BookCue::save(const fs::path& file) const {
    std::string text;
    if (bom_) text += "\xEF\xBB\xBF";
    for (const std::string& l : lines_) text += l + (crlf_ ? "\r\n" : "\n");
    return write_text_atomic(file, text, true);   // (temp file + rename: the auto-mark rewrites this every few seconds)
}

size_t BookCue::header_end() const {
    for (size_t i = 0; i < lines_.size(); ++i)
        if (is_key(lines_[i], "FILE")) return i;
    return lines_.size();
}

// The line starts with `key` (case-insensitive) followed by a blank or the end: "REM NARRATOR" does not match "REM NARRATORS".
bool BookCue::is_key(const std::string& line, const std::string& key) {
    const std::string l = trim_ws(line);
    if (l.size() < key.size() || upper(l.substr(0, key.size())) != upper(key)) return false;
    return l.size() == key.size() || std::isspace(static_cast<unsigned char>(l[key.size()]));
}

std::string BookCue::get(const std::string& key) const {
    const size_t end = header_end();
    for (size_t i = 0; i < end; ++i) {
        if (!is_key(lines_[i], key)) continue;
        std::string v = trim_ws(trim_ws(lines_[i]).substr(key.size()));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return std::string();
}

std::string BookCue::get_raw(const std::string& key) const {
    const size_t end = header_end();
    for (size_t i = 0; i < end; ++i)
        if (is_key(lines_[i], key)) return trim_ws(trim_ws(lines_[i]).substr(key.size()));
    return std::string();
}

void BookCue::set_raw(const std::string& key, const std::string& raw) {
    const std::string v = trim_ws(raw);
    const std::string line = key + " " + v;
    const size_t end = header_end();
    for (size_t i = 0; i < end; ++i) {
        if (!is_key(lines_[i], key)) continue;
        if (v.empty()) lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(i));
        else lines_[i] = line;
        return;
    }
    if (v.empty()) return;
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(end), line);
}

std::vector<std::string> BookCue::get_all_raw(const std::string& key) const {
    std::vector<std::string> out;
    const size_t end = header_end();
    for (size_t i = 0; i < end; ++i)
        if (is_key(lines_[i], key)) out.push_back(trim_ws(trim_ws(lines_[i]).substr(key.size())));
    return out;
}

void BookCue::set_all_raw(const std::string& key, const std::vector<std::string>& values) {
    size_t at = std::string::npos;
    for (size_t i = 0; i < header_end();) {
        if (is_key(lines_[i], key)) {
            if (at == std::string::npos) at = i;
            lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(i));
        } else ++i;
    }
    if (at == std::string::npos) at = header_end();
    for (const std::string& v : values) {
        const std::string t = trim_ws(v);
        if (t.empty()) continue;
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(at), key + " " + t);
        ++at;
    }
}

bool parse_bookmark(const std::string& raw, BookMark& out) {
    const size_t q1 = raw.find('"'), q2 = raw.rfind('"');
    if (q1 != 0 || q2 == std::string::npos || q2 <= q1) return false;
    out.file = raw.substr(1, q2 - 1);
    const char* p = raw.c_str() + q2 + 1;
    char* e = nullptr;
    out.sec = std::strtod(p, &e);
    out.when = 0;
    if (e && e != p) out.when = std::strtoll(e, nullptr, 10);
    return true;
}

std::string format_bookmark(const BookMark& m) {
    char b[64];
    std::snprintf(b, sizeof b, "%.1f %lld", std::max(0.0, m.sec), m.when);
    return "\"" + m.file + "\" " + b;
}

void BookCue::set(const std::string& key, const std::string& value) {
    std::string v = trim_ws(value);
    std::replace(v.begin(), v.end(), '"', '\'');   // CUE has no escape for a quote
    const bool is_date = upper(key) == "REM DATE";   // a year is written bare, like most taggers do
    const std::string line = key + " " + (is_date ? v : "\"" + v + "\"");
    const size_t end = header_end();
    for (size_t i = 0; i < end; ++i) {
        if (!is_key(lines_[i], key)) continue;
        if (v.empty()) lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(i));
        else lines_[i] = line;
        return;
    }
    if (v.empty()) return;
    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(end), line);
}

} // namespace muisc
