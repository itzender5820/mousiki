// Transliteration: writes Japanese, Korean and Chinese text in Latin letters (romaji, romanised
// Hangul, pinyin) for titles, tags and lyrics.
#include "translit.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace muisc {

#include "pinyin_table.inc"
#include "script_tables.inc"

namespace {

std::vector<char32_t> decode(const std::string& s) {
    std::vector<char32_t> out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        int n = 1;
        if (c >= 0xF0 && i + 3 < s.size() + 0) { cp = c & 0x07; n = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 2; }
        if (n > 1) {
            if (i + static_cast<size_t>(n) > s.size()) { out.push_back(c); ++i; continue; }
            for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3F);
        }
        out.push_back(cp);
        i += static_cast<size_t>(n);
    }
    return out;
}

// Decomposed kana (ト + U+3099 for ド, フ + U+309A for プ): joined to the one character, else the romaji got a stray mark.
std::vector<char32_t> compose_marks(const std::vector<char32_t>& v) {
    std::vector<char32_t> o;
    o.reserve(v.size());
    for (char32_t c : v) {
        if ((c == 0x3099 || c == 0x309A || c == 0xFF9E || c == 0xFF9F) && !o.empty()) {
            const char32_t b = o.back();
            const bool kata = b >= 0x30A1 && b <= 0x30FA;
            const char32_t k = kata ? b - 0x60 : b;
            const char32_t off = kata ? 0x60 : 0;
            const bool dk = c == 0x3099 || c == 0xFF9E;
            char32_t r = 0;
            const bool h_row = k == 0x306F || k == 0x3072 || k == 0x3075 || k == 0x3078 || k == 0x307B;
            if (dk) {
                const bool plain = (k >= 0x304B && k <= 0x3061 && ((k - 0x304B) % 2 == 0)) || k == 0x3064 || k == 0x3066 || k == 0x3068 || h_row;
                if (plain) r = k + 1;
                else if (k == 0x3046) r = 0x3094;
            } else if (h_row) {
                r = k + 2;
            }
            if (r) o.back() = r + off;
            continue;   // (a mark that goes with nothing is dropped)
        }
        o.push_back(c);
    }
    return o;
}

void put(std::string& o, char32_t cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
}

bool is_kana(char32_t c) { return (c >= 0x3041 && c <= 0x3096) || (c >= 0x30A1 && c <= 0x30FA) || c == 0x30FC || (c >= 0x31F0 && c <= 0x31FF) || (c >= 0xFF66 && c <= 0xFF9D); }
bool is_hangul(char32_t c) { return c >= 0xAC00 && c <= 0xD7A3; }
bool is_hanzi(char32_t c) { return c >= 0x3400 && c <= 0x9FFF; }
bool is_latin_letter(char32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 0xC0 && c <= 0x24F); }

// ---- Japanese ---------------------------------------------------------------------------------------------------------------
// The romaji of a hiragana (index = code point - 0x3041, the small vowels first).
const char* const kHira[] = {
    "a", "a", "i", "i", "u", "u", "e", "e", "o", "o",            // ぁ あ ぃ い ぅ う ぇ え ぉ お
    "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",  // か .. ご
    "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo", // さ .. ぞ
    "ta", "da", "chi", "ji", "",                                  // た だ ち ぢ っ
    "tsu", "zu", "te", "de", "to", "do",                          // つ づ て で と ど
    "na", "ni", "nu", "ne", "no",                                 // な .. の
    "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",   // は .. ぽ
    "ma", "mi", "mu", "me", "mo",                                 // ま .. も
    "ya", "ya", "yu", "yu", "yo", "yo",                           // ゃ や ゅ ゆ ょ よ
    "ra", "ri", "ru", "re", "ro",                                 // ら .. ろ
    "wa", "wa", "wi", "we", "wo", "n", "vu",                      // ゎ わ ゐ ゑ を ん ゔ
};

struct Mora { std::string r; bool ok = false; };

// A kana (hiragana or katakana) as hiragana code point; 0 if it is none.
char32_t to_hira(char32_t c) {
    if (c >= 0x30A1 && c <= 0x30F6) return c - 0x60;
    if (c >= 0x3041 && c <= 0x3096) return c;
    if (c >= 0xFF66 && c <= 0xFF9D) {   // half-width katakana (no dakuten handling: rare)
        static const char32_t h[] = {0x3092, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3083, 0x3085, 0x3087, 0x3063, 0x30FC,
                                     0x3042, 0x3044, 0x3046, 0x3048, 0x304A, 0x304B, 0x304D, 0x304F, 0x3051, 0x3053,
                                     0x3055, 0x3057, 0x3059, 0x305B, 0x305D, 0x305F, 0x3061, 0x3064, 0x3066, 0x3068,
                                     0x306A, 0x306B, 0x306C, 0x306D, 0x306E, 0x306F, 0x3072, 0x3075, 0x3078, 0x307B,
                                     0x307E, 0x307F, 0x3080, 0x3081, 0x3082, 0x3084, 0x3086, 0x3088,
                                     0x3089, 0x308A, 0x308B, 0x308C, 0x308D, 0x308F, 0x3093};
        const size_t i = c - 0xFF66;
        return i < sizeof h / sizeof h[0] ? h[i] : 0;
    }
    return 0;
}

const char* hira_romaji(char32_t h) {
    if (h == 0x3095) return "ka";   // small ka / ke (ゕ ゖ) are not in the table
    if (h == 0x3096) return "ke";
    return (h >= 0x3041 && h <= 0x3094) ? kHira[h - 0x3041] : "";
}

