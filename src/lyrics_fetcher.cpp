#include "lyrics_fetcher.h"
#include "TextSanitizer.h"
#include "console_log.h"
#include "http_client.h"
#include "tiny_json.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#include <zlib.h>

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

// --- TTML -> LRC conversion --------------------------------------------
// Ported from scripts/lrc.py (now removed) -- Better Lyrics returns
// word-level timing as TTML, which used to get converted to (enhanced)
// LRC text by a Python helper script using xml.etree.ElementTree. This
// is a small, purpose-built TTML walker rather than a general XML
// parser: it only understands exactly the shape Better Lyrics' TTML
// actually has (a flat sequence of <p begin="..."> paragraphs, each
// containing a flat sequence of <span begin="...">word </span>
// elements), which is all this ever needs to handle.

static std::string trim_copy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string xml_unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string ent = s.substr(i + 1, semi - i - 1);
                if (ent == "amp") { out += '&'; i = semi; continue; }
                if (ent == "lt") { out += '<'; i = semi; continue; }
                if (ent == "gt") { out += '>'; i = semi; continue; }
                if (ent == "quot") { out += '"'; i = semi; continue; }
                if (ent == "apos") { out += '\''; i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    try {
                        uint32_t cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                                    ? static_cast<uint32_t>(std::stoul(ent.substr(2), nullptr, 16))
                                    : static_cast<uint32_t>(std::stoul(ent.substr(1)));
                        append_utf8(out, cp);
                        i = semi;
                        continue;
                    } catch (...) { /* fall through, emit '&' as-is below */ }
                }
            }
        }
        out += s[i];
    }
    return out;
}

