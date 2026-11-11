#include "book_embedded.h"
#include "process_util.h"
#include "string_util.h"
#include "utf8_util.h"
#include "path_utf8.h"
#include "tiny_json.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

namespace muisc {

namespace fs = std::filesystem;

std::vector<EmbeddedChapter> parse_embedded_chapters(const std::string& ffprobe_json, double file_duration) {
    std::vector<EmbeddedChapter> out;
    tinyjson::Value root;
    if (!tinyjson::parse(ffprobe_json, root)) return out;
    const tinyjson::Value* list = root.find("chapters");
    if (!list || list->type != tinyjson::Type::Array) return out;
    struct Raw { double start, end; std::string title; };
    std::vector<Raw> raw;
    for (const tinyjson::Value& c : list->arr) {
        if (c.type != tinyjson::Type::Object) continue;
        // (ffprobe prints the times as strings: "start_time": "12.345000")
        auto num = [&](const char* key) {
            const tinyjson::Value* v = c.find(key);
            if (!v) return -1.0;
            if (v->type == tinyjson::Type::Number) return v->num;
            if (v->type == tinyjson::Type::String) { char* e = nullptr; const double d = std::strtod(v->str.c_str(), &e); return e != v->str.c_str() ? d : -1.0; }
            return -1.0;
        };
        Raw r{num("start_time"), num("end_time"), std::string()};
        if (r.start < 0.0 || !std::isfinite(r.start)) continue;
        if (!(r.end > r.start) || !std::isfinite(r.end)) r.end = -1.0;
        if (const tinyjson::Value* tags = c.find("tags")) {
            for (const auto& kv : tags->obj) {   // (the key's case varies between taggers: "title" / "TITLE")
                if (ascii_lower_str(kv.first) == "title") { r.title = trim_ws(kv.second.as_string()); break; }
            }
        }
        raw.push_back(std::move(r));
    }
    std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) { return a.start < b.start; });
    // every chapter ends where the next begins (a gap or an overlap in the file is closed); the last one at the end of the file
    for (size_t i = 0; i + 1 < raw.size(); ++i) raw[i].end = raw[i + 1].start;
    if (!raw.empty() && file_duration > raw.back().start) raw.back().end = file_duration;
    // a chapter shorter than a second: the one before takes its time over
    std::vector<Raw> kept;
    for (size_t i = 0; i < raw.size(); ++i) {
        const bool known = raw[i].end > raw[i].start;
        if (known && raw[i].end - raw[i].start < 1.0 && !kept.empty()) { kept.back().end = raw[i].end; continue; }
        kept.push_back(raw[i]);
    }
    if (kept.size() < 2) return out;
    if (kept.front().start < 1.0) kept.front().start = 0.0;   // (a first chapter at 0.3 s starts at 0)
    for (size_t i = 0; i < kept.size(); ++i) {
        EmbeddedChapter c;
        c.start = kept[i].start;
        c.end = kept[i].end;
        c.title = kept[i].title.empty() ? "Chapter " + std::to_string(i + 1) : kept[i].title;
        out.push_back(std::move(c));
    }
    return out;
}

namespace {
struct Cached { long long size = 0; long long mtime = 0; std::vector<EmbeddedChapter> chapters; };
std::mutex g_mu;
std::map<std::string, Cached>& cache() { static std::map<std::string, Cached> c; return c; }
}

std::vector<EmbeddedChapter> read_embedded_chapters(const fs::path& file) {
    std::error_code ec;
    const long long size = static_cast<long long>(fs::file_size(file, ec));
    if (ec) return {};
    const long long mtime = static_cast<long long>(fs::last_write_time(file, ec).time_since_epoch().count());
    const std::string key = path_utf8(file);
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = cache().find(key);
        if (it != cache().end() && it->second.size == size && it->second.mtime == mtime) return it->second.chapters;
    }
    std::vector<EmbeddedChapter> chapters;
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-show_chapters", "-show_entries", "format=duration", "-of", "json", key}, false);
    if (r.ok()) {
        double dur = 0.0;
        tinyjson::Value root;
        if (tinyjson::parse(r.out, root)) if (const tinyjson::Value* f = root.find("format")) { const tinyjson::Value* d = f->find("duration"); if (d) dur = d->type == tinyjson::Type::String ? std::strtod(d->str.c_str(), nullptr) : d->as_number(); }
        chapters = parse_embedded_chapters(r.out, dur);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    cache()[key] = Cached{size, mtime, chapters};
    return chapters;
}

bool embedded_chapters_worth_asking(const fs::path& file, size_t files_in_book) {
    const std::string ext = ascii_lower_str(path_utf8(file.extension()));
    if (ext == ".m4b" || ext == ".mka") return true;
    return files_in_book <= 2;
}

} // namespace muisc
