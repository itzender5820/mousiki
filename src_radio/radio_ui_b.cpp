// Radio mode screen (part of radio_ui.cpp, split for the build time; the helpers are declared in radio_ui_internal.h).
#include "radio_ui_internal.h"

namespace muisc::radio {
namespace detail {


std::vector<std::string> build_browse(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows) {
    const BrowseModel& bm = m.browse;
    std::vector<std::string> out;
    const int left_w = W / 2;
    const int right_w = W - left_w;                       // equal panes, the odd column (if any) goes right
    const std::string list_ansi = s.c.list_fg.empty() ? "" : ansi_fg(s.c.list_fg);

    // --- the six input panes ---------------------------------------------------------------------
    auto field_box = [&](int i, int w) {
        const bool focused = static_cast<int>(bm.focus) == i;
        std::vector<std::string> b;
        b.push_back(box_top(s, w, pane_title(s, kBrowseTitles[i], focused), s.border));
        b.push_back(box_line(s, field_row(list_ansi, "", bm.text[i], bm.edit[i], focused, w - 4, "", s.header, false), s.border));
        b.push_back(box_bottom(s, w, "", s.border_bottom));
        return b;
    };
    for (int r = 0; r < 3; ++r) {
        const auto L = field_box(2 * r, left_w);
        const auto R = field_box(2 * r + 1, right_w);
        for (size_t i = 0; i < L.size(); ++i) out.push_back(L[i] + R[i]);
    }

    // --- RESULTS | STATION INFO --------------------------------------------------------------------
    const int fixed_rows = 9 + 3;                          // six panes + hint line + status line + "tuned in" line
    const int list_rows = std::max(6, rows - fixed_rows - 2);
    static const std::vector<BrowseStation> kNoResults;
    const std::vector<BrowseStation>& res = bm.results ? *bm.results : kNoResults;
    const int n = static_cast<int>(res.size());
    const int cursor = std::clamp(bm.cursor, 0, std::max(0, n - 1));
    const int first = follow_scroll(scroll_id(6), cursor, list_rows, n);
    const bool results_focus = bm.focus == BrowseFocus::Results;

    std::string title = "RESULTS";
    if (bm.state == BrowseState::Searching) title += " (searching ...)";
    else if (bm.state == BrowseState::Done) title += " (" + std::to_string(n) + (n >= kBrowseLimit ? "+" : "") + ")";
    else if (bm.state == BrowseState::Error) title += " (error)";

    // streams that are already in the station list (same stream = same_stream_url(), not just the same string)
    auto is_listed = [&](const BrowseStation& b) {
        if (!m.stations) return false;
        for (const auto& st : *m.stations) if (same_stream_url(st.url, b.stream_url()) || same_stream_url(st.url, b.url)) return true;
        return false;
    };

    std::vector<std::string> left;
    left.push_back(box_top(s, left_w, pane_title(s, title, results_focus), s.border));
    {
        const int inner = left_w - 4;
        const std::string sep = s.c.list_separator;
        const int sepw = display_width(sep);
        const int tail_w = 8;                              // "128k AAC"
        const int cc_w = 2;                                // country code
        const int flex = std::max(8, inner - (2 + 3 * (sepw + 2) + cc_w + tail_w));
        const int name_w = flex * 58 / 100;
        const int tags_w = flex - name_w;
        bool any_hover = false;
        for (int r = 0; r < list_rows; ++r) {
            const int vi = first + r;
            if (vi >= n) {
                std::string msg;
                if (n == 0 && r == 0) {
                    if (bm.state == BrowseState::Searching) msg = "Searching radio-browser.info ...";
                    else if (bm.state == BrowseState::Error) msg = bm.message;
                    else if (bm.state == BrowseState::Done) msg = "No stations found. Try fewer or broader terms.";
                    else msg = "Fill in one or more panes above and press ENTER.";
                }
                left.push_back(box_line(s, seg_line({{bm.state == BrowseState::Error ? s.legend : s.header, msg}}, inner), s.border));
                continue;
            }
            const BrowseStation& b = res[static_cast<size_t>(vi)];
            const bool hovered = vi == cursor && results_focus;
            any_hover = any_hover || hovered;
            const std::string name_shown = hovered ? marquee_or_truncate(bm.marquee_row, bm.marquee_since, m.t_sec, shown_text(b.name), name_w, vi)
                                                   : pad_right(truncate_str(shown_text(b.name), name_w), name_w);
            std::string tail = b.codec;
            if (b.bitrate > 0) tail = std::to_string(b.bitrate) + "k " + b.codec;
            const bool listed = is_listed(b);
            const std::string text = std::string(listed ? "\u2022" : " ") + " " + name_shown + " " + sep + " "
                                   + pad_right(utf8_take(b.countrycode, cc_w), cc_w) + " " + sep + " "
                                   + pad_right(truncate_str(tail, tail_w), tail_w) + " " + sep + " "
                                   + pad_right(truncate_str(b.tags, tags_w), tags_w);
            // Lit whatever way the station was tuned (this menu, the main STATIONS list, a preset ...): same_stream_url()
            // compares either of the result's addresses with the tuned one, ignoring http/https, "/" and tracking queries.
            const bool tuned = st.state != StreamState::Idle && !st.tuned_url.empty()
                            && (same_stream_url(b.stream_url(), st.tuned_url) || same_stream_url(b.url, st.tuned_url));
            // same look as the main screen's STATIONS pane: the tuned station is lit, the cursor row wins over it
            std::string style = tuned ? s.list_playing : s.list;
            if (!b.online && !tuned) style = s.header;     // stations that failed their last check are dimmed
            if (hovered) style = s.list_cursor;
            left.push_back(box_line(s, seg_line({{style, text}}, inner), s.border));
        }
        if (!any_hover) bm.marquee_row = -1;               // nothing hovered: the next hover restarts the scroll
    }
    const int hidden = std::max(0, n - (first + list_rows));
    left.push_back(box_bottom(s, left_w, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));

    // station info of the hovered result (follows the cursor whichever pane has the keyboard)
    std::vector<std::string> right;
    right.push_back(box_top(s, right_w, "STATION INFO", s.border));
    {
        const int inner = right_w - 4;
        std::vector<std::string> info;
        auto kv = [&](const std::string& label, const std::string& value, int max_lines = 1) {
            const auto lines = wrap_lines(value.empty() ? "-" : value, std::max(8, inner - 11), max_lines);
            for (size_t i = 0; i < lines.size(); ++i)
                info.push_back(seg_line({{i == 0 ? s.key : "", pad_right(i == 0 ? label : "", 9)}, {"", i == 0 ? ": " : "  "}, {s.val, lines[i]}}, inner));
        };
        if (n > 0) {
            const BrowseStation& b = res[static_cast<size_t>(cursor)];
            std::string country = b.country;
            if (!b.countrycode.empty()) country += (country.empty() ? "" : " ") + std::string("(") + b.countrycode + ")";
            std::string tags = b.tags;
            for (size_t i = 0; i + 1 < tags.size(); ++i) if (tags[i] == ',' && tags[i + 1] != ' ') { tags.insert(i + 1, " "); }
            std::string rate = b.bitrate > 0 ? std::to_string(b.bitrate) + " kbps" : "";
            if (!b.codec.empty()) rate += (rate.empty() ? "" : " ") + b.codec;
            if (b.hls) rate += " (HLS)";
            kv("Name", shown_text(b.name), 2);
            kv("Country", country);
            kv("State", b.state);
            kv("Language", b.language);
            kv("Tags", tags, 3);
            kv("Bitrate", rate);
            kv("Votes", std::to_string(b.votes));
            kv("Clicks", std::to_string(b.clickcount) + " in the last 24 h");
            kv("Online", b.online ? "yes" + (b.lastchecktime.empty() ? std::string() : "  (checked " + b.lastchecktime + " UTC)")
                                  : "NO at the last check" + (b.lastchecktime.empty() ? std::string() : "  (" + b.lastchecktime + " UTC)"));
            kv("Homepage", b.homepage, 2);
            kv("Stream", b.stream_url(), 3);
            std::string listed;
            if (is_listed(b)) listed = "already in your station list";
            kv("In list", listed.empty() ? "no  ([a] adds it)" : listed);
        } else {
            info.push_back(seg_line({{s.header, "Hover a result to see its details here."}}, inner));
        }
        for (int r = 0; r < list_rows; ++r)
            right.push_back(box_line(s, r < static_cast<int>(info.size()) ? info[static_cast<size_t>(r)] : spaces(inner), s.border));
    }
    right.push_back(box_bottom(s, right_w, "", s.border_bottom));
    for (size_t i = 0; i < left.size(); ++i) out.push_back(left[i] + right[i]);

    // --- hints (same style as the PRESETS menu), status line, filler -------------------------------
    out.push_back(seg_line(legend_segs(s.legend, s.legend_cmd, "[TAB] Next pane | [ENTER] Search / Tune | [\u2191\u2193] Navi. | [/] Back to NAME | [ESC] Clear / Close | RESULTS: [a] Add to list"), W));
    std::string status = bm.flash;
    if (status.empty()) {
        if (bm.state == BrowseState::Searching) status = "searching ...";
        else if (bm.state == BrowseState::Error) status = bm.message;
        else if (bm.state == BrowseState::Done && n > 0) status = std::to_string(n) + " stations from " + bm.server + "  (most clicked first, offline ones hidden)";
    }
    out.push_back(seg_line({{s.header, status}}, W));
    // What is playing right now -- always shown, whether or not it is among the results.
    {
        std::string state_txt;
        switch (st.state) {
            case StreamState::Live:         state_txt = "LIVE " + fmt_hms(st.listening_sec); break;
            case StreamState::Buffering:    state_txt = "buffering"; break;
            case StreamState::Connecting:   state_txt = "connecting"; break;
            case StreamState::Reconnecting: state_txt = "reconnecting"; break;
            case StreamState::Failed:       state_txt = "offline"; break;
            default: break;
        }
        std::vector<Seg> row = {{s.key, "Tuned in: "}};
        if (st.state == StreamState::Idle || st.tuned_name.empty()) {
            row.push_back({s.header, "nothing"});
        } else {
            row.push_back({s.val, shown_text(st.tuned_name)});
            row.push_back({"", "  (" + state_txt + ")"});
            const std::string np = st.info.artist.empty() ? st.info.title : st.info.artist + " - " + st.info.title;
            if (!np.empty()) row.push_back({s.header, "  \u266A " + shown_text(np)});
        }
        out.push_back(seg_line(row, W));
    }
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    out.resize(static_cast<size_t>(rows));
    return out;
}

// ===========================================================================================
// CHEATSHEET ('?') -- full-screen, same construction as the music player's cheat sheet
// (App::build_cheatsheet_screen): one table of  KEY | DESCRIPTION  rows grouped in categories. A
// category's title is drawn in bold Header colour above a blank spacer row and sits on the category's
// first entry. Scrollable: scroll is counted in display lines (a category costs three: spacer +
// title + the row itself), so a title scrolls like any other line.
// Key labels are the DEFAULT keys (lowercase = the plain key, SHIFT+x = the capital); the REFERENCE settings tab rebinds most of them.
// ===========================================================================================
// How a key is shown in the cheat sheet and on the REFERENCE tab: a capital letter is a Shift press, so "T" reads
// "SHIFT+t" (the binding itself stays "T"); labels such as "SHIFT+t" / "CTRL+SHIFT+u" get the lower-case letter too.
std::string pretty_key(const std::string& k) {
    if (k == "@SWITCHKEY") return muisc::mode_key_label(g_mode_key_next);   // the key set in the player's REFERENCE tab, named for this keyboard
    if (k == "@SWITCHBACK") return muisc::mode_key_label(g_mode_key_prev);
    if (k == "(" || k == ")") return muisc::symbol_key_label(k);   // SHIFT+8 / SHIFT+9 on a German keyboard
    if (k.size() == 1 && k[0] >= 'A' && k[0] <= 'Z') return std::string("SHIFT+") + static_cast<char>(k[0] + 32);
    std::string o = k;
    size_t p = 0;
    while ((p = o.find("SHIFT+", p)) != std::string::npos) {
        p += 6;
        if (p < o.size() && o[p] >= 'A' && o[p] <= 'Z' && (p + 1 >= o.size() || !std::isalpha(static_cast<unsigned char>(o[p + 1]))))
            o[p] = static_cast<char>(o[p] + 32);
    }
    return o;
}

// Word-wraps `text` into lines of at most `width` columns (a longer single word is cut).
std::vector<std::string> wrap_words(const std::string& text, int width) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i <= text.size()) {
        const size_t j = text.find(' ', i);
        std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        while (width > 0 && display_width(word) > width) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            const std::string head = utf8_take(word, width);
            if (head.empty()) break;
            out.push_back(head);
            word = word.substr(head.size());
        }
        if (cur.empty()) cur = word;
        else if (display_width(cur) + 1 + display_width(word) <= width) cur += " " + word;
        else { out.push_back(cur); cur = word; }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    if (!cur.empty() || out.empty()) out.push_back(cur);
    return out;
}

