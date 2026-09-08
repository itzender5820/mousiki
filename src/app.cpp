#include "app.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace muisc {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(cur);
    return words;
}

// Standard edit distance (single-char insert/delete/substitute cost 1).
// Small strings only (track titles/artists/search words) — the O(n*m)
// DP table is negligible at this scale.
int levenshtein(const std::string& a, const std::string& b) {
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 0; i <= n; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= m; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost});
        }
    }
    return dp[n][m];
}

// Typo-tolerant match score: higher is better, negative means "not a
// match at all" (excluded from results). An exact literal substring
// match always outranks every fuzzy match, regardless of how good the
// fuzzy quality is — so "another love" typed exactly always sits above
// a merely-close fuzzy hit. Within the fuzzy tier, each query word must
// find a reasonably close word somewhere in the target (a query word
// that's wildly different from everything in the target means this
// isn't really a match, even if some OTHER query word happens to fit)
// — this is what lets "anogher lobe" (typo'd "another love") still find
// the track: word-level edit distance tolerates the substituted
// characters that a plain substring or subsequence check would miss
// entirely (neither "anogher" nor "lobe" appears anywhere in "another
// love" as literal text).
double fuzzy_score(const std::string& query, const std::string& target) {
    std::string q = lower(query), t = lower(target);
    if (q.empty()) return 0.0;

    size_t pos = t.find(q);
    if (pos != std::string::npos) {
        return 1000.0 - std::min<double>(static_cast<double>(pos), 900.0); // tier 1: exact substring, earlier position ranks higher
    }

    auto q_words = split_words(q);
    auto t_words = split_words(t);
    if (q_words.empty() || t_words.empty()) return -1.0;

    double total_quality = 0.0;
    for (auto& qw : q_words) {
        double best_quality = -1.0;
        for (auto& tw : t_words) {
            size_t max_len = std::max(qw.size(), tw.size());
            if (max_len == 0) continue;
            double normalized = static_cast<double>(levenshtein(qw, tw)) / static_cast<double>(max_len);
            double quality = 1.0 - normalized;
            if (quality > best_quality) best_quality = quality;
        }
        if (best_quality < 0.55) return -1.0; // this query word doesn't fit anywhere close enough -- not a real match
        total_quality += best_quality;
    }
    return (total_quality / static_cast<double>(q_words.size())) * 100.0; // tier 2: fuzzy, always below tier 1's range
}

std::string fmt_mmss(double seconds) {
    if (seconds < 0) return "--:--";
    int total = static_cast<int>(seconds);
    std::ostringstream oss;
    oss.width(2); oss.fill('0'); oss << (total / 60) << ":";
    oss.width(2); oss.fill('0'); oss << (total % 60);
    return oss.str();
}

// --- shared box-drawing helpers ------------------------------------------
// Every panel is built against `total_width` (the FULL visual width of the
// box, borders included) so two boxes placed side by side on the same row
// always sum to exactly the width the caller asked for, and single-box
// rows line up with everything above/below them. Content is always
// total_width-4 (for the "│ X │" pattern), padded with the codepoint-safe
// helpers from terminal_ui.cpp so multi-byte glyphs can't throw off the
// column count the way byte-length padding did before.

} // namespace

std::string App::box_top(const std::string& label, int total_width, const std::string& border_ansi) const {
    std::string lbl = label.empty() ? "" : (" " + label + " ");
    std::string prefix = settings_.box_upper_left + settings_.box_horizontal + lbl;
    int used = display_width(prefix);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_upper_right;
    s = pad_right(s, total_width);
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_bottom(int total_width, const std::string& footer, const std::string& border_ansi) const {
    std::string prefix = footer.empty() ? (settings_.box_lower_left + settings_.box_horizontal) : (settings_.box_lower_left + settings_.box_horizontal + " " + footer + " ");
    int used = display_width(prefix);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_lower_right;
    s = pad_right(s, total_width);
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_line(const std::string& content, int total_width, const std::string& border_ansi) const {
    int inner = std::max(0, total_width - 4);
    std::string padded = pad_right(truncate_str(content, inner), inner);
    if (border_ansi.empty()) return settings_.box_vertical + " " + padded + " " + settings_.box_vertical;
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    return bar + " " + padded + " " + bar;
}

namespace {

// Center-aligns plain (no-ANSI) text within `width` visual columns.
std::string center_pad(const std::string& text, int width) {
    std::string t = truncate_str(text, width);
    int pad = std::max(0, width - display_width(t));
    int left = pad / 2;
    int right = pad - left;
    return std::string(left, ' ') + t + std::string(right, ' ');
}

// Visualizer bars use ONLY this glyph set (per explicit instruction) —
// distinct from WaveformQuantizer's 6-level top/mid/bot triples used by
// the progress bar.
const char* fft_glyph(int level) {
    switch (std::clamp(level, 0, 4)) {
        case 0: return " ";
        case 1: return "\u28C0"; // ⣀
        case 2: return "\u28E4"; // ⣤
        case 3: return "\u28F6"; // ⣶
        default: return "\u28FF"; // ⣿
    }
}

// Word-wraps one lyric line to `width` visual columns (breaking on word
// boundaries, never mid-word), center-aligning each resulting row, and
// bakes in ANSI highlighting for words already "sung" (their timestamp
// <= elapsed) when this is the active line. Width/centering math is done
// on PLAIN text first — color codes are spliced in afterwards, since they
// don't occupy display columns but would otherwise confuse the
// codepoint-counting padding helpers.
// Counts UTF-8 codepoints (not bytes) -- used for the letter-by-letter
// lyrics reveal below. terminal_ui.cpp has an equivalent utf8_take(), but
// it's `static` (file-local) so it isn't reachable from here.
static int lyric_utf8_count(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n; // skip UTF-8 continuation bytes
    return n;
}

// Byte-safe prefix of the first `n` UTF-8 codepoints of `s`.
static std::string lyric_utf8_prefix(const std::string& s, int n) {
    if (n <= 0) return "";
    int count = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        if (++count > n) return s.substr(0, start);
    }
    return s;
}

std::vector<std::string> render_lyric_line_wrapped(const LyricLine& line, double elapsed, int width,
                                                     bool is_active, const Settings& settings) {
    struct W { std::string text; double t; bool has_ts; };
    std::vector<W> words;
    if (!line.words.empty()) {
        for (const auto& wt : line.words) words.push_back({wt.second, wt.first, true});
    } else {
        std::istringstream iss(line.full_text);
        std::string w;
        while (iss >> w) words.push_back({w, line.start_time, false});
    }
    if (words.empty() || width <= 0) return {std::string(std::max(0, width), ' ')};

    std::vector<std::vector<W>> rows;
    std::vector<W> cur;
    int cur_len = 0;
    for (auto& w : words) {
        int wlen = display_width(w.text);
        int candidate = cur.empty() ? wlen : cur_len + 1 + wlen;
        if (!cur.empty() && candidate > width) {
            rows.push_back(cur);
            cur.clear();
            cur_len = wlen;
        } else {
            cur_len = candidate;
        }
        cur.push_back(w);
    }
    if (!cur.empty()) rows.push_back(cur);

    std::vector<std::string> out;
    for (auto& row : rows) {
        std::vector<std::string> mapped_words;
        int plain_len = 0;
        for (size_t i = 0; i < row.size(); ++i) {
            std::string mapped = apply_font_map(row[i].text, settings.font_map);
            mapped_words.push_back(mapped);
            plain_len += display_width(mapped);
            if (i > 0) plain_len += 1;
        }
        int total_pad = std::max(0, width - plain_len);
        int left_pad, right_pad;
        if (settings.lyrics_alignment == 1) { left_pad = 0; right_pad = total_pad; }           // left
        else if (settings.lyrics_alignment == 2) { left_pad = total_pad; right_pad = 0; }       // right
        else { left_pad = total_pad / 2; right_pad = total_pad - left_pad; }                    // center (default)

        // Word-level karaoke highlight: find the word currently being sung
        // (the last word whose timestamp has passed but the *next* one
        // hasn't) and render just that one underlined on top of the
        // normal active-word color -- gives the highlight a moving
        // "leading edge" instead of every already-sung word looking
        // identical. (A continuous per-frame pulse used to live here too,
        // applied to whole line-synced-only lines with no way to turn it
        // off -- removed; that wasn't requested and had no toggle.)
        int currently_singing = -1;
        for (size_t i = 0; i < row.size(); ++i) {
            if (is_active && row[i].has_ts && row[i].t <= elapsed) currently_singing = static_cast<int>(i);
        }
        const char* kUnderline = "\x1b[4m";

        std::string s(left_pad, ' ');
        std::string word_ansi = ansi_for(settings.active_word_color) + bg_ansi_for(settings.active_word_bg_color);
        std::string active_line_ansi = ansi_for(settings.active_line_color) + bg_ansi_for(settings.active_line_bg_color);
        std::string inactive_ansi = ansi_for(settings.inactive_line_color) + bg_ansi_for(settings.inactive_line_bg_color);
        // Word-by-word / letter-by-letter modes hide not-yet-sung words in
        // the active line entirely (blanked to spaces, same width, so the
        // alignment doesn't jump around) instead of showing them in the
        // active-line color right away -- that's the actual "progressive
        // reveal" that was asked for, as opposed to the old always-fully-
        // visible line with just a moving color highlight.
        bool progressive = is_active && (settings.lyrics_animation == 1 || settings.lyrics_animation == 2);
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) s += ' ';
            bool sung = is_active && row[i].has_ts && row[i].t <= elapsed;
            bool no_word_ts_but_active = is_active && !row[i].has_ts;
            bool is_current = sung && static_cast<int>(i) == currently_singing;
            if (is_current && settings.lyrics_animation == 2) {
                // Letter-by-letter: interpolate how many characters of the
                // CURRENT word are revealed so far. Prefer the next word's
                // timestamp (within this wrapped row) as the end bound; if
                // this is the last word in the row (no next timestamp to
                // interpolate toward), fall back to the previous word's
                // pace instead of just popping the whole word in at once --
                // that "spawns out of nowhere" look was the bug. With no
                // timing context at all (a single-word line), use a
                // reasonable fixed pace.
                std::string full = mapped_words[i];
                int nchars = lyric_utf8_count(full);
                double dur = -1.0;
                if (i + 1 < row.size() && row[i + 1].has_ts && row[i + 1].t > row[i].t) {
                    dur = row[i + 1].t - row[i].t;
                } else if (i > 0 && row[i - 1].has_ts && row[i].t > row[i - 1].t) {
                    dur = row[i].t - row[i - 1].t;
                } else {
                    dur = 0.4;
                }
                double frac = std::clamp((elapsed - row[i].t) / dur, 0.0, 1.0);
                int revealed = static_cast<int>(frac * nchars);
                std::string shown = lyric_utf8_prefix(full, revealed);
                int hidden_cols = display_width(full) - display_width(shown);
                s += word_ansi + kUnderline + shown + "\x1b[0m" + std::string(std::max(0, hidden_cols), ' ');
            } else if (is_current) {
                s += word_ansi + kUnderline + mapped_words[i] + "\x1b[0m";
            } else if (sung) {
                s += word_ansi + mapped_words[i] + "\x1b[0m";
            } else if (progressive && row[i].has_ts) {
                s += std::string(display_width(mapped_words[i]), ' '); // not sung yet -- hidden, not just dimmed
            } else if (no_word_ts_but_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else if (is_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else {
                s += inactive_ansi + mapped_words[i] + "\x1b[0m";
            }
        }
        s += std::string(right_pad, ' ');
        out.push_back(s);
    }
    return out;
}

fs::path find_lyrics_script() {
    if (const char* env = std::getenv("MOUSIKI_SCRIPTS_DIR")) {
        fs::path p = fs::path(env) / "fetch_lyrics.py";
        if (fs::exists(p)) return p;
    }
    fs::path cwd_candidate = fs::path("scripts") / "fetch_lyrics.py";
    if (fs::exists(cwd_candidate)) return cwd_candidate;

#if defined(__APPLE__)
    char exe_buf[4096];
    uint32_t size = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        std::error_code ec;
        fs::path exe_dir = fs::canonical(fs::path(exe_buf), ec).parent_path();
        if (!ec) {
            fs::path p = exe_dir / "scripts" / "fetch_lyrics.py";
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path() / "scripts" / "fetch_lyrics.py";
            if (fs::exists(p)) return p;
        }
    }
#else
    char exe_buf[4096];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        fs::path exe_dir = fs::path(exe_buf).parent_path();
        fs::path p = exe_dir / "scripts" / "fetch_lyrics.py";
        if (fs::exists(p)) return p;
        p = exe_dir.parent_path() / "scripts" / "fetch_lyrics.py";
        if (fs::exists(p)) return p;
    }
#endif
    return cwd_candidate;
}

} // namespace