// "HH:MM:SS.sss" / "MM:SS.sss" / "SS.sss" -> seconds.
static bool parse_ttml_time(const std::string& value, double& out_sec) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : value) {
        if (c == ':') { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    try {
        if (parts.size() == 3) {
            out_sec = std::stod(parts[0]) * 3600.0 + std::stod(parts[1]) * 60.0 + std::stod(parts[2]);
        } else if (parts.size() == 2) {
            out_sec = std::stod(parts[0]) * 60.0 + std::stod(parts[1]);
        } else {
            out_sec = std::stod(parts[0]);
        }
    } catch (...) {
        return false;
    }
    return true;
}

static std::string local_tag_name(const std::string& raw) {
    size_t i = 0;
    while (i < raw.size() && (std::isspace(static_cast<unsigned char>(raw[i])) || raw[i] == '/')) ++i;
    size_t start = i;
    while (i < raw.size() && !std::isspace(static_cast<unsigned char>(raw[i])) && raw[i] != '/') ++i;
    std::string name = raw.substr(start, i - start);
    size_t colon = name.find(':');
    return colon != std::string::npos ? name.substr(colon + 1) : name;
}

// Extracts name="value" (or name='value') from a tag's raw attribute
// text. Requires the match be preceded by whitespace (or be at the very
// start) so "begin" doesn't accidentally match inside e.g. "xml:begin".
static bool xml_get_attr(const std::string& tag_attrs, const std::string& name, std::string& out) {
    std::string needle = name + "=";
    size_t pos = 0;
    while ((pos = tag_attrs.find(needle, pos)) != std::string::npos) {
        if (pos == 0 || std::isspace(static_cast<unsigned char>(tag_attrs[pos - 1]))) break;
        pos += needle.size();
    }
    if (pos == std::string::npos) return false;
    size_t vstart = pos + needle.size();
    if (vstart >= tag_attrs.size()) return false;
    char quote = tag_attrs[vstart];
    if (quote != '"' && quote != '\'') return false;
    size_t vend = tag_attrs.find(quote, vstart + 1);
    if (vend == std::string::npos) return false;
    out = tag_attrs.substr(vstart + 1, vend - vstart - 1);
    return true;
}

struct TtmlWord { double begin_sec; std::string text; };
struct TtmlLine { double begin_sec; std::vector<TtmlWord> words; };

static std::vector<TtmlLine> parse_ttml(const std::string& ttml) {
    std::vector<TtmlLine> result;
    size_t i = 0, n = ttml.size();

    bool in_p = false;
    TtmlLine current_line{0.0, {}};

    bool in_span = false;
    int span_depth = 0;
    double span_begin = 0.0;
    std::string span_text;

    while (i < n) {
        size_t lt = ttml.find('<', i);
        if (lt == std::string::npos) {
            if (in_span) span_text += xml_unescape(ttml.substr(i));
            break;
        }
        if (lt > i && in_span) span_text += xml_unescape(ttml.substr(i, lt - i));

        size_t gt = ttml.find('>', lt);
        if (gt == std::string::npos) break; // malformed -- bail, keep whatever's parsed so far
        std::string tag_content = ttml.substr(lt + 1, gt - lt - 1);
        i = gt + 1;

        if (tag_content.empty() || tag_content[0] == '?' || tag_content[0] == '!') continue; // decl/comment/doctype

        bool is_close = tag_content[0] == '/';
        bool self_close = !tag_content.empty() && tag_content.back() == '/';
        std::string body = is_close ? tag_content.substr(1) : tag_content;
        if (self_close && !body.empty() && body.back() == '/') body.pop_back();
        std::string name = local_tag_name(body);

        if (!is_close) {
            if (name == "p") {
                std::string begin_attr;
                double sec;
                if (xml_get_attr(body, "begin", begin_attr) && parse_ttml_time(begin_attr, sec)) {
                    if (in_p && !current_line.words.empty()) result.push_back(current_line); // defensive: p's shouldn't nest
                    current_line = TtmlLine{sec, {}};
                    in_p = true;
                }
                if (self_close && in_p) { result.push_back(current_line); in_p = false; }
            } else if (name == "span") {
                if (in_span) {
                    ++span_depth; // nested span -- its text still folds into the outer captured word (matches itertext() semantics)
                } else if (in_p) {
                    std::string begin_attr;
                    double sec;
                    if (xml_get_attr(body, "begin", begin_attr) && parse_ttml_time(begin_attr, sec)) {
                        in_span = true;
                        span_depth = 1;
                        span_begin = sec;
                        span_text.clear();
                    }
                }
                if (self_close && in_span) {
                    if (--span_depth == 0) {
                        in_span = false;
                        if (!span_text.empty()) current_line.words.push_back({span_begin, span_text});
                    }
                }
            }
        } else {
            if (name == "p") {
                if (in_p) { result.push_back(current_line); in_p = false; }
            } else if (name == "span" && in_span) {
                if (--span_depth == 0) {
                    in_span = false;
                    if (!span_text.empty()) current_line.words.push_back({span_begin, span_text});
                }
            }
        }
    }
    if (in_p && !current_line.words.empty()) result.push_back(current_line); // trailing unclosed <p> -- be lenient

    return result;
}

static std::string lrc_timestamp(double sec, char open, char close) {
    long total_ms = static_cast<long>(std::llround(sec * 1000.0));
    if (total_ms < 0) total_ms = 0;
    long minutes = total_ms / 60000;
    long seconds = (total_ms % 60000) / 1000;
    long ms = total_ms % 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%c%02ld:%02ld.%03ld%c", open, minutes, seconds, ms, close);
    return buf;
}

// enhanced=true reproduces ttml_to_enhanced_lrc()'s output exactly
// ("[mm:ss.sss]<mm:ss.sss>word <mm:ss.sss>word ..."); enhanced=false
// reproduces ttml_to_lrc()'s plain line-synced output.
static std::string ttml_lines_to_lrc(const std::vector<TtmlLine>& lines, bool enhanced) {
    std::vector<std::string> out_lines;
    for (const auto& line : lines) {
        if (line.words.empty()) continue;
        std::string joined;
        for (const auto& w : line.words) {
            if (enhanced) joined += lrc_timestamp(w.begin_sec, '<', '>');
            joined += w.text;
        }
        std::string trimmed = trim_copy(joined);
        if (trimmed.empty()) continue;
        out_lines.push_back(lrc_timestamp(line.begin_sec, '[', ']') + trimmed);
    }
    std::string result;
    for (size_t i = 0; i < out_lines.size(); ++i) {
        if (i) result += "\n";
        result += out_lines[i];
    }
    return result;
}

// --- Better Lyrics / LRCLIB (native HTTP, no external process) ---------
// Ported from scripts/lrc.py's get_better_lyrics()/get_lrclib_lyrics()/
// get_lyrics() (now removed) -- same two APIs, same fallback order
// (Better Lyrics' word-level timing first, LRCLIB's line-synced-only
// second), just called directly via libcurl instead of shelling out to
// a Python script.

static bool try_better_lyrics(const std::string& song, const std::string& artist,
                               std::string& out_lrc, bool& out_enhanced, std::string& err) {
    std::string body;
    bool ok = http_get("https://lyrics-api.boidu.dev/getLyrics", {{"s", song}, {"a", artist}}, body, err);
    ConsoleLog::instance().log_command(
        "GET https://lyrics-api.boidu.dev/getLyrics?s=" + song + "&a=" + artist,
        ok ? ("received " + std::to_string(body.size()) + " bytes") : ("failed: " + err),
        ok ? 0 : 1);
    if (!ok) return false;
    std::string ttml;
    if (!json_get_string(body, "ttml", ttml) || ttml.empty()) {
        err = "Better Lyrics returned no TTML";
        return false;
    }
    std::vector<TtmlLine> lines = parse_ttml(ttml);
    std::string enhanced_lrc = ttml_lines_to_lrc(lines, /*enhanced=*/true);
    if (!enhanced_lrc.empty()) {
        out_lrc = enhanced_lrc;
        out_enhanced = true;
        return true;
    }
    std::string plain_lrc = ttml_lines_to_lrc(lines, /*enhanced=*/false);
    if (!plain_lrc.empty()) {
        out_lrc = plain_lrc;
        out_enhanced = false;
        return true;
    }
    err = "Better Lyrics TTML had no usable lines";
    return false;
}

// --- KuGou (word-level, via KRC) ----------------------------------------
// Ported from a community KuGou lyrics scraper: search, download the
// encrypted KRC blob, decrypt it (a well-documented technique published
// across several open-source lyrics tools -- "krc1" magic bytes, a fixed
// 16-byte XOR key, then zlib inflate), then transpile KRC's own
// word-timing format into the same enhanced-LRC text ttml_lines_to_lrc()
// already produces, so parse_lrc() doesn't need a third code path to
// understand it -- everything downstream of this function only ever
// sees "[mm:ss.mmm]<mm:ss.mmm>word..." either way.

static const uint8_t kKrcKey[16] = {
    0x40, 0x47, 0x61, 0x77, 0x5e, 0x32, 0x74, 0x47,
    0x51, 0x36, 0x31, 0x2d, 0xce, 0xd2, 0x6e, 0x69
};

static std::vector<uint8_t> base64_decode(const std::string& in) {
    static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static std::vector<int> table = [] {
        std::vector<int> t(256, -1);
        for (int i = 0; i < 64; ++i) t[static_cast<unsigned char>(chars[i])] = i;
        return t;
    }();
    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (table[c] == -1) continue;
        val = (val << 6) + table[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static bool zlib_inflate_to_string(const std::vector<uint8_t>& compressed, std::string& out) {
    z_stream strm{};
    strm.avail_in = static_cast<uInt>(compressed.size());
    strm.next_in = const_cast<Bytef*>(compressed.data());
    if (inflateInit(&strm) != Z_OK) return false;

    char buffer[4096];
    int ret;
    out.clear();
    do {
        strm.avail_out = sizeof(buffer);
        strm.next_out = reinterpret_cast<Bytef*>(buffer);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&strm);
            return false;
        }
        out.append(buffer, sizeof(buffer) - strm.avail_out);
    } while (strm.avail_out == 0 && ret != Z_STREAM_END);

    inflateEnd(&strm);
    return true;
}

static std::string decrypt_krc(const std::string& b64) {
    std::vector<uint8_t> data = base64_decode(b64);
    if (data.size() <= 4 || data[0] != 'k' || data[1] != 'r' || data[2] != 'c' || data[3] != '1') return "";
    std::vector<uint8_t> decrypted(data.size() - 4);
    for (size_t i = 4; i < data.size(); ++i) decrypted[i - 4] = data[i] ^ kKrcKey[(i - 4) % 16];
    std::string out;
    if (!zlib_inflate_to_string(decrypted, out)) return "";
    return out;
}

// One KRC line: "[line_start_ms,line_dur_ms]<word_offset_ms,word_dur_ms,flag>word<...>word..."
// -- word offsets are relative to the line start, not absolute.
static std::string krc_to_enhanced_lrc(const std::string& krc_plain) {
    static const std::regex line_re(R"(^\[([0-9]+),([0-9]+)\](.*)$)");
    static const std::regex word_re(R"(<([0-9]+),([0-9]+),[0-9]+>([^<]*))");

    std::istringstream iss(krc_plain);
    std::string line;
    std::vector<std::string> out_lines;

    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        std::smatch m;
        if (!std::regex_match(line, m, line_re)) continue;
        long long line_start_ms;
        try {
            line_start_ms = std::stoll(m[1].str());
        } catch (...) {
            continue;
        }
        std::string word_seq = m[3].str();

        std::string joined;
        auto wb = std::sregex_iterator(word_seq.begin(), word_seq.end(), word_re);
        auto we = std::sregex_iterator();
        for (auto it = wb; it != we; ++it) {
            std::smatch wm = *it;
            long long off, dur;
            try {
                off = std::stoll(wm[1].str());
                dur = std::stoll(wm[2].str());
            } catch (...) {
                continue;
            }
            (void)dur; // word end time isn't carried in the enhanced-LRC format we emit, only each word's start
            double word_start_sec = static_cast<double>(line_start_ms + off) / 1000.0;
            joined += lrc_timestamp(word_start_sec, '<', '>') + wm[3].str();
        }
        std::string trimmed = trim_copy(joined);
        if (trimmed.empty()) continue;
        out_lines.push_back(lrc_timestamp(static_cast<double>(line_start_ms) / 1000.0, '[', ']') + trimmed);
    }

    std::string result;
    for (size_t i = 0; i < out_lines.size(); ++i) {
        if (i) result += "\n";
        result += out_lines[i];
    }
    return result;
}

