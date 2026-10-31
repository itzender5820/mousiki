// Translations of lyrics: saves and loads a translation per track, lyric version and target language,
// identified by a fingerprint of the original lines so it is dropped when the lyrics change.
#include "lyrics_translate.h"
#include "path_utf8.h"
#include "process_util.h"
#include "tiny_json.h"
#include "translit.h"
#if defined(_WIN32)
#include "win_compat.h"
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace muisc {

using namespace tinyjson;

namespace {

std::string fingerprint(const std::vector<std::string>& lines) {
    unsigned long long h = 1469598103934665603ULL;   // FNV-1a
    for (const auto& l : lines) {
        for (unsigned char c : l) { h ^= c; h *= 1099511628211ULL; }
        h ^= 0x0a; h *= 1099511628211ULL;
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", h);
    return buf;
}

std::string single_line(std::string s) {
    for (auto& c : s) if (c == '\n' || c == '\r') c = ' ';
    return s;
}

} // namespace

fs::path lyrics_translation_file(const fs::path& track_path, const LyricsVersion& v, const std::string& to, const std::string& service) {
    const fs::path dir = lyrics_folder_of(track_path);
    if (dir.empty()) return {};
    std::string name;
    if (v.kind == LyricsVersion::Kind::Embedded || v.file.empty()) name = path_utf8(track_path.stem()) + ".embedded";
    else name = path_utf8(v.file.stem());      // "Song" for the copy in use, "Song.v2026...lrclib" for an older one
    return dir / "translations" / path_from_utf8(name + "." + to + (service.empty() ? std::string() : "." + service) + ".txt");
}

namespace {

// Reads one translation file; false when it is missing, damaged, or made for other lyrics than `plain`.
bool read_translation_file(const fs::path& p, const std::vector<std::string>& plain, LyricsTranslation& out) {
    if (p.empty()) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::string head;
    std::getline(in, head);
    while (!head.empty() && (head.back() == '\r' || head.back() == '\n')) head.pop_back();
    if (head.compare(0, 21, "#mousiki-translation ") != 0) return false;
    auto field = [&](const std::string& key) {
        const size_t i = head.find(" " + key + "=");
        if (i == std::string::npos) return std::string();
        const size_t b = i + key.size() + 2;
        const size_t e = head.find(' ', b);
        return head.substr(b, e == std::string::npos ? std::string::npos : e - b);
    };
    if (field("hash") != fingerprint(plain)) return false;     // other lyrics than the ones this was made for
    LyricsTranslation t;
    t.from = field("from");
    t.service = field("svc");
    if (t.service == "-") t.service.clear();
    if (field("same") == "1") { t.ok = true; t.same = true; out = t; return true; }
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        t.lines.push_back(line);
    }
    if (t.lines.size() != plain.size()) return false;
    t.ok = true;
    out = std::move(t);
    return true;
}

// The service named in the first line of a file (older versions: the only place it is written), "" when unknown.
std::string service_in_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string head;
    if (!in.is_open() || !std::getline(in, head)) return std::string();
    const size_t i = head.find(" svc=");
    if (i == std::string::npos) return std::string();
    const size_t b = i + 5, e = head.find_first_of(" \r\n", b);
    const std::string v = head.substr(b, e == std::string::npos ? std::string::npos : e - b);
    return v == "-" ? std::string() : v;
}

}  // namespace

bool load_lyrics_translation(const fs::path& track_path, const LyricsVersion& v, const std::string& to,
                             const std::vector<std::string>& plain, LyricsTranslation& out, const std::string& service) {
    if (!service.empty() && read_translation_file(lyrics_translation_file(track_path, v, to, service), plain, out)) return true;
    const fs::path legacy = lyrics_translation_file(track_path, v, to);   // the name older versions used
    if (!service.empty()) {   // only when that file was made by this service
        LyricsTranslation t;
        if (!read_translation_file(legacy, plain, t) || t.service != service) return false;
        out = std::move(t);
        return true;
    }
    return read_translation_file(legacy, plain, out);
}

