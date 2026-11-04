#include "playlist_match.h"

#include "translit.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>

namespace muisc {
namespace {

using U32 = std::u32string;

// --- text normalisation ------------------------------------------------------------------------------------------------------------

U32 decode(const std::string& s) {
    U32 o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = c;
        int n = 1;
        if (c >= 0xF0) { cp = c & 0x07; n = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; n = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; n = 2; }
        if (n > 1) {
            if (i + n > s.size()) { ++i; continue; }
            bool ok = true;
            for (int k = 1; k < n; ++k) {
                const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (!ok) { ++i; continue; }
        } else if (c >= 0x80) { ++i; continue; }   // a stray continuation byte
        o.push_back(cp);
        i += n;
    }
    return o;
}

// Letters without accents, lower case; 0 = not a letter or digit (a separator).
char32_t fold(char32_t c) {
    if (c < 0x80) {
        if (c >= 'A' && c <= 'Z') return c + 32;
        return ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : 0;
    }
    if (c < 0xC0) return 0;
    if (c == 0xD7 || c == 0xF7) return 0;
    if (c <= 0xFF) {
        static const char* map =   // C0..FF
            "aaaaaaaceeeeiiii" "dnooooo\0ouuuuyts" "aaaaaaaceeeeiiii" "dnooooo\0ouuuuyty";
        const char m = map[c - 0xC0];
        return m ? static_cast<char32_t>(m) : 0;
    }
    if (c <= 0x17F) {   // Latin Extended-A (ā ē ī ō ū č š ž ł ő ...)
        struct R { char32_t lo, hi; char r; };
        static const R rs[] = {{0x100,0x105,'a'},{0x106,0x10D,'c'},{0x10E,0x111,'d'},{0x112,0x11B,'e'},{0x11C,0x123,'g'},{0x124,0x127,'h'},
                               {0x128,0x131,'i'},{0x134,0x135,'j'},{0x136,0x138,'k'},{0x139,0x142,'l'},{0x143,0x14B,'n'},{0x14C,0x153,'o'},
                               {0x154,0x159,'r'},{0x15A,0x161,'s'},{0x162,0x167,'t'},{0x168,0x173,'u'},{0x174,0x175,'w'},{0x176,0x178,'y'},
                               {0x179,0x17E,'z'}};
        for (const R& r : rs) if (c >= r.lo && c <= r.hi) return static_cast<char32_t>(r.r);
        return 0;
    }
    if (c >= 0x410 && c <= 0x42F) return c + 32;      // Cyrillic capitals
    if (c == 0x401) return 0x451;
    if (c >= 0x2000 && c <= 0x206F) return 0;          // general punctuation
    if (c >= 0x3000 && c <= 0x3004) return 0;
    if (c >= 0x3008 && c <= 0x3020) return 0;
    if (c >= 0xFF01 && c <= 0xFF0F) return 0;
    if (c >= 0xFF1A && c <= 0xFF20) return 0;
    if (c >= 0xFF3B && c <= 0xFF40) return 0;
    if (c >= 0xFF5B && c <= 0xFF65) return 0;
    if (c >= 0xFF21 && c <= 0xFF3A) return c - 0xFF21 + 'a';   // full-width Latin
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 0xFF41 + 'a';
    if (c >= 0xFF10 && c <= 0xFF19) return c - 0xFF10 + '0';
    return c;
}

bool opens(char32_t c) { return c == '(' || c == '[' || c == '{' || c == 0x3010 || c == 0x3014 || c == 0xFF08 || c == 0xFF3B; }
bool closes(char32_t c) { return c == ')' || c == ']' || c == '}' || c == 0x3011 || c == 0x3015 || c == 0xFF09 || c == 0xFF3D; }

struct Key {
    U32 flat;       // letters and digits, folded, no separators
    U32 sorted;     // the words sorted, then joined (artists: "Kenshi Yonezu" = "Yonezu Kenshi"); empty when not wanted
    uint64_t bloom = 0;
};

uint64_t bit_of(char32_t c) { return uint64_t(1) << ((c ^ (c >> 6)) & 63); }

Key make_key(const std::string& utf8, bool with_sorted) {
    const U32 cps = decode(utf8);
    auto build = [&](bool strip_brackets) {
        std::vector<U32> words(1);
        int depth = 0;
        for (char32_t c : cps) {
            if (strip_brackets) {
                if (opens(c)) { ++depth; continue; }
                if (closes(c)) { if (depth > 0) --depth; continue; }
                if (depth > 0) continue;
            }
            const char32_t f = fold(c);
            if (f) words.back().push_back(f);
            else if (!words.back().empty()) words.emplace_back();
        }
        if (words.back().empty()) words.pop_back();
        return words;
    };
    std::vector<U32> words = build(true);
    if (words.empty()) words = build(false);   // "(Intro)" alone is a title
    Key k;
    for (const U32& w : words) k.flat += w;
    if (with_sorted) {
        std::sort(words.begin(), words.end());
        for (const U32& w : words) k.sorted += w;
    }
    for (char32_t c : k.flat) k.bloom |= bit_of(c);
    return k;
}

// --- similarity ---------------------------------------------------------------------------------------------------------------------

int edit_distance(const U32& a, const U32& b, int limit) {
    const int la = static_cast<int>(a.size()), lb = static_cast<int>(b.size());
    std::vector<int> prev(lb + 1), cur(lb + 1);
    for (int j = 0; j <= lb; ++j) prev[j] = j;
    for (int i = 1; i <= la; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        for (int j = 1; j <= lb; ++j) {
            const int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[j] = std::min({sub, prev[j] + 1, cur[j - 1] + 1});
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > limit) return limit + 1;
        std::swap(prev, cur);
    }
    return prev[lb];
}

// 0 when below `thr`.
double sim_str(const U32& a, const U32& b, double thr) {
    if (a.empty() || b.empty()) return 0.0;
    const int la = static_cast<int>(a.size()), lb = static_cast<int>(b.size()), mx = std::max(la, lb);
    const int allowed = static_cast<int>(mx * (1.0 - thr) + 1e-9);
    if (std::abs(la - lb) > allowed) return 0.0;
    const int d = edit_distance(a, b, allowed);
    if (d > allowed) return 0.0;
    return 1.0 - static_cast<double>(d) / mx;
}

int popcount64(uint64_t x) { int n = 0; while (x) { x &= x - 1; ++n; } return n; }

double sim_key(const Key& a, const Key& b, double thr, bool use_sorted) {
    if (a.flat.empty() || b.flat.empty()) return 0.0;
    const int mx = static_cast<int>(std::max(a.flat.size(), b.flat.size()));
    const int allowed = static_cast<int>(mx * (1.0 - thr) + 1e-9);
    // Each distinct character that one side has and the other lacks costs at least one edit.
    if (popcount64(a.bloom & ~b.bloom) > allowed || popcount64(b.bloom & ~a.bloom) > allowed) return 0.0;
    double s = sim_str(a.flat, b.flat, thr);
    if (use_sorted && !a.sorted.empty() && !b.sorted.empty() && (a.sorted != a.flat || b.sorted != b.flat))
        s = std::max(s, sim_str(a.sorted, b.sorted, thr));
    return s;
}

// A text as it is and, when it has Japanese / Korean / Chinese characters, as Latin letters.
std::vector<Key> keys_of(const std::string& text, bool with_sorted) {
    std::vector<Key> v;
    if (text.empty()) return v;
    Key k = make_key(text, with_sorted);
    if (k.flat.empty()) return v;
    v.push_back(std::move(k));
    if (translit_possible(text)) {
        const std::string t = transliterate(text);
        if (t != text) {
            Key tk = make_key(t, with_sorted);
            if (!tk.flat.empty() && tk.flat != v[0].flat) v.push_back(std::move(tk));
        }
    }
    return v;
}

void add_keys(std::vector<Key>& to, const std::string& text, bool with_sorted) {
    for (Key& k : keys_of(text, with_sorted)) {
        bool dup = false;
        for (const Key& o : to) if (o.flat == k.flat) { dup = true; break; }
        if (!dup) to.push_back(std::move(k));
    }
}

double best_sim(const std::vector<Key>& a, const std::vector<Key>& b, double thr, bool use_sorted) {
    double best = 0.0;
    for (const Key& x : a) for (const Key& y : b) best = std::max(best, sim_key(x, y, thr, use_sorted));
    return best;
}

// --- file names ---------------------------------------------------------------------------------------------------------------------

std::string file_stem(const std::string& path) {
    size_t cut = path.find_last_of("/\\");
    std::string name = cut == std::string::npos ? path : path.substr(cut + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0 && name.size() - dot >= 3 && name.size() - dot <= 6) {   // ".mp3", ".flac", ".m4a" (not "Dr. Dre")
        bool ext = true;
        for (size_t i = dot + 1; i < name.size(); ++i) if (!std::isalnum(static_cast<unsigned char>(name[i]))) ext = false;
        if (ext) name.erase(dot);
    }
    return name;
}

std::string folder_name(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    if (cut == std::string::npos || cut == 0) return std::string();
    const size_t cut2 = path.find_last_of("/\\", cut - 1);
    return path.substr(cut2 == std::string::npos ? 0 : cut2 + 1, cut - (cut2 == std::string::npos ? 0 : cut2 + 1));
}

// The folder above the folder ("Artist" in Artist/Album/Song.mp3).
std::string grandparent_name(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    if (cut == std::string::npos || cut == 0) return std::string();
    return folder_name(path.substr(0, cut));
}

// "01 - ", "1-02. ", "07_" at the start.
std::string strip_track_number(std::string s) {
    for (int round = 0; round < 2; ++round) {
        size_t i = 0;
        while (i < s.size() && i < 3 && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        if (i == 0 || i >= s.size()) return s;
        size_t j = i;
        while (j < s.size() && (s[j] == ' ' || s[j] == '.' || s[j] == '_' || s[j] == '-')) ++j;
        if (j == i || j >= s.size()) return s;
        s.erase(0, j);
    }
    return s;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
}

struct Split { std::string artist, title; };

// "Artist - Title" (also "Artist - Album - Title": the first part, the last part).
Split split_stem(std::string stem) {
    stem = strip_track_number(stem);
    replace_all(stem, " \xE2\x80\x93 ", " - ");   // en dash
    replace_all(stem, " \xE2\x80\x94 ", " - ");   // em dash
    Split s;
    const size_t first = stem.find(" - ");
    if (first == std::string::npos) { s.title = stem; return s; }
    const size_t last = stem.rfind(" - ");
    s.artist = stem.substr(0, first);
    s.title = stem.substr(last + 3);
    return s;
}

}  // namespace

// --- the matcher ---------------------------------------------------------------------------------------------------------------------

struct PlaylistMatcher::Impl {
    struct Lib {
        std::vector<Key> titles;
        std::vector<Key> artists;
        std::string folder, tag_artist;
    };
    const std::vector<MatchTrack>* tracks = nullptr;
    std::vector<Lib> lib;
    std::unordered_map<U32, std::vector<int>> by_name;   // file name (and its Latin spelling) -> tracks
};

PlaylistMatcher::PlaylistMatcher(const std::vector<MatchTrack>& library) : d_(new Impl) {
    d_->tracks = &library;
    d_->lib.resize(library.size());
    for (size_t i = 0; i < library.size(); ++i) {
        const MatchTrack& t = library[i];
        const std::string stem = t.stem.empty() ? file_stem(t.path) : t.stem;
        const Split sp = split_stem(stem);
        Impl::Lib& L = d_->lib[i];
        add_keys(L.titles, t.tag_title, false);
        add_keys(L.titles, sp.title, false);
        add_keys(L.artists, t.tag_artist, true);
        add_keys(L.artists, sp.artist, true);
        add_keys(L.artists, t.folder, true);
        add_keys(L.artists, grandparent_name(t.path), true);
        L.folder = t.folder;
        L.tag_artist = t.tag_artist;
        for (const Key& k : keys_of(strip_track_number(stem), false)) {
            auto& v = d_->by_name[k.flat];
            if (v.empty() || v.back() != static_cast<int>(i)) v.push_back(static_cast<int>(i));
        }
    }
}

PlaylistMatcher::~PlaylistMatcher() { delete d_; }

MatchResult PlaylistMatcher::find(const MatchEntry& e) const {
    MatchResult none;
    const std::string stem = file_stem(e.path);
    const Split sp = split_stem(stem);
    const std::string folder = folder_name(e.path);

    std::vector<Key> e_artists;   // all places an artist can come from
    add_keys(e_artists, e.extinf_artist, true);
    add_keys(e_artists, sp.artist, true);
    const bool real_artist = !e_artists.empty();
    std::vector<Key> real_artists = e_artists;
    add_keys(e_artists, folder, true);
    add_keys(e_artists, grandparent_name(e.path), true);

    // 1. the file name
    {
        std::vector<int> cands;
        for (const Key& k : keys_of(strip_track_number(stem), false)) {
            auto it = d_->by_name.find(k.flat);
            if (it == d_->by_name.end()) continue;
            for (int i : it->second) if (std::find(cands.begin(), cands.end(), i) == cands.end()) cands.push_back(i);
        }
        int best = -1, best_pts = -1;
        std::vector<Key> fkeys = keys_of(folder, false);
        for (int i : cands) {
            const Impl::Lib& L = d_->lib[i];
            // The same name but another artist ("Intro", "Track 1"): not this one.
            if (!e.extinf_artist.empty() && !L.tag_artist.empty() &&
                best_sim(keys_of(e.extinf_artist, true), keys_of(L.tag_artist, true), 0.0, true) < 0.4) continue;
            int pts = 0;
            if (!fkeys.empty() && best_sim(fkeys, keys_of(L.folder, false), kMatchThreshold, false) >= 1.0) pts += 2;
            else if (real_artist && best_sim(real_artists, L.artists, kMatchThreshold, true) > 0.0) pts += 1;
            if (pts > best_pts) { best_pts = pts; best = i; }
        }
        if (best >= 0) { none.kind = MatchKind::FileName; none.index = best; none.score = 1.0; return none; }
    }

    // 2. title and artist, fuzzy
    std::vector<Key> e_titles;
    add_keys(e_titles, e.extinf_title, false);
    add_keys(e_titles, sp.title, false);
    if (e_titles.empty()) return none;

    int best = -1;
    double best_score = 0.0;
    int title_only_hits = 0, title_only_index = -1;
    double title_only_score = 0.0;
    for (size_t i = 0; i < d_->lib.size(); ++i) {
        const Impl::Lib& L = d_->lib[i];
        const double ts = best_sim(e_titles, L.titles, kMatchThreshold, false);
        if (ts <= 0.0) continue;
        const double as = best_sim(e_artists, L.artists, kMatchThreshold, true);
        if (as > 0.0) {
            const double score = (ts + as) / 2.0;
            if (score > best_score) { best_score = score; best = static_cast<int>(i); }
        } else if (!real_artist && ts >= kMatchTitleOnly) {
            ++title_only_hits;
            title_only_index = static_cast<int>(i);
            title_only_score = ts;
        }
    }
    if (best >= 0) { none.kind = MatchKind::TitleArtist; none.index = best; none.score = best_score; return none; }
    if (title_only_hits == 1) { none.kind = MatchKind::TitleArtist; none.index = title_only_index; none.score = title_only_score; }
    return none;
}

double text_similarity(const std::string& a, const std::string& b) {
    return best_sim(keys_of(a, true), keys_of(b, true), 0.0, true);
}

}  // namespace muisc
