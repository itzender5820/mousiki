// The remote app's phone screen for the music player (see remote_server.h): a narrow, tall version of the main screen,
// drawn with the player's own pieces -- the braille disk (its own turn, speed 0.10), the meta data, the waveform progress
// bar, the dummy buttons (here they work) and boxes with the commands, in the colours of the theme in use. The phone only
// paints these lines and sends back the command of the box that was tapped.
#include "app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "path_utf8.h"
#include "remote_server.h"
#include "settings.h"
#include "terminal_ui.h"
#include "waveform.h"

namespace muisc {

namespace {

constexpr const char* kR = "\x1b[0m";

std::string plain_of(const std::string& s) {   // the text without colour escapes
    std::string o;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '\x1b' && k + 1 < s.size() && s[k + 1] == '[') {
            k += 2;
            while (k < s.size() && !(s[k] >= 0x40 && s[k] <= 0x7e)) ++k;
            continue;
        }
        o += s[k];
    }
    return o;
}
int vis(const std::string& s) { return display_width(plain_of(s)); }
std::string blanks(int n) { return std::string(static_cast<size_t>(std::max(0, n)), ' '); }

std::string mmss(double seconds) {
    if (seconds < 0) return "--:--";
    const int t = static_cast<int>(seconds);
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d", t / 60, t % 60);
    return b;
}

const char* mode_label(int play_mode) {
    switch (play_mode) {
        case 1: return "REPEAT";
        case 2: return "SHUFFLE";
        case 3: return "STOP";
        case 4: return "Q+STOP";
        default: return "LIST";
    }
}

} // namespace

void App::phone_publish() {
    int cols = 0, rows = 0;
    const auto now = std::chrono::steady_clock::now();
    if (!remote::net::screen_wanted(cols, rows)) { phone_last_ = {}; return; }
    // the disk turns at 0.10 while something plays (frozen when paused, like the disk on the main screen)
    if (phone_last_ != std::chrono::steady_clock::time_point{}) {
        const double dt = std::min(0.25, std::chrono::duration<double>(now - phone_last_).count());
        if (has_track_ && !player_.is_paused())
            phone_angle_ = std::fmod(phone_angle_ + kAngularVelocity * 0.10 * dt, 2.0 * 3.14159265358979323846);
    }
    phone_last_ = now;
    if (now - phone_built_ < std::chrono::milliseconds(70)) return;   // at most ~14 screens a second
    phone_built_ = now;
    std::vector<remote::net::Zone> zones;
    phone_pic_.active = false;
    const std::vector<std::string> lines = build_phone_screen(cols, rows, zones);
    remote::net::set_picture(phone_pic_.active && phone_view_ == 0, phone_pic_.x, phone_pic_.y, phone_pic_.w, phone_pic_.h, phone_pic_.key);
    remote::net::set_screen(audiobook_ ? "audiobook" : "player", cols, rows, lines, zones, settings_.list_color);
}