// ===========================================================================================
// The help wizard (SHIFT+w): the same pages the music player shows (src/wizard.h), drawn in
// the MUSIC PLAYER's colours rather than the radio's, so the feature looks like one thing in
// both modes. The text, layout and state are all wizard::render()'s; this only paints them.
// ===========================================================================================

std::vector<std::string> build_wizard(const Style& s, const UiModel& m, int W, int rows) {
    const WizardPalette p = player_wizard_palette();
    const std::string wb  = ansi_fg(p.border);
    const std::string wbb = ansi_fg(p.border_bot);
    const int body_rows = std::max(1, rows - 2);        // the top and bottom border take two rows

    static const muisc::DiskArt disk;
    std::vector<std::string> disk_frame;
    if (wizard::shows_disk(m.wizard.page)) disk_frame = disk.frame(m.wizard_angle);
    const wizard::View v = wizard::render(m.wizard, W, body_rows, disk_frame);

    const std::string lead = spaces(v.left);
    const std::string trail = spaces(std::max(0, W - v.left - v.box_w));
    const std::string bar = wb + s.c.box_vertical + kReset;
    const std::string header = "\x1b[1m" + ansi_fg(p.header);
    const std::string legend = ansi_fg(p.legend);
    const std::string base = ansi_fg(p.list);
    std::string cursor = ansi_fg(p.cursor_fg) + ansi_bg(p.cursor_bg);
    if (cursor.empty()) cursor = "\x1b[7m";

    std::vector<std::string> out;
    // the title is drawn in the header colour; box_edge() brings the border colour back in front of the dashes
    out.push_back(lead + box_top(s, v.box_w, header + v.title + kReset, wb) + trail);
    for (const wizard::Row& row : v.rows) {
        std::string line = lead + bar;
        for (const wizard::Span& sp : row) {
            switch (sp.role) {
                case wizard::Role::Header: line += header + sp.text + kReset; break;
                case wizard::Role::Legend: line += legend + sp.text + kReset; break;
                case wizard::Role::Cursor: line += cursor + sp.text + kReset; break;
                case wizard::Role::Match:  line += header + "\x1b[4m" + sp.text + kReset; break;
                case wizard::Role::Disk:
                case wizard::Role::Scroll: line += gradient_fg(p.disk_top, p.disk_bottom, sp.t) + sp.text + kReset; break;
                default:                   line += base + sp.text + kReset; break;
            }
        }
        out.push_back(line + bar + trail);
    }

    std::string bottom = "[SHIFT+w / ESC] close";
    if (v.search) {
        bottom = "[ESC] clear / back  [ENTER] open the chapter  [\xE2\x86\x91\xE2\x86\x93] choose  [SHIFT+\xE2\x86\x91\xE2\x86\x93] jump 5";
        out.push_back(lead + box_bottom(s, v.box_w, bottom, wbb) + trail);
        return out;
    }
    bottom += "  [t] TOC  [SHIFT+s] search";
    if (v.can_back) bottom += "  [<-] back";
    if (v.can_next) bottom += "  [->] next";
    if (v.toc_page) bottom += "  [\xE2\x86\x91\xE2\x86\x93] select  [SHIFT+\xE2\x86\x91\xE2\x86\x93] jump  [ENTER] open";
    else if (v.scrollable) bottom += "  [\xE2\x86\x91\xE2\x86\x93] scroll  [SHIFT+\xE2\x86\x91\xE2\x86\x93] page";
    out.push_back(lead + box_bottom(s, v.box_w, bottom, wbb) + trail);
    return out;
}

std::vector<std::string> build_cheatsheet(const Style& s, const UiModel& m, int W, int rows) {
    std::vector<std::string> out;
    out.push_back(box_top(s, W, "CHEATSHEET", s.border));

    const int visible = std::max(1, rows - 2);        // the top and bottom border take two rows
    const int inner = std::max(0, W - 4);

    // Every entry becomes display lines first (a category: blank spacer + title; a long description wraps), so the scroll
    // range counts what is really drawn. The description column starts after the widest key + 2 spaces.
    int key_w = 0;
    for (const auto& r : kCheatRows) key_w = std::max(key_w, display_width(pretty_key(r.key)));
    key_w = std::min(key_w, std::max(10, inner / 2));
    const int desc_col = key_w + 2;
    const int desc_w = std::max(10, inner - desc_col);
    const std::string header_bold = "\x1b[1m" + s.legend;   // bold + Header colour, like the player's header_sgr()
    struct DLine { std::string plain, ansi; };
    std::vector<DLine> dl;
    std::vector<int> row_dline;
    std::vector<std::string> row_category;
    std::string category;
    for (const auto& r : kCheatRows) {
        row_dline.push_back(static_cast<int>(dl.size()) + (r.header ? 1 : 0));
        if (r.header) category = r.header;
        row_category.push_back(category);
        if (r.header) { dl.push_back({"", ""}); dl.push_back({r.header, header_bold}); }
        const std::string key = pretty_key(r.key);
        const auto parts = wrap_words(r.desc, desc_w);
        for (size_t i = 0; i < parts.size(); ++i)
            dl.push_back({(i == 0 ? pad_right(truncate_str(key, key_w), desc_col) : spaces(desc_col)) + parts[i], ""});
    }
    const int total = static_cast<int>(dl.size());
    const int max_scroll = std::max(0, total - visible);
    if (m.cheat_jump_row >= 0 && m.cheat_jump_row < static_cast<int>(row_dline.size()))
        m.cheat_scroll = std::max(0, row_dline[static_cast<size_t>(m.cheat_jump_row)] - 2);   // the category title above stays in view
    m.cheat_jump_row = -1;
    m.cheat_scroll = std::clamp(m.cheat_scroll, 0, max_scroll);

    if (m.cheat_searching) {
        // ----- SHIFT+s: the search view replaces the table -----
        const std::string mark = header_bold + "\x1b[4m";
        const std::vector<std::string> words = muisc::helpsearch::split_words(m.cheat_query);
        struct Res { int row; muisc::helpsearch::Hit hit; std::string doc; };
        std::vector<Res> res;
        if (!words.empty()) {
            size_t i = 0;
            for (const auto& r : kCheatRows) {
                // the key is padded to the table's key column, so the descriptions line up as in the table
                std::string doc = pad_right(truncate_str(pretty_key(r.key), key_w), desc_col) + r.desc + "  [" + row_category[i] + "]";
                muisc::helpsearch::Hit h = muisc::helpsearch::match(words, doc);
                if (muisc::helpsearch::worth_showing(h)) res.push_back({static_cast<int>(i), std::move(h), std::move(doc)});
                ++i;
            }
            std::stable_sort(res.begin(), res.end(), [](const Res& a, const Res& b) { return muisc::helpsearch::better(a.hit, b.hit); });
            if (res.size() > 300) res.resize(300);
        }
        m.cheat_result_rows.clear();
        for (const Res& r : res) m.cheat_result_rows.push_back(r.row);
        m.cheat_sel = std::clamp(m.cheat_sel, 0, std::max(0, static_cast<int>(res.size()) - 1));

        int used = 0;
        {   // the query: caret / marked range in reverse video
            std::string q = m.cheat_query;
            const size_t caret = std::min(m.cheat_qcaret, q.size()), anchor = std::min(m.cheat_qanchor, q.size());
            const size_t a = std::min(caret, anchor), b = std::max(caret, anchor);
            std::vector<Seg> segs = {{header_bold, "Search: "}};
            if (a != b) {
                segs.push_back({"", q.substr(0, a)});
                segs.push_back({"\x1b[7m", q.substr(a, b - a)});
                segs.push_back({"", q.substr(b)});
            } else {
                size_t n = 0;
                if (caret < q.size()) { n = 1; while (caret + n < q.size() && (static_cast<unsigned char>(q[caret + n]) & 0xC0) == 0x80) ++n; }
                segs.push_back({"", q.substr(0, caret)});
                segs.push_back({"\x1b[7m", n ? q.substr(caret, n) : std::string(" ")});
                segs.push_back({"", q.substr(caret + n)});
            }
            out.push_back(box_line(s, seg_line(segs, inner), s.border));
            ++used;
        }
        {
            int full = 0;
            for (const Res& r : res) if (r.hit.full()) ++full;
            std::string info;
            if (words.empty()) info = "Type what you look for: words in any order, typos and related words are found too.";
            else if (res.empty()) info = "Nothing found. Try fewer or other words.";
            else info = std::to_string(res.size()) + (res.size() == 1 ? " command" : " commands") + (full < static_cast<int>(res.size()) ? " (" + std::to_string(full) + " with all words, the rest with some)" : "");
            out.push_back(box_line(s, seg_line({{s.legend, info}}, inner), s.border));
            out.push_back(box_line(s, spaces(inner), s.border));
            used += 2;
        }
        const int text_w = std::max(10, inner - 2);
        auto wrap_offsets = [&](const std::string& str, int width, int cont_width, size_t max_lines) {
            std::vector<std::pair<size_t, size_t>> lines;
            size_t i = 0, line_start = std::string::npos, line_end = 0;
            int line_w = 0;
            while (i < str.size()) {
                const size_t gap_from = i;
                while (i < str.size() && str[i] == ' ') ++i;
                if (i >= str.size()) break;
                size_t j = i;
                while (j < str.size() && str[j] != ' ') ++j;
                const int ww = display_width(str.substr(i, j - i));
                const int gap = static_cast<int>(i - gap_from);
                if (line_start != std::string::npos && line_w + gap + ww > (lines.empty() ? width : cont_width)) {
                    lines.emplace_back(line_start, line_end);
                    line_start = std::string::npos;
                    if (lines.size() >= max_lines) return lines;
                }
                if (line_start == std::string::npos) { line_start = i; line_w = ww; }
                else line_w += gap + ww;
                line_end = j;
                i = j;
            }
            if (line_start != std::string::npos && lines.size() < max_lines) lines.emplace_back(line_start, line_end);
            return lines;
        };
        std::vector<std::vector<std::pair<size_t, size_t>>> blocks;
        for (const Res& r : res) blocks.push_back(wrap_offsets(r.doc, text_w, std::max(10, text_w - desc_col), 3));
        const int room = std::max(1, visible - used);
        m.cheat_first = std::clamp(m.cheat_first, 0, std::max(0, static_cast<int>(res.size()) - 1));
        if (m.cheat_sel < m.cheat_first) m.cheat_first = m.cheat_sel;
        for (;;) {
            int h = 0;
            for (int i = m.cheat_first; i <= m.cheat_sel && i < static_cast<int>(res.size()); ++i) h += static_cast<int>(blocks[static_cast<size_t>(i)].size());
            if (h <= room || m.cheat_first >= m.cheat_sel) break;
            ++m.cheat_first;
        }
        int rows_left = room;
        for (int i = m.cheat_first; i < static_cast<int>(res.size()) && rows_left > 0; ++i) {
            const auto& bl = blocks[static_cast<size_t>(i)];
            const Res& r = res[static_cast<size_t>(i)];
            for (size_t k = 0; k < bl.size() && rows_left > 0; ++k, --rows_left) {
                std::vector<Seg> segs = {{(k == 0 && i == m.cheat_sel) ? "\x1b[7m" : "", (k == 0 && i == m.cheat_sel) ? "\xE2\x96\xB6 " : "  "}};
                if (k > 0) segs.push_back({"", spaces(desc_col)});   // wrapped lines continue under the description column
                size_t pos = bl[k].first;
                for (const auto& rg : r.hit.ranges) {
                    if (rg.to <= bl[k].first || rg.from >= bl[k].second) continue;
                    const size_t a = std::max(rg.from, bl[k].first), b = std::min(rg.to, bl[k].second);
                    if (a > pos) segs.push_back({"", r.doc.substr(pos, a - pos)});
                    segs.push_back({mark, r.doc.substr(a, b - a)});
                    pos = b;
                }
                if (pos < bl[k].second) segs.push_back({"", r.doc.substr(pos, bl[k].second - pos)});
                out.push_back(box_line(s, seg_line(segs, inner), s.border));
            }
        }
        while (rows_left-- > 0) out.push_back(box_line(s, spaces(inner), s.border));
        out.push_back(box_bottom(s, W, "[ESC] clear / back  [ENTER] show in the table  [\xE2\x86\x91\xE2\x86\x93] choose", s.border_bottom));
        return out;
    }
    int shown = 0;
    for (int i = m.cheat_scroll; i < total && shown < visible; ++i, ++shown)
        out.push_back(box_line(s, seg_line({{dl[static_cast<size_t>(i)].ansi, dl[static_cast<size_t>(i)].plain}}, inner), s.border));
    while (shown < visible) { out.push_back(box_line(s, spaces(inner), s.border)); ++shown; }

    std::string bottom = "[? / ESC] close  [SHIFT+s] search";
    if (max_scroll > 0) bottom += "  [\u2191\u2193] scroll  [SHIFT+\u2191\u2193] Page  " + std::to_string(m.cheat_scroll + 1) + "/" + std::to_string(total);
    out.push_back(box_bottom(s, W, bottom, s.border_bottom));
    return out;
}