App::App() {
    settings_ = load_settings();
    lyrics_script_ = find_lyrics_script();
    all_local_tracks_ = local_source_.scan(settings_.local_music_paths);
    local_view_ = all_local_tracks_;
    launch_row_meta_resolver();
}


int App::hotkey_string_to_key(const std::string& s) {
    if (s == "ARROW_KEY_UP") return 'A';
    if (s == "ARROW_KEY_DOWN") return 'B';
    if (s == "ARROW_KEY_RIGHT") return 'C';
    if (s == "ARROW_KEY_LEFT") return 'D';
    if (s == "ENTER") return '\n';
    if (s == "TAB") return 9;
    if (s == "SPACE") return ' ';
    if (s == "ESC") return 27;
    if (s == "BACKSPACE") return 127;
    if (s.size() == 1) return static_cast<int>(s[0]);
    return 0;
}

std::string App::resolve_hotkey_action(int key) const {
    for (const auto& [action, key_str] : settings_.hotkeys) {
        if (hotkey_string_to_key(key_str) == key) return action;
    }
    return "";
}

// ---------------------------------------------------------------------
// Search / list state
// ---------------------------------------------------------------------

// Shared by refresh_local_view() (final, committed query) and the live
// incremental-search preview (whatever's currently typed, before Enter)
// so both behave identically -- what you see while typing is exactly
// what you'll get if you confirm it.
//
// An empty query returns the library sorted per local_sort_mode_ (see
// apply_local_sort()). A real query filters to fuzzy-matching tracks
// only and sorts PURELY by match quality, highest first -- this
// deliberately throws away the sort order entirely rather than using it
// as a tiebreak, so the best match always sits at the top regardless of
// where it happened to fall alphabetically/by-folder.
//
// Matches against both title AND artist -- using the real probed artist
// tag (row_meta_cache_) where it's already been resolved for that row,
// falling back to the cheap parent-folder guess otherwise. This is why
// "search by artist name" gets more accurate the more of the library
// you've scrolled past (each visible row lazily resolves its real tag).
std::vector<LocalTrack> App::filter_and_rank_local(const std::string& query) const {
    if (query.empty()) {
        auto result = all_local_tracks_;
        apply_local_sort(result);
        return result;
    }

    std::vector<std::pair<double, const LocalTrack*>> scored;
    scored.reserve(all_local_tracks_.size());
    for (const auto& t : all_local_tracks_) {
        std::string artist = t.folder_artist;
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            auto it = row_meta_cache_.find(t.path.string());
            if (it != row_meta_cache_.end() && !it->second.artist.empty()) artist = it->second.artist;
        }
        double title_score = fuzzy_score(query, t.title);
        double artist_score = fuzzy_score(query, artist);
        double score = std::max(title_score, artist_score);
        if (score > 0.0) scored.emplace_back(score, &t);
    }
    std::stable_sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<LocalTrack> result;
    result.reserve(scored.size());
    for (auto& [score, t] : scored) result.push_back(*t);
    return result;
}

const char* App::sort_mode_name(int mode) {
    switch (mode) {
        case 1: return "title A-Z";
        case 2: return "artist A-Z";
        default: return "folder order";
    }
}

// Applied only when browsing with no active search query -- a fuzzy
// search's relevance ranking always wins over the manual sort mode.
// Duration isn't a sort option (yet): most rows only get a real duration
// once they've been lazily probed for display, so sorting by it up front
// would show mostly-unprobed rows in an arbitrary order until the
// background resolver catches up.
void App::apply_local_sort(std::vector<LocalTrack>& tracks) const {
    if (local_sort_mode_ == 1) {
        std::stable_sort(tracks.begin(), tracks.end(), [](const LocalTrack& a, const LocalTrack& b) {
            return lower(a.title) < lower(b.title);
        });
    } else if (local_sort_mode_ == 2) {
        std::stable_sort(tracks.begin(), tracks.end(), [this](const LocalTrack& a, const LocalTrack& b) {
            auto artist_of = [this](const LocalTrack& t) {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(t.path.string());
                return (it != row_meta_cache_.end() && !it->second.artist.empty()) ? it->second.artist : t.folder_artist;
            };
            return lower(artist_of(a)) < lower(artist_of(b));
        });
    }
    // mode 0: leave as scanned (folder order) -- no-op
}

void App::refresh_local_view() {
    local_view_ = filter_and_rank_local(last_local_query_);
    selected_ = 0;
    scroll_ = 0;
}

// Called on every keystroke while typing in the search box, before
// Enter is pressed — this is the "incremental search" behavior: the
// list updates live as you type instead of only after confirming. Local
// queries get filtered+ranked immediately via the same fuzzy logic
// submit_search() will commit on Enter. An "s:" (online search) prefix
// is left alone here — firing a network request on every keystroke
// would be wasteful and slow, so online search still only fires on
// Enter — but the view is reset back to whatever it was before '/' was
// pressed so a stale local preview doesn't linger behind the search box
// while an online query is being typed.
void App::update_live_search_preview() {
    std::string buf = search_buffer_;
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "s:") {
        list_source_ = pre_search_list_source_;
        local_view_ = filter_and_rank_local(pre_search_local_query_);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    list_source_ = ListSource::Local;
    local_view_ = filter_and_rank_local(buf);
    selected_ = 0;
    scroll_ = 0;
}

void App::submit_search() {
    std::string buf = search_buffer_;
    // trim
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "s:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_online_query_ = query;
        list_source_ = ListSource::Online;
        if (search_in_progress_.load()) {
            status_line_ = "still searching, hang on ...";
            return;
        }
        launch_search_async(query.empty() ? "music" : query);
    } else {
        last_local_query_ = buf;
        list_source_ = ListSource::Local;
        refresh_local_view();
    }
}

// ---------------------------------------------------------------------
// Playback start
// ---------------------------------------------------------------------

void App::write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                 double t_probe, double t_total, const std::string& error) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    fs::path dir = fs::path(home) / ".cache" / "mousiki";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream log(dir / "load_timing.log", std::ios::app);
    if (!log.is_open()) return;

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    log << timebuf << " track=\"" << title << "\" source=" << (is_local ? "local" : "online");
    if (!is_local) log << " resolve=" << t_resolve << "s";
    // This is now "time to first sound", not "time to fully decoded" —
    // decode itself streams in after this point, off the critical path.
    log << " probe=" << t_probe << "s time_to_playback=" << t_total << "s";
    if (!error.empty()) log << " ERROR=\"" << error << "\"";
    log << "\n";
}

void App::launch_load_async(fs::path local_path, std::string title, std::string artist,
                             std::string location_label, bool is_local, std::string video_id) {
    if (load_thread_.joinable()) load_thread_.join(); // previous job already signaled done, safe to reap
    load_in_progress_ = true;
    load_ready_ = false;
    load_stage_ = is_local ? 4 : 1;
    load_started_at_ = std::chrono::steady_clock::now();
    if (!is_local) status_line_ = "resolving \"" + title + "\" ...";

    // This thread ONLY resolves (online) and probes metadata/duration —
    // both fast, no full decode. It publishes a result and returns. Full
    // decode is a SEPARATE, detached thread spawned at the bottom, so
    // this thread (the one the main loop's next launch_load_async call
    // will join()) is never blocked waiting on decode — that's what
    // makes it safe to join from launch_load_async without risking a
    // freeze if the user switches tracks again quickly.
    load_thread_ = std::thread([this, local_path, title, artist, location_label, is_local, video_id]() {
        using clock = std::chrono::steady_clock;
        auto t_start = clock::now();
        auto elapsed_s = [](clock::time_point from) {
            return std::chrono::duration<double>(clock::now() - from).count();
        };

        PendingLoad pl;
        pl.title = title;
        pl.artist = artist;
        pl.location_label = location_label;

        double t_resolve = 0.0, t_probe = 0.0;

        fs::path path = local_path;
        if (!is_local) {
            load_stage_ = 1;
            auto t0 = clock::now();
            std::string err;
            auto resolved = youtube_.resolve_by_id(video_id, title, artist, &err);
            t_resolve = elapsed_s(t0);
            if (!resolved) {
                pl.error = "download failed: " + err;
                write_load_timing_log(title, is_local, t_resolve, 0, elapsed_s(t_start), pl.error);
                std::lock_guard<std::mutex> lk(load_mutex_);
                pending_load_ = std::move(pl);
                load_ready_ = true;
                return;
            }
            path = resolved->cached_path;
            pl.title = resolved->title;
            pl.artist = resolved->artist;
        }
        pl.path = path;

        load_stage_ = 4;
        auto t2 = clock::now();
        pl.metadata = probe_metadata(path, pl.title, pl.artist, pl.location_label);
        double duration = probe_duration_seconds(path);
        t_probe = elapsed_s(t2);
        pl.total_sec = duration > 0 ? static_cast<size_t>(duration) : 0;

        pl.pcm = std::make_shared<StreamingPcm>();
        pl.pcm->reserve_for_seconds(duration > 0 ? duration : 300.0, 44100);
        pl.success = true;

        write_load_timing_log(pl.title, is_local, t_resolve, t_probe, elapsed_s(t_start), "");

        {
            std::lock_guard<std::mutex> lk(load_mutex_);
            pending_load_ = pl;
            load_ready_ = true;
        }

        // Decode continues independently from here — detached because it
        // may still be running when the user switches to a different
        // track, and the StreamingPcm it's filling stays alive via the
        // shared_ptr captured below (and via Player's own reference, if
        // this track is still the one playing) for exactly as long as it
        // needs to. Known tradeoff: if the app quits while a decode is
        // still in flight, that ffmpeg subprocess can be orphaned rather
        // than cleanly killed — worth fixing with real process-group
        // tracking later, not a correctness or crash risk today.
        std::shared_ptr<StreamingPcm> pcm = pl.pcm;
        fs::path decode_path = path;
        std::string wtitle = pl.title, wartist = pl.artist;
        bool waveform_smooth = settings_.waveform_smooth; // captured by value — see below, avoids a cross-thread read of settings_
        std::thread([this, decode_path, pcm, waveform_smooth]() {
            stream_decode_ffmpeg(decode_path, *pcm);

            // Deferred mini-waveform pass — only starts once decode is
            // fully done, never gates playback.
            if (!pcm->decode_failed.load()) {
                // PERF: decode is done — pcm->data is no longer being
                // written to, so we pass it directly as a const-ref
                // instead of making a full snapshot copy.  A 5-minute
                // track at 44100 Hz is ~50 MB; that copy was the single
                // biggest reason the waveform appeared so late after
                // playback started, because it doubled the working-set
                // size and stalled the RMS pass behind a large memcpy.
                auto envelope = WaveformQuantizer::generate_high_res_envelope(pcm->data, 4096, waveform_smooth);
                std::lock_guard<std::mutex> lk(waveform_mutex_);
                pending_waveform_envelope_ = std::move(envelope);
                waveform_pending_ready_ = true;
            }
        }).detach();
    });
}