bool is_vowel_char(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

void macron(std::string& o) {   // the last vowel of `o` gets a long mark
    if (o.empty()) return;
    const char v = o.back();
    const char* m = v == 'a' ? "\xC4\x81" : v == 'i' ? "\xC4\xAB" : v == 'u' ? "\xC5\xAB" : v == 'e' ? "\xC4\x93" : v == 'o' ? "\xC5\x8D" : nullptr;
    if (!m) { o += '-'; return; }
    o.pop_back();
    o += m;
}
bool ends_with_macron_or_vowel(const std::string& o) { return !o.empty() && is_vowel_char(o.back()); }

// One mora starting at v[i] (the kana as code points); `used` = how many code points it took; romaji empty = none.
std::string mora_at(const std::vector<char32_t>& v, size_t i, size_t& used) {
    used = 1;
    const char32_t h = to_hira(v[i]);
    if (!h) return "";
    std::string base = hira_romaji(h);
    if (h == 0x3063 || h == 0x3093 || h == 0x30FC) return base;
    const char32_t nx = i + 1 < v.size() ? to_hira(v[i + 1]) : 0;
    // small ゃ ゅ ょ after an i-column kana: kya, sha, ja ...
    if ((nx == 0x3083 || nx == 0x3085 || nx == 0x3087) && base.size() >= 2 && base.back() == 'i') {
        const char vowel = nx == 0x3083 ? 'a' : nx == 0x3085 ? 'u' : 'o';
        used = 2;
        if (base == "shi" || base == "chi" || base == "ji") return base.substr(0, base.size() - 1) + vowel;   // sha chu jo
        return base.substr(0, base.size() - 1) + "y" + vowel;                                              // kya nyu ryo
    }
    // small ぁ ぃ ぅ ぇ ぉ after ふ ゔ う て で し じ ち: fa, fi, va, wi, ti, she ...
    if (nx == 0x3041 || nx == 0x3043 || nx == 0x3045 || nx == 0x3047 || nx == 0x3049) {
        const char vowel = nx == 0x3041 ? 'a' : nx == 0x3043 ? 'i' : nx == 0x3045 ? 'u' : nx == 0x3047 ? 'e' : 'o';
        std::string pre;
        if (h == 0x3075) pre = "f";
        else if (h == 0x3094) pre = "v";
        else if (h == 0x3046 && vowel != 'u') pre = "w";
        else if (h == 0x3066 && vowel == 'i') pre = "t";
        else if (h == 0x3067 && vowel == 'i') pre = "d";
        else if ((h == 0x3057 || h == 0x3058 || h == 0x3061) && vowel == 'e') pre = h == 0x3057 ? "sh" : h == 0x3058 ? "j" : "ch";
        if (!pre.empty()) { used = 2; return pre + vowel; }
    }
    return base;
}

// ---- Korean -----------------------------------------------------------------------------------------------------------------
const char* const kInitial[19] = {"g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h"};
const char* const kMedial[21] = {"a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae", "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i"};
// the final consonant: its sound at the end of a word / before a consonant, and (compound finals) the part that stays when the rest
// moves on to a following vowel ("stay"), and what moves ("move")
struct Final { const char* end; const char* stay; const char* move; };
const Final kFinal[28] = {
    {"", "", ""}, {"k", "", "g"}, {"k", "", "kk"}, {"k", "k", "s"}, {"n", "", "n"}, {"n", "n", "j"}, {"n", "n", ""}, {"t", "", "d"},
    {"l", "", "r"}, {"k", "l", "g"}, {"m", "l", "m"}, {"l", "l", "b"}, {"l", "l", "s"}, {"l", "l", "t"}, {"p", "l", "p"}, {"l", "l", ""},
    {"m", "", "m"}, {"p", "", "b"}, {"p", "p", "s"}, {"t", "", "s"}, {"t", "", "ss"}, {"ng", "ng", ""}, {"t", "", "j"}, {"t", "", "ch"},
    {"k", "", "k"}, {"t", "", "t"}, {"p", "", "p"}, {"t", "", ""},
};

// ---- the switch -------------------------------------------------------------------------------------------------------------
std::atomic<int> g_default_hanzi{0};   // 0 leave, 1 read as Chinese (pinyin), 2 read as Japanese (the kanji reader is asked)
std::atomic<bool> g_macrons{false};
std::atomic<bool> g_wo{false};
std::mutex g_mu;
std::unordered_map<std::string, std::string> g_whole;       // a whole tag text -> what to show ("Anri 杏里" -> "Anri"), learned from the artist tags
std::unordered_set<std::string> g_reader_keys;               // the keys of g_whole that a kanji reader wrote (their long vowels are marked, see style_reader_text)
std::unordered_set<std::string> g_kanji_asked;               // texts that were queued for a kanji reader
std::unordered_set<std::string> g_kanji_done;                // texts a kanji reader answered or failed for
std::vector<std::string> g_kanji_pending;
std::unordered_set<std::string> g_kanji_pending_set;
std::string g_kanji_file;
std::string g_kanji_rules;                                   // the rules id written as the first line of that file (empty = none)
bool g_ja_kana = false;                                      // the last transliterate_impl() text has a run of kana (five or more): word spacing needs the reader (janome)
bool g_ja_kept = false;                                      // the last transliterate_impl() left kanji of a Japanese text as they are
unsigned g_cache_version = 0;                               // bumped by everything that can change an answer (the cache of transliterate())
std::unordered_map<std::string, std::string> g_readings;   // whole text -> Latin reading (built in, the user's file, learned)
std::unordered_map<std::string, Script> g_hints;            // whole text -> its language
std::unordered_set<std::string> g_tried;                    // names the MusicBrainz lookup found nothing for
std::unordered_set<std::string> g_pending_set;
std::vector<std::string> g_pending;                         // names waiting for the lookup
std::unordered_map<std::string, std::unordered_map<std::string, int>> g_votes;   // artist -> Latin candidate -> tracks
std::unordered_set<std::string> g_voted_paths;
std::unordered_map<std::string, std::unordered_set<std::string>> g_cand_artists;   // Latin candidate -> artists it was seen for (a folder shared by many artists names none of them)
std::string g_learned_file;
size_t g_max_key = 0;                                       // the longest key of g_readings, in code points

bool in_table(const char32_t* a, size_t n, char32_t c) { return std::binary_search(a, a + n, c); }
bool japanese_only_char(char32_t c) { return is_hanzi(c) && in_table(kJapaneseOnly, sizeof kJapaneseOnly / sizeof kJapaneseOnly[0], c); }
bool chinese_only_char(char32_t c) { return is_hanzi(c) && in_table(kChineseOnly, sizeof kChineseOnly / sizeof kChineseOnly[0], c); }

Script script_of_cps(const std::vector<char32_t>& v) {
    bool zh = false;
    for (char32_t c : v) {
        if (is_kana(c) || japanese_only_char(c)) return Script::Japanese;
        if (chinese_only_char(c)) zh = true;
    }
    return zh ? Script::Chinese : Script::Unknown;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

void add_reading_locked(const std::string& cjk, const std::string& latin) {
    if (cjk.empty() || latin.empty()) return;
    g_readings[cjk] = latin;
    ++g_cache_version;
    g_max_key = std::max(g_max_key, decode(cjk).size());
}

void append_learned_locked(const std::string& line) {
    if (g_learned_file.empty()) return;
    std::ofstream out(g_learned_file, std::ios::app | std::ios::binary);
    if (out) out << line << "\n";
}

// the built-in readings: well-known Japanese artists whose names are kanji (the reading cannot be worked out from the characters)
const char* const kBuiltinReadings[][2] = {
    {"米津玄師", "Yonezu Kenshi"}, {"椎名林檎", "Shiina Ringo"}, {"東京事変", "Tokyo Jihen"}, {"星野源", "Hoshino Gen"},
    {"山下達郎", "Yamashita Tatsuro"}, {"坂本龍一", "Sakamoto Ryuichi"}, {"久石譲", "Hisaishi Joe"}, {"福山雅治", "Fukuyama Masaharu"},
    {"玉置浩二", "Tamaki Koji"}, {"中島みゆき", "Nakajima Miyuki"}, {"松任谷由実", "Matsutoya Yumi"}, {"浜崎あゆみ", "Hamasaki Ayumi"},
    {"安室奈美恵", "Amuro Namie"}, {"中島美嘉", "Nakashima Mika"}, {"一青窈", "Hitoto Yo"}, {"絢香", "Ayaka"},
    {"槇原敬之", "Makihara Noriyuki"}, {"小田和正", "Oda Kazumasa"}, {"桑田佳祐", "Kuwata Keisuke"}, {"吉田拓郎", "Yoshida Takuro"},
    {"井上陽水", "Inoue Yosui"}, {"矢沢永吉", "Yazawa Eikichi"}, {"氷室京介", "Himuro Kyosuke"}, {"布袋寅泰", "Hotei Tomoyasu"},
    {"久保田利伸", "Kubota Toshinobu"}, {"大塚愛", "Otsuka Ai"}, {"平井堅", "Hirai Ken"}, {"藤井風", "Fujii Kaze"},
    {"優里", "Yuuri"}, {"嵐", "Arashi"}, {"菅田将暉", "Suda Masaki"}, {"澤野弘之", "Sawano Hiroyuki"}, {"梶浦由記", "Kajiura Yuki"},
    {"菅野よう子", "Kanno Yoko"}, {"植松伸夫", "Uematsu Nobuo"}, {"光田康典", "Mitsuda Yasunori"}, {"下村陽子", "Shimomura Yoko"},
    {"近藤浩治", "Kondo Koji"}, {"古代祐三", "Koshiro Yuzo"}, {"宇多田ヒカル", "Utada Hikaru"}, {"竹内まりや", "Takeuchi Mariya"},
    {"七尾旅人", "Nanao Tabito"}, {"長渕剛", "Nagabuchi Tsuyoshi"}, {"斉藤和義", "Saito Kazuyoshi"}, {"奥田民生", "Okuda Tamio"},
    {"忌野清志郎", "Imawano Kiyoshiro"}, {"尾崎豊", "Ozaki Yutaka"}, {"松田聖子", "Matsuda Seiko"}, {"山口百恵", "Yamaguchi Momoe"},
    {"美空ひばり", "Misora Hibari"}, {"サザンオールスターズ", "Southern All Stars"}, {"一青窕", "Hitoto Yo"},
};
bool g_builtin_done = false;

void ensure_builtin_locked() {
    if (g_builtin_done) return;
    g_builtin_done = true;
    for (const auto& e : kBuiltinReadings) add_reading_locked(e[0], e[1]);
}

// "Kenshi Yonezu - 米津玄師", "米津玄師 (Kenshi Yonezu)": the Latin and the Chinese-character part of a name written both ways.
bool latin_only_cp(char32_t c) { return c < 0x80 ? (std::isalnum(static_cast<int>(c)) || c == ' ' || c == '.' || c == '\'' || c == '&' || c == ',' || c == '-' || c == '!' ) : (c >= 0xC0 && c <= 0x24F); }
bool split_pair(const std::string& text, std::string& cjk, std::string& latin) {
    std::vector<std::string> segs;
    std::string cur;
    const std::vector<char32_t> v = decode(text);
    auto flush = [&]() { const std::string t = trimmed(cur); if (!t.empty()) segs.push_back(t); cur.clear(); };
    for (size_t i = 0; i < v.size(); ++i) {
        const char32_t c = v[i];
        const bool sep_pair = (c == ' ' && i + 2 < v.size() && (v[i + 1] == '-' || v[i + 1] == '/' || v[i + 1] == '|') && v[i + 2] == ' ');
        if (c == '(' || c == ')' || c == '[' || c == ']' || c == 0xFF08 || c == 0xFF09 || c == '|' || c == '/' || c == 0xFF0F || c == 0xFF5C) { flush(); continue; }
        if (sep_pair) { flush(); i += 2; continue; }
        put(cur, c);
    }
    flush();
    if (segs.size() != 2) return false;
    int latin_i = -1, cjk_i = -1;
    for (int k = 0; k < 2; ++k) {
        const std::vector<char32_t> sv = decode(segs[static_cast<size_t>(k)]);
        bool all_latin = !sv.empty(), has_cjk = false, any_latin = false;
        int letters = 0;
        for (char32_t c : sv) {
            if (!latin_only_cp(c)) all_latin = false;
            if (is_hanzi(c) || is_kana(c) || is_hangul(c)) has_cjk = true;
            if (latin_only_cp(c)) { any_latin = true; if (std::isalpha(static_cast<int>(c < 0x80 ? c : 'a'))) ++letters; }
        }
        (void)any_latin;
        if (all_latin && letters >= 2) latin_i = k;
        else if (has_cjk) { bool lat = false; for (char32_t c : sv) if (c < 0x80 && std::isalpha(static_cast<int>(c))) lat = true; if (!lat) cjk_i = k; }
    }
    if (latin_i < 0 || cjk_i < 0 || latin_i == cjk_i) return false;
    cjk = segs[static_cast<size_t>(cjk_i)];
    latin = segs[static_cast<size_t>(latin_i)];
    if (std::count(cjk.begin(), cjk.end(), ' ') >= 2) return false;   // a lyrics line with a Latin remark at its end ("パラッパ パラッパ パラッパ (wow-oh)"), not a name
    return true;
}

std::atomic<bool> g_on{false};

}  // namespace

bool translit_possible(const std::string& s) {
    for (unsigned char c : s) if ((c >= 0xE3 && c <= 0xED) || c == 0xEF) return true;
    return false;
}

static std::string transliterate_impl(const std::string& text);   // (g_mu is held)

namespace {
// "Kaneko Ayano カネコアヤノ", "HAKU ハク。 (Jap.)", "Anri 杏里": a Latin name written beside the same name in CJK characters (no brackets).
struct LatinCjk { std::string latin, pre, suf, cjk; bool has_kanji = false, has_kana = false, one_side = false; std::string kana_romaji; };

bool cjk_char(char32_t c) { return is_hanzi(c) || is_kana(c) || is_hangul(c); }
bool cjk_filler(char32_t c) { return c == ' ' || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF01 && c <= 0xFF0F) || c == 0xFF1F || c == 0x30FB; }

bool split_latin_cjk(const std::string& text, LatinCjk& o) {
    const std::vector<char32_t> v = compose_marks(decode(text));
    size_t f = v.size(), l = 0;
    for (size_t i = 0; i < v.size(); ++i) if (cjk_char(v[i])) { if (f == v.size()) f = i; l = i; }
    if (f == v.size()) return false;
    for (size_t i = f; i <= l; ++i) if (!cjk_char(v[i]) && !cjk_filler(v[i])) return false;   // Latin inside the CJK part: not this shape
    size_t end = l + 1;
    while (end < v.size() && (v[end] == 0x3002 || v[end] == 0xFF01 || v[end] == 0x3001 || v[end] == 0xFF1F)) ++end;   // ハク。
    std::string pre, suf, cjk, kana;
    for (size_t i = 0; i < f; ++i) put(pre, v[i]);
    for (size_t i = end; i < v.size(); ++i) put(suf, v[i]);
    for (size_t i = f; i < end; ++i) {
        put(cjk, v[i]);
        if (is_kana(v[i])) { put(kana, v[i]); o.has_kana = true; }
        if (is_hanzi(v[i]) || is_hangul(v[i])) o.has_kanji = true;
    }
    pre = trimmed(pre); suf = trimmed(suf);
    if (!pre.empty() && (pre.back() == '(' || pre.back() == '[')) return false;   // "Name (名前)": that is split_pair's shape
    if (!suf.empty() && (suf[0] == ')' || suf[0] == ']')) return false;
    if (pre.empty() && suf.empty()) return false;
    int letters = 0, words = 1;
    for (char32_t c : decode(pre + " " + suf)) { if (c < 0x80 && std::isalpha(static_cast<int>(c))) ++letters; if (c == ' ') ++words; if (cjk_char(c)) return false; }
    if (letters < 2 || words > 8) return false;
    o.pre = pre; o.suf = suf;
    o.latin = pre.empty() ? suf : suf.empty() ? pre : pre + " " + suf;
    o.cjk = trimmed(cjk);
    o.one_side = pre.empty() != suf.empty() && (pre.empty() ? suf : pre).find('(') == std::string::npos;
    o.kana_romaji = kana.empty() ? std::string() : transliterate_impl(kana);
    return true;
}

// Letters only, long vowels folded: "Kaneko Ayano" / "kanekoayano", "Ōtsuka" / "otsuka".
std::string compare_key(const std::string& s) {
    std::string o;
    for (char32_t c : decode(s)) {
        switch (c) {
            case 0x101: case 0xE0: case 0xE1: o += 'a'; break;
            case 0x113: case 0xE8: case 0xE9: o += 'e'; break;
            case 0x12B: case 0xEC: case 0xED: o += 'i'; break;
            case 0x14D: case 0xF2: case 0xF3: o += 'o'; break;
            case 0x16B: case 0xF9: case 0xFA: o += 'u'; break;
            default: if (c < 0x80 && std::isalpha(static_cast<int>(c))) o += static_cast<char>(std::tolower(static_cast<int>(c)));
        }
    }
    std::string r;
    for (char ch : o) { if (!r.empty() && r.back() == ch && (ch == 'o' || ch == 'u')) continue; r += ch; }   // oo uu = o u
    for (size_t p = 0; (p = r.find("ou", p)) != std::string::npos;) r.erase(p + 1, 1);                       // ou = o
    return r;
}

size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// The smallest edit distance of `needle` to any part of `hay`.
size_t edit_distance_inside(const std::string& needle, const std::string& hay) {
    std::vector<size_t> prev(hay.size() + 1, 0), cur(hay.size() + 1);
    for (size_t i = 1; i <= needle.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= hay.size(); ++j) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (needle[i - 1] == hay[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return *std::min_element(prev.begin(), prev.end());
}

// Does the kana part say what the Latin part says?  (the kana alone: the whole name, order of the words swapped allowed; with kanji: the kana
// that are there are found inside the Latin spelling)
bool latin_cjk_agree(const LatinCjk& p) {
    if (!p.has_kana) return false;
    const std::string r = compare_key(p.kana_romaji);
    if (r.size() < 3) return false;
    const std::string l = compare_key(p.latin);
    if (!p.has_kanji) {
        auto close = [&](const std::string& a) { return !a.empty() && edit_distance(a, r) * 10 <= std::max(a.size(), r.size()) * 3; };
        if (close(l) || close(compare_key(p.pre)) || close(compare_key(p.suf))) return true;
        std::vector<std::string> words;
        std::string cur;
        for (char ch : p.latin + " ") { if (ch == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); } else cur += ch; }
        if (words.size() == 2) return close(compare_key(words[1] + words[0]));   // "Ayano Kaneko" / カネコアヤノ
        return false;
    }
    return edit_distance_inside(r, l) * 3 <= r.size();
}

}  // namespace


// The screens ask for the same few hundred strings every frame: answers are kept until a reading / language hint changes.
static std::unordered_map<std::string, std::string> g_cache;
static unsigned g_cache_for = 0;
static int g_cache_hanzi = 0;
static int g_cache_style = 0;

// The kanji reader (scripts/kanji_reading.py) marks long vowels with letters of its own: a i u e o with a macron = the vowel doubled,
// o with a circumflex = o + u. Here they get the style of the settings: macrons (the circumflex one is an o macron as well) or
// the plain spelling (aa ii uu ee oo, ou).
static std::string style_reader_text(const std::string& in, bool macrons) {
    static const char* const kMarked[][2] = {{"\xC4\x81", "aa"}, {"\xC4\xAB", "ii"}, {"\xC5\xAB", "uu"}, {"\xC4\x93", "ee"}, {"\xC5\x8D", "oo"}, {"\xC3\xB4", "ou"}};
    std::string out;
    for (size_t i = 0; i < in.size();) {
        bool hit = false;
        if (static_cast<unsigned char>(in[i]) >= 0xC3) {
            for (const auto& m : kMarked)
                if (in.compare(i, 2, m[0]) == 0) {
                    out += macrons ? (std::string(m[0]) == "\xC3\xB4" ? std::string("\xC5\x8D") : std::string(m[0])) : std::string(m[1]);
                    i += 2;
                    hit = true;
                    break;
                }
        }
        if (!hit) out += in[i++];
    }
    return out;
}

std::string transliterate(const std::string& text) {
    if (!translit_possible(text)) return text;
    std::lock_guard<std::mutex> lock(g_mu);
    ensure_builtin_locked();
    const int zh = g_default_hanzi.load();
    const int style = (g_macrons.load() ? 1 : 0) | (g_wo.load() ? 2 : 0);
    if (g_cache_for != g_cache_version || g_cache_hanzi != zh || g_cache_style != style || g_cache.size() > 6000) { g_cache.clear(); g_cache_for = g_cache_version; g_cache_hanzi = zh; g_cache_style = style; }
    const auto hit = g_cache.find(text);
    if (hit != g_cache.end()) return hit->second;
    std::string r;
    {
        const auto w = g_whole.find(trimmed(text));
        LatinCjk pair;
        if (w != g_whole.end()) r = g_reader_keys.count(w->first) ? style_reader_text(w->second, g_macrons.load()) : w->second;
        else if (split_latin_cjk(text, pair) && latin_cjk_agree(pair)) r = pair.latin;   // (the pair is written both ways: the Latin one)
        else {
            r = transliterate_impl(text);
            if ((g_ja_kept || g_ja_kana) && text.size() <= 240 && g_kanji_pending.size() < 3000 && text.find_first_of("\n\t\r") == std::string::npos) {   // a kanji reader may know it
                const std::string key = trimmed(text);
                if (!g_kanji_done.count(key) && g_kanji_pending_set.insert(key).second) { g_kanji_pending.push_back(key); g_kanji_asked.insert(key); }
            }
        }
    }
    g_cache.emplace(text, r);
    return r;
}

static std::string transliterate_impl(const std::string& text) {
    g_ja_kept = g_ja_kana = false;   // (also for the early return below: the caller asks both after the call)
    {   // a name written both ways ("Kenshi Yonezu (米津玄師)"): its Latin part is all there is to show
        std::string cjk, latin;
        if (split_pair(text, cjk, latin)) return latin;
    }
    const std::vector<char32_t> v = compose_marks(decode(text));
    bool kana = false;
    for (char32_t c : v) if (is_kana(c)) { kana = true; break; }
    // what the Chinese characters of this text are: Japanese (they stay) or Chinese (pinyin)
    bool hanzi_as_pinyin = false;
    bool japanese_ctx = false;
    g_ja_kept = false;
    g_ja_kana = false;
    {
        Script sc = kana ? Script::Japanese : script_of_cps(v);
        if (sc == Script::Unknown) {
            const auto it = g_hints.find(trimmed(text));
            if (it != g_hints.end()) sc = it->second;
        }
        if (sc == Script::Unknown) {   // LatinHanziDefault: what Chinese characters without any hint are
            const int d = g_default_hanzi.load();
            if (d == 1) sc = Script::Chinese;
            else if (d == 2) sc = Script::Japanese;   // the characters stay, but the kanji reader (if installed) reads them
        }
        hanzi_as_pinyin = sc == Script::Chinese;
        japanese_ctx = sc == Script::Japanese;
    }

    if (kana) { size_t n = 0; for (char32_t c : v) if (is_kana(c)) ++n; g_ja_kana = n >= 5; }
    std::string o;
    o.reserve(text.size() * 2);
    bool geminate = false;            // a small tsu waits for the next sound
    bool last_cjk_word = false;       // the last thing written was a converted word (so a space may be needed before a Latin letter)
    bool last_kept_cjk = false;       // the last thing written was a Chinese character kept as it is (Japanese text)
    bool particle_break = false;      // の / を after a kanji was written: the next word starts after a space (日本 no shitipoppu)
    auto word_break = [&]() { if (!o.empty() && o.back() != ' ') o += ' '; };
    for (size_t i = 0; i < v.size(); ++i) {
        const char32_t c = v[i];
        if (particle_break) {
            particle_break = false;
            if (!o.empty() && o.back() != ' ' && (is_kana(c) || is_hanzi(c) || is_hangul(c) || is_latin_letter(c))) o += ' ';
        }
        // the reading dictionary: the longest known word starting here
        if ((is_hanzi(c) || is_kana(c)) && g_max_key > 0) {
            bool hit = false;
            for (size_t len = std::min(g_max_key, v.size() - i); len >= 1 && !hit; --len) {
                std::string key;
                for (size_t k = 0; k < len; ++k) put(key, v[i + k]);
                const auto it = g_readings.find(key);
                if (it == g_readings.end()) continue;
                if (!o.empty() && o.back() != ' ' && o.back() != '"' && o.back() != '(') o += ' ';
                o += it->second;
                i += len - 1;
                last_cjk_word = true;
                last_kept_cjk = false;
                geminate = false;
                hit = true;
            }
            if (hit) continue;
        }
        // full-width forms and CJK punctuation
        if (c >= 0xFF01 && c <= 0xFF5E) { put(o, c - 0xFEE0); last_cjk_word = false; last_kept_cjk = false; continue; }
        if (c == 0x3000) { o += ' '; last_cjk_word = false; last_kept_cjk = false; continue; }
        if (c == 0x3001 || c == 0xFF0C) { o += ','; last_cjk_word = false; continue; }
        if (c == 0x3002) { o += '.'; last_cjk_word = false; continue; }
        if (c == 0x300C || c == 0x300D || c == 0x300E || c == 0x300F) { o += '"'; last_cjk_word = false; continue; }
        if (c == 0x30FB) { o += ' '; last_cjk_word = false; continue; }
        if (c == 0x301C) { o += '~'; last_cjk_word = false; continue; }

        if (is_kana(c)) {
            if (last_kept_cjk) word_break();
            last_kept_cjk = false;
            const char32_t h = to_hira(c);
            if (c == 0x30FC) {
                if (!ends_with_macron_or_vowel(o)) o += '-';
                else if (g_macrons.load()) macron(o);
                else o += o.back();   // koohii
                last_cjk_word = true;
                continue;
            }
            if (h == 0x3063) { geminate = true; continue; }
            // A full-size つ / ツ where a small tsu belongs: lyrics sites and old spelling write いつて for いって (itte, not itsute).
            // Only before て / た (the verb forms: 行って, 分かった) and after a kana; かつて (formerly) stays katsute.
            if (h == 0x3064 && i > 0 && i + 1 < v.size() && is_kana(v[i - 1]) && v[i - 1] != 0x30FC && to_hira(v[i - 1]) != 0x3093 && to_hira(v[i - 1]) != 0x3063 &&
                (to_hira(v[i + 1]) == 0x3066 || to_hira(v[i + 1]) == 0x305F) && !(to_hira(v[i - 1]) == 0x304B && to_hira(v[i + 1]) == 0x3066 && (i < 2 || !is_kana(v[i - 2])))) {
                geminate = true;
                continue;
            }
            // The particle は is read wa: on its own (は いけない), after a particle at the end of a phrase (では、には、とは、へは、からは) or
            // before ない (ではない), and after a kanji (僕は). It stays ha inside words. The particle へ on its own is e.
            if ((c == 0x306F || c == 0x3078) && h == c) {
                auto in_word = [](char32_t x) { return is_kana(x) || is_hanzi(x) || is_hangul(x) || is_latin_letter(x) || (x >= '0' && x <= '9'); };
                const bool alone_before = i == 0 || !in_word(v[i - 1]);
                const bool alone_after = i + 1 >= v.size() || !in_word(v[i + 1]);
                bool is_particle = alone_before && alone_after;
                if (c == 0x306F && !is_particle && i > 0) {
                    const char32_t pv = v[i - 1];
                    const bool after_particle = pv == 0x3067 || pv == 0x306B || pv == 0x3068 || pv == 0x3078 || pv == 0x3089;   // で に と へ ら (から)
                    const bool before_nai = i + 2 < v.size() && v[i + 1] == 0x306A && (v[i + 2] == 0x3044 || v[i + 2] == 0x304B);   // ない / なか(った)
                    if (after_particle && (alone_after || before_nai)) is_particle = true;
                    else if (is_hanzi(pv)) is_particle = true;
                }
                if (is_particle) {
                    if (!o.empty() && o.back() != ' ' && is_hanzi(i > 0 ? v[i - 1] : 0)) o += ' ';
                    o += c == 0x306F ? "wa" : "e";
                    if (i > 0 && is_hanzi(v[i - 1]) && i + 1 < v.size()) particle_break = true;
                    last_cjk_word = true;
                    geminate = false;
                    continue;
                }
            }
            size_t used = 1;
            std::string r = mora_at(v, i, used);
            if (h == 0x3093) {   // ん: n, with an apostrophe before a vowel or y
                size_t nu = 1;
                const std::string nr = i + 1 < v.size() ? mora_at(v, i + 1, nu) : std::string();
                o += (!nr.empty() && (is_vowel_char(nr[0]) || nr[0] == 'y')) ? "n'" : "n";
                last_cjk_word = true;
                continue;
            }
            if (r.empty()) { last_cjk_word = true; continue; }
            if (geminate) {
                geminate = false;
                if (r.compare(0, 2, "ch") == 0) o += "t";
                else if (!is_vowel_char(r[0])) o += r[0];
            }
            // macron style: おう おお -> ō, うう -> ū (in the same word)
            if (g_macrons.load() && !o.empty() && ((h == 0x3046 && r == "u" && (o.back() == 'o' || o.back() == 'u')) || (h == 0x304A && r == "o" && o.back() == 'o'))) {
                macron(o);
                last_cjk_word = true;
                continue;
            }
            if (h == 0x3092 && !g_wo.load()) r = "o";   // the particle を
            o += r;
            if ((c == 0x306E || c == 0x3092) && i > 0 && is_hanzi(v[i - 1]) && i + 1 < v.size()) particle_break = true;
            i += used - 1;
            last_cjk_word = true;
            continue;
        }
        if (is_hangul(c)) {
            const unsigned s = static_cast<unsigned>(c - 0xAC00);
            const unsigned ini = s / (21 * 28), med = (s % (21 * 28)) / 28, fin = s % 28;
            if (last_kept_cjk) word_break();
            last_kept_cjk = false;
            o += kInitial[ini];
            o += kMedial[med];
            bool link = false;   // the final consonant moves to the next syllable when that one starts with the silent ieung
            if (fin && i + 1 < v.size() && is_hangul(v[i + 1])) link = ((v[i + 1] - 0xAC00) / (21 * 28)) == 11 && fin != 21;
            if (fin) {
                if (link) { o += kFinal[fin].stay; o += kFinal[fin].move; }
                else o += kFinal[fin].end;
            }
            last_cjk_word = true;
            continue;
        }
        if (is_hanzi(c)) {
            if (!hanzi_as_pinyin) {   // Japanese (or unknown) text: the reading needs a dictionary, so the character stays
                if (last_cjk_word) word_break();
                put(o, c);
                if (japanese_ctx) g_ja_kept = true;
                last_kept_cjk = true;
                last_cjk_word = false;
                continue;
            }
            const int idx = static_cast<int>(c) - kPinyinFirst;
            const char* py = (idx >= 0 && idx <= kPinyinLast - kPinyinFirst) ? kPinyinSyllables[kPinyinIndex[idx]] : "";
            if (!*py) { if (last_cjk_word) word_break(); put(o, c); last_cjk_word = false; continue; }
            if (!o.empty() && o.back() != ' ' && o.back() != '"' && o.back() != '(') o += ' ';
            o += py;
            last_cjk_word = true;
            continue;
        }
        // anything else
        if (last_cjk_word && is_latin_letter(c)) word_break();
        last_cjk_word = false;
        last_kept_cjk = false;
        geminate = false;
        put(o, c);
    }
    return o;
}

Script script_of(const std::string& s) {
    if (!translit_possible(s)) return Script::Unknown;
    return script_of_cps(decode(s));
}

void translit_learn(const std::string& title, const std::string& artist, const std::string& album, const std::string& path) {
    if (!translit_possible(title) && !translit_possible(artist) && !translit_possible(album)) return;
    std::lock_guard<std::mutex> lock(g_mu);
    ensure_builtin_locked();
    const std::string fields[3] = {artist, album, title};
    // names written both ways teach their reading
    for (const std::string& f : fields) {
        std::string cjk, latin;
        if (translit_possible(f) && split_pair(f, cjk, latin) && !g_readings.count(cjk)) add_reading_locked(cjk, latin);
    }
    if (translit_possible(artist)) {   // "Kaneko Ayano カネコアヤノ", "Anri 杏里": an artist tag that holds the Latin name and the original one
        LatinCjk pair;
        const std::string name = trimmed(artist);
        if (split_latin_cjk(name, pair) && !g_whole.count(name) && pair.latin.size() <= 60) {
            g_whole[name] = pair.latin;
            ++g_cache_version;
            const size_t cps = decode(pair.cjk).size();
            if (pair.one_side && ((pair.has_kanji && cps >= 2) || cps >= 4) && !g_readings.count(pair.cjk)) add_reading_locked(pair.cjk, pair.latin);
        }
    }
    if (!artist.empty() && g_pending.size() < 4000) {   // an artist name with Chinese characters and no reading yet: a candidate for the MusicBrainz lookup
        const std::string name = trimmed(artist);
        const std::vector<char32_t> av = decode(name);
        bool hanzi = false;
        for (char32_t c : av) if (is_hanzi(c)) { hanzi = true; break; }
        if (hanzi && av.size() <= 40 && !g_readings.count(name) && !g_whole.count(name) && !g_tried.count(name) && g_pending_set.insert(name).second) g_pending.push_back(name);
    }
    // an artist written in Chinese characters / kana: what the folders and the file name call it in Latin letters (a vote per track) ...
    if (translit_possible(artist) && !path.empty() && !g_readings.count(trimmed(artist)) && g_voted_paths.insert(path).second) {
        const std::string name = trimmed(artist);
        std::vector<std::string> cands;
        {   // the folder names (artist folder, album folder) and the artist part of "Artist - Title"
            size_t e = path.find_last_of("/\\");
            const std::string dir = e == std::string::npos ? std::string() : path.substr(0, e);
            const std::string file = e == std::string::npos ? path : path.substr(e + 1);
            const size_t e2 = dir.find_last_of("/\\");
            const std::string d1 = e2 == std::string::npos ? dir : dir.substr(e2 + 1);
            const size_t e3 = e2 == std::string::npos ? std::string::npos : dir.substr(0, e2).find_last_of("/\\");
            const std::string d2 = e2 == std::string::npos ? std::string() : (e3 == std::string::npos ? dir.substr(0, e2) : dir.substr(e3 + 1, e2 - e3 - 1));
            cands.push_back(trimmed(d1));
            cands.push_back(trimmed(d2));
            const size_t dash = file.find(" - ");
            if (dash != std::string::npos) cands.push_back(trimmed(file.substr(0, dash)));
        }
        auto same = [](std::string a, std::string b) { for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return a == b; };
        for (std::string c : cands) {
            for (char& ch : c) if (ch == '_') ch = ' ';
            if (c.size() < 3 || c.size() > 60 || same(c, title) || same(c, album)) continue;
            const std::vector<char32_t> cv = decode(c);
            bool ok = true;
            int letters = 0;
            for (char32_t ch : cv) { if (!latin_only_cp(ch)) { ok = false; break; } if (ch < 0x80 && std::isalpha(static_cast<int>(ch))) ++letters; }
            if (!ok || letters < 3) continue;
            ++g_votes[name][c];
            g_cand_artists[c].insert(name);
        }
        const auto vi = g_votes.find(name);
        if (vi != g_votes.end()) {
            std::string top;
            int top_n = 0, second_n = 0;
            for (const auto& [cand, n] : vi->second) {
                if (n > top_n) { second_n = top_n; top_n = n; top = cand; }
                else if (n > second_n) second_n = n;
            }
            if (top_n >= 3 && top_n >= 2 * second_n && g_cand_artists[top].size() == 1) {   // most tracks of the artist agree on one name
                add_reading_locked(name, top);
                append_learned_locked(name + " = " + top);
                g_votes.erase(name);
            }
        }
    }
    // the language of the track: the evidence of any of its tags, else what is known of the same texts from other tracks
    Script sc = Script::Unknown;
    for (const std::string& f : fields) {
        if (!translit_possible(f)) continue;
        const Script one = script_of_cps(decode(f));
        if (one == Script::Japanese) { sc = Script::Japanese; break; }
        if (one == Script::Chinese) sc = Script::Chinese;
    }
    if (sc == Script::Unknown) {
        for (const std::string& f : fields) {
            const auto it = g_hints.find(trimmed(f));
            if (it != g_hints.end()) { sc = it->second; break; }
        }
    }
    if (sc == Script::Unknown) return;
    for (const std::string& f : fields) {
        if (!translit_possible(f)) continue;
        const std::vector<char32_t> v = decode(f);
        bool hanzi = false;
        for (char32_t c : v) if (is_hanzi(c)) { hanzi = true; break; }
        if (hanzi && g_hints.emplace(trimmed(f), sc).second) ++g_cache_version;   // (the first answer stays)
    }
}

void translit_load_readings(const std::string& path) {
    std::ifstream in(path);
    if (!in) return;
    std::lock_guard<std::mutex> lock(g_mu);
    ensure_builtin_locked();
    std::string line;
    while (std::getline(in, line)) {
        line = trimmed(line);
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) line = trimmed(line.substr(3));
        if (line.empty() || line[0] == '#') continue;
        if (line.compare(0, 5, "none:") == 0) { g_tried.insert(trimmed(line.substr(5))); continue; }
        if (line.compare(0, 8, "chinese:") == 0) { g_hints[trimmed(line.substr(8))] = Script::Chinese; ++g_cache_version; continue; }
        if (line.compare(0, 9, "japanese:") == 0) { g_hints[trimmed(line.substr(9))] = Script::Japanese; ++g_cache_version; continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        add_reading_locked(trimmed(line.substr(0, eq)), trimmed(line.substr(eq + 1)));
    }
}

void translit_set_learned_file(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_learned_file = path;
    }
    translit_load_readings(path);   // (the same format as the user's file)
}