// The key legend of the big STATIONS overlay: a group continues on further rows when its keys do not fit one (broken between two
// commands, never inside one), so on a narrow terminal nothing is cut off. The pane gives the rows the legend needs.
std::vector<LegendGroup> stations_legend_groups(const StationsOverlay& so) {
    std::vector<LegendGroup> g;
    g.push_back({"Move", {"[\u2191\u2193] Navi.", "[ENTER] Tune/Open", "[/] Search", "[p:] Lists", "[ESC] Clear / Exit\001Exit"}, 3});
    g.push_back({"Edit", {"[a] Add URL", "[SHIFT+c] Edit entry\001Edit", "[DEL] Delete\001Del",
                          so.undo_count > 0 ? "[SHIFT+u] Undo (" + std::to_string(so.undo_count) + ")\001Undo" : "[SHIFT+u] Undo"}, 3});
    g.push_back({"Playback", {"[n/b/#] Next / back / random\001Next/Back", "[p] Mute", "[x] Stop", "[+/-] Volume"}, 3});
    g.push_back({"View", {"[SHIFT+t] Sort", "[L] Close"}, 3});
    return g;
}

std::vector<std::string> build_stations_overlay(const Style& s, const UiModel& m, const RadioStatus& st, int W, int rows) {
    const StationsOverlay& so = m.stov;
    const std::vector<Station> none;
    const auto& stations = m.stations ? *m.stations : none;
    std::vector<std::string> out = build_search(s, m, st, W);                 // the same search box as the main screen
    // the legend as tall as it has to be (a third of the screen at most), then the status line
    const int legend_rows = static_cast<int>(layout_legend_tree(stations_legend_groups(so), W - 1, std::max(3, rows / 3)).size());
    const int hint_rows = legend_rows + 1;
    const int body = std::max(3, rows - static_cast<int>(out.size()) - 2 - hint_rows);   // rows between the pane's borders
    const int inner = W - 4;
    const std::string sep = s.c.list_separator;

    std::string label;
    if (m.search_lists) label = "STATION LISTS (ENTER: show the stations)";
    else if (!m.active_list.empty()) label = "STATIONS (LIST: " + utf8_take(m.active_list, 24) + " - ESC: all)";
    else label = std::string("STATIONS (") + (m.sort_az ? "sort: A-Z" : "sort: list order") + ")";
    const int total = m.search_lists ? static_cast<int>(m.lvisible.size()) : static_cast<int>(m.visible.size());
    out.push_back(box_top(s, W, label, s.border));

    const StationList* shown_list = nullptr;
    if (!m.active_list.empty()) for (const auto& l : m.lists) if (l.name == m.active_list) { shown_list = &l; break; }
    const int idx_w = static_cast<int>(std::to_string(std::max<size_t>(1, shown_list ? shown_list->items.size() : stations.size())).size());
    const int country_w = 16, tail_w = 8;
    const int flex = std::max(12, inner - (idx_w + 5 * (2 + static_cast<int>(display_width(sep))) + country_w + tail_w));
    const int name_w = flex * 40 / 100, pre_w = flex * 20 / 100, genre_w = flex - name_w - pre_w;

    if (m.search_lists) {
        const int lcursor = std::clamp(m.lcursor, 0, std::max(0, total - 1));
        const int first = follow_scroll(scroll_id(7), lcursor, body, total);
        for (int r = 0; r < body; ++r) {
            const int li = first + r;
            if (li >= total) {
                out.push_back(box_line(s, seg_line({{s.header, total == 0 && r == 0 ? (m.lists.empty() ? "no station lists yet - SHIFT+k creates one" : "no station list matches") : ""}}, inner), s.border));
                continue;
            }
            const StationList& l = m.lists[static_cast<size_t>(m.lvisible[static_cast<size_t>(li)])];
            const std::string cnt = std::to_string(l.items.size()) + (l.items.size() == 1 ? " station" : " stations");
            const int nw = std::max(5, inner - 3 - (2 * display_width(sep) + 3) - 13);
            const std::string text = pad_right(std::to_string(li + 1), 3) + sep + " " + pad_right(truncate_str(shown_text(l.name), nw), nw) + " " + sep + " " + pad_right(cnt, 13);
            out.push_back(box_line(s, seg_line({{li == lcursor && m.search_focus ? s.list_cursor : s.list, text}}, inner), s.border));
        }
        out.push_back(box_bottom(s, W, std::max(0, total - (first + body)) > 0 ? "( " + std::to_string(total - (first + body)) + " more )" : "", s.border_bottom));
        m.stov.marquee_row = -1;
    } else {
        // column captions (the first row of the pane)
        out.push_back(box_line(s, seg_line({{s.header, pad_right("NO", idx_w) + sep + " " + pad_right("NAME", name_w) + " " + sep + " " + pad_right("PRESET NAME", pre_w) + " " + sep + " "
                                                       + pad_right("GENRE", genre_w) + " " + sep + " " + pad_right("COUNTRY", country_w) + " " + sep + " " + pad_right("STREAM", tail_w)}}, inner), s.border));
        const int rowsn = body - 1;
        const int cursor = std::clamp(m.cursor, 0, std::max(0, total - 1));
        const int first = follow_scroll(scroll_id(8), cursor, rowsn, total);
        for (int r = 0; r < rowsn; ++r) {
            const int vi = first + r;
            if (vi >= total) { out.push_back(box_line(s, seg_line({{s.header, total == 0 && r == 0 ? "no station matches" : ""}}, inner), s.border)); continue; }
            const int idx = m.visible[static_cast<size_t>(vi)];
            const Station& stn = stations[static_cast<size_t>(idx)];
            std::string tail = stn.codec_hint;
            if (stn.bitrate_hint > 0) tail = std::to_string(stn.bitrate_hint) + "k " + stn.codec_hint;
            const bool hovered = vi == cursor;   // also while searching (Up/Down move it, ENTER tunes it)
            const std::string name_shown = hovered ? marquee_or_truncate(so.marquee_row, so.marquee_since, m.t_sec, shown_text(stn.name), name_w, idx)
                                                   : pad_right(truncate_str(shown_text(stn.name), name_w), name_w);
            int number = idx + 1;
            if (shown_list) number = static_cast<int>(std::find(shown_list->items.begin(), shown_list->items.end(), idx) - shown_list->items.begin()) + 1;
            const std::string text = pad_right(std::to_string(number), idx_w) + sep + " " + name_shown + " " + sep + " " + pad_right(truncate_str(stn.preset_name, pre_w), pre_w) + " " + sep + " "
                                   + pad_right(truncate_str(stn.genre, genre_w), genre_w) + " " + sep + " " + pad_right(truncate_str(stn.country, country_w), country_w) + " " + sep + " "
                                   + pad_right(truncate_str(tail, tail_w), tail_w);
            std::string style = (idx == st.tuned_index && st.state != StreamState::Idle) ? s.list_playing : s.list;
            if (hovered) style = s.list_cursor;
            out.push_back(box_line(s, seg_line({{style, text}}, inner), s.border));
        }
        if (m.search_focus || total == 0) m.stov.marquee_row = -1;
        const int hidden = std::max(0, total - (first + rowsn));
        out.push_back(box_bottom(s, W, hidden > 0 ? "( " + std::to_string(hidden) + " more )" : "", s.border_bottom));
    }
    push_legend(out, s, stations_legend_groups(so), W, std::max(3, rows / 3));
    out.push_back(seg_line({{s.header, utf8_take(so.flash, W)}}, W));
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    out.resize(static_cast<size_t>(rows));

    // --- the two small overlays, drawn over the finished screen
    auto splice_panel = [&](const std::vector<std::string>& panel, int pw) {
        const int ph = static_cast<int>(panel.size());
        const int x = std::max(0, (W - pw) / 2);
        const int y = std::clamp((rows - ph) / 2 - 2, 0, std::max(0, rows - ph));
        for (int i = 0; i < ph; ++i) out[static_cast<size_t>(y + i)] = splice_line(out[static_cast<size_t>(y + i)], x, panel[static_cast<size_t>(i)], pw);
    };
    const std::string base = s.c.list_fg.empty() ? "" : ansi_fg(s.c.list_fg);
    if (so.add_open) {
        const int pw = std::min(kAddOverlayW, W), pi = pw - 4;
        std::vector<std::string> panel;
        panel.push_back(box_top(s, pw, "Add a station by URL", s.border));
        panel.push_back(box_line(s, field_row(base, "URL:  ", so.add_url, so.add_edit[0], so.add_field == 0, pi, "", s.legend, false), s.border));
        panel.push_back(box_line(s, field_row(base, "NAME: ", so.add_name, so.add_edit[1], so.add_field == 1, pi, so.add_name.empty() && so.add_field != 1 ? "(optional)" : "", s.legend, false), s.border));
        panel.push_back(box_line(s, seg_line({{s.legend, so.add_error.empty() ? "http(s) stream address; an empty NAME becomes the host name" : so.add_error}}, pi), s.border));
        panel.push_back(box_bottom(s, pw, "[ENTER] add  [TAB] URL / NAME  [ESC] cancel", s.border_bottom));
        splice_panel(panel, pw);
    }
    if (so.edit_open) {
        static const char* const labels[StationsOverlay::kEditFields] = {"NAME:        ", "PRESET NAME: ", "URL:         ", "GENRE:       ", "COUNTRY:     ", "CODEC:       ", "KBPS:        "};
        static const char* const helps[StationsOverlay::kEditFields] = {
            "The station's name in the lists", "A shorter name for the PRESETS pane; empty = the station name", "http(s) stream address",
            "e.g. Jazz", "e.g. DE", "AAC, MP3 ... (shown until the stream tells)", "0 = unknown"};
        const int pw = std::min(kEditOverlayW, W), pi = pw - 4;
        std::vector<std::string> panel;
        panel.push_back(box_top(s, pw, "Edit entry", s.border));
        for (int f = 0; f < StationsOverlay::kEditFields; ++f)
            panel.push_back(box_line(s, field_row(base, labels[f], so.edit_text[f], so.edit_ed[f], so.edit_field == f, pi, "", s.legend, false), s.border));
        panel.push_back(box_line(s, seg_line({{s.legend, so.edit_error.empty() ? std::string(helps[std::clamp(so.edit_field, 0, StationsOverlay::kEditFields - 1)]) : so.edit_error}}, pi), s.border));
        panel.push_back(box_bottom(s, pw, "[ENTER] save  [TAB/\u2191\u2193] field  [ESC] cancel", s.border_bottom));
        splice_panel(panel, pw);
    }
    if (so.del_idx >= 0 && so.del_idx < static_cast<int>(stations.size())) {
        const int pw = std::min(kDelOverlayW, W), pi = pw - 4;
        const Station& d = stations[static_cast<size_t>(so.del_idx)];
        std::vector<std::string> panel;
        panel.push_back(box_top(s, pw, "Delete station", s.border));
        panel.push_back(box_line(s, seg_line({{s.header, utf8_take(d.name, pi)}}, pi), s.border));
        panel.push_back(box_line(s, seg_line({{"\x1b[41;97m", pad_right(utf8_take(" Removed from the stations file, all presets and lists.", pi), pi)}}, pi), s.border));
        panel.push_back(box_line(s, seg_line({{s.legend, "SHIFT+u takes it back as long as Mousiki runs."}}, pi), s.border));
        panel.push_back(box_bottom(s, pw, "[Y]es delete  [N]o / [ESC] keep", s.border_bottom));
        splice_panel(panel, pw);
    }
    return out;
}