// Used to block here waiting for the previous track's fetch thread to
// finish (join()) before starting a new one. That's a real main-thread
// stall: whenever lyrics aren't quickly available (still mid network-call
// chain) and the user skips to another track before it resolves,
// switching songs would hang until the abandoned fetch finished.
// Detaching instead, with an epoch guard so a late-arriving stale result
// just gets discarded rather than clobbering the new/retried track's
// lyrics. Shared by the initial per-track fetch (poll_pending_load) and
// the manual retry hotkey (handle_key's 'l' case).
void App::launch_lyrics_fetch(std::string title, std::string artist, fs::path path) {
    lyrics_ready_ = false;
    int my_epoch = ++lyrics_epoch_;
    std::thread([this, title, artist, path, my_epoch]() {
        LyricsResult r = fetch_synced_lyrics(title, artist, lyrics_script_.string(), path);
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (my_epoch != lyrics_epoch_.load()) return; // a newer/retried fetch has since started — discard
        lyrics_result_ = std::move(r);
        lyrics_ready_ = true;
    }).detach();
}

void App::poll_pending_load() {
    if (!load_ready_.load()) return;
    PendingLoad pl;
    {
        std::lock_guard<std::mutex> lk(load_mutex_);
        // BUG FIX #4: clear the flag while still holding load_mutex_ so
        // the load thread cannot race-write pending_load_ again in the
        // window between the copy and the flag reset.
        if (!load_ready_.load()) return; // re-check under lock (spurious wakeup guard)
        pl = pending_load_;
        load_ready_ = false;
    }
    load_in_progress_ = false;
    load_stage_ = 0;

    if (!pl.success) {
        status_line_ = pl.error;
        return;
    }

    // Player::play() stops whatever it was previously playing as its own
    // first step, so no separate explicit stop() call is needed here —
    // and doing it inside play() (below, off the main thread) is what
    // lets this whole switch never touch the main thread.
    current_pcm_ = pl.pcm;
    total_sec_ = pl.total_sec;
    metadata_ = pl.metadata;
    current_path_ = pl.path;
    has_track_ = true;
    player_.clear_finished(); // see clear_finished()'s comment — closes the race that caused the double-skip bug
    waveform_envelope_.clear();
    waveform_ready_ = false;
    waveform_pending_ready_ = false;
    ++waveform_epoch_; // BUG FIX #5: invalidate any in-flight waveform from the previous track
    last_lyrics_status_.clear();
    fft_.reset(); // don't let the previous track's spectrum tail linger into this one's first frame

    launch_lyrics_fetch(pl.title, pl.artist, pl.path);

    // This is the whole point of the redesign: play() is handed a
    // StreamingPcm that may have zero frames decoded yet. The audio
    // callback plays silence for anything past what's been decoded and
    // self-corrects the instant more arrives — so sound starts the
    // moment decode produces its first chunk, not after the whole track.
    // Dispatched off the main thread — see launch_device_play_async().
    launch_device_play_async();
    status_line_.clear();
}

void App::launch_device_play_async() {
    // Never join here — that would risk blocking whichever thread calls
    // this (poll_pending_load / advance_track, both on the main thread)
    // on however long the OLD device op takes to finish. Detach it: the
    // old attempt just finishes on its own (Player::play() stops the
    // previous device as its first step anyway, so an old in-flight
    // play() call safely becomes a no-op-ish teardown once it gets to
    // run, even if a newer one has already taken over by then).
    //
    // BUG FIX #2: previously the detached old thread could call
    // ma_device_start() on a device that the new thread had already torn
    // down inside play() → stop() — undefined behaviour and the root
    // cause of audio glitches on fast track-switching. The generation
    // counter lets the old thread detect that it has been superseded and
    // bail out before it ever touches the device.
    if (device_thread_.joinable()) device_thread_.detach();
    int my_gen = ++device_gen_;
    auto pcm = current_pcm_;
    int vol = player_.volume() > 0 ? player_.volume() : 70;
    device_thread_ = std::thread([this, pcm, vol, my_gen]() {
        std::lock_guard<std::mutex> lk(device_mutex_);
        if (my_gen != device_gen_.load()) return; // superseded — a newer play request won
        player_.play(pcm, 0.0, vol, &fft_);
    });
}

void App::poll_pending_waveform() {
    if (!waveform_pending_ready_.load()) return;
    std::vector<float> envelope;
    {
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        envelope = std::move(pending_waveform_envelope_);
    }
    waveform_pending_ready_ = false;
    waveform_envelope_ = std::move(envelope);
    waveform_ready_ = true;
    waveform_reveal_start_ = std::chrono::steady_clock::now(); // starts the 700ms left-to-right reveal
}

void App::launch_search_async(const std::string& query) {
    if (search_thread_.joinable()) search_thread_.join();
    search_in_progress_ = true;
    search_ready_ = false;
    status_line_ = "searching online for \"" + query + "\" ...";

    search_thread_ = std::thread([this, query]() {
        auto results = online_.search(query);
        std::lock_guard<std::mutex> lk(search_mutex_);
        pending_search_results_ = std::move(results);
        search_ready_ = true;
    });
}

void App::poll_pending_search() {
    if (!search_ready_.load()) return;
    std::vector<OnlineResult> results;
    {
        std::lock_guard<std::mutex> lk(search_mutex_);
        results = std::move(pending_search_results_);
    }
    search_ready_ = false;
    search_in_progress_ = false;

    online_view_ = std::move(results);
    selected_ = 0;
    scroll_ = 0;
    status_line_ = online_view_.empty() ? "no online results" : "";
}

void App::play_selected() {
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) start_local_track(local_view_[selected_]);
    else start_online_track(online_view_[selected_]);
}

void App::play_relative(int delta) {
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0) return;
    selected_ = std::clamp(selected_ + delta, 0, static_cast<int>(list_len) - 1);
    if (selected_ >= scroll_ + kListVisibleRows) scroll_ = selected_ - kListVisibleRows + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::play_relative_random() {
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0) return;
    if (list_len == 1) { selected_ = 0; play_selected(); return; }
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(list_len) - 1);
    int next;
    do { next = dist(rng); } while (next == selected_);
    selected_ = next;
    if (selected_ >= scroll_ + kListVisibleRows) scroll_ = selected_ - kListVisibleRows + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::advance_track() {
    has_track_ = false;

    // The queue always takes priority over play_mode — it's an explicit
    // user-built-up-next list.
    if (!queue_.empty()) {
        QueueItem item = queue_.front();
        queue_.erase(queue_.begin());
        if (queue_selected_ > 0) --queue_selected_; // indices shifted down by the erase
        clamp_queue_selected();
        if (item.is_local) {
            LocalTrack t{fs::path(item.local_path).stem().string(), item.local_path, item.artist};
            start_local_track(t);
        } else {
            OnlineResult r{item.video_id, item.title, item.artist};
            start_online_track(r);
        }
        return;
    }

    switch (settings_.play_mode) {
        case 1: // loop — same track, already fully decoded, no reload needed
            launch_device_play_async();
            has_track_ = true;
            player_.clear_finished();
            break;
        case 2: // shuffle
            play_relative_random();
            break;
        case 3: // stop
            break; // leave has_track_ false, no auto-advance
        default: // list (sequential)
            play_relative(1);
            break;
    }
}

void App::queue_add_selected() {
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) {
        const auto& t = local_view_[selected_];
        queue_.push_back({true, t.title, t.folder_artist, t.path, ""});
    } else {
        const auto& r = online_view_[selected_];
        queue_.push_back({false, r.title, r.uploader, {}, r.video_id});
    }
    clamp_queue_selected();
}

void App::queue_remove_last() {
    if (!queue_.empty()) queue_.pop_back();
    clamp_queue_selected();
}

void App::clamp_queue_selected() {
    if (queue_.empty()) { queue_selected_ = 0; queue_scroll_ = 0; return; }
    queue_selected_ = std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1);
    if (queue_selected_ >= queue_scroll_ + kListVisibleRows) queue_scroll_ = queue_selected_ - kListVisibleRows + 1;
    if (queue_selected_ < queue_scroll_) queue_scroll_ = queue_selected_;
}

void App::queue_remove_hovering() {
    if (queue_.empty() || queue_selected_ < 0 || queue_selected_ >= static_cast<int>(queue_.size())) return;
    queue_.erase(queue_.begin() + queue_selected_);
    clamp_queue_selected();
}

void App::queue_move_hovering(int dir) {
    if (queue_.empty()) return;
    int target = queue_selected_ + dir;
    if (target < 0 || target >= static_cast<int>(queue_.size())) return; // already at an edge
    std::swap(queue_[queue_selected_], queue_[target]);
    queue_selected_ = target;
    clamp_queue_selected();
}

// Tab layout: 0=Colors, 1=On/Off, 2=Animation, 3=Reference, 4=About App.
// Reference-tab rows map to specific well-known hotkey action keys in
// settings_.hotkeys (a plain string->string map already), so they don't
// need their own struct fields the way Colors/On-Off/Animation do.
static const char* kRefHotkeyNames[] = {
    "HKeySetting", "HKeyNavigateUp", "HKeyNavigateDown", "HKeyPlay", "HKeyPlayNextSong",
    "HKeyPlayPreviousSong", "HKeyToggleRepeat", "HKeyToggleShuffle", "HKeySearch",
    "HKeySearchOnline", "HKeyQuit",
};
static constexpr int kRefRowCount = 11;

std::string* App::color_field_ptr(int row, int col) {
    switch (row) {
        case 0: return col == 0 ? &settings_.border_color : &settings_.border_color_bottom;
        case 1: return col == 0 ? &settings_.disk_color : &settings_.disk_color_end;
        case 2: return col == 0 ? &settings_.meta_key_color : &settings_.meta_val_color;
        case 3: return col == 0 ? &settings_.visualizer_color : &settings_.visualizer_color_end;
        case 4: return col == 0 ? &settings_.progress_played_color : &settings_.progress_remaining_color;
        case 5: return col == 0 ? &settings_.list_color : &settings_.list_inactive_bg_color;
        case 6: return col == 0 ? &settings_.list_playing_color : &settings_.list_playing_bg_color;
        case 7: return col == 0 ? &settings_.list_cursor_color : &settings_.list_cursor_bg_color;
        case 8: return col == 0 ? &settings_.queue_color : &settings_.queue_inactive_bg_color;
        case 9: return col == 0 ? &settings_.queue_playing_color : &settings_.queue_playing_bg_color;
        case 10: return col == 0 ? &settings_.queue_cursor_color : &settings_.queue_cursor_bg_color;
        case 11: return col == 0 ? &settings_.inactive_line_color : &settings_.inactive_line_bg_color;
        case 12: return col == 0 ? &settings_.active_line_color : &settings_.active_line_bg_color;
        case 13: return col == 0 ? &settings_.active_word_color : &settings_.active_word_bg_color;
        default: return nullptr;
    }
}

int App::settings_max_row() const {
    // Matches get_max_row(): SCHEMA.size() - 1 for each tab, extended for
    // the Reference tab's appended font-map rows and the About tab's
    // scrollable text (both computed dynamically, not hardcoded, so they
    // track the actual font_map/about_app_lines content).
    switch (settings_tab_) {
        case 0: return 13; // COLOR_SCHEMA: 14 rows
        case 1: return 6;  // ONOFF_SCHEMA: 7 rows
        case 2: return 7;  // ANIM_SCHEMA: 8 rows
        case 3: {
            int letters = 0;
            for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) ++letters;
            return kRefRowCount + letters - 1; // 11 hotkeys + N font-map rows
        }
        case 4: {
            int MAX_Y = std::max(main_frame_height(80) - 2, 10);
            int visible = std::max(1, MAX_Y - 3);
            int total = static_cast<int>(settings_.about_app_lines.size());
            return std::max(0, total - visible); // scroll range, not a field cursor
        }
        default: return 0;
    }
}

