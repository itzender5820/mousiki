// The notes file of an audio book (see book_notes.h).
#include "book_notes.h"
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

namespace book_notes {

namespace {
} // namespace

fs::path path_for(const fs::path& book_dir) {
    fs::path d = book_dir;
    if (!d.has_filename()) d = d.parent_path();
    return d / path_from_utf8(path_utf8(d.filename()) + "_notes.txt");
}

std::string time_text(double sec) {
    const long long t = static_cast<long long>(std::max(0.0, sec));
    char b[40];
    std::snprintf(b, sizeof b, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

bool parse_time(const std::string& str, double& sec) {
    const std::string s = trim_ws(str);
    if (s.empty()) return false;
    double total = 0.0;
    size_t pos = 0;
    int parts = 0;
    while (pos <= s.size() && parts < 3) {
        const size_t colon = s.find(':', pos);
        const std::string piece = s.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
        if (piece.empty()) return false;
        char* end = nullptr;
        const double v = std::strtod(piece.c_str(), &end);
        if (end == piece.c_str() || *end != '\0' || v < 0) return false;
        total = total * 60.0 + v;
        ++parts;
        if (colon == std::string::npos) { sec = total; return true; }
        pos = colon + 1;
    }
    return false;
}

std::vector<BookNote> parse(const std::string& text) {
    std::vector<BookNote> out;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    BookNote cur;
    bool open = false;
    auto close = [&]() {
        if (!open) return;
        while (!cur.text.empty() && trim_ws(cur.text.back()).empty()) cur.text.pop_back();   // the blank line between two notes
        out.push_back(std::move(cur));
        cur = BookNote();
        open = false;
    };
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (first) { first = false; if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3); }
        if (line.compare(0, 4, "=== ") == 0) {
            const size_t a = line.find(" | ", 4);
            const size_t b = a == std::string::npos ? std::string::npos : line.find(" | ", a + 3);
            double sec = 0;
            if (a != std::string::npos && b != std::string::npos && parse_time(line.substr(a + 3, b - a - 3), sec)) {
                close();
                cur.file = trim_ws(line.substr(4, a - 4));
                cur.sec = sec;
                cur.title = trim_ws(line.substr(b + 3));
                const size_t m = cur.title.rfind(" {set=");   // the dates (written by format())
                if (m != std::string::npos && cur.title.back() == '}') {
                    long long st = 0, ch = 0;
                    if (std::sscanf(cur.title.c_str() + m, " {set=%lld changed=%lld}", &st, &ch) == 2) { cur.set = st; cur.changed = ch; cur.title.erase(m); }
                }
                open = true;
                continue;
            }
        }
        if (open) {
            if (line.compare(0, 4, " ===") == 0) line.erase(0, 1);   // an escaped text line
            cur.text.push_back(line);
        }
    }
    close();
    return out;
}

std::string format(const std::vector<BookNote>& notes) {
    std::string out;
    for (const BookNote& n : notes) {
        out += "=== " + n.file + " | " + time_text(n.sec) + " | " + n.title;
        if (n.set > 0 || n.changed > 0) out += " {set=" + std::to_string(n.set) + " changed=" + std::to_string(n.changed) + "}";
        out += "\n";
        for (const std::string& l : n.text) out += ((l.compare(0, 3, "===") == 0 || l.compare(0, 4, " ===") == 0) ? " " : "") + l + "\n";
        out += "\n";
    }
    return out;
}

bool load(const fs::path& file, std::vector<BookNote>& out) {
    out.clear();
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    out = parse(ss.str());
    return true;
}

bool save(const fs::path& file, const std::vector<BookNote>& notes) {
    return write_text_atomic(file, format(notes), true);
}

int merge(std::vector<BookNote>& into, const std::vector<BookNote>& from) {
    int added = 0;
    for (const BookNote& n : from) {
        if (std::find(into.begin(), into.end(), n) != into.end()) continue;
        into.push_back(n);
        ++added;
    }
    return added;
}

} // namespace book_notes
} // namespace muisc