std::vector<std::string> App::build_phone_screen(int W, int H, std::vector<remote::net::Zone>& zones) const {
    W = std::max(W, 40);
    const int inner = W - 4;
    const std::string B = ansi_for(settings_.border_color, false);
    const std::string BB = ansi_for(settings_.border_color_bottom, false);
    const std::string head = ansi_for(settings_.header_color, true);
    const std::string legend = ansi_for(settings_.legend_color, false);
    const std::string bar = B + settings_.box_vertical + kR;
    const std::string k_col = settings_.meta_key_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_key_color);
    const std::string v_col = settings_.meta_val_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_val_color);
    const std::string played = ansi_for(settings_.progress_played_color);
    const std::string pending = ansi_for(settings_.progress_remaining_color);

    // ---- how many rows each part gets ----
    // Fixed rows: top, tabs, disk, separator, separator, 3 waveform rows, bottom = disk + 8; buttons 3, volume 1, 3 x 3 boxes = 13
    const int disk_h = disk_.height();
    int meta_rows = 5, gap = 1, disk_pad = 1;
    auto need = [&]() { return 1 + 1 + disk_pad * 2 + disk_h + 1 + meta_rows + 1 + 3 + 1 + gap + 3 + 1 + gap + 9; };
    while (need() > H && meta_rows > 3) --meta_rows;
    if (need() > H) disk_pad = 0;
    if (need() > H) gap = 0;
    while (need() > H && meta_rows > 1) --meta_rows;
    while (need() < H && meta_rows < 8) ++meta_rows;
    const int spare = std::max(0, H - need());
    const int top_pad = spare / 2;

    std::vector<std::string> out;
    auto row = [&](const std::string& content) {   // "│ content │" with content padded to the inner width
        out.push_back(bar + " " + content + blanks(inner - vis(content)) + kR + " " + bar);
    };
    auto sep = [&](const std::string& label) {      // "├─ LABEL ───┤"
        std::string s = B + "├" + settings_.box_horizontal + " " + label + " ";
        const int dashes = std::max(0, W - 2 - display_width(settings_.box_horizontal) - display_width(label) - 2);
        s += B;
        for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
        out.push_back(s + "┤" + kR);
    };
    // ---- a row of three boxes: [title] label, each a tap zone ----
    struct Btn { std::string key, label, cmd, arg; };
    auto boxes = [&](const std::vector<Btn>& b) {
        const int n = static_cast<int>(b.size());
        const int bw = std::max(10, (W - 2) / n);
        const int left = (W - bw * n) / 2;
        std::string l0 = blanks(left), l1 = blanks(left), l2 = blanks(left);
        const int y = static_cast<int>(out.size());
        for (int i = 0; i < n; ++i) {
            l0 += box_top(b[i].key, bw, B);
            const std::string lab = truncate_str(b[i].label, bw - 4);
            const int pad = bw - 4 - display_width(lab);
            l1 += bar + " " + B + blanks(pad / 2) + lab + blanks(pad - pad / 2) + kR + " " + bar;
            l2 += box_bottom(bw, "", BB);
            if (!b[i].cmd.empty()) zones.push_back({left + i * bw, y, bw, 3, b[i].cmd, b[i].arg});
        }
        out.push_back(l0 + blanks(W - left - bw * n));
        out.push_back(l1 + blanks(W - left - bw * n));
        out.push_back(l2 + blanks(W - left - bw * n));
    };


    // ---- the LISTS screen (the LISTS box of the main screen opens it) ----
    // On top the local list (as sorted / filtered on the computer; tap a title: it plays), below it, smaller, the saved
    // playlists (tap one: its titles go into the queue). A tap on a pane that is not in focus only gives it the focus;
    // UP / DOWN page through the pane in focus, a whole page at a time. At the bottom CLEAR QUEUE (tap twice).
    if (phone_view_ == 1 && audiobook_) {
        // The audio book's LISTS: the library as in the big library overlay -- the folders (open / close with a tap), the books
        // and their chapters indented below them; a tap on a chapter plays it from its start. One pane, paged by UP / DOWN.
        out.clear();
        zones.clear();
        const std::vector<FoRow> rows = phone_book_rows();
        const int nr = static_cast<int>(rows.size());
        int per = 0, per2 = 0;
        remote::net::phone_lists_rows(H, per, per2);
        per = std::max(1, per + per2 - 1);
        const int pages = std::max(1, (nr + per - 1) / per);
        const int page = std::clamp(phone_page_, 0, pages - 1);
        const std::string on_col = ansi_for(settings_.tab_current_color, true);
        const std::string now_col = ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color);
        out.push_back(box_top("LIBRARY", W, B));
        zones.push_back({0, 0, W, 1, "@menu", ""});
        row(phone_note_.empty() ? legend + truncate_str("tap a folder or a chapter (plays)", inner) + kR
                                : "\x1b[38;5;246m" + truncate_str(phone_note_, inner) + kR);
        {
            const std::string label = "▶ AUDIO BOOKS (" + std::to_string(nr) + ")  " + std::to_string(page + 1) + "/" + std::to_string(pages);
            const std::string lab = truncate_str(label, W - 8);
            std::string h = B + "├" + settings_.box_horizontal + " " + on_col + lab + kR + " " + B;
            const int dashes = std::max(0, W - 2 - display_width(settings_.box_horizontal) - display_width(lab) - 2);
            for (int k = 0; k < dashes; ++k) h += settings_.box_horizontal;
            out.push_back(h + "┤" + kR);
        }
        for (int i = 0; i < per; ++i) {
            const int k = page * per + i;
            if (k >= nr) { row(i == 0 && nr == 0 ? legend + truncate_str("no books (AUDIO BOOK PATH in the settings)", inner) + kR : ""); continue; }
            const FoRow& r = rows[static_cast<size_t>(k)];
            const bool now = !r.is_dir && has_track_ && current_is_local_ && path_utf8(current_path_) == r.path;
            const std::string indent = blanks(std::min(r.depth, 6) * 2);
            std::string head_txt = r.is_dir ? (r.expanded ? "▾ " : "▸ ") : (now ? "▶ " : "  ");
            const std::string cnt = r.is_dir ? " (" + std::to_string(r.count) + ")" : std::string();
            const std::string name = truncate_str(apply_font_map(r.label, settings_.font_map), std::max(4, inner - display_width(indent) - 2 - display_width(cnt)));
            const std::string text = indent + head_txt + name + cnt;
            if (now) row(now_col + pad_right(text, inner) + kR);
            else if (r.is_dir) row(k_col + text + kR);
            else row(v_col + text + kR);
            zones.push_back({0, static_cast<int>(out.size()) - 1, W, 1, r.is_dir ? "book-open" : "track", std::to_string(k + 1)});
        }
        out.push_back(box_bottom(W, "", BB));
        while (static_cast<int>(out.size()) < H - 3) out.push_back(blanks(W));
        boxes({{"", "▲ UP", "remote-scroll", "-1"}, {"", "BACK", "remote-view", "0"}, {"", "DOWN ▼", "remote-scroll", "+1"}});
        if (static_cast<int>(out.size()) > H) out.resize(static_cast<size_t>(H));
        for (auto& z : zones) if (z.y + z.h > H) z.h = std::max(0, H - z.y);
        zones.erase(std::remove_if(zones.begin(), zones.end(), [](const remote::net::Zone& z) { return z.h <= 0; }), zones.end());
        return panel_gradient(out);
    }
    if (phone_view_ == 1) {
        out.clear();
        zones.clear();
        const std::vector<PlaylistSummary> pls = playlist_summaries();
        const int nt = static_cast<int>(local_view_.size()), np = static_cast<int>(pls.size());
        int per = 0, per2 = 0;
        remote::net::phone_lists_rows(H, per, per2);
        const int pages = std::max(1, (nt + per - 1) / per), pages2 = std::max(1, (np + per2 - 1) / per2);
        const int page = std::clamp(phone_page_, 0, pages - 1), page2 = std::clamp(phone_page2_, 0, pages2 - 1);
        const std::string on_col = ansi_for(settings_.tab_current_color, true), off_col = ansi_for(settings_.tab_other_color, false);
        const std::string now_col = ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color);
        const std::string q_n = std::to_string(queue_.size());

        out.push_back(box_top("LISTS", W, B));
        zones.push_back({0, 0, W, 1, "@menu", ""});
        row(phone_note_.empty() ? legend + truncate_str("queue: " + q_n + (queue_.size() == 1 ? " title" : " titles") + "  |  tap a list to page it", inner) + kR
                                : "\x1b[38;5;246m" + truncate_str(phone_note_, inner) + kR);
        // a pane header "├─ ▶ NAME (n)  1/3 ───┤": the focus marker and colour of the tab strips; tapping it gives the focus
        auto header = [&](const std::string& name, int count, int pg, int pgs, bool focus, int which) {
            const std::string label = (focus ? "▶ " : "  ") + name + " (" + std::to_string(count) + ")  " + std::to_string(pg + 1) + "/" + std::to_string(pgs);
            const std::string lab = truncate_str(label, W - 8);
            std::string s = B + "├" + settings_.box_horizontal + " " + (focus ? on_col : off_col) + lab + kR + " " + B;
            const int dashes = std::max(0, W - 2 - display_width(settings_.box_horizontal) - display_width(lab) - 2);
            for (int k = 0; k < dashes; ++k) s += settings_.box_horizontal;
            out.push_back(s + "┤" + kR);
            if (!focus) zones.push_back({0, static_cast<int>(out.size()) - 1, W, 1, "remote-focus", std::to_string(which)});
        };
        // the local list: one title per line, the playing one marked
        header("LOCAL AUDIO FILES", nt, page, pages, phone_focus_ == 0, 0);
        const int list_y = static_cast<int>(out.size());
        for (int i = 0; i < per; ++i) {
            const int k = page * per + i;
            if (k >= nt) { row(i == 0 && nt == 0 ? legend + truncate_str("no local titles (PATHS in the settings)", inner) + kR : ""); continue; }
            const LocalTrack& lt = local_view_[static_cast<size_t>(k)];
            const bool now = has_track_ && current_is_local_ && lt.path == current_path_;
            const std::string num = pad_right(std::to_string(k + 1), 5);
            const std::string name = truncate_str((now ? "▶ " : "") + apply_font_map(lt.title, settings_.font_map), inner - 5);
            if (now) row(k_col + num + kR + now_col + pad_right(name, inner - 5) + kR);
            else row(k_col + num + kR + v_col + name + kR);
            if (phone_focus_ == 0) zones.push_back({0, static_cast<int>(out.size()) - 1, W, 1, "track", std::to_string(k + 1)});
        }
        if (phone_focus_ != 0) zones.push_back({0, list_y, W, per, "remote-focus", "0"});
        // the saved playlists: one per line with the number of titles
        header("PLAYLISTS", np, page2, pages2, phone_focus_ == 1, 1);
        const int pl_y = static_cast<int>(out.size());
        for (int i = 0; i < per2; ++i) {
            const int k = page2 * per2 + i;
            if (k >= np) { row(i == 0 && np == 0 ? legend + truncate_str("no saved playlists yet (SHIFT+p on the computer)", inner) + kR : ""); continue; }
            const PlaylistSummary& ps = pls[static_cast<size_t>(k)];
            const std::string count = std::to_string(ps.track_count);
            const std::string num = pad_right(std::to_string(k + 1), 5);
            const std::string name = truncate_str(apply_font_map(ps.name, settings_.font_map), inner - 5 - display_width(count) - 2);
            row(k_col + num + kR + v_col + name + kR + blanks(inner - 5 - display_width(name) - display_width(count)) + legend + count + kR);
            if (phone_focus_ == 1) zones.push_back({0, static_cast<int>(out.size()) - 1, W, 1, "playlist", std::to_string(k + 1)});
        }
        if (phone_focus_ != 1) zones.push_back({0, pl_y, W, per2, "remote-focus", "1"});
        out.push_back(box_bottom(W, "", BB));
        while (static_cast<int>(out.size()) < H - 6) out.push_back(blanks(W));
        boxes({{"", "▲ UP", "remote-scroll", "-1"}, {"", "BACK", "remote-view", "0"}, {"", "DOWN ▼", "remote-scroll", "+1"}});
        {
            const bool armed = phone_clear_armed_ != std::chrono::steady_clock::time_point{} &&
                               std::chrono::steady_clock::now() - phone_clear_armed_ < std::chrono::seconds(5);
            boxes({{"", armed ? "TAP AGAIN TO CLEAR " + q_n : "CLEAR QUEUE (" + q_n + ")", "clear-queue", armed ? "1" : "0"}});
        }
        if (static_cast<int>(out.size()) > H) out.resize(static_cast<size_t>(H));
        for (auto& z : zones) if (z.y + z.h > H) z.h = std::max(0, H - z.y);
        zones.erase(std::remove_if(zones.begin(), zones.end(), [](const remote::net::Zone& z) { return z.h <= 0; }), zones.end());
        return panel_gradient(out);
    }

    for (int i = 0; i < top_pad; ++i) out.push_back(blanks(W));

    // ---- title + mode tabs ----
    out.push_back(box_top("MOUSIKI PLUS", W, B));
    zones.push_back({0, static_cast<int>(out.size()) - 1, W, 1, "@menu", ""});   // the title: the remote's own settings
    {
        const std::string cur = ansi_for(settings_.tab_current_color, false), other = ansi_for(settings_.tab_other_color, false);
        // the modes that are on, in the order of the mode key; the one in use in brackets, a tap on another one goes to it
        // (the next mode in the order: switch-mode, the previous one: switch-mode -1)
        static const char* const kNames[3] = {"PLAYER", "RADIO", "BOOKS"};
        const int me = audiobook_ ? 2 : 0;
        int mypos = 0;
        for (int i = 0; i < 3; ++i) if (settings_.mode_order[i] == me) mypos = i;
        std::string tabs;
        struct Tab { int x, w; const char* arg; };
        std::vector<Tab> hits;
        for (int i = 0; i < 3; ++i) {
            const int m = settings_.mode_order[i];
            if (m != me && !settings_.use_mode[m]) continue;
            const std::string name = m == me ? std::string("[") + kNames[m] + "]" : std::string(kNames[m]);
            if (!tabs.empty()) tabs += "  ";
            hits.push_back({vis(tabs), static_cast<int>(name.size()), m == me ? "" : (i == (mypos + 1) % 3 ? "1" : (i == (mypos + 2) % 3 ? "-1" : "1"))});
            tabs += (m == me ? cur : other) + name + kR;
        }
        const std::string hint = legend + "Switch >>" + kR;   // a tap target: the computer's key (SHIFT and +) means nothing on a phone
        row(tabs + blanks(inner - vis(tabs) - vis(hint)) + hint);
        const int ty = static_cast<int>(out.size()) - 1;
        for (const Tab& t : hits) if (*t.arg) zones.push_back({2 + t.x, ty, t.w, 1, "switch-mode", t.arg});
        zones.push_back({2 + inner - vis(hint), ty, vis(hint), 1, "switch-mode", ""});
    }

    // ---- the disk ----
    for (int i = 0; i < disk_pad; ++i) row("");
    {
        if (audiobook_) {
            // the audio book: the book's cover or the standard picture takes the disk's place (the same picture as on the computer)
            std::string pkey;
            const std::string* png = book_phone_png(pkey);
            if (png) {   // the phone draws the real picture (full resolution) over these rows: it fetches it once per key
                if (!remote::net::picture_has(pkey)) remote::net::picture_store(pkey, *png);
                phone_pic_ = {true, 2, static_cast<int>(out.size()), inner, disk_h, pkey};
                for (int i = 0; i < disk_h; ++i) row("");
                zones.push_back({2, static_cast<int>(out.size()) - disk_h, inner, disk_h, "play-pause", ""});
            } else {
            int pw = 0, ph = 0;
            std::vector<std::string> pic = book_phone_picture(pw, ph);
            if (pic.empty()) pic.assign(static_cast<size_t>(std::max(1, ph)), std::string());   // the cover is being read
            const int left = std::max(0, (inner - pw) / 2);
            const int top = static_cast<int>(out.size());
            for (int i = 0; i < disk_h; ++i) {
                const int ai = i - (disk_h - static_cast<int>(pic.size())) / 2;
                row(ai >= 0 && ai < static_cast<int>(pic.size()) ? blanks(left) + pic[static_cast<size_t>(ai)] : std::string());
            }
            zones.push_back({2 + left, top, std::max(1, pw), disk_h, "play-pause", ""});
            }
        } else if (const std::string* cpng = [&]() -> const std::string* {   // the album cover takes the disk's place (as on the computer)
                       std::string ck;
                       const std::string* p = player_phone_png(ck);
                       if (p && !remote::net::picture_has(ck)) remote::net::picture_store(ck, *p);
                       phone_cover_key_ = p ? ck : std::string();
                       return p;
                   }()) {
            phone_pic_ = {true, 2, static_cast<int>(out.size()), inner, disk_h, phone_cover_key_};
            for (int i = 0; i < disk_h; ++i) row("");
            zones.push_back({2, static_cast<int>(out.size()) - disk_h, inner, disk_h, "play-pause", ""});
            (void)cpng;
        } else {
        std::vector<std::string> frame = disk_.frame(phone_angle_);
        const int dw = disk_.width();
        const int left = std::max(0, (inner - dw) / 2);
        for (size_t r = 0; r < frame.size(); ++r) {
            const float t = frame.size() > 1 ? static_cast<float>(r) / static_cast<float>(frame.size() - 1) : 0.0f;
            row(blanks(left) + gradient_ansi(settings_.disk_color, settings_.disk_color_end, t) + frame[r] + kR);
        }
        const int disk_top = static_cast<int>(out.size()) - static_cast<int>(frame.size());
        zones.push_back({2 + left, disk_top, dw, static_cast<int>(frame.size()), "play-pause", ""});   // the disk itself: play / pause
        }
    }
    for (int i = 0; i < disk_pad; ++i) row("");

    // ---- meta data ----
    sep("META DATA");
    {
        std::vector<std::string> lines;
        if (has_track_) {
            const TrackMetadata md = display_metadata();
            const int vw = std::max(4, inner - 12);
            auto kv = [&](const char* label, const std::string& value) {
                if (value.empty() || value == "-") return;
                lines.push_back(k_col + pad_right(apply_font_map(label, settings_.font_map), 10) + kR + ": " + v_col +
                                truncate_str(apply_font_map(value, settings_.font_map), vw) + kR);
            };
            kv("Title", md.name);
            kv("Artist", md.artist);
            kv("Album", md.album);
            kv("Year", md.year);
            kv("Sampling", metadata_.sampling);
            kv("Format", format_text());
            kv("Type", metadata_.type);
            kv("File size", metadata_.file_size);
        }
        const int top_blank = has_track_ ? 0 : (meta_rows - 1) / 2;
        for (int i = 0; i < meta_rows; ++i) {
            if (!has_track_) {
                const std::string msg = i == top_blank ? "No Track Loaded" : i == top_blank + 1 ? "Hit [" + hotkey_text("HKeyPlayNextSong", "n") + "] to proceed" : std::string();
                row(!msg.empty() ? blanks((inner - display_width(msg)) / 2) + ansi_for(settings_.list_color) + msg + kR : std::string());
            } else row(i < static_cast<int>(lines.size()) ? lines[static_cast<size_t>(i)] : std::string());
        }
    }

    // ---- progress bar (the braille waveform, or the plain bar when the waveform is off) ----
    sep("PROGRESS BAR");
    {
        const double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
        const int wave_w = inner;
        const int active = total_sec_ > 0 ? std::clamp(static_cast<int>(elapsed / static_cast<double>(total_sec_) * wave_w), 0, wave_w) : 0;
        const int first_row = static_cast<int>(out.size());
        if (settings_.element_waveform) {
            std::vector<int> levels = !waveform_envelope_.empty() && waveform_ready_ ? WaveformQuantizer::resample_for_ui(waveform_envelope_, wave_w)
                                                                                      : std::vector<int>(static_cast<size_t>(wave_w), 0);
            std::string top, mid, bot;
            for (int i = 0; i < wave_w; ++i) {
                const BrailleColumn c = WaveformQuantizer::get_column(i < static_cast<int>(levels.size()) ? levels[static_cast<size_t>(i)] : 0);
                if (i == 0 || i == active) { const std::string& a = i < active ? played : pending; top += a; mid += a; bot += a; }
                top += c.top; mid += c.mid; bot += c.bot;
            }
            row(top + kR);
            row(mid + kR);
            row(bot + kR);
        } else {
            const int bw = std::max(0, inner - 2), filled = total_sec_ > 0 ? std::clamp(static_cast<int>(elapsed / static_cast<double>(total_sec_) * bw), 0, bw) : 0;
            row("");
            row("[" + played + std::string(static_cast<size_t>(filled), '#') + kR + pending + std::string(static_cast<size_t>(bw - filled), '-') + kR + "]");
            row("");
        }
        // tapping the waveform jumps there (seek by the difference)
        if (has_track_ && total_sec_ > 0) {
            for (int c = 0; c < wave_w; c += 1) {
                const double target = (static_cast<double>(c) + 0.5) / wave_w * static_cast<double>(total_sec_);
                char arg[16];
                std::snprintf(arg, sizeof arg, "%+.0f", target - elapsed);
                zones.push_back({2 + c, first_row, 1, 3, "seek", arg});
            }
        }
        const std::string ts = ansi_for(settings_.progress_timestamp_color.empty() ? settings_.border_color : settings_.progress_timestamp_color, false);
        const std::string time_plain = "[ " + mmss(elapsed) + " ]" + settings_.box_horizontal + "[ " + mmss(static_cast<double>(total_sec_)) + " ]";
        std::string bottom = BB + settings_.box_lower_left + settings_.box_horizontal + " " + ts + time_plain + kR + " " + BB;
        const int used = display_width(settings_.box_lower_left + settings_.box_horizontal + " " + time_plain + " ");
        for (int i = 0; i < W - used - 1; ++i) bottom += settings_.box_horizontal;
        out.push_back(bottom + settings_.box_lower_right + kR);
    }

    for (int i = 0; i < gap; ++i) out.push_back(blanks(W));
    // the dummy buttons of the main screen -- working ones: seek back, play / pause, seek forward (as the arrow keys)
    const std::string play_label = (has_track_ && !player_.is_paused()) ? "PAUSE" : "PLAY";
    boxes({{"", "<<<", "seek", "-5"}, {"", play_label, "play-pause", ""}, {"", ">>>", "seek", "+5"}});

    // the volume bar (tap: mute / unmute)
    {
        const int vol = muted_ ? 0 : player_.volume();
        const int hashes = std::clamp(vol * 20 / 100, 0, 20);
        const std::string v = B + "VOLUME BAR:[" + kR + played + std::string(static_cast<size_t>(hashes), '#') + kR + pending +
                              std::string(static_cast<size_t>(20 - hashes), '-') + kR + B + "]" + kR + " " + (muted_ ? std::string("muted") : std::to_string(vol) + "%");
        const int left = std::max(0, (W - vis(v)) / 2);
        out.push_back(blanks(left) + v + blanks(W - left - vis(v)));
        zones.push_back({left, static_cast<int>(out.size()) - 1, vis(v), 1, "toggle-mute", ""});
    }
    for (int i = 0; i < gap; ++i) out.push_back(blanks(W));

    boxes({{hotkey_text("HKeyPlayPreviousSong", "b"), "PREV", "prev", ""},
           {hotkey_text("HKeyShuffleNext", "#"), "RANDOM", "shuffle", ""},
           {hotkey_text("HKeyPlayNextSong", "n"), "NEXT", "next", ""}});
    boxes({{hotkey_text("HKeyDecreaseVolume", "-"), "VOL -", "volume-down", ""},
           {hotkey_text("HKeyToggleMute", "x"), muted_ ? "UNMUTE" : "MUTE", "toggle-mute", ""},
           {hotkey_text("HKeyIncreaseVolume", "+"), "VOL +", "volume-up", ""}});
    boxes({{hotkey_text("HKeyCyclePlayMode", "m"), mode_label(settings_.play_mode), "playback-mode", ""},
           {"", "LISTS", "remote-view", "1"},
           {"", "MANUAL", "@manual", ""}});

    while (static_cast<int>(out.size()) < H) out.push_back(blanks(W));
    if (static_cast<int>(out.size()) > H) out.resize(static_cast<size_t>(H));
    // a screen too short for everything: what was cut off cannot be tapped either
    zones.erase(std::remove_if(zones.begin(), zones.end(), [&](const muisc::remote::net::Zone& z) { return z.y + z.h > H || z.x + z.w > W; }), zones.end());
    return panel_gradient(out);   // BorderGradient=smooth: the border lines fade from the top to the bottom colour
}

} // namespace muisc