std::string App::settings_get_value(int row, int col) const {
    // Matches getVal(k): config value if set, else the hardcoded default
    // for the 7 keys that have one, else "___" (unset). Colors format
    // their own "___"-equivalent as "none" at render time instead.
    if (settings_tab_ == 0) {
        std::string* p = const_cast<App*>(this)->color_field_ptr(row, col);
        return p ? *p : "";
    }
    if (settings_tab_ == 1) {
        bool v = false;
        switch (row) {
            case 0: v = settings_.element_disk; break;
            case 1: v = settings_.element_dummy_buttons; break;
            case 2: v = settings_.element_queue; break;
            case 3: v = settings_.element_waveform; break;
            case 4: v = settings_.element_lyrics; break;
            case 5: v = settings_.element_lyrics_placeholder_ball; break;
            case 6: v = settings_.element_visualizer; break;
        }
        return v ? "true" : "false";
    }
    if (settings_tab_ == 2) {
        switch (row) {
            case 0: return std::to_string(settings_.visualizer_fluidity);
            case 1: return settings_.waveform_smooth ? "smooth" : "raw";
            case 2: return std::to_string(settings_.disk_rotation_speed).substr(0, 4);
            case 3: return settings_.play_mode == 1 ? "loop" : settings_.play_mode == 2 ? "shuffle" : settings_.play_mode == 3 ? "stop" : "list";
            case 4: return std::to_string(settings_.visualizer_degradation_speed);
            case 5: return std::to_string(settings_.visualizer_viscosity);
            case 6: return settings_.lyrics_alignment == 1 ? "left" : settings_.lyrics_alignment == 2 ? "right" : "center";
            case 7: {
                static const char* names[] = {"full", "word by word", "letter by letter", "active line only", "active word only", "line by line"};
                return names[std::clamp(settings_.lyrics_animation, 0, 5)];
            }
        }
    }
    if (settings_tab_ == 3 && row >= 0 && row < kRefRowCount) {
        auto it = settings_.hotkeys.find(kRefHotkeyNames[row]);
        return it != settings_.hotkeys.end() ? it->second : "";
    }
    return "";
}

// Matches g_options: the fixed value lists that Left/Right cycles
// through. Empty return = not cyclable (Colors and Reference rows,
// exactly like the reference's g_options map has no entries for those).
std::vector<std::string> App::settings_options_for(int tab, int row) const {
    if (tab == 1) return {"true", "false"};
    if (tab == 2) {
        switch (row) {
            case 0: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 1: return {"raw", "smooth"};
            case 2: return {"0.01", "0.05", "0.10", "0.17", "0.25", "0.50", "0.75", "1.00"};
            case 3: return {"list", "loop", "shuffle"};
            case 4: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 5: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 6: return {"left", "center", "right"};
            case 7: return {"full", "word by word", "line by line", "letter by letter", "active line only", "active word only"};
        }
    }
    return {};
}

// Matches g_config[k] = edit_buffer -- stored close to verbatim, no
// clamping. A bool/enum field that doesn't recognize the typed text
// just leaves the setting unchanged, since there's no way to store
// arbitrary text in a typed field the way the reference's string map can.
void App::settings_commit_edit() {
    const std::string& buf = color_edit_buffer_;
    auto to_lower = [](std::string v) { for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; };

    if (settings_tab_ == 0) {
        std::string* p = color_field_ptr(settings_row_, settings_col_);
        if (p) *p = buf;
    } else if (settings_tab_ == 1) {
        std::string v = to_lower(buf);
        bool is_true = (v == "true"), is_false = (v == "false");
        if (!is_true && !is_false) return;
        switch (settings_row_) {
            case 0: settings_.element_disk = is_true; break;
            case 1: settings_.element_dummy_buttons = is_true; break;
            case 2: settings_.element_queue = is_true; break;
            case 3: settings_.element_waveform = is_true; break;
            case 4: settings_.element_lyrics = is_true; break;
            case 5: settings_.element_lyrics_placeholder_ball = is_true; break;
            case 6: settings_.element_visualizer = is_true; break;
        }
    } else if (settings_tab_ == 2) {
        std::string v = to_lower(buf);
        switch (settings_row_) {
            case 0: try { settings_.visualizer_fluidity = std::stoi(buf); } catch (...) {} break;
            case 1: settings_.waveform_smooth = (v == "smooth"); break;
            case 2: try { settings_.disk_rotation_speed = std::stod(buf); } catch (...) {} break;
            case 3: settings_.play_mode = (v == "loop") ? 1 : (v == "shuffle") ? 2 : (v == "stop") ? 3 : 0; break;
            case 4: try { settings_.visualizer_degradation_speed = std::stoi(buf); } catch (...) {} break;
            case 5: try { settings_.visualizer_viscosity = std::stoi(buf); } catch (...) {} break;
            case 6: settings_.lyrics_alignment = (v == "left") ? 1 : (v == "right") ? 2 : 0; break;
            case 7:
                if (v == "word by word") settings_.lyrics_animation = 1;
                else if (v == "letter by letter") settings_.lyrics_animation = 2;
                else if (v == "active line only") settings_.lyrics_animation = 3;
                else if (v == "active word only") settings_.lyrics_animation = 4;
                else if (v == "line by line") settings_.lyrics_animation = 5;
                else settings_.lyrics_animation = 0;
                break;
        }
    } else if (settings_tab_ == 3 && settings_row_ >= 0 && settings_row_ < kRefRowCount) {
        settings_.hotkeys[kRefHotkeyNames[settings_row_]] = buf;
    }
}

// Matches cycle_option(): find the current value's index in its options
// list and step by `dir`, wrapping. No-op if this row has no options
// list at all.
void App::settings_cycle(int dir) {
    auto opts = settings_options_for(settings_tab_, settings_row_);
    if (opts.empty()) return;
    std::string cur = settings_get_value(settings_row_, 0);
    int idx = -1;
    for (size_t i = 0; i < opts.size(); ++i) if (opts[i] == cur) { idx = static_cast<int>(i); break; }
    idx = (idx == -1) ? 0 : (idx + dir + static_cast<int>(opts.size())) % static_cast<int>(opts.size());
    color_edit_buffer_ = opts[idx];
    settings_commit_edit();
    status_line_ = "TOGGLED -> " + opts[idx];
    if (settings_tab_ == 2 && settings_row_ == 1) recompute_waveform_for_current_track();
}

void App::handle_settings_key(int key) {
    if (mode_ == Mode::ColorEdit) {
        if (key == 27) { mode_ = Mode::Settings; return; } // cancel, discard buffer
        if (key == '\r' || key == '\n') {
            std::string key_name = (settings_tab_ == 3 && settings_row_ >= 0 && settings_row_ < kRefRowCount)
                                  ? kRefHotkeyNames[settings_row_] : "";
            settings_commit_edit();
            status_line_ = key_name.empty() ? "UPDATED" : ("UPDATED " + key_name);
            mode_ = Mode::Settings;
            return;
        }
        if (key == 127 || key == 8) { if (!color_edit_buffer_.empty()) color_edit_buffer_.pop_back(); return; }
        if (key >= 32 && key < 127 && color_edit_buffer_.size() < 18) color_edit_buffer_ += static_cast<char>(key);
        return;
    }

    if (key == 9) { // Tab
        settings_tab_ = (settings_tab_ + 1) % kSettingsTabCount;
        settings_row_ = 0;
        settings_col_ = 0;
        return;
    }
    // Explicit spec from the user, overriding the reference's own
    // key semantics for this exact case (reference's 's' saves without
    // closing; here 's' saves AND exits, Esc/q just exits without saving).
    if (key == 's' || key == 'S') { save_settings(settings_); status_line_ = "SAVED"; mode_ = Mode::Browse; return; }
    if (key == 27 || key == 'q' || key == 'Q') {
        mode_ = Mode::Browse;
        return;
    }
    if (settings_tab_ == 4) {
        // About App: no fields to edit, but Up/Down still scroll the text.
        if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
        if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
        return;
    }

    if (key == '\r' || key == '\n') {
        if (settings_tab_ == 3 && settings_row_ >= kRefRowCount) return; // font-map rows are read-only display
        color_edit_buffer_ = settings_get_value(settings_row_, settings_col_);
        mode_ = Mode::ColorEdit;
        return;
    }
    if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
    if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
    if (key == 'C') { // right
        if (settings_tab_ == 0) { if (color_field_ptr(settings_row_, 1)) settings_col_ = 1; }
        else settings_cycle(1);
        return;
    }
    if (key == 'D') { // left
        if (settings_tab_ == 0) settings_col_ = 0;
        else settings_cycle(-1);
        return;
    }
}

void App::start_local_track(const LocalTrack& track) {
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    fs::path parent = track.path.parent_path().filename();
    launch_load_async(track.path, track.title, track.folder_artist == "-" ? "" : track.folder_artist,
                       parent.string() + "/", /*is_local=*/true, /*video_id=*/"");
}

void App::start_online_track(const OnlineResult& result) {
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    launch_load_async({}, result.title, result.uploader, "youtube", /*is_local=*/false, result.video_id);
}

// ---------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------