std::vector<std::string> translit_take_pending(size_t max) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<std::string> out;
    while (!g_pending.empty() && out.size() < max) {
        std::string n = g_pending.front();
        g_pending.erase(g_pending.begin());
        g_pending_set.erase(n);
        if (g_readings.count(n) || g_tried.count(n)) continue;   // (learned in the meantime)
        out.push_back(std::move(n));
    }
    return out;
}

void translit_lookup_found(const std::string& cjk, const std::string& latin) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_readings.count(cjk)) return;
    add_reading_locked(cjk, latin);
    append_learned_locked(cjk + " = " + latin);
}

void translit_lookup_none(const std::string& cjk) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_tried.insert(cjk).second) return;
    append_learned_locked("none: " + cjk);
}

void translit_lookup_retry(const std::string& cjk) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_readings.count(cjk) || g_tried.count(cjk)) return;
    if (g_pending_set.insert(cjk).second) g_pending.push_back(cjk);
}

void translit_set_default_chinese(bool chinese) { g_default_hanzi.store(chinese ? 1 : 0); }
void translit_set_default_hanzi(int mode) { g_default_hanzi.store(mode == 1 || mode == 2 ? mode : 0); }
void translit_set_style(bool macrons, bool wo) { g_macrons.store(macrons); g_wo.store(wo); }