std::string fmt_clock_len(double sec) {   // 3:45 / 1:02:03 for a song length; "" when unknown
    if (sec < 0) return "";
    const long long t = static_cast<long long>(sec + 0.5);
    char buf[32];
    if (t >= 3600) std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(buf, sizeof buf, "%lld:%02lld", t / 60, t % 60);
    return buf;
}

std::vector<std::string> build_history_menu(const Style& s, const UiModel& m, int W, int rows) {
    const HistoryModel& hm = m.hmenu;
    const int inner = W - 4;
    const std::string base = s.c.list_fg.empty() ? "" : ansi_fg(s.c.list_fg);
    std::vector<std::string> out;
    {
        const auto strip = menu_tab_strip(s, W, "LISTENING HISTORY", {"HISTORY", "TOP CHANNELS", "HABITS"}, hm.tab, s.border, s.border);
        out.push_back(strip[0]);
        out.push_back(strip[1]);
    }
    // one fused pane: strip (2 lines) + body + bottom border (carries the info text) + hint + status
    const int body = std::max(4, rows - 2 - 1 - 2);
    std::string info;                                         // sits on the bottom border line
    if (hm.tab == 0 && hm.hist) info = std::to_string(hm.hist->size()) + " lines (max " + std::to_string(kMaxHistoryEntries) + ")";
    else if (hm.tab == 1) info = hm.most_first ? "most listened first" : "least listened first";

    struct Line { std::string text; int style = 0; };        // 0 plain, 1 header, 2 section header, 3 rule
    std::vector<Line> lines;
    int cursor = -1;
    auto T = [](const std::string& str, int n) { return pad_right(truncate_str(shown_text(str), n), n); };
    auto R = [](const std::string& str, int n) { return pad_left(truncate_str(str, n), n); };
    // Marks the plain-text cells with spaces so the columns line up; colouring is only done per whole line.
    if (hm.tab == 0) {
        const int when_w = 11, len_w = 7, chan_w = 22;
        const int rest = std::max(10, inner - when_w - chan_w - len_w - 6);
        const int art_w = rest * 2 / 5, tit_w = rest - art_w;
        lines.push_back({T("WHEN", when_w) + "  " + T("CHANNEL", chan_w) + "  " + T("ARTIST", art_w) + " " + T("TITLE", tit_w) + " " + R("HEARD", len_w), 1});
        const size_t n = hm.hist ? hm.hist->size() : 0;
        if (n == 0) lines.push_back({"nothing recorded yet -- tune a station; a line is added whenever artist or title changes", 0});
        for (size_t i = 0; i < n; ++i) {
            const HistoryEntry& e = hm.hist->newest(i);
            const bool live = i == 0 && hm.hist->live_at_top();
            const std::string ttl = e.title.empty() && e.artist.empty() ? "(no title)" : e.title;
            lines.push_back({T(live ? "now" : muisc::format_when(e.at), when_w) + "  " + T(e.channel, chan_w) + "  " + T(e.artist, art_w) + " " +
                             T(ttl, tit_w) + " " + R(muisc::format_mmss(e.listened), len_w), 0});
        }
        if (n > 0) cursor = std::clamp(hm.cursor, 0, static_cast<int>(n) - 1) + 1;
    } else if (hm.tab == 1) {
        const int songs_w = 6, time_w = 10;
        const int chan_w = std::max(10, inner - songs_w - time_w - 4);
        lines.push_back({T("CHANNEL", chan_w) + "  " + R("LINES", songs_w) + "  " + R(std::string("LISTENED ") + (hm.most_first ? "▼" : "▲"), time_w), 1});
        lines.push_back({repeat("─", inner), 3});
        const std::vector<HistoryChannelRow> top = hm.hist ? hm.hist->top_channels(hm.most_first) : std::vector<HistoryChannelRow>{};
        if (top.empty()) lines.push_back({"nothing recorded yet", 0});
        for (const auto& r : top)
            lines.push_back({T(r.channel, chan_w) + "  " + R(std::to_string(r.songs), songs_w) + "  " + R(muisc::format_len(r.listened), time_w), 0});
        if (!top.empty()) cursor = std::clamp(hm.top_cursor, 0, static_cast<int>(top.size()) - 1) + 2;
    } else {
        const HistoryStats st = hm.hist ? hm.hist->stats(hm.now) : HistoryStats{};
        const int lbl_w = std::max(20, inner - 24);
        auto num1 = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
        auto cat = [&](const char* name) { if (!lines.empty()) lines.push_back({"", 0}); lines.push_back({name, 2}); };
        auto item = [&](const std::string& label, const std::string& value) { lines.push_back({T(label, lbl_w) + value, 0}); };
        cat("SESSIONS");
        item("Sessions (a gap of 30 min starts a new one)", std::to_string(st.sessions));
        item("Average session length", muisc::format_len(st.avg_session_sec));
        item("Songs per session", num1(st.songs_per_session));
        cat("TIME LISTENED");
        item("Listened today", muisc::format_len(st.today_sec));
        item("Average day with radio", muisc::format_len(st.avg_day_sec));
        item("Days with radio", std::to_string(st.days));
        item("Total listened", muisc::format_len(st.total_sec));
        cat("CHANNELS");
        item("History lines (lifetime)", std::to_string(st.entries));
        item("Different channels", std::to_string(st.channels));
        item("Favourite channel", st.top_channel.empty() ? std::string("-") : truncate_str(st.top_channel, 40));
        item("Busiest hour", st.busiest_hour < 0 ? std::string("-") : (st.busiest_hour < 10 ? "0" : "") + std::to_string(st.busiest_hour) + ":00");
        cat("LISTENING BY HOUR OF THE DAY");
        {
            static const char* lv[9] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
            double mx = 0;
            for (double v : st.hours) mx = std::max(mx, v);
            std::string bars = "  ", labs = "  ";
            for (int h = 0; h < 24; ++h) {
                const int l = mx > 0 ? static_cast<int>(std::ceil(st.hours[static_cast<size_t>(h)] / mx * 8.0 - 1e-9)) : 0;
                bars += std::string(" ") + lv[std::clamp(l, 0, 8)] + lv[std::clamp(l, 0, 8)] + " ";
                char b[8]; std::snprintf(b, sizeof b, " %02d ", h);
                labs += b;
            }
            lines.push_back({bars, 0});
            lines.push_back({labs, 0});
        }
        cat("LISTENING BY WEEKDAY");
        {
            // The bar and the AVERAGE column show the average time on one such weekday (HistoryStats::weekday_avg), the
            // CUMULATIVE column the lifetime total. Only the two value columns get a name, in the header colour.
            static const char* wd[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
            const int day_w = 11, avg_w = 10, cum_w = 10;   // "AVERAGE" / "CUMULATIVE" and values like "123h 45m"
            const int bar_w = std::clamp(inner - (2 + day_w + 2 + avg_w + 2 + cum_w), 10, 40);
            lines.push_back({spaces(2 + day_w + bar_w + 2) + T("AVERAGE", avg_w) + "  " + T("CUMULATIVE", cum_w), 2});
            double mx = 0;
            for (double v : st.weekday_avg) mx = std::max(mx, v);
            for (int d = 0; d < 7; ++d) {
                const double avg = st.weekday_avg[static_cast<size_t>(d)];
                const double cum = st.weekdays[static_cast<size_t>(d)];
                const int len = mx > 0 ? std::clamp(static_cast<int>(std::round(avg / mx * bar_w)), 0, bar_w) : 0;
                lines.push_back({"  " + pad_right(wd[d], day_w) + repeat("█", len) + spaces(bar_w - len) + "  " +
                                 T(muisc::format_len(avg), avg_w) + "  " + T(muisc::format_len(cum), cum_w), 0});
            }
        }
    }

    int start;
    if (cursor >= 0) start = std::clamp(cursor - (body - 1) / 2, 0, std::max(0, static_cast<int>(lines.size()) - body));
    else {
        hm.scroll = std::clamp(hm.scroll, 0, std::max(0, static_cast<int>(lines.size()) - body));
        start = hm.scroll;
    }
    int shown = 0;
    for (int k = start; k < static_cast<int>(lines.size()) && shown < body; ++k, ++shown) {
        const Line& ln = lines[static_cast<size_t>(k)];
        const std::string txt = pad_right(truncate_str(ln.text, inner), inner);
        if (ln.style == 3) out.push_back(box_line(s, s.legend + txt + kReset, s.border));
        else if (ln.style == 1 || ln.style == 2) out.push_back(box_line(s, s.header + txt + kReset, s.border));
        else if (k == cursor) out.push_back(box_line(s, s.list_cursor + txt + kReset, s.border));
        else out.push_back(box_line(s, base + txt + kReset, s.border));
    }
    for (; shown < body; ++shown) out.push_back(box_line(s, spaces(inner), s.border));
    out.push_back(box_bottom(s, W, info, s.border_bottom));

    std::string hint;
    if (hm.tab == 0) hint = "[←→/TAB] Tab  [1/2/3] Tab  [↑↓] Move  [HOME/END] Ends  [y] Find on YouTube + download  [ESC] Exit";
    else if (hm.tab == 1) hint = "[←→/TAB] Tab  [1/2/3] Tab  [↑↓] Move  [r] Flip sort  [ESC] Exit";
    else hint = "[←→/TAB] Tab  [1/2/3] Tab  [↑↓] Scroll  [ESC] Exit";
    out.push_back(seg_line(legend_segs(s.legend, s.legend_cmd, hint), W));
    out.push_back(seg_line({{"\x1b[38;5;246m", utf8_take(hm.flash, W)}}, W));
    while (static_cast<int>(out.size()) < rows) out.push_back(spaces(W));
    out.resize(static_cast<size_t>(rows));

    // --- the YouTube overlay -------------------------------------------------------------------
    if (hm.yt_open) {
        const int ow = std::min(kYtOverlayW, W);
        const int oi = ow - 4;
        std::vector<std::string> panel;
        panel.push_back(box_top(s, ow, "Find on YouTube + download", s.border));
        panel.push_back(box_line(s, field_row(s.search_idle, "Search: ", hm.yt_text, hm.yt_edit, hm.yt_in_field, oi, "", s.legend, false, s.search), s.border));
        std::string status;
        if (hm.yt.searching) status = "searching...";
        else if (!hm.yt.error.empty()) status = hm.yt.error;
        else if (hm.yt.searched) status = std::to_string(hm.yt.items.size()) + " results";
        else status = "[ENTER] searches";
        panel.push_back(box_line(s, pad_right(truncate_str(status, oi), oi), s.border));
        const int n = static_cast<int>(hm.yt.items.size());
        const int cur = std::clamp(hm.yt_cursor, 0, std::max(0, n - 1));
        const int first = follow_scroll(scroll_id(9), cur, kYtResultRows, n);
        for (int i = 0; i < kYtResultRows; ++i) {
            const int idx = first + i;
            if (idx >= n) { panel.push_back(box_line(s, spaces(oi), s.border)); continue; }
            const YtItem& it = hm.yt.items[static_cast<size_t>(idx)];
            const char* mark = it.state == 1 ? "[..]" : it.state == 2 ? "[ok]" : it.state == 3 ? "[!!]" : "[  ]";
            const std::string len = fmt_clock_len(it.duration_sec);
            const int title_w = std::max(8, oi - 5 - 1 - 8);
            std::string label = it.title + (it.uploader.empty() ? "" : "  (" + it.uploader + ")");
            std::string txt = std::string(mark) + " " + T(label, title_w) + " " + R(len, 8);
            const bool on = !hm.yt_in_field && idx == cur;
            panel.push_back(box_line(s, (on ? s.list_cursor : base) + pad_right(truncate_str(txt, oi), oi) + kReset, s.border));
        }
        std::string note;
        if (n > 0 && cur < n) {
            const YtItem& it = hm.yt.items[static_cast<size_t>(cur)];
            if (it.state == 2) note = "saved: " + it.note;
            else if (it.state == 3) note = "failed: " + it.note;
            else if (it.state == 1) note = "downloading...";
        }
        if (note.empty()) note = "to: " + hm.yt_dir;
        panel.push_back(box_line(s, s.legend + pad_right(truncate_str(note, oi), oi) + kReset, s.border));
        panel.push_back(box_bottom(s, ow, hm.yt_in_field ? "[ENTER] search  [↓/TAB] results  [ESC] close"
                                                           : "[ENTER] download  [↑↓] select  [TAB] edit  [ESC] close", s.border_bottom));
        const int ph = static_cast<int>(panel.size());
        const int x = std::max(0, (W - ow) / 2);
        const int y = std::clamp((rows - ph) / 2, 0, std::max(0, rows - ph));
        for (int i = 0; i < ph; ++i)
            out[static_cast<size_t>(y + i)] = splice_line(out[static_cast<size_t>(y + i)], x, panel[static_cast<size_t>(i)], ow);
    }
    return out;
}

// The theme picker (also used to choose a template inside the creator): one theme per line with colour swatches.
std::vector<std::string> build_theme_picker(const RadioSettings& c, const SettingsModel& sm, int W, int rows) {
    const int inner = W - 4;
    const int H = std::max(rows, kUiRows);
    auto bar = [&](int y) {
        if (!c.border_smooth) return ansi_fg(y >= H - 3 ? c.border_bottom : c.border_top);   // rough: only the bottom border line changes
        const float t = H > 1 ? static_cast<float>(y) / static_cast<float>(H - 1) : 0.0f;
        return gradient_fg(c.border_top, c.border_bottom, std::round(t * 32.0f) / 32.0f);
    };
    const std::string legend = ansi_fg(c.legend), header = ansi_fg(c.header);
    std::vector<std::string> out;
    const std::string title = sm.theme_ui == 1 ? "─ CHOOSE THEME " : "─ CHOOSE THEME AS TEMPLATE ";
    out.push_back(bar(0) + "┌" + title + repeat("─", std::max(0, W - 2 - display_width(title))) + "┐" + kReset);
    const int n = static_cast<int>(sm.theme_list.size());
    const int visible = std::max(1, H - 4);
    const int first = std::clamp(sm.theme_sel - visible / 2, 0, std::max(0, n - visible));
    // the marker: the theme in use (picker), or the one the draft matches / is based on (template picker)
    int cur = sm.theme_ui == 1 ? theme_index(c, sm.theme_list) : -1;
    if (sm.theme_ui == 3) {
        cur = theme_index(sm.theme_draft, sm.theme_list);
        for (int k = 0; k < n && cur < 0; ++k) if (sm.theme_list[static_cast<size_t>(k)].name == sm.theme_base.name) cur = k;
    }
    static const struct { const char* key; bool bg; } kSw[] = {
        {"ColorBorderTop", false}, {"ColorBorderBottom", false}, {"ColorOnAirUpperLeft", false}, {"ColorOnAirBottomRight", false},
        {"ColorMetadataKey", false}, {"ColorMetadataVal", false}, {"ColorVolumeCurrent", false},
        {"ColorListInactiveFg", false}, {"ColorListPlayingFg", false}, {"ColorListCursorBg", true}};
    for (int r = 0; r < visible; ++r) {
        const int y = static_cast<int>(out.size());
        const std::string b = bar(y) + "│" + kReset;
        std::vector<Seg> segs;
        const int i = first + r;
        if (i < n) {
            const themes::Theme& t = sm.theme_list[static_cast<size_t>(i)];
            segs.push_back({header, i == cur ? "● " : "  "});
            // your own themes (not built in) are named in the header colour
            segs.push_back({(t.builtin ? std::string() : header) + (i == sm.theme_sel ? "\x1b[7m" : ""), pad_right(truncate_str(t.name, themes::kMaxNameLen), themes::kMaxNameLen)});
            segs.push_back({"", "  "});
            for (const auto& k : kSw) {
                const std::string* v = t.get(k.key, true);
                const std::string val = v ? *v : "";
                segs.push_back({k.bg ? ansi_bg(val) : ansi_fg(val), k.bg ? "    " : "████"});
                segs.push_back({"", " "});
            }
        }
        // more themes than rows: a scrollbar in the last column (a thumb as tall as the visible part is of the list)
        std::string sb = " ";
        {
        muisc::overlay::Thumb th;
        if (muisc::overlay::scroll_thumb(first, visible, n, th))
            sb = bar(y) + (r >= th.at && r < th.at + th.len ? "\u2588" : "\u2591") + kReset;   // border-gradient colour of that row
        }
        out.push_back(b + " " + seg_line(segs, inner - 2) + " " + sb + " " + b);
    }
    while (static_cast<int>(out.size()) < H - 3) {
        const std::string b = bar(static_cast<int>(out.size())) + "│" + kReset;
        out.push_back(b + " " + spaces(inner) + " " + b);
    }
    out.push_back(bar(H - 3) + "└" + repeat("─", std::max(0, W - 2)) + "┘" + kReset);
    if (sm.leave_prompt == 4 && sm.theme_sel >= 0 && sm.theme_sel < n)
        out.push_back(seg_line({{"\x1b[1;7m", " Do you want to delete theme \"" + truncate_str(sm.theme_list[static_cast<size_t>(sm.theme_sel)].name, themes::kMaxNameLen) + "\"? [y] Yes, delete  [n / ESC] No, keep it"}}, W));
    else
    out.push_back(seg_line(legend_segs(legend, ansi_fg(c.legend_cmd), std::string(sm.theme_ui == 1 ? "[↑↓] Select  [SHIFT+↑↓] Page  [SHIFT+p] Preview  [ENTER] Apply  [SHIFT+d] Delete your own theme  [ESC] Back"
                                                      : "[↑↓] Select  [SHIFT+↑↓] Page  [SHIFT+p] Preview  [ENTER] Use as template  [ESC] Back")), W));
    out.push_back(seg_line({{"\x1b[38;5;246m", utf8_take(sm.status, W)}}, W));
    return out;
}

// SHIFT+e / SHIFT+i overlays: export the current colours as a theme file / import theme files from a folder.
std::vector<std::string> build_theme_io(const RadioSettings& c, const SettingsModel& sm, int W, int rows) {
    const int inner = W - 4;
    const int H = std::max(rows, kUiRows);
    auto bar = [&](int y) {
        if (!c.border_smooth) return ansi_fg(y >= H - 3 ? c.border_bottom : c.border_top);
        const float t = H > 1 ? static_cast<float>(y) / static_cast<float>(H - 1) : 0.0f;
        return gradient_fg(c.border_top, c.border_bottom, std::round(t * 32.0f) / 32.0f);
    };
    const std::string legend = ansi_fg(c.legend), header = ansi_fg(c.header);
    const bool exp = sm.theme_ui == 4;
    std::vector<std::string> out;
    const std::string title = exp ? "─ EXPORT THEME " : "─ IMPORT THEMES ";
    out.push_back(bar(0) + "┌" + title + repeat("─", std::max(0, W - 2 - display_width(title))) + "┐" + kReset);
    auto row_of = [&](const std::vector<Seg>& segs) {
        const std::string b = bar(static_cast<int>(out.size())) + "│" + kReset;
        out.push_back(b + " " + seg_line(segs, inner) + " " + b);
    };
    auto field = [&](const char* label, const std::string& value, bool sel, int width) {
        std::vector<Seg> segs = {{"\x1b[1m" + header, pad_right(label, 15)}, {"", ": "}};
        if (sel && sm.editing) {
            const EditPaint p = paint_edit_field(sm.buffer, sm.edit, width, "\x1b[41;37m", true);
            segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, width + 1 - p.cols))});
        } else {
            segs.push_back({sel ? "\x1b[7m" : "", pad_right(utf8_take(value, width), width + 1)});
        }
        row_of(segs);
    };
    row_of({});
    const int fw = std::max(20, W - 24);
    if (exp) {
        field("NAME", sm.theme_io_name, sm.theme_io_row == 0, themes::kMaxNameLen);
        field("FOLDER", sm.theme_io_dir, sm.theme_io_row == 1, fw);
        row_of({});
        row_of({{legend, utf8_take("Exports the colors now set as one theme file: " + (std::filesystem::path(sm.theme_io_dir) / themes::file_name_for(sm.theme_io_name)).string(), inner)}});
        row_of({{legend, "(radio and player colors are both kept in the file; import it with SHIFT+i)"}});
    } else {
        field("FOLDER", sm.theme_io_dir, sm.theme_io_row == 0, fw);
        row_of({});
        const int nf = static_cast<int>(sm.theme_io_files.size());
        row_of({{"\x1b[1m" + header, "Theme files found (" + std::to_string(nf) + ")"}});
        const int visible = std::max(1, H - 3 - static_cast<int>(out.size()));
        const int first = std::clamp(sm.theme_io_sel - visible / 2, 0, std::max(0, nf - visible));
        for (int r = 0; r < visible && first + r < nf; ++r) {
            const int i = first + r;
            const size_t nth = themes::read_theme_file(sm.theme_io_files[static_cast<size_t>(i)]).size();
            const std::string cnt = std::to_string(nth) + (nth == 1 ? " theme" : " themes");
            const bool sel = sm.theme_io_row == 1 && i == sm.theme_io_sel;
            row_of({{sel ? "\x1b[7m" : "", pad_right(utf8_take(sm.theme_io_files[static_cast<size_t>(i)].filename().string(), inner - 12), inner - 12)}, {legend, pad_left(cnt, 12)}});
        }
        if (nf == 0) row_of({{legend, "no theme files (*.theme, *.txt) in this folder"}});
    }
    while (static_cast<int>(out.size()) < H - 3) row_of({});
    out.push_back(bar(H - 3) + "└" + repeat("─", std::max(0, W - 2)) + "┘" + kReset);
    out.push_back(seg_line(legend_segs(legend, ansi_fg(c.legend_cmd), std::string(sm.editing ? "[ENTER] Apply  [ESC] Cancel  [←→] Cursor  [Ctrl+C/X/V] Copy/Cut/Paste"
                                   : exp ? "[↑↓] Field  [ENTER] Edit  [s] Export  [ESC] Cancel"
                                         : "[↑↓] Select  [ENTER] Import the file (or edit the folder)  [a] Import all  [ESC] Cancel")), W));
    out.push_back(seg_line({{"\x1b[38;5;246m", utf8_take(sm.status, W)}}, W));
    return out;
}