void App::handle_key(int key) {
    if (key == 0) return;

    if (mode_ == Mode::ColorEdit || mode_ == Mode::Settings) {
        handle_settings_key(key);
        return;
    }

    if (mode_ == Mode::Search) {
        if (key == 27) {
            // Cancel: put the view back exactly as it was before '/' was
            // pressed, discarding whatever the live preview below was
            // showing.
            list_source_ = pre_search_list_source_;
            last_local_query_ = pre_search_local_query_;
            refresh_local_view();
            mode_ = Mode::Browse;
            return;
        }
        if (key == '\r' || key == '\n') { submit_search(); mode_ = Mode::Browse; return; }
        if (key == 127 || key == 8) {
            if (!search_buffer_.empty()) search_buffer_.pop_back();
            update_live_search_preview();
            return;
        }
        if (key >= 32 && key < 127) {
            search_buffer_ += static_cast<char>(key);
            update_live_search_preview();
            return;
        }
        return;
    }

    // Mode::Browse
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();

    switch (key) {
        case 's': case 'S':
            mode_ = Mode::Settings;
            settings_tab_ = 0;
            settings_row_ = 0;
            settings_col_ = 0;
            break;
        case 9: // Tab: toggle Up/Down + reorder focus between the list and the queue
            queue_focus_ = !queue_focus_;
            break;
        case 'A': // up
            if (queue_focus_) {
                if (queue_selected_ > 0) --queue_selected_;
                clamp_queue_selected();
            } else {
                if (selected_ > 0) --selected_;
                if (selected_ < scroll_) scroll_ = selected_;
            }
            break;
        case 'B': // down
            if (queue_focus_) {
                if (!queue_.empty() && queue_selected_ < static_cast<int>(queue_.size()) - 1) ++queue_selected_;
                clamp_queue_selected();
            } else {
                if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
                // BUGFIX: selection could move past the visible window without
                // the window ever following it, leaving the highlighted row
                // invisible below row 8 instead of the list scrolling up.
                if (selected_ >= scroll_ + kListVisibleRows) scroll_ = selected_ - kListVisibleRows + 1;
            }
            break;
        case 'C': // right = seek forward
            if (has_track_) player_.seek_relative(5.0);
            break;
        case 'D': // left = seek back ... OR, while queue-focused, move the hovering queue item down.
            // Left-arrow and Shift+D are indistinguishable at the terminal-
            // input layer (see TerminalIO::poll_key) — reusing this case
            // for reordering while queue-focused means seeking is
            // unavailable during that time, but that's an acceptable
            // trade since you're not usually seeking while reordering a
            // queue anyway.
            if (queue_focus_) queue_move_hovering(1);
            else if (has_track_) player_.seek_relative(-5.0);
            break;
        case 'u': case 'U': // move the hovering queue item up (only meaningful once you've Tab'd into the queue)
            queue_move_hovering(-1);
            break;
        case 'p': case 'P': // play/pause
            if (has_track_) { if (player_.is_paused()) player_.resume(); else player_.pause(); }
            break;
        case '1': // volume up
            if (has_track_) player_.set_volume(std::min(100, player_.volume() + 5));
            break;
        case '2': // volume down
            if (has_track_) player_.set_volume(std::max(0, player_.volume() - 5));
            break;
        case 'n': case 'N': // next song (within current list)
            play_relative(1);
            break;
        case 'b': // prev song (within current list)
            play_relative(-1);
            break;
        case 'a': // add selected to queue
            queue_add_selected();
            break;
        case 'd': // remove last queued item
            queue_remove_last();
            break;
        case 'l': case 'L': // retry lyrics fetch for the current track
            if (has_track_) {
                launch_lyrics_fetch(metadata_.name, metadata_.artist == "-" ? "" : metadata_.artist, current_path_);
                status_line_ = "retrying lyrics ...";
            }
            break;
        case 'w': case 'W': // toggle waveform style (raw/smooth) directly, without going into Settings
            settings_.waveform_smooth = !settings_.waveform_smooth;
            recompute_waveform_for_current_track();
            status_line_ = settings_.waveform_smooth ? "waveform: smooth" : "waveform: raw";
            break;
        case 't': // remove the hovering song from the queue
            queue_remove_hovering();
            status_line_ = "removed from queue";
            break;
        case 'T': // cycle local-list sort mode (folder order -> title A-Z -> artist A-Z)
            local_sort_mode_ = (local_sort_mode_ + 1) % 3;
            refresh_local_view();
            status_line_ = std::string("sort: ") + sort_mode_name(local_sort_mode_);
            break;
        case '\r': case '\n':
            play_selected();
            break;
        case '/':
            mode_ = Mode::Search;
            search_buffer_.clear();
            pre_search_list_source_ = list_source_;
            pre_search_local_query_ = last_local_query_;
            break;
        case 27: // ESC -- back to the home view: full local library, no
                 // filter, from the top. Same destination regardless of
                 // how buried you are (mid search results, viewing
                 // online results, scrolled deep into the list).
            list_source_ = ListSource::Local;
            last_local_query_.clear();
            refresh_local_view();
            status_line_.clear();
            break;
        case 'r': case 'R': // force a full redraw -- for when a resize
                             // raced the render loop and left a torn/
                             // stale frame on screen. hard_clear is
                             // normally only set on a detected width or
                             // mode change; this forces it once
                             // unconditionally on the very next frame.
            force_redraw_ = true;
            break;
        case 'q': case 'Q':
            quit_ = true;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------
// Lazy metadata probing for whatever's currently visible in the list
// ---------------------------------------------------------------------

// This used to call probe_row_meta() here directly — which spawns a real
// ffprobe SUBPROCESS, synchronously, on the render thread, once per
// newly-visible row, every single frame a new row scrolled into view.
// Scroll through a big library quickly (or hit one slow/hanging file —
// weird encode, flaky storage) and the whole UI thread — rendering AND
// input — blocks for however long those subprocess calls take, with no
// way to even press 'q' to get out of it. That's what was crashing the
// whole player.
//
// Fix: use the native in-process binary-header parser (native_duration.h
// — pure pread() syscalls, no subprocess, effectively can't hang) for
// duration here, directly on the render thread — genuinely safe now.
// Anything that parser can't handle (unsupported format, corrupt file)
// gets picked up by a ONE-TIME background sweep (launch_row_meta_resolver,
// started once after the initial scan) that walks the whole library
// sequentially and falls back to ffprobe there — off the main thread
// entirely, never gating rendering or input.
void App::ensure_visible_row_meta() {
    if (list_source_ != ListSource::Local) return;
    for (int i = scroll_; i < std::min<int>(local_view_.size(), scroll_ + kListVisibleRows); ++i) {
        const auto& t = local_view_[i];
        std::string key = t.path.string();
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            if (row_meta_cache_.count(key)) continue; // already resolved (native path or background sweep)
        }
        uint32_t dur = probe_duration_native(t.path);
        if (dur > 0) {
            RowMeta rm;
            rm.duration_sec = static_cast<double>(dur);
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            // BUG FIX #1: cap the cache so it can't grow proportionally
            // to an arbitrarily large library (a 50k-track collection
            // would otherwise accumulate tens of MB that are never freed).
            if (row_meta_cache_.size() < 4096)
                row_meta_cache_[key] = rm;
        }
        // else: leave unresolved — native parsing is cheap enough to just
        // retry next frame, and the background sweep will fill it in via
        // ffprobe regardless, so there's no real cost to not caching a miss.
    }
}

// Re-runs quantization on the already-decoded PCM for whatever's
// currently playing — used when the smooth/raw toggle changes mid-track,
// so the effect shows up right away instead of only on the next track.
// No re-decode needed: current_pcm_ already holds everything decoded so
// far (streaming decode may still be filling it in, hence the acquire
// load of `available` rather than assuming it's complete).
void App::recompute_waveform_for_current_track() {
    if (!has_track_ || !current_pcm_) return;
    std::shared_ptr<StreamingPcm> pcm = current_pcm_;
    bool smooth = settings_.waveform_smooth;
    // BUG FIX #5: increment epoch before spawning — any already-running
    // waveform thread will see its own epoch is stale and discard its
    // result instead of racing to overwrite pending_waveform_envelope_.
    int my_epoch = ++waveform_epoch_;
    std::thread([this, pcm, smooth, my_epoch]() {
        size_t n = pcm->available.load(std::memory_order_acquire);
        if (n == 0) return;
        std::vector<float> snapshot(pcm->data.begin(), pcm->data.begin() + static_cast<long>(n));
        auto envelope = WaveformQuantizer::generate_high_res_envelope(snapshot, 4096, smooth);
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        if (my_epoch != waveform_epoch_.load()) return; // superseded — discard
        pending_waveform_envelope_ = std::move(envelope);
        waveform_pending_ready_ = true;
    }).detach();
}

void App::launch_row_meta_resolver() {
    if (row_meta_resolver_started_.exchange(true)) return; // only ever runs once per app session
    std::vector<fs::path> paths;
    paths.reserve(all_local_tracks_.size());
    for (auto& t : all_local_tracks_) paths.push_back(t.path);

    // Detached, not tracked-and-joined: a whole-library ffprobe sweep
    // could take a while on a big collection, and joining it at shutdown
    // would just trade one flavor of "blocked and can't do anything" for
    // another. Same tradeoff already accepted for decode threads —
    // worst case on quit is one orphaned ffprobe call, not a crash.
    std::thread([this, paths]() {
        for (auto& p : paths) {
            std::string key = p.string();
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                if (row_meta_cache_.count(key)) continue; // native parse (or an earlier pass) already got it
            }
            RowMeta rm = probe_row_meta(p); // ffprobe fallback — slow, but background-thread-only now
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            if (row_meta_cache_.size() < 4096) // BUG FIX #1: same cap as ensure_visible_row_meta
                row_meta_cache_[key] = rm;
        }
    }).detach();
}

// ---------------------------------------------------------------------
// Panel builders
// ---------------------------------------------------------------------