std::string with_latin_spelling(const std::string& s) {
    if (!translit_possible(s)) return s;
    const std::string l = transliterate(s);
    return l == s ? s : s + " " + l;
}

bool translit_enabled() { return g_on.load(std::memory_order_relaxed); }
void set_translit_enabled(bool on) { g_on.store(on, std::memory_order_relaxed); }
std::string shown_text(const std::string& s) { return translit_enabled() ? transliterate(s) : s; }

}  // namespace muisc

namespace muisc {

void translit_set_kanji_cache_file(const std::string& path, const std::string& rules_id) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_kanji_file = path;
    g_kanji_rules = rules_id;
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (in.is_open() && !rules_id.empty()) {
        // the first line says which rules made the readings below it
        const std::streampos start = in.tellg();
        std::string first;
        const bool has_content = static_cast<bool>(std::getline(in, first));
        while (!first.empty() && (first.back() == '\r' || first.back() == '\n')) first.pop_back();
        if (has_content && first != "#rules " + rules_id) {   // other rules, or an older file without the line: start again
            in.close();
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out) out << "#rules " << rules_id << "\n";
            ++g_cache_version;
            return;
        }
        in.clear();
        in.seekg(start);
    }
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.compare(0, 7, "#rules ") == 0) continue;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        const std::string text = line.substr(0, tab), reading = line.substr(tab + 1);
        g_kanji_done.insert(text);
        if (!reading.empty() && g_whole.emplace(text, reading).second) g_reader_keys.insert(text);
    }
    ++g_cache_version;
}