std::vector<TranslationInfo> list_lyrics_translations(const fs::path& track_path, const LyricsVersion& v,
                                                      const std::vector<std::string>& plain) {
    std::vector<TranslationInfo> found;
    const fs::path probe = lyrics_translation_file(track_path, v, "xx");   // <dir>/translations/<name>.xx.txt
    if (probe.empty() || plain.empty()) return found;
    const std::string prefix = path_utf8(probe.stem().stem()) + ".";        // "<name>."
    std::error_code ec;
    for (fs::directory_iterator it(probe.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string fname = path_utf8(it->path().filename());
        if (fname.size() <= prefix.size() + 4 || fname.compare(0, prefix.size(), prefix) != 0) continue;
        if (fname.compare(fname.size() - 4, 4, ".txt") != 0) continue;
        std::string code = fname.substr(prefix.size(), fname.size() - prefix.size() - 4);   // "en" (older) or "en.google"
        std::string svc;
        const size_t dot = code.find('.');
        if (dot != std::string::npos) {
            svc = code.substr(dot + 1);
            code = code.substr(0, dot);
            bool known = false;
            for (const auto& s : translate_services()) if (s.first == svc) known = true;
            if (!known) continue;   // another version's file ("<name>.v2026....lrclib.en.txt")
        }
        if (code.empty() || code.size() > 8 || code.find('.') != std::string::npos) continue;
        LyricsTranslation t;
        if (!read_translation_file(it->path(), plain, t) || t.same) continue;
        found.push_back({code, t.from, svc.empty() ? t.service : svc});
    }
    // one entry per (language, service) -- an older file and a new one of the same service are the same translation
    std::vector<TranslationInfo> uniq;
    for (const auto& a : found) {
        bool seen = false;
        for (const auto& u : uniq) if (u.to == a.to && u.service == a.service) { seen = true; break; }
        if (!seen) uniq.push_back(a);
    }
    std::sort(uniq.begin(), uniq.end(), [](const TranslationInfo& a, const TranslationInfo& b) {
        const std::string la = language_name(a.to), lb = language_name(b.to);
        return la != lb ? la < lb : a.service < b.service;
    });
    return uniq;
}

bool delete_lyrics_translation(const fs::path& track_path, const LyricsVersion& v, const std::string& to, const std::string& service) {
    std::error_code ec;
    bool any = false;
    if (!service.empty()) {
        const fs::path p = lyrics_translation_file(track_path, v, to, service);
        if (!p.empty() && fs::remove(p, ec)) any = true;
    }
    const fs::path legacy = lyrics_translation_file(track_path, v, to);
    if (!legacy.empty() && (service.empty() || service_in_file(legacy) == service) && fs::remove(legacy, ec)) any = true;
    return any;
}

bool save_lyrics_translation(const fs::path& track_path, const LyricsVersion& v, const std::string& to,
                             const std::vector<std::string>& plain, const LyricsTranslation& t) {
    const fs::path p = lyrics_translation_file(track_path, v, to, t.service);
    if (p.empty() || !t.ok) return false;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << "#mousiki-translation to=" << to << " from=" << (t.from.empty() ? "?" : t.from)
        << " svc=" << (t.service.empty() ? "-" : t.service)
        << " lines=" << plain.size() << " hash=" << fingerprint(plain) << (t.same ? " same=1" : "") << "\n";
    if (!t.same) for (const auto& l : t.lines) out << single_line(l) << "\n";
    const bool good = out.good();
    out.close();
    if (good && !t.service.empty()) {   // an older "<name>.en.txt" made by this same service is replaced by this file
        const fs::path legacy = lyrics_translation_file(track_path, v, to);
        if (service_in_file(legacy) == t.service) fs::remove(legacy, ec);
    }
    return good;
}

LyricsTranslation translate_lyrics_lines(const std::vector<std::string>& plain, const TranslateRequest& request,
                                         const fs::path& script) {
    LyricsTranslation res;
    res.service = request.service;
    std::error_code ec;
    if (!fs::exists(script, ec)) { res.error = "translation script not found: " + path_utf8(script); return res; }
    Args python = python_command();
    if (python.empty()) { res.error = "Python 3 not found on PATH -- translation unavailable"; return res; }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path req = fs::temp_directory_path(ec) / path_from_utf8("mousiki_translate_" + std::to_string(stamp) + ".json");
    {
        Value root = Value::make_obj();
        root.set("service", Value::make_str(request.service));
        root.set("from", Value::make_str(request.from));
        root.set("to", Value::make_str(request.to));
        Value arr = Value::make_arr();
        for (const auto& l : plain) arr.arr.push_back(Value::make_str(l));
        root.set("lines", arr);
        std::ofstream o(req, std::ios::binary | std::ios::trunc);
        if (!o.is_open()) { res.error = "could not write " + path_utf8(req); return res; }
        o << write(root);
    }
    Args cmd = python;
    cmd.push_back(path_utf8(script));
    cmd.push_back(path_utf8(req));
    const ProcResult r = run_capture(cmd, false);
    fs::remove(req, ec);
    if (r.exit_code < 0) { res.error = "python3 not found on PATH -- translation unavailable"; return res; }
    Value v;
    if (r.out.empty() || !parse(r.out, v) || v.type != Type::Object) {
        res.error = "the translation helper gave no answer (exit " + std::to_string(r.exit_code) + ")";
        return res;
    }
    if (auto* ok = v.find("ok")) res.ok = ok->as_bool(false);
    if (auto* l = v.find("lang")) res.from = l->as_string();
    if (!res.ok) {
        std::string err, detail;
        if (auto* e = v.find("error")) err = e->as_string();
        if (auto* d = v.find("detail")) detail = d->as_string();
        res.error = err == "NETWORK" ? "no connection to the translation service (" + detail + ")"
                  : err == "RATE_LIMIT" ? "the service is busy (" + detail + ") -- wait a minute or pick another service with [t]"
                  : err == "SERVICE" ? "the service did not translate: " + detail
                  : detail.empty() ? "translation failed" : detail;
        return res;
    }
    if (auto* s = v.find("same")) res.same = s->as_bool(false);
    if (!res.same) {
        if (auto* arr = v.find("lines"); arr && arr->type == Type::Array)
            for (const auto& x : arr->arr) res.lines.push_back(x.as_string());
        if (res.lines.size() != plain.size()) {
            res.ok = false;
            res.error = "the translation does not match the lyrics (" + std::to_string(res.lines.size()) + " of " +
                        std::to_string(plain.size()) + " lines)";
        }
    }
    return res;
}

const std::vector<std::pair<std::string, std::string>>& translate_languages() {
    static const std::vector<std::pair<std::string, std::string>> kList = [] {
        std::vector<std::pair<std::string, std::string>> v = {
            {"af", "Afrikaans"}, {"sq", "Albanian"}, {"ar", "Arabic"}, {"hy", "Armenian"}, {"eu", "Basque"}, {"bn", "Bengali"},
            {"bg", "Bulgarian"}, {"ca", "Catalan"}, {"zh", "Chinese"}, {"hr", "Croatian"}, {"cs", "Czech"}, {"da", "Danish"},
            {"nl", "Dutch"}, {"en", "English"}, {"et", "Estonian"}, {"tl", "Filipino"}, {"fi", "Finnish"}, {"fr", "French"},
            {"gl", "Galician"}, {"ka", "Georgian"}, {"de", "German"}, {"el", "Greek"}, {"gu", "Gujarati"}, {"he", "Hebrew"},
            {"hi", "Hindi"}, {"hu", "Hungarian"}, {"is", "Icelandic"}, {"id", "Indonesian"}, {"ga", "Irish"}, {"it", "Italian"},
            {"ja", "Japanese"}, {"kn", "Kannada"}, {"ko", "Korean"}, {"la", "Latin"}, {"lv", "Latvian"}, {"lt", "Lithuanian"},
            {"mk", "Macedonian"}, {"ms", "Malay"}, {"ml", "Malayalam"}, {"mt", "Maltese"}, {"mr", "Marathi"}, {"no", "Norwegian"},
            {"fa", "Persian"}, {"pl", "Polish"}, {"pt", "Portuguese"}, {"pa", "Punjabi"}, {"ro", "Romanian"}, {"ru", "Russian"},
            {"sr", "Serbian"}, {"sk", "Slovak"}, {"sl", "Slovenian"}, {"es", "Spanish"}, {"sw", "Swahili"}, {"sv", "Swedish"},
            {"ta", "Tamil"}, {"te", "Telugu"}, {"th", "Thai"}, {"tr", "Turkish"}, {"uk", "Ukrainian"}, {"ur", "Urdu"},
            {"vi", "Vietnamese"}, {"cy", "Welsh"}, {"yi", "Yiddish"},
        };
        return v;
    }();
    return kList;
}

const std::vector<std::pair<std::string, std::string>>& translate_services() {
    static const std::vector<std::pair<std::string, std::string>> kList = {
        {"google", "Google Translate"}, {"mymemory", "MyMemory"}};
    return kList;
}

std::string translate_service_name(const std::string& id) {
    for (const auto& s : translate_services()) if (s.first == id) return s.second;
    return id.empty() ? std::string("unknown service") : id;
}

std::string language_name(const std::string& code) {
    std::string base = code.substr(0, code.find('-'));
    if (base == "iw") base = "he";
    if (base == "jw") base = "jv";
    if (base == "nb" || base == "nn") base = "no";
    if (code == "zh-TW") return "Chinese (traditional)";
    for (const auto& k : translate_languages()) if (k.first == base) return k.second;
    if (base == "jv") return "Javanese";
    return code.empty() ? std::string("unknown language") : code;
}

} // namespace muisc

