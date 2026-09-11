#include "lyrics_fetcher.h"
#include "TextSanitizer.h"
#include "process_util.h"
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace muisc {

// Sidecar lyrics file lives next to the track, same stem, .lrc extension —
// e.g. "Song Title.opus" -> "Song Title.lrc". Works for both a user's own
// library and the yt-dlp cache dir (both are "the music folder" for
// whatever track lives there), and is what lets a previously-fetched
// track show lyrics offline.
static fs::path sidecar_path(const fs::path& track_path) {
    if (track_path.empty()) return {};
    return track_path.parent_path() / (track_path.stem().string() + ".lrc");
}

static bool load_sidecar(const fs::path& track_path, std::string& out_lrc) {
    fs::path p = sidecar_path(track_path);
    if (p.empty()) return false;
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream oss;
    oss << in.rdbuf();
    out_lrc = oss.str();
    return !out_lrc.empty();
}

static void save_sidecar(const fs::path& track_path, const std::string& lrc) {
    fs::path p = sidecar_path(track_path);
    if (p.empty() || lrc.empty()) return;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (out.is_open()) out << lrc;
}

// --- minimal JSON field extraction -----------------------------------
// The helper script's output shape is fixed and simple (see
// scripts/fetch_lyrics.py), so a tiny hand-rolled extractor avoids
// pulling in a JSON dependency for one flat object.

static bool json_get_bool(const std::string& json, const std::string& key, bool fallback) {
    std::regex re("\"" + key + "\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (std::regex_search(json, m, re)) return m[1] == "true";
    return fallback;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parses 4 hex digits starting at s[pos]; false (out untouched) if out
// of range or non-hex.
static bool parse_hex4(const std::string& s, size_t pos, uint32_t& out) {
    if (pos + 4 > s.size()) return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; ++k) {
        int h = hex_val(s[pos + k]);
        if (h < 0) return false;
        v = (v << 4) | static_cast<uint32_t>(h);
    }
    out = v;
    return true;
}

static void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

static std::string json_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == 'n') { out += '\n'; ++i; continue; }
            if (n == 't') { out += '\t'; ++i; continue; }
            if (n == 'r') { out += '\r'; ++i; continue; }
            if (n == '"' || n == '\\' || n == '/') { out += n; ++i; continue; }
            if (n == 'u') {
                // \uXXXX -- was falling through untouched before (only
                // n/t/"/\/ were handled), which is exactly why non-ASCII
                // lyrics (CJK titles, curly quotes, em-dashes, etc --
                // anything Python's json.dumps escapes as \uXXXX by
                // default) rendered as literal "\u4f5c"-style text
                // instead of the actual characters.
                uint32_t cp;
                if (parse_hex4(s, i + 2, cp)) {
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate -- must be immediately followed
                        // by a low surrogate to form one real codepoint
                        // (characters outside the BMP, e.g. some emoji).
                        uint32_t low;
                        if (i + 7 < s.size() && s[i + 6] == '\\' && s[i + 7] == 'u' &&
                            parse_hex4(s, i + 8, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            uint32_t combined = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            append_utf8(out, combined);
                            i += 11; // consumed \uXXXX\uXXXX (12 chars; loop's ++i covers the 12th)
                            continue;
                        }
                        // Unpaired high surrogate -- fall through and
                        // emit the raw escape rather than a broken codepoint.
                    } else {
                        append_utf8(out, cp);
                        i += 5; // consumed \uXXXX (6 chars; loop's ++i covers the 6th)
                        continue;
                    }
                }
            }
        }
        out += s[i];
    }
    return out;
}

static bool json_get_string(const std::string& json, const std::string& key, std::string& out) {
    // Finds "key":"....(possibly escaped)...."
    std::string needle = "\"" + key + "\"";
    size_t kpos = json.find(needle);
    if (kpos == std::string::npos) return false;
    size_t colon = json.find(':', kpos + needle.size());
    if (colon == std::string::npos) return false;
    size_t qstart = json.find('"', colon);
    if (qstart == std::string::npos) return false;
    size_t i = qstart + 1;
    std::string raw;
    while (i < json.size()) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            raw += json[i];
            raw += json[i + 1];
            i += 2;
            continue;
        }
        if (json[i] == '"') break;
        raw += json[i];
        ++i;
    }
    out = json_unescape(raw);
    return true;
}

// --- enhanced/plain LRC parsing ----------------------------------------