static bool try_kugou(const std::string& song, const std::string& artist, std::string& out_lrc, std::string& err) {
    std::string query = artist.empty() ? song : (artist + " " + song);

    std::string search_body;
    bool ok = http_get("http://lyrics.kugou.com/search",
                        {{"ver", "1"}, {"man", "yes"}, {"client", "pc"}, {"keyword", query}},
                        search_body, err);
    ConsoleLog::instance().log_command(
        "GET http://lyrics.kugou.com/search?keyword=" + query,
        ok ? ("received " + std::to_string(search_body.size()) + " bytes") : ("failed: " + err), ok ? 0 : 1);
    if (!ok) return false;

    tinyjson::Value root;
    if (!tinyjson::parse(search_body, root) || root.type != tinyjson::Type::Object) {
        err = "KuGou: malformed search response";
        return false;
    }
    const tinyjson::Value* candidates = root.find("candidates");
    if (!candidates || candidates->type != tinyjson::Type::Array || candidates->arr.empty()) {
        err = "KuGou: no candidate tracks found";
        return false;
    }

    // KuGou's own search already ranks by relevance to `query` -- just
    // take its top result rather than re-scoring candidates ourselves.
    const tinyjson::Value& selected = candidates->arr[0];
    std::string lyric_id = selected.find("id") ? selected.find("id")->as_string() : "";
    std::string access_key = selected.find("accesskey") ? selected.find("accesskey")->as_string() : "";
    if (lyric_id.empty()) {
        err = "KuGou: top candidate missing an id";
        return false;
    }

    std::string dl_body;
    ok = http_get("http://lyrics.kugou.com/download",
                   {{"ver", "1"}, {"client", "pc"}, {"id", lyric_id}, {"accesskey", access_key},
                    {"fmt", "krc"}, {"charset", "utf8"}},
                   dl_body, err);
    ConsoleLog::instance().log_command(
        "GET http://lyrics.kugou.com/download?id=" + lyric_id,
        ok ? ("received " + std::to_string(dl_body.size()) + " bytes") : ("failed: " + err), ok ? 0 : 1);
    if (!ok) return false;

    tinyjson::Value dl_root;
    if (!tinyjson::parse(dl_body, dl_root) || dl_root.type != tinyjson::Type::Object) {
        err = "KuGou: malformed download response";
        return false;
    }
    const tinyjson::Value* content = dl_root.find("content");
    if (!content || content->type != tinyjson::Type::String || content->str.empty()) {
        err = "KuGou: download response missing content";
        return false;
    }

    std::string krc_plain = decrypt_krc(content->str);
    if (krc_plain.empty()) {
        err = "KuGou: KRC decrypt/decompress failed";
        return false;
    }

    std::string enhanced = krc_to_enhanced_lrc(krc_plain);
    if (enhanced.empty()) {
        err = "KuGou: KRC had no usable lines";
        return false;
    }
    out_lrc = enhanced;
    return true;
}