std::vector<std::string> build_settings_impl(const RadioSettings& c, const SettingsModel& sm, int W, int rows, const SettingsModel* cr) {
    const int inner = W - 4;
    const int H = std::max(rows, kUiRows);
    auto bar = [&](int y) {   // frame colour of line y: fades from border_top to border_bottom
        if (!c.border_smooth) return ansi_fg(y >= H - 3 ? c.border_bottom : c.border_top);   // rough: only the bottom border line changes
        const float t = H > 1 ? static_cast<float>(y) / static_cast<float>(H - 1) : 0.0f;
        return gradient_fg(c.border_top, c.border_bottom, std::round(t * 32.0f) / 32.0f);
    };
    const std::string legend = ansi_fg(c.legend), header = ansi_fg(c.header);
    const std::string tab_cur = ansi_fg(c.tab_current), tab_oth = ansi_fg(c.tab_other);
    const std::string V = "│", Hz = "─";
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(H));

    // --- tab strip (2 lines):  ┌─ SETTINGS ─────────┐  COLORS  ┌──┐  [GENERAL]  ┌─────...─┐
    //                           │                    └──────────┘  └────────────┘       ... │
    if (cr) {   // CREATE THEME: title, the NAME / TEMPLATE row, a divider; the COLORS rows of the draft follow
        const std::string title = "─ CREATE THEME ";
        out.push_back(bar(0) + "┌" + title + repeat(Hz, std::max(0, W - 2 - display_width(title))) + "┐" + kReset);
        const std::string b = bar(1) + V + kReset;
        std::vector<Seg> segs;
        segs.push_back({"\x1b[1m" + header, pad_right("NAME", 15)});
        segs.push_back({"", ": "});
        const bool on_name = cr->theme_row < 0 && cr->theme_col == 0;
        const bool on_tpl = cr->theme_row < 0 && cr->theme_col == 1;
        if (on_name && cr->editing) {
            const EditPaint p = paint_edit_field(cr->buffer, cr->edit, themes::kMaxNameLen, "\x1b[41;37m", true);
            segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, themes::kMaxNameLen + 1 - p.cols))});
        } else {
            const bool empty = cr->theme_name.empty();
            segs.push_back({on_name ? "\x1b[7m" : (empty ? legend : ""),
                            pad_right(empty ? std::string("(type a name)") : cr->theme_name, themes::kMaxNameLen + 1)});
        }
        segs.push_back({"", "   "});
        segs.push_back({on_tpl ? "\x1b[7m" : "", " CHOOSE THEME AS TEMPLATE "});
        segs.push_back({legend, "  based on " + cr->theme_base.name});
        out.push_back(b + " " + seg_line(segs, inner) + " " + b);
        out.push_back(bar(2) + "├" + repeat(Hz, std::max(0, W - 2)) + "┤" + kReset);
    } else {
        std::vector<Seg> top, bot;
        const std::string b0 = bar(0), b1 = bar(1);
        top.push_back({b0, "┌─ SETTINGS " + repeat(Hz, 9) + "┐"});
        bot.push_back({b1, V}); bot.push_back({"", spaces(20)}); bot.push_back({b1, "└"});
        int used = 22;
        for (int i = 0; i < 6; ++i) {
            const bool cur = i == sm.tab;
            const std::string label = cur ? std::string("[") + kSettingsTabs[i] + "]" : std::string(kSettingsTabs[i]);
            const int tw = display_width(label) + 4;
            top.push_back({"", "  "}); top.push_back({cur ? tab_cur : tab_oth, label}); top.push_back({"", "  "});
            bot.push_back({b1, repeat(Hz, tw)});
            used += tw;
            if (i < 5) {
                top.push_back({b0, "┌──┐"});
                bot.push_back({b1, "┘  └"});
                used += 4;
            } else {
                const int rest = std::max(0, W - used - 2);
                top.push_back({b0, "┌" + repeat(Hz, rest) + "┐"});
                bot.push_back({b1, "┘" + spaces(rest) + V});
            }
        }
        out.push_back(seg_line(top, W));
        out.push_back(seg_line(bot, W));
    }
    auto line = [&](const std::vector<Seg>& segs) {
        const int y = static_cast<int>(out.size());
        const std::string b = bar(y) + V + kReset;
        out.push_back(b + " " + seg_line(segs, inner) + " " + b);
    };

    if (sm.tab == 1 || sm.tab == 2) {
        // ---- GENERAL and ANIMA/VIZ: rows under category headers (like the player's tabs); scrolls with the cursor
        struct DLine { std::vector<Seg> segs; int row = -1; };
        std::vector<DLine> dl;
        int rr = 0;
        rs_set_hifi_rows(c.bars_style == 1);
        for (const SettingsLine& l : rs_layout(sm.tab)) {
            if (l.header) {   // the layout carries its own blank lines (empty headers); the tab starts with one, like the other tabs
                if (rr == 0 && dl.empty()) dl.push_back({{}, -1});
                dl.push_back({l.text[0] ? std::vector<Seg>{{"\x1b[1m" + header, std::string("   ") + l.text}} : std::vector<Seg>{}, -1});
                continue;
            }
            const bool sel = rr == sm.row;
            std::string v = rs_row_value(c, sm.tab, rr);
            // the "disk" look of the Volume indicator is shown as two braille blocks of the animation itself
            if ((sm.tab == 1 || sm.tab == 2) && v == muisc::kVolIndicatorNames[muisc::kVolStyleDisk]) v = muisc::volume_disk_snippet(muisc::volume_clock_angle());
            std::vector<Seg> segs = {{"", "   " + pad_right(l.text, 26) + ":  "}, {sel ? "\x1b[7m" : "", v}};
            if ((sm.tab == 1 || sm.tab == 2) && std::string(l.text) == "Volume indicator") {   // two values: the look | gradient or not (LEFT / RIGHT switch, ENTER changes)
                segs = {{"", "   " + pad_right(l.text, 26) + ":  "}, {sel && sm.col == 0 ? "\x1b[7m" : "", v},
                        {"", spaces(std::max(0, 10 - display_width(v)))},
                        {sel && sm.col == 1 ? "\x1b[7m" : "", c.volume_gradient ? "gradient   " : "no gradient"}};
                if (sel) segs.push_back({tab_cur, "   [ENTER] change"});
            } else
            if (sel) segs.push_back({tab_cur, spaces(std::max(0, 20 - display_width(v))) + "< \u2194 >"});
            dl.push_back({segs, rr});
            ++rr;
        }
        const int avail = std::max(1, (H - 3) - static_cast<int>(out.size()));
        const int total = static_cast<int>(dl.size());
        int cur = 0;
        for (int i = 0; i < total; ++i) if (dl[static_cast<size_t>(i)].row == sm.row) cur = i;
        int first = std::clamp(cur - avail / 2, 0, std::max(0, total - avail));
        if (first <= 2 && cur < avail) first = 0;   // near the top: keep the first category header visible (as long as the cursor row still fits)
        else if (first > 0 && dl[static_cast<size_t>(first)].row >= 0 && dl[static_cast<size_t>(first) - 1].row < 0) first = std::max(0, first - 1);
        for (int i = first; i < total && i < first + avail; ++i) {
            std::vector<Seg> segs = dl[static_cast<size_t>(i)].segs;
            line(segs);
        }
        while (static_cast<int>(out.size()) < H - 3) line({});
    } else if (sm.tab == 4) {
        // ---- REFERENCE: the note, then the rebindable keys under category headers; scrolls with the cursor
        struct DLine { std::vector<Seg> segs; int action = -1; };
        std::vector<DLine> dl;
        dl.push_back({{}, -1});
        dl.push_back({{{legend, "   SEE CHEAT SHEET FOR FULL LIST OF COMMANDS, `?`"}}, -1});
        {   // selectable row 0: every key back to its default (Ctrl+Shift+U takes it back), right under the note
            const bool sel = sm.row == 0;
            dl.push_back({{{"", "   " + pad_right("Reset All Keys To Default", 26) + ":  "},
                           {sel ? "\x1b[7m" : "", pad_right("[ENTER] reset", 20)}}, 0});
        }
        for (int i = 0; i < kKeyActionCount; ++i) {
            if (kKeyActions[i].header) {
                dl.push_back({{}, -1});
                dl.push_back({{{"\x1b[1m" + header, std::string("   ") + kKeyActions[i].header}}, -1});
            }
            const bool sel = i + 1 == sm.row;
            std::vector<Seg> segs = {{"", "   " + pad_right(kKeyActions[i].label, 26) + ":  "}};
            if (sel && sm.editing) {
                const EditPaint p = paint_edit_field(sm.buffer, sm.edit, 19, "\x1b[41;37m", true);
                segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, 20 - p.cols))});
            } else {
                segs.push_back({sel ? "\x1b[7m" : "", pad_right(pretty_key(key_binding(c, i)), 20)});
            }
            dl.push_back({segs, i + 1});
        }
        const int avail = std::max(1, (H - 3) - static_cast<int>(out.size()));
        int cur = 0;
        for (size_t i = 0; i < dl.size(); ++i) if (dl[i].action == sm.row) cur = static_cast<int>(i);
        const int total = static_cast<int>(dl.size());
        const int first = std::clamp(cur - avail / 2, 0, std::max(0, total - avail));
        for (int i = first; i < total && i < first + avail; ++i) line(dl[static_cast<size_t>(i)].segs);
    } else if (sm.tab == 5) {
        // ---- ABOUT APP: the music player's text, Up / Down scroll
        const int avail = std::max(1, (H - 3) - static_cast<int>(out.size()) - 1);
        line({});
        // centred like the music player's ABOUT tab: each line centred, the block sits vertically in the middle of the rows,
        // moved two rows up (a longer text scrolls and then starts at the top)
        const int total = static_cast<int>(about_app_text().size());
        int bw = 0;
        for (const std::string& s : about_app_text()) bw = std::max(bw, display_width(s));
        const int pad = std::max(0, (inner - bw) / 2);
        const int top = total >= avail ? 0 : std::max(0, (avail - total) / 2 - 2);
        const int first = total > avail ? std::clamp(sm.row, 0, total - avail) : 0;
        for (int r = 0; r < avail; ++r) {
            const int i = first + r - top;
            if (r < top || i < 0 || i >= total) line({});
            else line({{"", spaces(pad) + about_app_text()[static_cast<size_t>(i)]}});
        }
    } else if (sm.tab == 3) {
        // ---- PATHS: the folder fields, the "same as music player" toggle and the history folder (like the player's PATHS tab:
        // names in the header colour, values in the colour of inactive list rows, an empty field shows the default folder)
        line({});
        const StoragePaths sp = resolve_storage_paths(c);
        const std::string list_ink = ansi_fg(c.list_fg) + ansi_bg(c.list_bg);
        constexpr int kFieldW = 60;
        for (int r = 0; r < kPathRowCount; ++r) {
            const bool sel = r == sm.row;
            std::vector<Seg> segs = {{header, "   " + pad_right(kPathRows[r], 28)}, {"", ":  "}};
            if (path_row_is_bool(r)) {
                const std::string v = c.download_same_as_player ? "ON" : "OFF";
                segs.push_back({sel ? "\x1b[7m" : list_ink, v});
                if (sel) segs.push_back({tab_cur, spaces(std::max(0, 20 - display_width(v))) + "< \u2194 >"});
            } else {
                const std::string& val = *path_row_text(c, r);
                const bool unused = r == 2 && c.download_same_as_player;
                if (sel && sm.editing) {
                    const EditPaint p = paint_edit_field(sm.buffer, sm.edit, kFieldW - 1, "\x1b[41;37m", true);
                    segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, kFieldW - p.cols))});
                } else if (val.empty() || unused) {
                    // the folder in effect: the base the radio's own sub-folders are created in
                    std::filesystem::path def;
                    if (r == 0) def = sp.stations_file.parent_path();
                    else if (r == 1) def = sp.presets_file.parent_path();
                    else if (r == 2) def = sp.download_dir.parent_path();
                    else if (r == 5) def.clear();   // no audio book folder yet
                    else def = sp.history_dir.parent_path();
                    const std::string shown = unused ? std::string("SAME PATH AS THE MUSIC PLAYER IS USED") : r == 5 ? std::string("NOT SET (audio books are off until a folder is given)") : def.string();
                    segs.push_back({sel ? "\x1b[7m" : list_ink, pad_right(utf8_take(shown, kFieldW), kFieldW)});
                } else {
                    segs.push_back({sel ? "\x1b[7m" : list_ink, pad_right(utf8_take(val, kFieldW), kFieldW)});
                }
            }
            line(segs);
            if (r != 2) line({});   // DOWNLOAD PATH and "Same folder as music player" belong together
        }
        line({{legend, "   Audio books: every folder in it with audio files is one book. A .cue file beside each book"}});
        line({{legend, "   records where you last stopped listening."}});
        line({{legend, "   When folder paths are changed, the respective files are only copied, never moved, and no deletions are performed."}});
    } else {
        // ---- COLORS
        const int crow = cr ? sm.row : color_row_of_tab(sm.row);   // the colour row the cursor is on (-1: THEME / SMOOTHNESS)
        auto cellv = [&](int r, int col) -> std::string {
            const std::string* f = color_field(c, r, col);
            if (!f) return "";
            return (sm.editing && crow == r && sm.col == col) ? sm.buffer : *f;
        };
        if (!cr) {   // THEME | CREATE THEME, on top
            const int ci = theme_index(c, sm.theme_list);
            const std::string tn = ci >= 0 ? sm.theme_list[static_cast<size_t>(ci)].name : std::string("(custom)");
            const bool on = sm.row == 0;
            line({{"\x1b[1m" + header, pad_right("THEME", 15)}, {"", pad_right("SELECT", 14) + ": "},
                  {on && sm.col == 0 ? "\x1b[7m" : "", pad_right(truncate_str(tn, themes::kMaxNameLen), themes::kMaxNameLen + 1)}, {"", "   "},
                  {on && sm.col == 1 ? "\x1b[7m" : "", " CREATE THEME "}});
            const std::string b = bar(static_cast<int>(out.size()));
            line({{b, repeat(Hz, inner)}});
        }
        auto preview = [&](int r) -> std::vector<Seg> {
            const ColorRowSpec& spec = kColorRows[r];
            const auto is = [&](std::string RadioSettings::* f) { return spec.field1 == f; };
            const std::string v1 = cellv(r, 0), v2 = cellv(r, 1);
            auto grad = [&](const std::string& a, const std::string& b) {
                std::vector<Seg> g;
                for (int i = 0; i < 26; ++i) g.push_back({gradient_fg(a, b, static_cast<float>(i) / 25.0f), "█"});
                return g;
            };
            if (is(&RadioSettings::border_top)) return grad(v1, v2);
            if (is(&RadioSettings::on_air_upper_left)) return grad(v1, v2);
            if (is(&RadioSettings::viz_left)) return grad(v1, v2);
            if (is(&RadioSettings::osci_left)) return grad(v1, v2);
            if (is(&RadioSettings::meta_key))
                return {{ansi_fg(v1.empty() ? c.list_fg : v1), "Station"}, {"", " : "}, {ansi_fg(v2.empty() ? c.list_fg : v2), "Deutschlandfunk"}};
            if (is(&RadioSettings::freq_line))
                return {{ansi_fg(v1), "──╦──┬──╦── "}, {ansi_fg(v2), "92  ·  96"}};
            if (is(&RadioSettings::volume_current))
                return {{"", "VOLUME BAR:["}, {ansi_fg(v1), muisc::volume_bar_fill(c.volume_indicator, 9, muisc::volume_clock_angle())}, {ansi_fg(v2), "-----------"}, {"", "]"}};
            auto styled = [&](const std::string& fg, const std::string& bg, bool cursor) {
                std::string a = ansi_fg(fg) + ansi_bg(bg);
                if (cursor && a.empty()) a = "\x1b[7m";
                return a;
            };
            if (is(&RadioSettings::search_fg))
                return {{ansi_fg(c.search_inactive_fg.empty() ? c.list_fg : c.search_inactive_fg) + ansi_bg(c.search_inactive_bg), "/s: "},
                        {ansi_fg(v1.empty() ? c.list_fg : v1) + ansi_bg(v2.empty() ? c.list_bg : v2), "QUERY"}};
            if (is(&RadioSettings::search_inactive_fg))
                return {{ansi_fg(v1.empty() ? c.list_fg : v1) + ansi_bg(v2), "/s:               "}};
            if (is(&RadioSettings::list_fg)) return {{styled(v1, v2, false), "3 | Station name      | Pop "}};
            if (is(&RadioSettings::list_playing_fg)) return {{styled(v1, v2, false), "3 | Station (tuned)    | Pop "}};
            if (is(&RadioSettings::list_cursor_fg)) return {{styled(v1, v2, true), "3 | Station (hovered)  | Pop "}};
            if (is(&RadioSettings::preset_inactive_fg)) return {{styled(v1, v2, false), "5 -"}};
            if (is(&RadioSettings::preset_key_fg)) return {{ansi_fg(v1), "1"}, {ansi_fg(c.list_fg) + ansi_bg(c.list_bg), " Station name"}};
            if (is(&RadioSettings::header)) return {{ansi_fg(v1), "tuning ...  buffering 40%"}};
            if (is(&RadioSettings::legend)) return {{ansi_fg(v1), "[TAB]"}, {ansi_fg(v2), " Pane  "}, {ansi_fg(v1), "[ENTER]"}, {ansi_fg(v2), " Tune"}};
            if (is(&RadioSettings::tab_current)) return {{ansi_fg(v1), "[COLORS]"}, {"", "  "}, {ansi_fg(v2), "GENERAL  ANIMA/VIZ"}};
            if (is(&RadioSettings::focus)) return {{ansi_fg(c.list_fg), "STATIONS "}, {ansi_fg(v1), "\u25C0"}};
            return {};
        };
        for (int r = 0; r < kColorRowCount; ++r) {
            if (r == kColorRowsAboveDivider) {
                const std::string b = bar(static_cast<int>(out.size()));
                line({{b, repeat(Hz, inner)}});
            }
            const ColorRowSpec& spec = kColorRows[r];
            std::vector<Seg> segs;
            segs.push_back({"\x1b[1m" + header, pad_right(spec.group, 15)});
            auto emit_cell = [&](int col, const std::string& label, int label_w, bool right_align) {
                segs.push_back({"", right_align ? pad_left(label, label_w) : pad_right(label, label_w)});
                segs.push_back({"", ": "});
                const std::string v = cellv(r, col);
                const bool sel = crow == r && sm.col == col;
                if (sel && sm.editing) {   // caret and marked range, like the path and key fields
                    const EditPaint p = paint_edit_field(sm.buffer, sm.edit, 4, "\x1b[41;37m", true);
                    segs.push_back({"\x1b[41;37m", p.s + spaces(std::max(0, 5 - p.cols))});
                }
                else
                    segs.push_back({sel ? "\x1b[7m" : "", pad_right(v, 5)});
                const bool is_bg = (col == 1) && spec.label2 && std::string(spec.label2) == "BG";
                segs.push_back({"", " "});
                segs.push_back({is_bg ? ansi_bg(v) : ansi_fg(v), is_bg ? "  " : "██"});
            };
            emit_cell(0, spec.label1, 14, false);
            if (spec.label2) {
                segs.push_back({"", "    "});
                emit_cell(1, spec.label2, 13, true);
            } else {
                segs.push_back({"", spaces(4 + 13 + 2 + 5 + 3)});
            }
            segs.push_back({"", "  "});
            for (auto& g : preview(r)) segs.push_back(g);
            line(segs);
            if (!cr && r == 0) {   // SMOOTHNESS: how the frame's colour runs from TOP to BOTTOM
                const bool on = sm.row == 2;
                std::vector<Seg> sg = {{"", pad_right("", 15)}, {"", pad_right("SMOOTHNESS", 14) + ": "},
                                       {on ? "\x1b[7m" : "", pad_right(c.border_smooth ? "smooth" : "rough", 7)}};
                if (on) sg.push_back({tab_cur, "< \u2194 >"});
                sg.push_back({legend, std::string(on ? "  " : "       ") + (c.border_smooth ? "frame fades from TOP to BOTTOM" : "only the bottom border line = BOTTOM")});
                line(sg);
            }
        }
    }
    const int body_end = H - 3;   // the square bottom border, the hint line and the status line follow
    while (static_cast<int>(out.size()) < body_end) line({});
    out.push_back(bar(H - 3) + "└" + repeat(Hz, std::max(0, W - 2)) + "┘" + kReset);
    std::string hint;
    if (sm.tab == 4 && sm.editing) hint = "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type one key (or SPACE / TAB / BACKSPACE)";
    else if (sm.tab == 4) hint = "[TAB] Switch  [ENTER] Change  [DEL] Default  [Ctrl+Shift+U] Undo  [s] Save settings  [ESC/q] Discard";
    else if (sm.tab == 5) hint = "[TAB] Switch  [\u2191\u2193] Scroll  [ESC/q] Back";
    else if (sm.tab == 3 && sm.editing) hint = "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type or paste a folder (empty = default)";
    else if (sm.tab == 3) hint = "[TAB] Switch  [ENTER] Edit / toggle  [s] Save settings  [ESC/q] Discard";
    else if (sm.tab >= 1) hint = "[TAB] Switch  [←→/ENTER] Change  [s] Save settings  [ESC/q] Discard";
    else if (cr && cr->editing) hint = cr->theme_row < 0 ? "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type the theme's name (25 characters at most)"
                                                       : "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type a palette number 0-255 (empty or 0 = terminal colour)";
    else if (cr) hint = "[ENTER] Edit/Open  [s] Save theme  [ESC] Discard";
    else if (sm.editing) hint = "[ENTER] Apply  [ESC] Cancel  [\u2190\u2192] Cursor  type a palette number 0-255 (empty or 0 = terminal colour)";
    else hint = "[TAB] Switch  [ENTER] Edit  [e] Edit/Save current state  [SHIFT+e/i] Ex-/Import theme  [s] Save settings  [ESC/q] Discard";
    if (sm.leave_prompt == 3)
        hint = " Do you want to overwrite theme \"" + truncate_str(themes::sanitize_name(sm.theme_name), themes::kMaxNameLen) + "\"? [y] Yes, overwrite  [n / ESC] No, keep editing";
    else if (sm.leave_prompt == 2) hint = " Unsaved theme! [s] Save it  [d] Discard it  [ESC] Keep editing";
    else if (sm.leave_prompt == 1) hint = " Unsaved changes! [s] Save and leave  [d] Discard and leave  [ESC] Keep editing";
    out.push_back(seg_line(sm.leave_prompt ? std::vector<Seg>{{"\x1b[1;7m", hint}} : legend_segs(legend, ansi_fg(c.legend_cmd), hint), W));
    out.push_back(seg_line({{"\x1b[38;5;246m", utf8_take(sm.status.empty() && sm.dirty ? std::string("Unsaved changes! Save with `s` or discard with `ESC`.") : sm.status, W)}}, W));
    return out;
}
std::vector<std::string> build_adev_overlay(const RadioSettings& c, const SettingsModel& sm) {
    const int W = kAdevOverlayW, inner = W - 4;
    const std::string border = ansi_fg(c.border_top), border_bottom = ansi_fg(c.border_bottom);
    const std::string bar = border + c.box_vertical + kReset;
    auto row = [&](const std::string& plain, bool hi) {
        const std::string x = truncate_str(plain, inner);
        const std::string body = x + spaces(std::max(0, inner - display_width(x)));
        return bar + " " + (hi ? "\x1b[7m" + body + kReset : body) + " " + bar;
    };
    auto entry = [&](bool cursor, const std::string& name, const std::string& right) {
        const std::string l = cursor ? "> " : "  ";
        const std::string n = truncate_str(name, std::max(4, inner - 2 - display_width(right) - 1));
        const int gap = std::max(1, inner - 2 - display_width(n) - display_width(right));
        return l + n + spaces(gap) + right;
    };
    auto top = [&](const std::string& title) {
        const std::string t = " " + title + " ";
        return border + c.box_upper_left + c.box_horizontal + t + repeat(c.box_horizontal, std::max(0, W - 3 - display_width(t))) + c.box_upper_right + kReset;
    };
    auto bottom = [&](const std::string& text) {
        const std::string t = " " + truncate_str(text, W - 8) + " ";
        const int dashes = std::max(0, W - 2 - display_width(t));
        return border_bottom + c.box_lower_left + repeat(c.box_horizontal, dashes / 2) + t + repeat(c.box_horizontal, dashes - dashes / 2) + c.box_lower_right + kReset;
    };
    std::string def_name;
    for (const auto& d : sm.adev_list) if (d.is_default) def_name = d.name;
    std::vector<std::string> lines;
    lines.push_back(top("Choose Audio Device"));
    lines.push_back(row("", false));
    lines.push_back(row(entry(sm.adev_sel == 0, "System Default" + (def_name.empty() ? std::string() : " (" + def_name + ")"), c.audio_device.empty() ? "in use" : ""), sm.adev_sel == 0));
    bool there = false;
    for (size_t i = 0; i < sm.adev_list.size(); ++i) {
        const int r = static_cast<int>(i) + 1;
        const bool chosen = !c.audio_device.empty() && c.audio_device == sm.adev_list[i].name;
        if (chosen) there = true;
        std::string mark = chosen ? "in use" : "";
        if (chosen && !sm.adev_active.empty() && sm.adev_active != sm.adev_list[i].name) mark = "not opened";
        lines.push_back(row(entry(sm.adev_sel == r, sm.adev_list[i].name, mark), sm.adev_sel == r));
    }
    if (!c.audio_device.empty() && !there) lines.push_back(row("  " + truncate_str(c.audio_device, inner - 18) + " (not found)", false));
    if (!sm.adev_err.empty()) lines.push_back(row("  " + sm.adev_err, false));
    lines.push_back(row("", false));
    for (const std::string& l : wrap_lines("The choice is the same for the music player, the radio and the audio books. The stream moves to the new device at once.", inner, 3))
        lines.push_back(row(l, false));
    lines.push_back(row("", false));
    lines.push_back(bottom("[UP/DOWN] [ENTER] use  [r] look again  [ESC] close"));
    return lines;
}
std::vector<std::string> build_bars_overlay(const RadioSettings& c, const UiModel& m) {
    const int W = kBarsOverlayW, inner = W - 4;
    const std::string border = ansi_fg(c.border_top), border_bottom = ansi_fg(c.border_bottom);
    const std::vector<int> rows = muisc::bars_rows(c);
    const int n = static_cast<int>(rows.size());
    const int at = std::clamp(m.overlay_row, 0, n - 1);
    int scroll = m.bars_scroll;
    const int room = m.rows - (kPanelH + 2) - 5;   // the frame: top, blank, blank, hint, bottom
    const muisc::BarsWindow w = muisc::bars_window(n, at, scroll, room);
    const_cast<UiModel&>(m).bars_scroll = scroll;
    std::vector<std::string> lines;
    const std::string t = " Spectrum Bars ";
    lines.push_back(border + c.box_upper_left + c.box_horizontal + t + repeat(c.box_horizontal, std::max(0, W - 3 - display_width(t))) + c.box_upper_right + kReset);
    auto frame = [&](const std::string& body, const std::string& right) { return border + c.box_vertical + kReset + " " + body + " " + right; };
    const std::string bar = border + c.box_vertical + kReset;
    lines.push_back(frame(spaces(inner), bar));
    for (int i = 0; i < w.cap; ++i) {
        const int ri = w.first + i;
        const std::string label = muisc::bars_row_label(rows[static_cast<size_t>(ri)]), val = muisc::bars_value(c, rows[static_cast<size_t>(ri)]);
        const std::string l = std::string(ri == at ? "> " : "  ") + label;
        const int gap = std::max(1, inner - display_width(l) - display_width(val));
        const std::string body = truncate_str(l + spaces(gap) + val, inner);
        const std::string padded = body + spaces(std::max(0, inner - display_width(body)));
        const bool thumb = w.scrolls && i >= w.thumb_at && i < w.thumb_at + w.thumb_len;
        lines.push_back(frame(ri == at ? "\x1b[7m" + padded + kReset : padded, thumb ? border + "\xe2\x94\x83" + kReset : bar));
    }
    lines.push_back(frame(spaces(inner), bar));
    const std::string hint = truncate_str("[UP/DOWN] row  [LEFT/RIGHT] change  [r] reset", inner);
    lines.push_back(frame(hint + spaces(std::max(0, inner - display_width(hint))), bar));
    const std::string b = " [B / ESC] close ";
    const int dashes = std::max(0, W - 2 - display_width(b));
    lines.push_back(border_bottom + c.box_lower_left + repeat(c.box_horizontal, dashes / 2) + b + repeat(c.box_horizontal, dashes - dashes / 2) + c.box_lower_right + kReset);
    return lines;
}