static std::vector<LyricLine> parse_lrc(const std::string& lrc_text, bool enhanced) {
    std::string sanitized_lrc = sanitize_lyric_text(lrc_text);
    std::vector<LyricLine> lines;
    std::istringstream stream(sanitized_lrc);
    std::string raw_line;

    static const std::regex line_ts_re(R"(^\[(\d+):(\d+(?:\.\d+)?)\])");
    static const std::regex word_ts_re(R"(<(\d+):(\d+(?:\.\d+)?)>)");

    while (std::getline(stream, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();

        std::smatch m;
        if (!std::regex_search(raw_line, m, line_ts_re)) continue; // skip metadata/[ar:]/[ti:] tags etc.

        double line_time = std::stod(m[1].str()) * 60.0 + std::stod(m[2].str());
        std::string rest = raw_line.substr(m.position(0) + m.length(0));

        LyricLine line;
        line.start_time = line_time;

        if (enhanced && rest.find('<') != std::string::npos) {
            // "<mm:ss.xx>word <mm:ss.xx>word ..." — split on word timestamps.
            auto begin = std::sregex_iterator(rest.begin(), rest.end(), word_ts_re);
            auto end = std::sregex_iterator();
            std::vector<std::pair<double, size_t>> marks; // (time, text-start-offset)
            for (auto it = begin; it != end; ++it) {
                std::smatch wm = *it;
                double t = std::stod(wm[1].str()) * 60.0 + std::stod(wm[2].str());
                marks.emplace_back(t, static_cast<size_t>(wm.position(0) + wm.length(0)));
            }
            for (size_t i = 0; i < marks.size(); ++i) {
                size_t start = marks[i].second;
                size_t end_off = (i + 1 < marks.size())
                    ? rest.find('<', start)
                    : rest.size();
                if (end_off == std::string::npos) end_off = rest.size();
                std::string word = rest.substr(start, end_off - start);
                // trim
                while (!word.empty() && std::isspace((unsigned char)word.front())) word.erase(word.begin());
                while (!word.empty() && std::isspace((unsigned char)word.back())) word.pop_back();
                if (!word.empty()) {
                    line.words.emplace_back(marks[i].first, word);
                    if (!line.full_text.empty()) line.full_text += ' ';
                    line.full_text += word;
                }
            }
        }

        if (line.words.empty()) {
            // plain line-synced LRC (or enhanced parse yielded nothing usable)
            while (!rest.empty() && std::isspace((unsigned char)rest.front())) rest.erase(rest.begin());
            line.full_text = rest;
        }

        lines.push_back(std::move(line));
    }

    return lines;
}

LyricsResult fetch_synced_lyrics(const std::string& title, const std::string& artist,
                                  const std::string& helper_script_path, const fs::path& track_path,
                                  bool force_network) {
    LyricsResult result;

    // 1) Local sidecar file — no subprocess at all if this hits.
    std::string local_lrc;
    if (!force_network && load_sidecar(track_path, local_lrc)) {
        result.lines = parse_lrc(local_lrc, /*enhanced=*/true);
        if (!result.lines.empty()) {
            result.status = LyricsStatus::Ok;
            result.source = "local";
            result.raw_lrc = local_lrc;
            result.message = "lyrics loaded (cached)";
            return result;
        }
        // fall through to network chain if the sidecar was empty/unparseable
    }

    // 2) Python helper: syncedlyrics only (word-level "enhanced" search
    //    first, falls back to plain line-synced search). Paxsenix used to
    //    sit in this chain as a faster fallback but was unreliable enough
    //    (frequent misses/garbage) that it's been dropped entirely.
    std::string cmd = "python3 " + shell_quote(helper_script_path) +
                       " " + shell_quote(title) + " " + shell_quote(artist);
    ProcResult r = run_capture(cmd, /*merge_stderr=*/false);

    if (r.exit_code < 0) {
        // posix_spawnp itself failed — python3 genuinely isn't on PATH.
        result.status = LyricsStatus::PythonMissing;
        result.message = "python3 not found on PATH — lyrics unavailable";
        return result;
    }

    if (r.out.empty()) {
        // python3 ran but the script produced no JSON — crash, timeout,
        // or killed by signal before it could emit().
        result.status = LyricsStatus::Error;
        result.message = "lyrics helper script produced no output (exit " +
                         std::to_string(r.exit_code) + ")";
        return result;
    }

    if (!json_get_bool(r.out, "ok", false)) {
        std::string err, detail;
        json_get_string(r.out, "error", err);
        json_get_string(r.out, "detail", detail);

        if (err == "MODULE_MISSING") {
            result.status = LyricsStatus::ModuleMissing;
            result.message = "run: pip install syncedlyrics";
        } else if (err == "NOT_FOUND") {
            result.status = LyricsStatus::NotFound;
            result.message = "no lyrics found for \"" + title + "\"";
        } else {
            result.status = LyricsStatus::Error;
            result.message = detail.empty() ? "lyrics fetch failed" : detail;
        }
        return result;
    }

    std::string lrc, source;
    bool enhanced = json_get_bool(r.out, "enhanced", false);
    json_get_string(r.out, "lrc", lrc);
    json_get_string(r.out, "source", source);

    result.lines = parse_lrc(lrc, enhanced);
    result.status = LyricsStatus::Ok;
    result.source = source.empty() ? "syncedlyrics" : source;
    result.raw_lrc = lrc;
    result.message = (enhanced ? "word-synced lyrics" : "line-synced lyrics") + std::string(" (") + result.source + ")";

    save_sidecar(track_path, lrc); // cache to disk for offline reuse next time
    return result;
}

} // namespace muisc