static bool try_lrclib(const std::string& song, const std::string& artist, std::string& out_lrc, std::string& err) {
    std::string body;
    bool ok = http_get("https://lrclib.net/api/get", {{"track_name", song}, {"artist_name", artist}}, body, err);
    ConsoleLog::instance().log_command(
        "GET https://lrclib.net/api/get?track_name=" + song + "&artist_name=" + artist,
        ok ? ("received " + std::to_string(body.size()) + " bytes") : ("failed: " + err),
        ok ? 0 : 1);
    if (!ok) return false;
    std::string synced;
    if (!json_get_string(body, "syncedLyrics", synced) || synced.empty()) {
        err = "LRCLIB has no synchronized lyrics";
        return false;
    }
    out_lrc = synced;
    return true;
}

static bool fetch_from_apis(const std::string& song, const std::string& artist,
                             std::string& out_lrc, bool& out_enhanced, std::string& out_source) {
    std::string err;
    if (try_better_lyrics(song, artist, out_lrc, out_enhanced, err)) {
        out_source = "better-lyrics";
        return true;
    }
    // KuGou sits between the two: word-level (like Better Lyrics) when it
    // has data, tried before falling all the way back to LRCLIB's
    // line-synced-only.
    if (try_kugou(song, artist, out_lrc, err)) {
        out_enhanced = true;
        out_source = "kugou";
        return true;
    }
    if (try_lrclib(song, artist, out_lrc, err)) {
        out_enhanced = false;
        out_source = "lrclib";
        return true;
    }
    return false;
}