std::vector<std::string> App::build_metadata_panel(int total_width) const {
    const int inner = total_width - 4;
    const int disk_w = settings_.element_disk ? disk_.width() : 0;
    const int panel_h = disk_.height();
    
    std::string sep = "  " + settings_.meta_separator + "  ";
    int sep_w = display_width(sep);
    const int fixed_extra = settings_.element_disk ? sep_w : 2;

    int avail = std::max(10, inner - disk_w - fixed_extra);
    int meta_w = avail;
    int lyrics_w = 0;
    if (settings_.element_lyrics) {
        meta_w = std::min(42, std::max(10, avail - 10));
        meta_w = std::min(meta_w, avail);
        lyrics_w = std::max(0, avail - meta_w);
    }

    std::vector<std::string> disk_frame;
    if (settings_.element_disk) {
        disk_frame = disk_.frame(angle_);
        while (static_cast<int>(disk_frame.size()) < panel_h) disk_frame.emplace_back(std::string(disk_w, ' '));
        for (size_t row_i = 0; row_i < disk_frame.size(); ++row_i) {
            float t = disk_frame.size() > 1 ? static_cast<float>(row_i) / static_cast<float>(disk_frame.size() - 1) : 0.0f;
            std::string disk_end = settings_.disk_color_end;
            std::string row_ansi = gradient_ansi(settings_.disk_color, disk_end, t);
            disk_frame[row_i] = row_ansi + disk_frame[row_i] + "\x1b[0m";
        }
    }

    double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    fft_.set_fluidity(settings_.visualizer_fluidity);
    fft_.set_degradation_speed(settings_.visualizer_degradation_speed);
    fft_.set_viscosity(settings_.visualizer_viscosity);

    // meta content rows
    std::vector<std::string> meta_rows(panel_h, std::string());
    bool viz_rows_colored = false;
    std::vector<int> bars; // computed once below, reused by the sphere visualizer fallback further down
    if (has_track_) {
        std::string k_col = settings_.meta_key_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_key_color);
        std::string v_col = settings_.meta_val_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_val_color);
        auto kv = [&](int row, const std::string& label, const std::string& value) {
            std::string mapped_label = apply_font_map(label, settings_.font_map);
            std::string mapped_val = apply_font_map(value, settings_.font_map);
            int avail_v = std::max(0, meta_w - 12);
            std::string l_pad = pad_right(mapped_label, 10);
            std::string v_tr = truncate_str(mapped_val, avail_v);
            std::string plain = l_pad + ": " + v_tr;
            std::string ansi = k_col + l_pad + "\x1b[0m" + ": " + v_col + v_tr + "\x1b[0m";
            ansi += std::string(std::max(0, meta_w - display_width(plain)), ' ');
            meta_rows[row] = ansi;
        };
        kv(1, "Name", metadata_.name);
        kv(2, "Artist", metadata_.artist);
        kv(3, "year", metadata_.year);
        kv(4, "sampling", metadata_.sampling);
        kv(5, "type", metadata_.type);
        kv(6, "format", metadata_.format);
        kv(7, "file size", metadata_.file_size);
        kv(8, "location", metadata_.location);
        if (!metadata_.extra_label.empty()) kv(9, metadata_.extra_label, metadata_.extra_value);

        // Real spectrum visualizer (KISS FFT), not a copy of the progress
        // bar's RMS envelope. Two rows: bottom row is the base level
        // (0-4), top row is whatever's left over above that (0-4) so
        // taller peaks build upward — attached to the panel's bottom row
        // per instruction, with the second row directly above it.
        if (settings_.element_visualizer) {
            int viz_w = std::min(meta_w, 48);
            bars = fft_.compute_bars(viz_w, viz_dt_);
            std::string viz_top, viz_bottom;
            int nbars = static_cast<int>(bars.size());
            for (int i = 0; i < nbars; ++i) {
                int level = bars[i];
                float t = nbars > 1 ? static_cast<float>(i) / static_cast<float>(nbars - 1) : 0.0f;
                std::string bar_ansi;
                if (!settings_.viz_center_color.empty()) {
                    bar_ansi = multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t);
                } else {
                    bar_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t);
                }
                viz_bottom += bar_ansi;
                viz_bottom += fft_glyph(std::min(level, 4));
                viz_top += bar_ansi;
                viz_top += fft_glyph(std::max(0, level - 4));
            }
            viz_top += "\x1b[0m";
            viz_bottom += "\x1b[0m";
            if (viz_w < meta_w) {
                std::string tail(meta_w - viz_w, ' ');
                viz_top += tail;
                viz_bottom += tail;
            }
            meta_rows[panel_h - 2] = viz_top;
            meta_rows[panel_h - 1] = viz_bottom;
            viz_rows_colored = true;
        } else {
            bars = fft_.compute_bars(48, viz_dt_);
        }
    } else {
        meta_rows[0] = "no track loaded - press / to search, Enter to play";
    }
    for (int i = 0; i < static_cast<int>(meta_rows.size()); ++i) {
        if (meta_rows[i].empty()) {
            meta_rows[i] = std::string(meta_w, ' ');
        } else if (!has_track_ && i == 0) {
            meta_rows[i] = pad_right(meta_rows[i], meta_w);
        }
        // all other rows (kv data, visualizer) are already perfectly padded
        // by their respective builders, and padding them again would miscount 
        // their ANSI color escapes as visible columns, truncating them.
    }

    // lyrics window: word-wrapped, center-aligned, word-level highlight on
    // the active line — windowed so the active line's wrapped block is
    // always vertically centered, blank-padded at the edges.
    std::vector<std::string> lyric_rows(panel_h, std::string(lyrics_w, ' '));
    bool lyrics_avail = false;
    std::vector<LyricLine> lines_copy;
    std::string lyrics_status;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (lyrics_ready_) {
            lines_copy = lyrics_result_.lines;
            lyrics_status = lyrics_result_.message;
            lyrics_avail = !lines_copy.empty();
        } else if (has_track_) {
            lyrics_status = "fetching lyrics ...";
        }
    }

    if (!lyrics_avail) {
        // A status message ("fetching...", "no lyrics found", etc.) is
        // only shown for the first 10s after it appears — after that the
        // sphere gets the whole panel to itself instead of a permanently
        // stuck caption line. Each distinct message content gets its own
        // fresh 10s window (so "fetching..." showing, then later
        // changing to "no lyrics found", each get their moment) rather
        // than one timer for the whole track.
        if (lyrics_status != last_lyrics_status_) {
            last_lyrics_status_ = lyrics_status;
            lyrics_status_shown_at_ = std::chrono::steady_clock::now();
        }
        double status_age = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - lyrics_status_shown_at_).count();
        bool show_caption = status_age < 10.0 && !lyrics_status.empty();

        if (has_track_ && lyrics_w >= 6 && panel_h >= 3 && settings_.element_lyrics_placeholder_ball) {
            // Fill the panel with the audio-reactive sphere instead of
            // leaving it blank — reuses `bars` (already computed for the
            // main spectrum strip above, same frame) rather than running
            // a second independent audio analysis.
            int sphere_rows_h = show_caption ? panel_h - 1 : panel_h;
            auto sphere_rows = sphere_.render(lyrics_w, sphere_rows_h, bars, viz_dt_);
            std::string sphere_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, 0.5f);
            for (int i = 0; i < static_cast<int>(sphere_rows.size()) && i < panel_h; ++i) {
                lyric_rows[i] = sphere_ansi + pad_right(sphere_rows[i], lyrics_w) + "\x1b[0m";
            }
            if (show_caption) {
                int pad = std::max(0, (lyrics_w - display_width(lyrics_status)) / 2);
                lyric_rows[panel_h - 1] = pad_right(std::string(pad, ' ') + lyrics_status, lyrics_w);
            }
        } else if (show_caption) {
            int pad = std::max(0, (lyrics_w - display_width(lyrics_status)) / 2);
            lyric_rows[0] = pad_right(std::string(pad, ' ') + lyrics_status, lyrics_w);
        }
    } else {
        int active = 0;
        for (size_t i = 0; i < lines_copy.size(); ++i) {
            if (lines_copy[i].start_time <= elapsed) active = static_cast<int>(i);
            else break;
        }

        if (settings_.lyrics_animation == 4) {
            // Only active word: show nothing but whichever single word is
            // currently being sung, centered alone in the panel. Falls
            // back to the whole line if this song has no word-level sync
            // data at all (nothing finer to show).
            const LyricLine& al = lines_copy[active];
            std::string word = al.full_text;
            if (!al.words.empty()) {
                word = al.words.front().second;
                for (const auto& wt : al.words) if (wt.first <= elapsed) word = wt.second; // last one <= elapsed
            }
            word = apply_font_map(word, settings_.font_map);
            int pad = std::max(0, (lyrics_w - display_width(word)) / 2);
            std::string line = std::string(pad, ' ') + word;
            std::string colored = ansi_for(settings_.active_word_color) + pad_right(truncate_str(line, lyrics_w), lyrics_w) + "\x1b[0m";
            lyric_rows[panel_h / 2] = colored;
        } else if (settings_.lyrics_animation == 3) {
            // Only active line: same per-word rendering as the default
            // view, just without the scrolling context lines around it.
            auto wrapped = render_lyric_line_wrapped(lines_copy[active], elapsed, lyrics_w, true, settings_);
            int start_row = std::max(0, (panel_h - static_cast<int>(wrapped.size())) / 2);
            for (size_t i = 0; i < wrapped.size() && start_row + static_cast<int>(i) < panel_h; ++i) {
                lyric_rows[start_row + i] = wrapped[i];
            }
        } else {
            // Full (default) and Word-by-word/Letter-by-letter (which only
            // change render_lyric_line_wrapped's *content*, not this
            // scrolling layout) all share the same multi-line context view.
            //
            // Only wrap lines actually near the visible window — wrapping
            // the whole song every frame would be wasted work.
            int context = 6;
            int lo = std::max(0, active - context);
            int hi = std::min(static_cast<int>(lines_copy.size()) - 1, active + context);

            std::vector<std::string> flat_rows;
            int active_row_start = 0, active_row_count = 1;
            for (int li = lo; li <= hi; ++li) {
                auto wrapped = render_lyric_line_wrapped(lines_copy[li], elapsed, lyrics_w, li == active, settings_);
                if (li == active) {
                    active_row_start = static_cast<int>(flat_rows.size());
                    active_row_count = static_cast<int>(wrapped.size());
                }
                for (auto& r : wrapped) flat_rows.push_back(std::move(r));
            }

            int active_mid = active_row_start + active_row_count / 2;
            int start = active_mid - panel_h / 2;
            for (int row = 0; row < panel_h; ++row) {
                int idx = start + row;
                if (idx >= 0 && idx < static_cast<int>(flat_rows.size())) lyric_rows[row] = flat_rows[idx];
            }
        }
    }

    // --- assemble bordered block ---
    // NOTE: lyric_rows may contain ANSI color codes (word-highlighting),
    // so this assembles rows by direct concatenation of pre-padded pieces
    // rather than routing through box_line()/pad_right() — those count
    // UTF-8 codepoints for width, and ANSI escape bytes would be
    // miscounted as visible columns, throwing off alignment. Every piece
    // here (disk_frame/meta_rows/lyric_rows) is already padded to its own
    // exact width, so the concatenation is guaranteed to equal `inner`.
    std::vector<std::string> out;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    out.push_back(box_top("", total_width, border_ansi));

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::string sep_ansi = ansi_for(settings_.border_color, false) + sep + "\x1b[0m";
    for (int row = 0; row < panel_h; ++row) {
        std::string content = "";
        if (settings_.element_disk) {
            content += disk_frame[row] + sep_ansi;
        } else {
            content += "  ";
        }
        content += meta_rows[row];
        if (settings_.element_lyrics) {
            content += lyric_rows[row];
        }
        out.push_back(bar + " " + content + " " + bar);
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_progress_panel(int total_width) const {
    const int button_content_w = 9;
    const int button_total_w = button_content_w + 4; // "│ X │"
    int side_panel_w = settings_.element_dummy_buttons ? (button_total_w * 3) : 38;
    int main_total_w = std::max(24, total_width - side_panel_w);
    int wave_w = main_total_w - 4;

    double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    int active_cols = (total_sec_ > 0) ? static_cast<int>((elapsed / static_cast<double>(total_sec_)) * wave_w) : 0;
    active_cols = std::clamp(active_cols, 0, wave_w);

    int reveal_cols = wave_w;
    if (waveform_ready_) {
        double reveal_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - waveform_reveal_start_).count();
        if (reveal_sec < 0.7) {
            reveal_cols = static_cast<int>((reveal_sec / 0.7) * wave_w);
        }
    } else {
        reveal_cols = 0;
    }

    std::string color_played = ansi_for(settings_.progress_played_color);
    std::string color_unplayed = ansi_for(settings_.progress_remaining_color);
    std::string color_reset = "\x1b[0m";

    std::string top_wave, mid_wave, bot_wave;
    std::vector<int> waveform_levels;
    if (!waveform_envelope_.empty()) {
        waveform_levels = WaveformQuantizer::resample_for_ui(waveform_envelope_, wave_w);
    } else {
        waveform_levels.assign(wave_w, 0);
    }

    for (int i = 0; i < wave_w; ++i) {
        bool revealed = i < reveal_cols;
        int level = (revealed && i < static_cast<int>(waveform_levels.size())) ? waveform_levels[i] : 0;
        BrailleColumn col = WaveformQuantizer::get_column(level);
        if (i == 0) {
            std::string c = (i < active_cols) ? color_played : color_unplayed;
            top_wave += c; mid_wave += c; bot_wave += c;
        } else if (i == active_cols) {
            top_wave += color_unplayed; mid_wave += color_unplayed; bot_wave += color_unplayed;
        }
        top_wave += col.top; mid_wave += col.mid; bot_wave += col.bot;
    }
    top_wave += color_reset; mid_wave += color_reset; bot_wave += color_reset;

    // Generic fallback bar for when the waveform element is switched
    // off: a plain "[#####-------]" fill on the middle row, blank above
    // and below it, instead of leaving the panel showing a stray braille
    // waveform on two of its three rows (top_wave/mid_wave used to render
    // unconditionally regardless of this setting -- only the bottom row
    // respected the toggle, which is the "turning off waveform doesn't
    // actually turn it off" bug).
    std::string generic_blank(wave_w, ' ');
    std::string generic_bar;
    if (!settings_.element_waveform) {
        int inner_w = std::max(0, wave_w - 2); // account for the '[' and ']'
        int filled = (total_sec_ > 0) ? static_cast<int>((elapsed / static_cast<double>(total_sec_)) * inner_w) : 0;
        filled = std::clamp(filled, 0, inner_w);
        generic_bar = "[" + color_played + std::string(filled, '#') + color_reset
                     + color_unplayed + std::string(inner_w - filled, '-') + color_reset + "]";
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string button_ansi = ansi_for(settings_.button_color, false);

    auto button_mid = [&](const std::string& text) {
        std::string centered = button_ansi + center_pad(text, button_content_w) + "\x1b[0m";
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        return bar + " " + centered + " " + bar;
    };
    std::string play_label = (has_track_ && player_.is_paused()) ? "PLAY" : (has_track_ ? "PAUSE" : "PLAY");

    std::vector<std::string> out;
    if (settings_.element_dummy_buttons) {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi) + box_top("", button_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi));
    } else {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi));
    }
    
    std::string row1_content = settings_.element_waveform ? top_wave : generic_blank;
    std::string row2_content = settings_.element_waveform ? mid_wave : generic_bar;
    std::string row3_content = settings_.element_waveform ? bot_wave : generic_blank;

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    if (settings_.element_dummy_buttons) {
        out.push_back(bar + " " + row1_content + " " + bar
                      + button_mid("<<<") + button_mid(play_label) + button_mid(">>>"));
        out.push_back(bar + " " + row2_content + " " + bar
                      + box_bottom(button_total_w, "", border_ansi_bottom) + box_bottom(button_total_w, "", border_ansi_bottom)
                      + box_bottom(button_total_w, "", border_ansi_bottom));
    } else {
        out.push_back(bar + " " + row1_content + " " + bar);
        out.push_back(bar + " " + row2_content + " " + bar);
    }

    int vol = player_.volume();
    int vol_hashes = (vol * 20) / 100;
    std::string vol_bar_text = std::string(vol_hashes, '#') + std::string(20 - vol_hashes, '-');
    std::string vol_tail = "VOLUME BAR:[" + button_ansi + vol_bar_text + "\x1b[0m" + "] " + std::to_string(vol) + "%";
    int side_w = total_width - main_total_w;
    // pad_left counts raw bytes, so it can't be used once vol_tail carries
    // ANSI bytes -- pad by the *visible* width instead (the volume-bar
    // color fix below is what introduced the mismatch).
    int visible_w = display_width("VOLUME BAR:[" + vol_bar_text + "] " + std::to_string(vol) + "%");
    if (side_w > visible_w) vol_tail = std::string(side_w - visible_w, ' ') + vol_tail;

    std::string time_plain = "[ " + fmt_mmss(elapsed) + " ]" + settings_.box_horizontal + "[ " + fmt_mmss(static_cast<double>(total_sec_)) + " ]";
    // Manual box-bottom construction (rather than the shared box_bottom()
    // helper) so the timestamp text can carry its own color: box_bottom()
    // measures/pads the footer as plain text, and embedding ANSI bytes
    // into that path would get miscounted as visible columns.
    std::string ts_ansi = ansi_for(settings_.progress_timestamp_color.empty() ? settings_.border_color : settings_.progress_timestamp_color, false);
    std::string time_colored = ts_ansi + time_plain + "\x1b[0m";
    std::string prefix_plain = settings_.box_lower_left + settings_.box_horizontal + " " + time_plain + " ";
    int used = display_width(prefix_plain);
    int dashes_n = std::max(0, main_total_w - used - 1);
    std::string bottom_line = border_ansi_bottom + settings_.box_lower_left + settings_.box_horizontal + " "
                             + time_colored + border_ansi_bottom + " "; // re-apply border color -- time_colored's own reset above would otherwise leave the rest of this line uncolored
    for (int i = 0; i < dashes_n; ++i) bottom_line += settings_.box_horizontal;
    bottom_line += settings_.box_lower_right;
    bottom_line += "\x1b[0m";
    out.push_back(bar + " " + row3_content + " " + bar + vol_tail);
    out.push_back(bottom_line);
    return out;
}
std::vector<std::string> App::build_search_bar(int total_width) const {
    std::string label = (list_source_ == ListSource::Online) ? "SEARCH ONLINE" : "SEARCH LOCAL";

    std::string content;
    if (mode_ == Mode::Search) {
        content = "/" + search_buffer_ + "\u2588"; // block cursor
    } else if (list_source_ == ListSource::Online) {
        content = "/s:" + last_online_query_;
    } else {
        content = "/l:" + last_local_query_;
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    int search_w = total_width - 5;
    out.push_back(box_top(label, search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
    out.push_back(box_line(content, search_w, border_ansi) + border_ansi + settings_.box_vertical + " ✦ " + settings_.box_vertical + "\x1b[0m");
    out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    return out;
}

std::vector<std::string> App::build_list_panel(int total_width, int height) const {
    bool online = (list_source_ == ListSource::Online);
    std::string label = online ? "ONLINE RESULTS"
                                : "LOCAL AUDIO FILES (sort: " + std::string(sort_mode_name(local_sort_mode_)) + ")";
    size_t total = online ? online_view_.size() : local_view_.size();
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);

    std::vector<std::string> out;
    out.push_back(box_top(label, total_width, border_ansi));

    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll_ + row;
        std::string content;
        if (idx < static_cast<int>(total)) {
            if (online) {
                const auto& r = online_view_[idx];
                const int uploader_w = 18;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - uploader_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(r.title, settings_.font_map);
                std::string t_uploader = apply_font_map(r.uploader, settings_.font_map);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_uploader, uploader_w), uploader_w);
            } else {
                const auto& t = local_view_[idx];
                const int artist_w = 16;
                const int dur_w = 5;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - artist_w - 2 - dur_w);
                double dur = -1;
                // BUGFIX: this was always the parent-folder name, even
                // though the metadata panel already reads the real ffprobe
                // artist tag for the loaded track — now the list uses that
                // same real tag (probed lazily for visible rows), falling
                // back to the folder guess only until it's been probed.
                std::string artist = t.folder_artist;
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(t.path.string());
                    if (it != row_meta_cache_.end()) {
                        dur = it->second.duration_sec;
                        if (!it->second.artist.empty()) artist = it->second.artist;
                    }
                }
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(t.title, settings_.font_map);
                std::string t_artist = apply_font_map(artist, settings_.font_map);
                std::string t_dur = apply_font_map(fmt_mmss(dur), settings_.font_map);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_artist, artist_w), artist_w) + settings_.list_separator + " "
                        + t_dur;
            }
        }
        bool sel = (idx == selected_) && idx < static_cast<int>(total);
        bool is_playing_row = has_track_ && !online && idx < static_cast<int>(total)
                               && local_view_[idx].path == current_path_;
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = ansi_for(settings_.list_cursor_color) + bg_ansi_for(settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else if (is_playing_row) {
            std::string playing_ansi = ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color);
            out.push_back(bar + " " + playing_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }

    std::string footer;
    int remaining = static_cast<int>(total) - (scroll_ + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_queue_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;
    std::string title = queue_focus_ ? "QUEUE (focused)" : "QUEUE";
    out.push_back(box_top(title, total_width, border_ansi));

    if (queue_.empty()) {
        int mid_row = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid_row) {
                std::string text = apply_font_map("ADD TRACKS TO QUEUE", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string queue_ansi = ansi_for(settings_.queue_color, false);
            out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
        }
    } else {
        for (int row = 0; row < height; ++row) {
            int idx = queue_scroll_ + row;
            std::string content;
            bool is_row_playing = false;
            bool is_row_hovering = false;
            if (idx < static_cast<int>(queue_.size())) {
                const auto& q = queue_[idx];
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(q.title, settings_.font_map);
                content = pad_right(t_idx, 3) + settings_.list_separator + " " + t_title;
                is_row_playing = has_track_ && q.is_local && q.local_path == current_path_;
                is_row_hovering = queue_focus_ && (idx == queue_selected_);
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string color_ansi;
            if (is_row_hovering) color_ansi = ansi_for(settings_.queue_cursor_color) + bg_ansi_for(settings_.queue_cursor_bg_color);
            else if (is_row_playing) color_ansi = ansi_for(settings_.queue_playing_color, true) + bg_ansi_for(settings_.queue_playing_bg_color);
            else color_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
            out.push_back(bar + " " + color_ansi + padded + "\x1b[0m " + bar);
        }
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

// ---------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------

int App::main_frame_height(int w) const {
    // Same panels, same order, same extra lines as the Browse-mode branch
    // of render_frame() actually emits -- computed here (not hardcoded)
    // so the Settings panel's height always tracks the real player view
    // even as those panels change in the future, rather than drifting
    // out of sync with a stale magic number.
    int h = static_cast<int>(build_metadata_panel(w).size());
    h += static_cast<int>(build_progress_panel(w).size());
    h += static_cast<int>(build_search_bar(w).size());
    h += kListVisibleRows;
    h += 1; // blank separator line
    h += 1; // status/loading line -- reserved even when currently empty, so this doesn't jitter frame to frame
    return h;
}

void App::build_settings_screen(std::ostringstream& frame, int W, int player_h) const {
    // Literal port of the reference SettingsEngine::render() -- same
    // columns, same labels, same schema text, same group-blanking, same
    // divider, same tab-wrap algorithm, same gradient border (applied to
    // this panel's own chrome too, not just the preview swatches), same
    // preview formulas, same bottom hint/status lines, same cursor
    // placement formula. Deliberately not "improved" or restructured.
    if (W < 80) W = 80;
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    // MAX_Y is the row of this panel's own bottom border; the hint line
    // and status line render below it (rows MAX_Y+1, MAX_Y+2), so the
    // total screen rows used here is MAX_Y+2 -- set to exactly match
    // player_h (total rows the Browse-mode view renders), never taller,
    // never shorter, per explicit requirement. Floored modestly so a
    // pathologically small player view still leaves room to render the
    // tab bar/hint/status at all.
    int MAX_Y = std::max(player_h - 2, 10);
    auto B = [&](int y) {
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom,
                              (MAX_Y > 1) ? static_cast<float>(y - 1) / (MAX_Y - 1) : 0.0f, false);
    };
    auto pos = [&](int y, int x, const std::string& s) { frame << "\x1b[" << y << ";" << x << "H" << s; };
    auto repeat = [](const std::string& s, int n) {
        std::string r; r.reserve(s.size() * static_cast<size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) r += s;
        return r;
    };
    // Matches pad(): no truncation if s is already >= width, just like
    // the reference -- a longer-than-expected value overflows into the
    // next column rather than getting cut off. Values here are always
    // short in practice (numbers, true/false, single-char hotkeys).
    auto pad = [](const std::string& s, int width, bool left_align = true) {
        int ulen = display_width(s);
        if (ulen >= width) return s;
        std::string spaces(width - ulen, ' ');
        return left_align ? (s + spaces) : (spaces + s);
    };

    static const char* kTabNames[] = {"COLORS", "ON/OFF", "ANIMATION", "REFERENCE", "ABOUT APP"};

    // 1. Tab-wrap algorithm.
    std::vector<int> top_tabs, bot_tabs;
    int w_track = 22;
    for (int i = 0; i < 5; ++i) {
        int t_len = static_cast<int>(std::string(kTabNames[i]).size()) + 10;
        if (w_track + t_len < W - 2) { top_tabs.push_back(i); w_track += t_len; }
        else bot_tabs.push_back(i);
    }
    int w = 0;
    std::string l1, l2;
    auto add = [&](const std::string& t, const std::string& b, int width) { l1 += t; l2 += b; w += width; };
    add("\u250c\u2500 SETTINGS \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510", "\u2502                    \u2514", 22);
    for (size_t idx = 0; idx < top_tabs.size(); ++idx) {
        int i = top_tabs[idx];
        std::string lab = (i == settings_tab_) ? ("[" + std::string(kTabNames[i]) + "]") : kTabNames[i];
        int lab_len = static_cast<int>(lab.size());
        bool is_last = (idx == top_tabs.size() - 1);
        add("  " + lab + "  \u250c", repeat("\u2500", 4 + lab_len) + "\u2518", 5 + lab_len);
        if (is_last) add("\u2500", " ", 1);
        else add("\u2500\u2500\u2510", "  \u2514", 3);
    }
    if (w < W - 1) add(repeat("\u2500", W - 1 - w), repeat(" ", W - 1 - w), W - 1 - w);
    add("\u2510", "\u2502", 1);
    pos(1, 1, B(1) + l1 + R);
    pos(2, 1, B(2) + l2 + R);

    // 2. Content.
    int y = 3;
    if (settings_tab_ == 0) {
        static const char* grp[14]  = {"BORDER_COLOR", "DISK", "METADATA", "VIZ", "PROGRESS_BAR",
                                        "LIST", "", "", "QUEUE", "", "", "LYRICS", "", ""};
        static const char* l1n[14]  = {"TOP", "TOP", "KEY", "LEFT", "PLAYED",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "ACTIVE L  FG", "ACTIVE W  FG"};
        static const char* l2n[14]  = {"BOTTOM", "BOTTOM", "VAL", "RIGHT", "PENDING",
                                        "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG"};
        for (int i = 0; i < 14; ++i) {
            if (i == 5) { pos(y, 1, B(y) + "\u251c" + repeat("\u2500", W - 2) + "\u2524" + R); y++; }
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);

            pos(y, 3, pad(grp[i], 15)); pos(y, 18, ":");

            pos(y, 20, pad(l1n[i], 14, false)); pos(y, 35, ":");
            {
                bool sel = (i == settings_row_ && settings_col_ == 0 && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && settings_col_ == 0 && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 7) : pad(settings_get_value(i, 0), 7);
                pos(y, 38, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            }

            pos(y, 48, pad(l2n[i], 7, false)); pos(y, 56, ":");
            {
                bool sel = (i == settings_row_ && settings_col_ == 1 && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && settings_col_ == 1 && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 7) : pad(settings_get_value(i, 1), 7);
                pos(y, 59, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            }

            std::string valA = settings_get_value(i, 0), valB = settings_get_value(i, 1);
            std::string rt;
            if (i == 0) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 1) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 2) rt = ansi_for(valA, false) + "NAME : " + R + ansi_for(valB, false) + "SONG.MP3" + R;
            else if (i == 3) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 4) rt = ansi_for(valA, false) + "[###########" + R + ansi_for(valB, false) + "----------]" + R;
            else if (i >= 5 && i <= 12) rt = bg_ansi_for(valB) + ansi_for(valA, false) + "THIS IS AN EXAMPLE TEXT" + R;
            else if (i == 13) {
                std::string aL_F = ansi_for(settings_.active_line_color, false), aL_B = bg_ansi_for(settings_.active_line_bg_color);
                std::string aW_F = ansi_for(valA, false), aW_B = bg_ansi_for(valB);
                std::string iN_F = ansi_for(settings_.inactive_line_color, false), iN_B = bg_ansi_for(settings_.inactive_line_bg_color);
                rt = aL_B + aL_F + "THIS IS " + R + aW_B + aW_F + "AN " + R + iN_B + iN_F + "EXAMPLE TEXT" + R;
            }
            if (!rt.empty()) pos(y, 72, rt);
            y++;
        }
    } else if (settings_tab_ == 1 || settings_tab_ == 2) {
        static const char* onoff_l[7] = {"Eliment Disk", "Dummy Buttons", "Queue Display", "WaveForm",
                                          "Lyrics Engine", "Lyric Ball", "Visualizer"};
        static const char* anim_l[8] = {"Vis. Fluidity", "Waveform Style", "Disk Speed", "Playback Mode",
                                         "Vis. Degradation", "Vis. Viscosity", "Lyrics Alignment", "Lyrics Animation"};
        int count = (settings_tab_ == 1) ? 7 : 8;
        const char* const* labels = (settings_tab_ == 1) ? onoff_l : anim_l;
        for (int i = 0; i < count; ++i) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, pad(labels[i], 25)); pos(y, 32, ":");

            bool sel = (i == settings_row_ && mode_ != Mode::ColorEdit);
            bool ed = (i == settings_row_ && mode_ == Mode::ColorEdit);
            std::string v = ed ? pad(color_edit_buffer_, 20) : pad(settings_get_value(i, 0), 20);
            pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);

            if (sel && !settings_options_for(settings_tab_, i).empty()) pos(y, 57, "\x1b[90m< \u2194 >\x1b[0m");
            y++;
        }
    } else if (settings_tab_ == 3) {
        // Reference tab: the 11 editable hotkeys, then a blank divider,
        // then a read-only display of the font-mapping table (section 4
        // of the config, "A={A,a}" style) loaded from config.txt -- as
        // "A = A, a" rows. Combined they're usually taller than the
        // player view, so this scrolls as one list (viewport follows
        // settings_row_, centered) rather than ever growing the panel
        // past player_h.
        static const char* ref_l[kRefRowCount] = {"Open Settings", "Navigate Up", "Navigate Down", "Play / Pause",
                                                    "Next Track", "Prev Track", "Toggle Repeat", "Toggle Shuffle",
                                                    "Search Local", "Search Online", "Quit Application"};
        std::vector<char> letters;
        for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
        int display_count = kRefRowCount + 1 + static_cast<int>(letters.size()); // +1 for the divider row
        int visible = std::max(1, MAX_Y - 3);
        auto to_display_row = [&](int selectable_row) {
            return (selectable_row < kRefRowCount) ? selectable_row : selectable_row + 1;
        };
        int cur_display = to_display_row(settings_row_);
        int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));

        for (int r = 0; r < visible; ++r) {
            int disp = scroll + r;
            if (disp >= display_count) break;
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            if (disp == kRefRowCount) { y++; continue; } // blank divider row
            if (disp < kRefRowCount) {
                int i = disp;
                pos(y, 6, pad(ref_l[i], 25)); pos(y, 32, ":");
                bool sel = (i == settings_row_ && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 20) : pad(settings_get_value(i, 0), 20);
                pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            } else {
                int li = disp - kRefRowCount - 1;
                char c = letters[li];
                const auto& pair = settings_.font_map.at(c);
                int selectable_row = kRefRowCount + li;
                bool sel = (selectable_row == settings_row_);
                std::string line = std::string(1, c) + " = " + pair.first + ", " + pair.second;
                pos(y, 6, (sel ? HI : "") + line + R);
            }
            y++;
        }
    } else if (settings_tab_ == 4) {
        // About App: shows settings_.about_app_lines (loaded verbatim
        // from config.txt's trailing ClassTextAboutApp={...}; block, not
        // a hardcoded string), scrolled so it never exceeds player_h.
        int visible = std::max(1, MAX_Y - 3);
        int total = static_cast<int>(settings_.about_app_lines.size());
        int scroll = std::clamp(settings_row_, 0, std::max(0, total - visible));
        for (int r = 0; r < visible; ++r) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            int idx = scroll + r;
            if (idx < total) pos(y, 6, settings_.about_app_lines[idx]);
            y++;
        }
    }

    while (y < MAX_Y) { pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R); y++; }

    // 3. Bottom frame & overflow tabs.
    if (bot_tabs.empty()) {
        pos(y, 1, B(y) + "\u2514" + repeat("\u2500", W - 2) + "\u2518" + R);
    } else {
        std::string bot = "\u2514\u2500";
        for (int i : bot_tabs) bot += (i == settings_tab_ ? " [" + std::string(kTabNames[i]) + "] \u2500" : "  " + std::string(kTabNames[i]) + "  \u2500");
        int rem_bot = W - static_cast<int>(bot.size()); if (rem_bot < 1) rem_bot = 1;
        pos(y, 1, B(y) + bot + repeat("\u2500", rem_bot - 1) + "\u2518" + R);
    }
    y++;

    pos(y, 1, "\x1b[90m[TAB] Switch | [\u2191\u2193\u2190\u2192] Navigate/Cycle | [ENTER] Edit | [S] Save | [Q] Quit\x1b[0m");
    y++;
    if (!status_line_.empty()) pos(y, 1, "\x1b[32m" + status_line_ + "\x1b[0m");

    // 4. In-place text editing cursor placement.
    if (mode_ == Mode::ColorEdit) {
        if (settings_tab_ == 0) {
            int cy = 3 + settings_row_ + (settings_row_ >= 5 ? 1 : 0);
            int cx = (settings_col_ == 0) ? 38 : 59;
            frame << "\x1b[" << cy << ";" << (cx + static_cast<int>(color_edit_buffer_.size())) << "H\x1b[?25h";
        } else {
            int cy = 3 + settings_row_;
            frame << "\x1b[" << cy << ";" << (35 + static_cast<int>(color_edit_buffer_.size())) << "H\x1b[?25h";
        }
    }
}
// ---------------------------------------------------------------------
// Frame assembly
// ---------------------------------------------------------------------