std::vector<std::string> build_settings(const RadioSettings& c, const SettingsModel& sm, int W, int rows) {
    if (sm.theme_ui == 4 || sm.theme_ui == 5) return build_theme_io(c, sm, W, rows);
    if (sm.theme_ui == 1 || sm.theme_ui == 3) return build_theme_picker(c, sm, W, rows);
    if (sm.theme_ui == 2) {   // the COLORS rows of the draft, in the draft's own colours
        SettingsModel tm = sm;
        tm.theme_ui = 0; tm.tab = 0;
        tm.row = sm.theme_row; tm.col = sm.theme_col;
        tm.editing = sm.editing && sm.theme_row >= 0;
        return build_settings_impl(sm.theme_draft, tm, W, rows, &sm);
    }
    std::vector<std::string> out = build_settings_impl(c, sm, W, rows, nullptr);
    if (sm.adev_open && sm.adev_from_settings) {   // GENERAL -> AUDIO -> Choose Audio Device, centred over the settings
        const std::vector<std::string> panel = build_adev_overlay(c, sm);
        const int H = static_cast<int>(out.size());
        const int x = std::max(0, (W - kAdevOverlayW) / 2);
        const int y = std::clamp((H - static_cast<int>(panel.size())) / 2, 0, std::max(0, H - static_cast<int>(panel.size())));
        for (size_t i = 0; i < panel.size() && y + static_cast<int>(i) < H; ++i)
            out[static_cast<size_t>(y) + i] = splice_line(out[static_cast<size_t>(y) + i], x, panel[i], kAdevOverlayW);
    }
    // the overlays over the settings (REMOTE APP, FONT): the radio's colours and box glyphs
    muisc::remote_ui::Style st;
    st.border = ansi_fg(c.border_top);
    st.border_bottom = ansi_fg(c.border_bottom);
    st.header = "\x1b[1m" + ansi_fg(c.header);   // captions, like the labels of the settings rows
    st.legend = ansi_fg(c.legend);
    st.text = ansi_fg(c.list_fg);
    st.value = st.text;
    st.change = "\x1b[41;37m";
    st.cursor = "\x1b[7m";
    st.ul = c.box_upper_left; st.ur = c.box_upper_right; st.ll = c.box_lower_left; st.lr = c.box_lower_right;
    st.h = c.box_horizontal;  st.v = c.box_vertical;
    if (c.border_smooth) st.border_at = [&c](float t) { return gradient_fg(c.border_top, c.border_bottom, t); };
    if (sm.remote_ov.open) {   // GENERAL -> REMOTE / UPDATES -> Remote App: the REMOTE APP overlay, centred over the settings
        const muisc::remote_ui::Values v{c.remote_app, c.remote_port, c.remote_pin};
        int w = 0;
        const int H = static_cast<int>(out.size());
        const std::vector<std::string> panel = muisc::remote_ui::build(sm.remote_ov, v, st, W - 2, H, w);
        const int x = std::max(0, (W - w) / 2);
        const int y = std::clamp((H - static_cast<int>(panel.size())) / 2, 0, std::max(0, H - static_cast<int>(panel.size())));
        for (size_t i = 0; i < panel.size() && y + static_cast<int>(i) < H; ++i)
            out[static_cast<size_t>(y) + i] = splice_line(out[static_cast<size_t>(y) + i], x, panel[i], w);
    }
    return out;
}

std::string fmt_loudness(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.0f", v);
    return buf;
}

} // namespace detail
} // namespace muisc::radio