// Ported from fetch_lyrics.py's clean_youtube_title() (now removed).
static std::string clean_youtube_title(const std::string& text) {
    std::string s = text;
    size_t bar = s.find('|');
    if (bar != std::string::npos) s = s.substr(0, bar);
    static const std::regex paren_re(R"(\([^)]*\))");
    static const std::regex bracket_re(R"(\[[^\]]*\])");
    s = std::regex_replace(s, paren_re, "");
    s = std::regex_replace(s, bracket_re, "");
    return trim_copy(s);
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
                                  const fs::path& track_path, bool force_network) {
    LyricsResult result;

    // 1) Local sidecar file — no network at all if this hits.
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

    // 2) Better Lyrics (word-level, via TTML) -> LRCLIB (line-synced)
    //    fallback, called directly over HTTP via libcurl. No external
    //    process, no Python -- this used to shell out to a Python helper
    //    script (scripts/fetch_lyrics.py / scripts/lrc.py, now removed)
    //    that did the same two calls with the `requests` package.
    std::string clean_title = clean_youtube_title(title);
    std::string lrc, source;
    bool enhanced = false;
    bool ok = fetch_from_apis(clean_title, artist, lrc, enhanced, source);

    // Same swapped-title/artist retry the old script did: a YouTube
    // video title like "Artist - Title" with no separate artist field
    // often resolves better as (title=Title, artist=Artist).
    if (!ok && artist.empty()) {
        size_t dash = clean_title.find('-');
        if (dash != std::string::npos) {
            std::string part0 = trim_copy(clean_title.substr(0, dash));
            std::string part1 = trim_copy(clean_title.substr(dash + 1));
            if (!part0.empty() && !part1.empty()) {
                ok = fetch_from_apis(part1, part0, lrc, enhanced, source);
            }
        }
    }

    if (!ok) {
        result.status = LyricsStatus::NotFound;
        result.message = "no lyrics found for \"" + title + "\"";
        return result;
    }

    result.lines = parse_lrc(lrc, enhanced);
    result.status = LyricsStatus::Ok;
    result.source = source;
    result.raw_lrc = lrc;
    result.message = (enhanced ? "word-synced lyrics" : "line-synced lyrics") + std::string(" (") + result.source + ")";

    save_sidecar(track_path, lrc); // cache to disk for offline reuse next time
    return result;
}

} // namespace muisc