std::vector<std::string> translit_take_kanji_pending(size_t max) {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<std::string> out;
    while (!g_kanji_pending.empty() && out.size() < max) {
        std::string n = std::move(g_kanji_pending.front());
        g_kanji_pending.erase(g_kanji_pending.begin());
        g_kanji_pending_set.erase(n);
        if (g_kanji_done.count(n) || g_whole.count(n) || g_readings.count(n)) continue;
        out.push_back(std::move(n));
    }
    return out;
}

void translit_kanji_found(const std::string& text, const std::string& reading) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_kanji_done.insert(text);
    if (reading.empty() || reading == text || reading.find_first_of("\n\t") != std::string::npos) return;
    if (g_whole.emplace(text, reading).second) { g_reader_keys.insert(text); ++g_cache_version; }
    if (!g_kanji_file.empty()) {
        // (the size is asked before opening: on Windows tellp() of a file opened for appending says 0 whatever it holds)
        bool fresh = true;
        {
            std::ifstream probe(g_kanji_file, std::ios::binary | std::ios::ate);
            fresh = !probe.is_open() || probe.tellg() <= std::streampos(0);
        }
        std::ofstream out(g_kanji_file, std::ios::app | std::ios::binary);
        if (out) {
            if (!g_kanji_rules.empty() && fresh) out << "#rules " << g_kanji_rules << "\n";   // a new file starts with its rules
            out << text << "\t" << reading << "\n";
        }
    }
}

void translit_kanji_retry(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_kanji_done.count(text) || g_whole.count(text)) return;
    if (g_kanji_pending_set.insert(text).second) g_kanji_pending.push_back(text);
}

size_t translit_kanji_waiting(const std::vector<std::string>& texts) {
    size_t n = 0;
    for (const std::string& t : texts) {
        if (t.empty() || !translit_possible(t)) continue;
        transliterate(t);   // (queues what the reader has not seen yet)
        std::lock_guard<std::mutex> lock(g_mu);
        const std::string key = trimmed(t);
        if (g_kanji_asked.count(key) && !g_kanji_done.count(key) && !g_whole.count(key)) ++n;
    }
    return n;
}

void translit_kanji_failed(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_kanji_done.insert(text);
}

}  // namespace muisc