std::string App::render_frame(TerminalIO& term) {
    int term_cols = term.cols();
    // Was clamped to a minimum of 80 regardless of the real terminal
    // width -- on a narrower phone terminal (the screenshots suggest
    // something closer to 40-46 visible columns), every line rendered
    // here would already be wider than the physical screen and get
    // wrapped by the terminal itself before the next frame's cursor-home
    // redraw overwrites it. That reads exactly like "truncated mid-word"
    // or "garbled" text even though nothing in this file actually cut it
    // off -- the terminal did, one line later than expected. Lowering the
    // floor so the app actually renders to the real width instead of
    // always assuming at least 80 columns are available.
    int W = std::clamp(term_cols, 40, 200);

    // The settings screen now renders at a fixed height (MAX_Y=21)
    // regardless of which tab is active -- matching the reference, which
    // does an unconditional \x1b[2J at the top of every settings frame
    // rather than tracking soft-clear/erase state at all. Settings is a
    // static, input-driven overlay (not the animated player view where
    // full-clear flicker actually matters), so always hard-clearing here
    // is simpler and correct, and sidesteps the tab-switch height-change
    // question entirely since height no longer varies by tab.
    bool hard_clear = (W != last_render_w_) || (mode_ != last_render_mode_) || force_redraw_;
    force_redraw_ = false; // one-shot -- consumed by this frame
    last_render_w_ = W;
    last_render_mode_ = mode_;
    const char* clear_prefix = hard_clear ? "\x1b[2J\x1b[H" : "\x1b[H";

    if (mode_ == Mode::Settings || mode_ == Mode::ColorEdit) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_settings_screen(frame, W, main_frame_height(std::max(W, 80)));
        return frame.str();
    }

    ensure_visible_row_meta();

    std::ostringstream frame;
    frame << clear_prefix;

    for (auto& l : build_metadata_panel(W)) frame << l << "\n";
    for (auto& l : build_progress_panel(W)) frame << l << "\n";
    for (auto& l : build_search_bar(W)) frame << l << "\n";

    int list_h = kListVisibleRows;
    if (settings_.element_queue) {
        int list_w = W / 2;
        int queue_w = W - list_w; // exact 50/50, remainder (odd W) goes to queue
        auto list_lines = build_list_panel(list_w, list_h);
        auto queue_lines = build_queue_panel(queue_w, list_h);
        size_t rows = std::max(list_lines.size(), queue_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < list_lines.size()) ? list_lines[i] : std::string(list_w, ' ');
            std::string r = (i < queue_lines.size()) ? queue_lines[i] : std::string(queue_w, ' ');
            frame << l << r << "\n";
        }
    } else {
        for (auto& l : build_list_panel(W, list_h)) frame << l << "\n";
    }

    frame << "\n";
    if (load_in_progress_.load() && load_stage_.load() == 1) {
        // Only the online resolve/download step shows a live status —
        // local loads are probe-only now (near-instant) and deliberately
        // silent, no "loading..." flash.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_started_at_).count();
        frame << "  resolving/downloading... (" << static_cast<int>(secs) << "s)\n";
    } else if (!status_line_.empty()) {
        frame << "  " << status_line_ << "\n";
    }

    frame << "\x1b[0J";
    return frame.str();
}