namespace muisc {

bool lyrics_export_translit_differs(const std::vector<std::string>& lines) {
    for (const std::string& l : lines)
        if (!l.empty() && translit_possible(l) && transliterate(l) != l) return true;
    return false;
}

std::string lyrics_export_text(const std::vector<std::string>& lines, const std::vector<std::string>* translation, bool transliteration) {
    auto lower = [](std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); return s; };
    const bool tr = translation && translation->size() == lines.size();
    std::string out;
    bool blank_last = true;   // nothing yet: a blank line is not needed at the start
    auto blank = [&]() { if (!blank_last) { out += '\n'; blank_last = true; } };
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty()) { blank(); continue; }
        out += lines[i];
        out += '\n';
        blank_last = false;
        if (transliteration && translit_possible(lines[i])) {
            const std::string t = transliterate(lines[i]);
            if (!t.empty() && t != lines[i]) { out += t; out += '\n'; }
        }
        if (tr) {
            const std::string& t = (*translation)[i];
            if (!t.empty() && lower(t) != lower(lines[i])) { out += t; out += '\n'; }
        }
        if (transliteration || tr) blank();   // (every group, so the groups stay the same size apart)
    }
    while (out.size() >= 2 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') out.pop_back();   // one line break at the end
    return out;
}

}  // namespace muisc