// ---------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------

int App::run() {
    if (!local_view_.empty()) {
        selected_ = 0;
        start_local_track(local_view_[0]);
    }
    TerminalIO term;
    last_frame_time_ = std::chrono::steady_clock::now();

    while (!quit_) {
        int key = term.poll_key();
        handle_key(key);

        poll_pending_search();
        poll_pending_load();
        poll_pending_waveform();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time_).count();
        last_frame_time_ = now;
        viz_dt_ = dt;

        if (has_track_) {
            player_.poll_elapsed();
            if (player_.finished()) advance_track();
        }
        // Disk only spins while something is actually playing — frozen
        // when idle or paused, per instruction.
        if (has_track_ && !player_.is_paused()) {
            angle_ = std::fmod(angle_ + kAngularVelocity * settings_.disk_rotation_speed * dt,
                               2.0 * 3.14159265358979323846);
        }

        std::cout << render_frame(term) << std::flush;
        // 25fps (was 12.5fps) — the 700ms waveform reveal animation only
        // got ~9 frames to work with at the old 80ms cadence, which
        // showed as a handful of visible ~11% jumps rather than a smooth
        // continuous expansion. Also smooths disk rotation and the
        // visualizer's motion generally. Text-frame rendering is cheap
        // enough that doubling the rate here is not a meaningful CPU/
        // battery concern.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    player_.stop();
    term.restore();
    save_settings(settings_);
    if (load_thread_.joinable()) load_thread_.join();
    if (search_thread_.joinable()) search_thread_.join();
    if (device_thread_.joinable()) device_thread_.join();
    std::cout << "\nbye.\n";
    return 0;
}

} // namespace muisc
