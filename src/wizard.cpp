// The help wizard: builds the pages from the text file, lays them out at the terminal width, and
// handles the keys, the contents list and the search.
#include "wizard.h"
#include "utf8_util.h"
#include "string_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

#include "help_search.h"
#include "text_edit.h"
#include "version.h"

// The text of the wizard, compiled in: `static const char* const kWizardTextChunks[] = { R"WIZ(...)WIZ", ..., nullptr };`
// Generated from Mousiki_Help_Wizard.txt by tools/wizard_to_inc.py (several chunks because some compilers
// limit the size of one string literal).
#include "wizard_text.inc"

using muisc::ascii_lower_str;
using muisc::trim_ws;

namespace wizard {
namespace {

// ---------------------------------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------------------------------

// One UTF-8 character per element (a wide character gets a second, empty cell in parse_cells(): a cell is a column).
std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 1;
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        if (i + n > s.size()) n = s.size() - i;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

// Is this UTF-8 character two columns wide (Chinese / Japanese / Korean letters, full-width forms)?
bool wide_char(const std::string& ch) {
    if (ch.size() < 3) return false;
    const unsigned char c0 = static_cast<unsigned char>(ch[0]);
    char32_t cp = 0;
    if (c0 >= 0xF0 && ch.size() >= 4) cp = ((c0 & 0x07) << 18) | ((static_cast<unsigned char>(ch[1]) & 0x3F) << 12) | ((static_cast<unsigned char>(ch[2]) & 0x3F) << 6) | (static_cast<unsigned char>(ch[3]) & 0x3F);
    else if (c0 >= 0xE0) cp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(ch[1]) & 0x3F) << 6) | (static_cast<unsigned char>(ch[2]) & 0x3F);
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF)
        || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
}

int cp_len(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}


std::string rtrim(const std::string& s) {
    size_t b = s.size();
    while (b > 0 && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(0, b);
}


std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string& s, const char* p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
    return s;
}

std::string spaces(int n) { return std::string(static_cast<size_t>(std::max(0, n)), ' '); }

// ---------------------------------------------------------------------------------------------------
// the document
// ---------------------------------------------------------------------------------------------------

struct Cell {
    std::string ch;
    Role role = Role::Text;
};

struct Page {
    bool toc = false;
    std::vector<std::string> raw;                 // as written (trailing blanks removed)
    std::vector<std::vector<Cell>> lines;         // parsed
    std::vector<int> line_entry;                  // toc pages: the entry of each line, else -1
    std::string number;                           // "2.1.1" / ""
    std::string norm_title;                       // upper-case letters and digits of the first line, without the number
    std::string border_parent, border_main;       // the chapter titles of the top border
    std::string group;                            // the main chapter: "0" = welcome + contents, "2.3" for 2.3.x
    bool welcome = false;                         // one of the two welcome pages (drawn with the disk)
};

struct Entry {
    int page = 0;           // the contents page it is written on
    int line = 0;
    std::string key;        // "2.1.2" / "2"
    std::string text;       // "Supported Formats"
    bool roman = false;
    int target = -1;        // page it leads to
};

struct Doc {
    std::vector<Page> pages;
    std::vector<Entry> entries;
    std::string problems;
};

// [[Header]] / [[Legend]] colour the "capitalised words" in front of them; see wizard.h.
void apply_marker(std::vector<Cell>& cells, size_t seg_start, Role role, bool toc_page, bool whole_segment) {
    const size_t n = cells.size();
    if (toc_page || whole_segment) {
        for (size_t k = toc_page ? 0 : seg_start; k < n; ++k) cells[k].role = role;
        return;
    }
    size_t end = n;
    bool first = true;
    while (end > seg_start) {
        while (end > seg_start && cells[end - 1].ch == " ") --end;       // blanks between the words
        if (end <= seg_start) break;
        size_t begin = end;
        while (begin > seg_start && cells[begin - 1].ch != " ") --begin;
        bool has_lower = false, has_alnum = false;
        for (size_t k = begin; k < end; ++k) {
            const char c = cells[k].ch.size() == 1 ? cells[k].ch[0] : '\0';
            if (c >= 'a' && c <= 'z') has_lower = true;
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) has_alnum = true;
        }
        const char c0 = cells[begin].ch.size() == 1 ? cells[begin].ch[0] : '\0';
        const bool title_case = c0 >= 'A' && c0 <= 'Z';
        bool take = false, stop_after = false;
        if (first) { take = !has_lower || title_case; stop_after = has_lower; }   // "WIZARD !!!" or a single "Settings"
        else take = !has_lower && (has_alnum || (end - begin == 1 && (cells[begin].ch == "/" || cells[begin].ch == "&" || cells[begin].ch == "+")));   // "AUDIO"
        if (!take) break;
        for (size_t k = begin; k < end; ++k) cells[k].role = role;
        end = begin;
        first = false;
        if (stop_after) break;
    }
}

std::vector<Cell> parse_cells(const std::string& line_in, bool toc_page) {
    const std::string line = replace_all(line_in, "{VERSION}", MOUSIKI_VERSION);
    std::vector<Cell> cells;
    size_t seg_start = 0;
    const std::string lead = trim_ws(line);
    const bool whole_kind = starts_with(lead, "=>") || starts_with(lead, "NOTE:");
    bool header_line = false;                      // [[Header_line]] somewhere in the line: all of it in the header colour
    auto push = [&](const std::string& text, Role r) {
        for (const std::string& ch : utf8_chars(text)) {
            cells.push_back({ch, r});
            if (wide_char(ch)) cells.push_back({"", r});   // (a wide letter takes two columns: one cell for each)
        }
    };
    for (size_t i = 0; i < line.size();) {
        if (line.compare(i, 2, "[[") == 0) {
            const size_t e = line.find("]]", i + 2);
            if (e != std::string::npos) {
                const std::string name_raw = trim_ws(line.substr(i + 2, e - i - 2));
                const std::string name = ascii_lower_str(name_raw);
                if (name == "header_line" || name == "header line" || name == "headerline") {
                    header_line = true;
                } else if (name == "header" || name == "legend") {
                    const bool at_end = trim_ws(line.substr(e + 2)).empty();
                    apply_marker(cells, seg_start, name == "header" ? Role::Header : Role::Legend, toc_page, whole_kind && at_end);
                    seg_start = cells.size();
                } else if (name != "new page") {
                    push(name_raw, Role::Header);        // an unknown [[WORD]] is shown as a header-coloured word
                }
                i = e + 2;
                continue;
            }
        }
        if (line[i] == '[' && line.compare(i, 2, "[[") != 0) {
            // [Text][[Legend]] / [Text][[Header]]: the text takes the marker's colour, the brackets are not shown
            const size_t e = line.find(']', i + 1);
            if (e != std::string::npos && e > i + 1 && line.find('[', i + 1) > e && line.compare(e + 1, 2, "[[") == 0) {
                const size_t me = line.find("]]", e + 3);
                if (me != std::string::npos) {
                    const std::string kind = ascii_lower_str(trim_ws(line.substr(e + 3, me - e - 3)));
                    if (kind == "legend" || kind == "header") {
                        push(line.substr(i + 1, e - i - 1), kind == "header" ? Role::Header : Role::Legend);
                        seg_start = cells.size();
                        i = me + 2;
                        continue;
                    }
                }
            }
        }
        if (line[i] == '[') {
            const size_t e = line.find(']', i + 1);
            if (e != std::string::npos && line.find('[', i + 1) > e) {
                push(line.substr(i, e - i + 1), Role::Legend);   // [KEY]
                i = e + 1;
                continue;
            }
        }
        size_t n = 1;
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c >= 0xF0) n = 4; else if (c >= 0xE0) n = 3; else if (c >= 0xC0) n = 2;
        n = std::min(n, line.size() - i);
        cells.push_back({line.substr(i, n), Role::Text});
        if (wide_char(line.substr(i, n))) cells.push_back({"", Role::Text});
        i += n;
    }
    while (!cells.empty() && cells.back().ch == " ") cells.pop_back();
    if (header_line) for (Cell& c : cells) c.role = Role::Header;
    return cells;
}

// The text of a line without its [[markers]].
std::string plain_text(const std::string& line) {
    std::string out;
    for (size_t i = 0; i < line.size();) {
        if (line.compare(i, 2, "[[") == 0) {
            const size_t e = line.find("]]", i + 2);
            if (e != std::string::npos) { i = e + 2; continue; }
        }
        if (line[i] == '[' && line.compare(i, 2, "[[") != 0) {   // [Text][[Legend]]: the frame is not shown
            const size_t e = line.find(']', i + 1);
            if (e != std::string::npos && e > i + 1 && line.compare(e + 1, 2, "[[") == 0) {
                const size_t me = line.find("]]", e + 3);
                if (me != std::string::npos) { out += line.substr(i + 1, e - i - 1); i = me + 2; continue; }
            }
        }
        out += line[i++];
    }
    return trim_ws(out);
}

bool is_number_token(const std::string& t) {
    if (t.empty()) return false;
    std::string s = t;
    if (s.back() == '.') s.pop_back();
    if (s.empty() || !std::isdigit(static_cast<unsigned char>(s[0])) || !std::isdigit(static_cast<unsigned char>(s.back()))) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
    return true;
}

bool is_roman_token(const std::string& t) {
    if (t.empty() || t.size() > 5) return false;
    for (char c : t) if (c != 'I' && c != 'V' && c != 'X') return false;
    return true;
}

// "2.1.2 Supported Formats" -> number "2.1.2", rest "Supported Formats"; "2 MUSIC PLAYER MODE" (or "II ...") -> a part, roman = true.
// "2.1.2" -> "2.1": the chapter a page number belongs to.
std::string chapter_key(const std::string& number) {
    const size_t a = number.find('.');
    if (a == std::string::npos) return number;
    const size_t b = number.find('.', a + 1);
    return b == std::string::npos ? number : number.substr(0, b);
}

bool split_number(const std::string& text, std::string& number, std::string& rest, bool& roman) {
    const size_t sp = text.find(' ');
    if (sp == std::string::npos) return false;
    std::string tok = text.substr(0, sp);
    const std::string r = trim_ws(text.substr(sp + 1));
    if (r.empty()) return false;
    if (is_number_token(tok)) {
        if (tok.back() == '.') tok.pop_back();
        number = tok; rest = r;
        roman = number.find('.') == std::string::npos;     // "2 MUSIC PLAYER MODE": a part; "2.1 Main UI": a chapter; "2.1.1 ...": a page
        return true;
    }
    if (is_roman_token(tok)) { number = tok; rest = r; roman = true; return true; }
    return false;
}

std::string normalise(const std::string& s) {
    std::string out;
    for (char c : s) if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

void finish_page(Doc& d, Page& p) {
    while (!p.raw.empty() && trim_ws(p.raw.back()).empty()) p.raw.pop_back();
    size_t skip = 0;
    while (skip < p.raw.size() && trim_ws(p.raw[skip]).empty()) ++skip;
    p.raw.erase(p.raw.begin(), p.raw.begin() + static_cast<std::ptrdiff_t>(skip));
    d.pages.push_back(std::move(p));
    p = Page();
}

void build_doc(Doc& d, const std::string& text) {
    // pages
    Page cur;
    bool in_page = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        line = replace_all(line, "\t", "    ");
        const std::string t = trim_ws(line);
        if (starts_with(t, "=>>>")) continue;                                 // a note to the author
        if (ascii_lower_str(t) == "[[new page]]") {
            if (in_page) finish_page(d, cur);
            in_page = true;
            continue;
        }
        if (!in_page) continue;
        cur.raw.push_back(rtrim(line));
    }
    if (in_page) finish_page(d, cur);

    // kind, number and title of every page
    for (Page& p : d.pages) {
        if (p.raw.empty()) continue;
        const std::string first = plain_text(p.raw[0]);
        p.toc = starts_with(upper(first), "TABLE OF CONTENT");
        std::string num, rest;
        bool roman = false;
        if (!p.toc && split_number(first, num, rest, roman) && !roman) { p.number = num; p.norm_title = normalise(rest); }
        else p.norm_title = normalise(first);
    }

    // contents entries
    for (size_t pi = 0; pi < d.pages.size(); ++pi) {
        Page& p = d.pages[pi];
        p.line_entry.assign(p.raw.size(), -1);
        if (!p.toc) continue;
        for (size_t li = 0; li < p.raw.size(); ++li) {
            std::string num, rest;
            bool roman = false;
            if (!split_number(plain_text(p.raw[li]), num, rest, roman)) continue;
            Entry e;
            e.page = static_cast<int>(pi);
            e.line = static_cast<int>(li);
            e.key = num;
            e.text = rest;
            e.roman = roman;
            p.line_entry[li] = static_cast<int>(d.entries.size());
            d.entries.push_back(e);
        }
    }


    // where an entry leads: the page with its number, else the page with its title, else the next entry's page
    for (Entry& e : d.entries) {
        if (!e.roman)
            for (size_t pi = 0; pi < d.pages.size() && e.target < 0; ++pi)
                if (!d.pages[pi].toc && !d.pages[pi].number.empty() && d.pages[pi].number == e.key) e.target = static_cast<int>(pi);
        const std::string want = normalise(e.text);
        for (size_t pi = 0; pi < d.pages.size() && e.target < 0; ++pi)
            if (!d.pages[pi].toc && d.pages[pi].number.empty() && !want.empty() && d.pages[pi].norm_title == want) e.target = static_cast<int>(pi);
    }
    std::vector<char> own(d.entries.size(), 0);   // the entries that found a page of their own
    for (size_t i = 0; i < d.entries.size(); ++i) own[i] = d.entries[i].target >= 0;
    for (int i = static_cast<int>(d.entries.size()) - 2; i >= 0; --i)
        if (d.entries[static_cast<size_t>(i)].target < 0) d.entries[static_cast<size_t>(i)].target = d.entries[static_cast<size_t>(i) + 1].target;
    // A chapter without a page of its own (3.1 "The Radio Main UI") is the first chapter of a part whose intro page ("RADIO PLAYER MODE")
    // sits right in front of the chapter's first page: choosing the chapter shows that intro page, not 9.1 right away.
    for (size_t i = 1; i < d.entries.size(); ++i) {
        Entry& e = d.entries[i];
        const Entry& prev = d.entries[i - 1];
        if (!own[i] && !e.roman && prev.roman && own[i - 1] && e.target >= 1 && prev.target == e.target - 1) e.target = prev.target;
    }

    // the chapter titles of the top border: "2 MUSIC PLAYER MODE" and "2.1 MAIN UI" for a page numbered 2.1.x
    for (size_t pi = 0; pi < d.pages.size(); ++pi) {
        Page& p = d.pages[pi];
        if (p.toc) continue;
        int main_entry = -1;
        if (!p.number.empty()) {
            const std::string main_key = chapter_key(p.number);
            for (size_t k = 0; k < d.entries.size(); ++k)
                if (!d.entries[k].roman && d.entries[k].key == main_key) { main_entry = static_cast<int>(k); break; }
        } else {
            for (size_t k = 0; k < d.entries.size(); ++k)
                if (d.entries[k].roman && d.entries[k].target == static_cast<int>(pi)) { main_entry = static_cast<int>(k); break; }
        }
        if (main_entry < 0) continue;
        const Entry& m = d.entries[static_cast<size_t>(main_entry)];
        p.border_main = m.key + " " + upper(m.text);
        if (!m.roman)
            for (int k = main_entry - 1; k >= 0; --k)
                if (d.entries[static_cast<size_t>(k)].roman) {
                    p.border_parent = d.entries[static_cast<size_t>(k)].key + " " + upper(d.entries[static_cast<size_t>(k)].text);
                    break;
                }
    }

    // the welcome pages, and the main chapter of every page (the arrows never leave it)
    int last_toc = -1;
    for (size_t pi = 0; pi < d.pages.size(); ++pi) if (d.pages[pi].toc) last_toc = static_cast<int>(pi);
    std::string prev_group;
    for (size_t pi = 0; pi < d.pages.size(); ++pi) {
        Page& p = d.pages[pi];
        p.welcome = pi < 2 && !p.toc;
        if (p.toc || static_cast<int>(pi) <= last_toc) p.group = "0";
        else if (!p.number.empty()) p.group = chapter_key(p.number);
        else if (!p.border_main.empty()) p.group = p.border_main.substr(0, p.border_main.find(' '));
        else p.group = (prev_group.empty() || prev_group == "0") ? "p" + std::to_string(pi) : prev_group;
        prev_group = p.group;
    }

    // parse the lines for drawing
    for (Page& p : d.pages) {
        p.lines.clear();
        for (const std::string& r : p.raw) p.lines.push_back(parse_cells(r, p.toc));
    }
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const Doc& doc() {
    static const Doc d = [] {
        Doc out;
        std::string text;
        if (const char* env = std::getenv("MOUSIKI_WIZARD_FILE")) {
            if (*env) {
                text = read_file(env);
                if (text.empty()) out.problems = std::string("MOUSIKI_WIZARD_FILE: cannot read ") + env + " (using the built-in text)";
            }
        }
        if (text.empty())
            for (const char* const* c = kWizardTextChunks; *c; ++c) text += *c;
        build_doc(out, text);
        if (out.pages.empty()) {
            Page p;
            p.raw = {"THE HELP WIZARD HAS NO TEXT [[Header]]", "", "Mousiki_Help_Wizard.txt did not contain a [[New Page]]."};
            for (const std::string& r : p.raw) p.lines.push_back(parse_cells(r, false));
            p.line_entry.assign(p.raw.size(), -1);
            out.pages.push_back(p);
        }
        return out;
    }();
    return d;
}

int last_page() { return static_cast<int>(doc().pages.size()) - 1; }

// first contents page, or -1
int first_toc_page() {
    const Doc& d = doc();
    for (size_t i = 0; i < d.pages.size(); ++i) if (d.pages[i].toc) return static_cast<int>(i);
    return -1;
}

// the contents entry that stands for a page: the last entry that leads exactly there, else the last one before it
int entry_for_page(int page) {
    const Doc& d = doc();
    int best = -1;
    for (size_t k = 0; k < d.entries.size(); ++k)
        if (d.entries[k].target == page) best = static_cast<int>(k);
    if (best >= 0) return best;
    for (size_t k = 0; k < d.entries.size(); ++k)
        if (d.entries[k].target >= 0 && d.entries[k].target <= page) best = static_cast<int>(k);
    return best;
}

int first_entry_on_page(int page) {
    const Doc& d = doc();
    for (size_t k = 0; k < d.entries.size(); ++k) if (d.entries[k].page == page) return static_cast<int>(k);
    return -1;
}

void go_to_page(State& st, int page) {
    page = std::clamp(page, st.min_page, last_page());
    if (page == st.page) return;
    st.page = page;
    st.scroll = 0;
    const Doc& d = doc();
    if (d.pages[static_cast<size_t>(page)].toc) {
        // keep the selection when it is on this page, else take the first entry written on it
        if (st.toc < 0 || st.toc >= static_cast<int>(d.entries.size()) || d.entries[static_cast<size_t>(st.toc)].page != page) {
            const int e = first_entry_on_page(page);
            if (e >= 0) st.toc = e;
        }
    } else {
        const int e = entry_for_page(page);
        if (e >= 0) st.toc = e;
    }
}

// ---------------------------------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------------------------------

Row to_row(const std::vector<Cell>& cells) {
    Row row;
    for (const Cell& c : cells) {
        if (row.empty() || row.back().role != c.role) { Span s; s.role = c.role; row.push_back(s); }
        row.back().text += c.ch;
    }
    return row;
}

int row_width(const Row& r) {
    int w = 0;
    for (const Span& s : r) {
        w += cp_len(s.text);
        for (const std::string& ch : utf8_chars(s.text)) if (wide_char(ch)) ++w;   // (a wide letter is two columns)
    }
    return w;
}

// A line wider than `width` is wrapped at a blank; the continuation keeps the indentation of the line.
std::vector<std::vector<Cell>> wrap_cells(std::vector<Cell> cells, int width, int* wrapped) {
    std::vector<std::vector<Cell>> out;
    width = std::max(8, width);
    size_t indent = 0;
    while (indent < cells.size() && cells[indent].ch == " ") ++indent;
    indent = std::min(indent, static_cast<size_t>(width / 3));
    bool first = true;
    while (static_cast<int>(cells.size()) > width) {
        size_t cut = static_cast<size_t>(width);
        size_t sp = cut;
        while (sp > indent + 1 && cells[sp].ch != " ") --sp;
        if (sp <= indent + 1) sp = cut;                       // no blank to break at: hard cut
        std::vector<Cell> head(cells.begin(), cells.begin() + static_cast<std::ptrdiff_t>(sp));
        while (!head.empty() && head.back().ch == " ") head.pop_back();
        out.push_back(std::move(head));
        size_t from = sp;
        while (from < cells.size() && cells[from].ch == " ") ++from;
        std::vector<Cell> tail(indent, Cell{" ", Role::Text});
        tail.insert(tail.end(), cells.begin() + static_cast<std::ptrdiff_t>(from), cells.end());
        cells = std::move(tail);
        if (first && wrapped) ++*wrapped;
        first = false;
    }
    out.push_back(std::move(cells));
    return out;
}

struct Laid {
    std::vector<int> line_row;      // the first row of each text line of the page (a normal page; the scroll target for a search hit)
    std::vector<Row> rows;          // without margins
    int selected_row = -1;          // the cursor row of a contents page
    int wrapped = 0;
};

// The widest line and the most lines of the contents pages: they are centred as one block, so that every one of them
// keeps its place when the selection moves from one to the next.
void toc_extent(int& w, int& h) {
    static int bw = -1, bh = 0;
    if (bw < 0) {
        bw = 0;
        for (const Page& p : doc().pages) {
            if (!p.toc) continue;
            bh = std::max(bh, static_cast<int>(p.lines.size()));
            for (const auto& l : p.lines) bw = std::max(bw, static_cast<int>(l.size()));
        }
    }
    w = bw;
    h = bh;
}

Laid layout(const Page& p, int inner, int margin, const State& st, const std::vector<std::string>& disk) {
    Laid out;
    const int cw = std::max(10, inner - 2 * margin);
    // a welcome page: the disk on the left, the text beside it
    if (p.welcome) {
        int disk_w = 0;
        for (const std::string& l : disk) disk_w = std::max(disk_w, cp_len(l));
        const int disk_h = static_cast<int>(disk.size());
        int left = std::max(margin, kWelcomeLeft);        // columns in front of the disk
        int text_w = kWelcomeTextWidth;
        bool room = disk_h > 0;
        if (room) {
            const int need = left + disk_w + kDiskGap + text_w + 1;
            if (need > inner) left = std::max(margin, left - (need - inner));     // a narrow frame: the disk moves left first
            text_w = std::min(kWelcomeTextWidth, inner - left - disk_w - kDiskGap - 1);
            room = text_w >= 40;
        }
        if (!room) text_w = cw;
        std::vector<Row> text;
        for (const auto& cells : p.lines)
            for (auto& piece : wrap_cells(cells, text_w, &out.wrapped)) text.push_back(to_row(piece));
        // the disk keeps the place it has in a 120x30 terminal: centred in the 26 rows below the blank row there
        constexpr int kDiskTopRow = 5;
        const int rows = std::max(static_cast<int>(text.size()), room ? kDiskTopRow + disk_h : 0);
        for (int i = 0; i < rows; ++i) {
            Row r;
            if (room) {
                r.push_back({Role::Text, spaces(left - margin), 0.0f});
                const int di = i - kDiskTopRow;
                if (di >= 0 && di < disk_h) {
                    Span s;
                    s.role = Role::Disk;
                    s.text = disk[static_cast<size_t>(di)];
                    s.t = disk_h > 1 ? static_cast<float>(di) / static_cast<float>(disk_h - 1) : 0.0f;
                    r.push_back(s);
                    const int w = cp_len(s.text);
                    if (w < disk_w) r.push_back({Role::Text, spaces(disk_w - w), 0.0f});
                } else {
                    r.push_back({Role::Text, spaces(disk_w), 0.0f});
                }
                r.push_back({Role::Text, spaces(kDiskGap), 0.0f});
            }
            if (i < static_cast<int>(text.size())) for (const Span& s : text[static_cast<size_t>(i)]) r.push_back(s);
            out.rows.push_back(std::move(r));
        }
        return out;
    }
    int toc_w = 0, toc_h = 0;
    if (p.toc) toc_extent(toc_w, toc_h);
    const int block_w = std::min(cw, toc_w);
    const int off = p.toc ? std::max(0, (cw - block_w) / 2) : 0;       // a contents page is centred as a block
    for (size_t li = 0; li < p.lines.size(); ++li) {
        out.line_row.push_back(static_cast<int>(out.rows.size()));
        const bool selected = p.toc && p.line_entry[li] >= 0 && p.line_entry[li] == st.toc;
        if (selected) {
            std::string text;
            for (const Cell& c : p.lines[li]) text += c.ch;
            const int w = cp_len(text);
            Row r;
            if (off > 0) r.push_back({Role::Text, spaces(off), 0.0f});
            r.push_back({Role::Cursor, text + spaces(std::max(w, block_w) - w), 0.0f});
            out.selected_row = static_cast<int>(out.rows.size());
            out.rows.push_back(std::move(r));
            continue;
        }
        for (auto& piece : wrap_cells(p.lines[li], p.toc ? std::max(cw - off, 8) : cw, &out.wrapped)) {
            Row r = to_row(piece);
            if (off > 0) r.insert(r.begin(), Span{Role::Text, spaces(off), 0.0f});
            out.rows.push_back(std::move(r));
        }
    }
    return out;
}

int margin_for(int box_w) { return box_w >= 100 ? kMargin : (box_w >= 70 ? 3 : 1); }

// the pages [first, last] of the main chapter a page belongs to
std::pair<int, int> chapter_range(int page) {
    const Doc& d = doc();
    const std::string& g = d.pages[static_cast<size_t>(page)].group;
    int a = page, b = page;
    while (a > 0 && d.pages[static_cast<size_t>(a - 1)].group == g) --a;
    while (b < last_page() && d.pages[static_cast<size_t>(b + 1)].group == g) ++b;
    return {a, b};
}


// ---------------------------------------------------------------------------------------------------
// search (the search key)
// ---------------------------------------------------------------------------------------------------

struct PageText {
    int page = 0;
    std::string title;                  // the first line of the page ("3.1 MUSIC FILE PATHS")
    std::vector<std::string> lines;     // plain text of each line (the markers removed); blank lines stay, so the index is the page line
};

const std::vector<PageText>& search_index() {
    static const std::vector<PageText> idx = [] {
        std::vector<PageText> v;
        const Doc& d = doc();
        for (size_t i = 0; i < d.pages.size(); ++i) {
            const Page& p = d.pages[i];
            if (p.toc || p.welcome) continue;     // the greeting and the contents lead nowhere new
            PageText t;
            t.page = static_cast<int>(i);
            for (const std::string& r : p.raw) t.lines.push_back(plain_text(r));
            t.title = t.lines.empty() ? std::string() : t.lines[0];
            v.push_back(std::move(t));
        }
        return v;
    }();
    return idx;
}

int popcount32(unsigned v) { int n = 0; for (; v; v >>= 1) n += static_cast<int>(v & 1u); return n; }

std::vector<std::string> query_words(const State& st) {
    std::vector<std::string> w = muisc::helpsearch::split_words(st.query);
    if (w.size() > 8) w.resize(8);
    return w;
}

// The lines of a page that matched, best first.
struct LineHit { int line = 0; muisc::helpsearch::Hit hit; };
std::vector<LineHit> matching_lines(const PageText& pt, const std::vector<std::string>& words) {
    std::vector<LineHit> out;
    for (size_t li = 1; li < pt.lines.size(); ++li) {      // line 0 is the title, shown on its own
        if (pt.lines[li].empty()) continue;
        muisc::helpsearch::Hit h = muisc::helpsearch::match(words, pt.lines[li]);
        if (h.matched > 0) out.push_back({static_cast<int>(li), std::move(h)});
    }
    std::stable_sort(out.begin(), out.end(), [](const LineHit& a, const LineHit& b) {
        if (a.hit.matched != b.hit.matched) return a.hit.matched > b.hit.matched;
        return a.hit.score > b.hit.score;
    });
    return out;
}

// Ranks the pages for the current query (cached until the query changes).
void compute_results(State& st) {
    if (st.cache_query == st.query) return;
    st.cache_query = st.query;
    st.results.clear();
    const std::vector<std::string> words = query_words(st);
    if (words.empty()) return;
    struct R { int page, line; bool full; int matched, score; };
    std::vector<R> rs;
    const unsigned all = (words.size() >= 31) ? ~0u : ((1u << words.size()) - 1u);
    for (const PageText& pt : search_index()) {
        unsigned mask = 0;
        int score = 0;
        const muisc::helpsearch::Hit th = muisc::helpsearch::match(words, pt.title);
        mask |= th.mask;
        score += th.score * 2;
        const std::vector<LineHit> lh = matching_lines(pt, words);
        for (const LineHit& l : lh) mask |= l.hit.mask;
        for (size_t i = 0; i < lh.size() && i < 3; ++i) score += lh[i].hit.score;
        if (!lh.empty()) score += lh[0].hit.matched * 25;      // several words in one line: a better fragment
        const int matched = popcount32(mask);
        if (matched == 0 || matched * 2 < static_cast<int>(words.size())) continue;
        rs.push_back({pt.page, lh.empty() ? 0 : lh[0].line, mask == all, matched, score});
    }
    std::stable_sort(rs.begin(), rs.end(), [](const R& a, const R& b) {
        if (a.full != b.full) return a.full;
        if (a.matched != b.matched) return a.matched > b.matched;
        return a.score > b.score;
    });
    if (rs.size() > 100) rs.resize(100);
    for (const R& r : rs) st.results.emplace_back(r.page, r.line);
}

// Byte offset after `n` code points from `pos`.
size_t cp_advance(const std::string& s, size_t pos, int n) {
    while (pos < s.size() && n > 0) {
        ++pos;
        while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) ++pos;
        --n;
    }
    return pos;
}

// One line of a result as spans: the part around the first match, the matches in Role::Match, cut with ... at both ends.
void excerpt(const std::string& line, const std::vector<muisc::helpsearch::Range>& rg, int width, Row& out) {
    const std::string ell = "\xE2\x80\xA6";
    size_t start = 0;
    if (!rg.empty() && cp_len(line.substr(0, rg.front().from)) > width / 2) {
        // begin a little before the first match, at a word start
        int back = width / 4;
        size_t p = rg.front().from;
        while (p > 0 && back > 0) {
            --p;
            while (p > 0 && (static_cast<unsigned char>(line[p]) & 0xC0) == 0x80) --p;
            --back;
        }
        while (p < rg.front().from && line[p] != ' ') ++p;
        while (p < rg.front().from && line[p] == ' ') ++p;
        start = p;
    }
    int budget = width - (start > 0 ? 1 : 0);
    size_t end = cp_advance(line, start, budget);
    const bool cut = end < line.size();
    if (cut) end = cp_advance(line, start, budget - 1);
    if (start > 0) out.push_back({Role::Text, ell, 0.0f});
    size_t pos = start;
    for (const auto& r : rg) {
        if (r.to <= start || r.from >= end) continue;
        const size_t a = std::max(r.from, start), b = std::min(r.to, end);
        if (a > pos) out.push_back({Role::Text, line.substr(pos, a - pos), 0.0f});
        out.push_back({Role::Match, line.substr(a, b - a), 0.0f});
        pos = b;
    }
    if (pos < end) out.push_back({Role::Text, line.substr(pos, end - pos), 0.0f});
    if (cut) out.push_back({Role::Text, ell, 0.0f});
}

// The rows of the search view (without the margin the frame adds).
std::vector<Row> search_rows(State& st, int cw, int body_rows) {
    std::vector<Row> rows;
    rows.push_back(Row());
    {   // the query, with the caret (or the marked range) in the cursor colour
        Row r;
        r.push_back({Role::Header, "Search: ", 0.0f});
        std::string q = st.query;
        size_t caret = std::min(st.qcaret, q.size()), anchor = std::min(st.qanchor, q.size());
        const int room = std::max(8, cw - 8 - 1);
        while (cp_len(q) > room && caret > 0) {      // a very long query: the front is cut off
            const size_t n = cp_advance(q, 0, 1);
            q.erase(0, n);
            caret = caret >= n ? caret - n : 0;
            anchor = anchor >= n ? anchor - n : 0;
        }
        const size_t a = std::min(caret, anchor), b = std::max(caret, anchor);
        if (a != b) {
            if (a > 0) r.push_back({Role::Text, q.substr(0, a), 0.0f});
            r.push_back({Role::Cursor, q.substr(a, b - a), 0.0f});
            if (b < q.size()) r.push_back({Role::Text, q.substr(b), 0.0f});
        } else {
            if (caret > 0) r.push_back({Role::Text, q.substr(0, caret), 0.0f});
            const size_t n = caret < q.size() ? cp_advance(q, caret, 1) - caret : 0;
            r.push_back({Role::Cursor, n > 0 ? q.substr(caret, n) : std::string(" "), 0.0f});
            if (caret + n < q.size()) r.push_back({Role::Text, q.substr(caret + n), 0.0f});
        }
        rows.push_back(std::move(r));
    }
    const std::vector<std::string> words = query_words(st);
    {
        std::string info;
        if (words.empty()) info = "Type what you look for: words in any order. Typos and related words (delete = remove) are found too.";
        else if (st.results.empty()) info = "Nothing found. Try fewer or other words.";
        else info = std::to_string(st.results.size()) + (st.results.size() == 1 ? " chapter" : " chapters") + " found, the best first";
        Row r;
        r.push_back({Role::Legend, info, 0.0f});
        rows.push_back(std::move(r));
        rows.push_back(Row());
    }
    const int n = static_cast<int>(st.results.size());
    st.sel = std::clamp(st.sel, 0, std::max(0, n - 1));
    st.first = std::clamp(st.first, 0, std::max(0, n - 1));
    const int room = std::max(1, body_rows - static_cast<int>(rows.size()));
    auto fragments_of = [&](int res) {
        std::vector<LineHit> lh;
        for (const PageText& pt : search_index()) {
            if (pt.page != st.results[static_cast<size_t>(res)].first) continue;
            lh = matching_lines(pt, words);
            if (lh.size() > 2) lh.resize(2);
            std::sort(lh.begin(), lh.end(), [](const LineHit& a, const LineHit& b) { return a.line < b.line; });
            break;
        }
        return lh;
    };
    auto block_height = [&](int res) { return 1 + static_cast<int>(std::max<size_t>(1, fragments_of(res).size())) + 1; };
    if (st.sel < st.first) st.first = st.sel;
    for (;;) {   // scroll until the selected result is fully in view
        int h = 0;
        for (int i = st.first; i <= st.sel && i < n; ++i) h += block_height(i);
        if (h <= room || st.first >= st.sel) break;
        ++st.first;
    }
    for (int i = st.first; i < n && static_cast<int>(rows.size()) < body_rows; ++i) {
        const int pg = st.results[static_cast<size_t>(i)].first;
        const PageText* pt = nullptr;
        for (const PageText& x : search_index()) if (x.page == pg) { pt = &x; break; }
        if (!pt) continue;
        Row head;
        head.push_back({i == st.sel ? Role::Cursor : Role::Text, i == st.sel ? "\xE2\x96\xB6 " : "  ", 0.0f});
        const muisc::helpsearch::Hit th = muisc::helpsearch::match(words, pt->title);
        Row titlespans;
        excerpt(pt->title, th.ranges, std::max(10, cw - 4), titlespans);
        for (Span sp : titlespans) { if (sp.role == Role::Text) sp.role = Role::Header; head.push_back(sp); }
        rows.push_back(std::move(head));
        const auto lh = fragments_of(i);
        for (const LineHit& l : lh) {
            if (static_cast<int>(rows.size()) >= body_rows) break;
            Row r;
            r.push_back({Role::Text, "    ", 0.0f});
            excerpt(pt->lines[static_cast<size_t>(l.line)], l.hit.ranges, std::max(10, cw - 6), r);
            rows.push_back(std::move(r));
        }
        if (lh.empty()) rows.push_back(Row());
        rows.push_back(Row());
    }
    return rows;
}

} // namespace

// ---------------------------------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------------------------------

bool shows_disk(int page) {
    const Doc& d = doc();
    return page >= 0 && page < static_cast<int>(d.pages.size()) && d.pages[static_cast<size_t>(page)].welcome;
}

void open(State& st, bool first_launch) {
    const Doc& d = doc();
    st.searching = false;
    if (first_launch && !st.started) {
        st.page = 0;
        st.min_page = 0;
        st.scroll = 0;
    } else if (!st.started) {
        st.min_page = std::min(1, last_page());
        st.page = std::min(1, last_page());
        st.scroll = 0;
    }
    st.started = true;
    if (st.page > last_page()) st.page = last_page();
    if (st.page < st.min_page) st.page = st.min_page;
    if (st.toc < 0 || st.toc >= static_cast<int>(d.entries.size())) st.toc = std::max(0, std::min(0, static_cast<int>(d.entries.size()) - 1));
}

bool handle_key(State& st, Key key) {
    const Doc& d = doc();
    switch (key) {
        case Key::Close: return true;
        case Key::Left:
        case Key::Right: {
            // only inside the main chapter of the page: the table of content is the way to another one
            const bool right = key == Key::Right;
            const auto range = chapter_range(st.page);
            const int to = st.page + (right ? 1 : -1);
            if (to < std::max(range.first, st.min_page) || to > range.second) break;
            go_to_page(st, to);
            break;
        }
        case Key::Up:
        case Key::Down: {
            const Page& p = d.pages[static_cast<size_t>(st.page)];
            if (p.toc && !d.entries.empty()) {
                const int n = static_cast<int>(d.entries.size());
                st.toc = std::clamp(st.toc + (key == Key::Down ? 1 : -1), 0, n - 1);
                const int pg = d.entries[static_cast<size_t>(st.toc)].page;
                if (pg != st.page) { st.page = pg; st.scroll = 0; }
            } else {
                st.scroll = std::clamp(st.scroll + (key == Key::Down ? 1 : -1), 0, std::max(0, st.max_scroll));
            }
            break;
        }
        case Key::PageUp:
        case Key::PageDown: {
            const Page& p = d.pages[static_cast<size_t>(st.page)];
            const int dir = key == Key::PageDown ? 1 : -1;
            if (p.toc && !d.entries.empty()) {
                const int n = static_cast<int>(d.entries.size());
                st.toc = std::clamp(st.toc + dir * 5, 0, n - 1);
                const int pg = d.entries[static_cast<size_t>(st.toc)].page;
                if (pg != st.page) { st.page = pg; st.scroll = 0; }
            } else {
                st.scroll = std::clamp(st.scroll + dir * std::max(1, st.view_rows - 1), 0, std::max(0, st.max_scroll));
            }
            break;
        }
        case Key::Enter: {
            const Page& p = d.pages[static_cast<size_t>(st.page)];
            if (p.toc) {
                if (st.toc >= 0 && st.toc < static_cast<int>(d.entries.size()) && d.entries[static_cast<size_t>(st.toc)].target >= 0)
                    go_to_page(st, d.entries[static_cast<size_t>(st.toc)].target);
            } else if (p.welcome) {
                const int t = first_toc_page();
                if (t >= 0) go_to_page(st, t);
            }
            break;
        }
        case Key::Back:
        case Key::Contents: {
            const int cur = st.page;
            if (d.pages[static_cast<size_t>(cur)].toc) break;
            const int e = entry_for_page(cur);
            int t = e >= 0 ? d.entries[static_cast<size_t>(e)].page : first_toc_page();
            if (t < 0) break;
            if (e >= 0) st.toc = e;
            st.page = t;
            st.scroll = 0;
            break;
        }
        case Key::Search: {
            st.searching = true;
            st.query.clear();
            st.qcaret = st.qanchor = 0;
            st.sel = st.first = 0;
            st.cache_query = "\x01";      // not a query: forces the first computation
            st.results.clear();
            break;
        }
        case Key::None: break;
    }
    return false;
}

void search_key(State& st, int key, bool arrow, bool shifted) {
    if (!st.searching) return;
    compute_results(st);
    const int n = static_cast<int>(st.results.size());
    if (!arrow && key == 27) {          // ESC: first the query goes, then the search view
        if (!st.query.empty()) { st.query.clear(); st.qcaret = st.qanchor = 0; st.sel = st.first = 0; }
        else st.searching = false;
        return;
    }
    if (arrow && (key == 'A' || key == 'B')) {
        const int step = shifted ? 5 : 1;
        st.sel = std::clamp(st.sel + (key == 'B' ? step : -step), 0, std::max(0, n - 1));
        return;
    }
    if (!arrow && (key == '\r' || key == '\n')) {
        if (n > 0) {
            const auto r = st.results[static_cast<size_t>(std::clamp(st.sel, 0, n - 1))];
            st.searching = false;
            go_to_page(st, r.first);
            st.jump_line = r.second;
        }
        return;
    }
    if (muisc::edit_text_key(st.query, st.qcaret, st.qanchor, key, 80, nullptr)) { st.sel = 0; st.first = 0; }
}

View render(State& st, int cols, int body_rows, const std::vector<std::string>& disk) {
    const Doc& d = doc();
    View v;
    v.box_w = std::clamp(cols, 20, kBoxWidth);
    v.left = std::max(0, (cols - v.box_w) / 2);
    const int inner = v.box_w - 2;
    const int margin = margin_for(v.box_w);
    if (st.page > last_page()) st.page = last_page();
    if (st.page < st.min_page) st.page = st.min_page;
    const Page& p = d.pages[static_cast<size_t>(st.page)];

    if (st.searching) {     // the search key: the search view replaces the page
        compute_results(st);
        v.title = "MOUSIKI PLUS HELP WIZARD \xE2\x94\x80 SEARCH";
        const std::vector<Row> body = search_rows(st, inner - 2 * margin - 1, body_rows);
        const std::string lead = spaces(margin);
        for (int line = 0; line < body_rows; ++line) {
            Row out;
            out.push_back({Role::Text, lead, 0.0f});
            int w = margin;
            if (line < static_cast<int>(body.size())) {
                for (const Span& sp : body[static_cast<size_t>(line)]) out.push_back(sp);
                w += row_width(body[static_cast<size_t>(line)]);
            }
            if (w < inner) out.push_back({Role::Text, spaces(inner - w), 0.0f});
            v.rows.push_back(std::move(out));
        }
        st.view_rows = std::max(1, body_rows);
        st.max_scroll = 0;
        v.page_no = st.page - st.min_page + 1;
        v.page_total = last_page() - st.min_page + 1;
        v.search = true;
        return v;
    }

    // top border title
    {
        const std::string base = "MOUSIKI PLUS HELP WIZARD";
        const std::string sep = " \xE2\x94\x80 ";                        // " ─ "
        const int room = v.box_w - 8;
        std::string t = base;
        if (!p.border_main.empty()) {
            std::string full = base + sep + (p.border_parent.empty() ? "" : p.border_parent + sep) + p.border_main;
            std::string short_ = base + sep + p.border_main;
            if (cp_len(full) <= room) t = full;
            else if (cp_len(short_) <= room) t = short_;
            else t = base;
        }
        v.title = t;
    }

    Laid laid = layout(p, inner, margin, st, disk);
    // a contents page is centred vertically too (as the block of all contents pages)
    if (p.toc) {
        int tw = 0, th = 0;
        toc_extent(tw, th);
        const int room = std::max(1, body_rows - 2);
        const int voff = th < room ? (room - th) / 2 : 0;
        if (voff > 0) {
            laid.rows.insert(laid.rows.begin(), static_cast<size_t>(voff), Row());
            if (laid.selected_row >= 0) laid.selected_row += voff;
        }
    }
    // an empty line at the beginning and at the end of every page (they scroll with it)
    laid.rows.insert(laid.rows.begin(), Row());
    laid.rows.push_back(Row());
    if (laid.selected_row >= 0) ++laid.selected_row;
    if (st.jump_line >= 0) {          // a search result was opened: show its line, with one line of context above
        if (!p.toc && st.jump_line < static_cast<int>(laid.line_row.size()))
            st.scroll = std::max(0, laid.line_row[static_cast<size_t>(st.jump_line)] + 1 - 2);
        st.jump_line = -1;
    }
    const int total = static_cast<int>(laid.rows.size());
    const int avail = std::max(1, body_rows);
    st.view_rows = avail;
    st.max_scroll = std::max(0, total - avail);
    if (p.toc && laid.selected_row >= 0) {          // the page follows the selection
        // one row of margin on both sides, so the empty line at the top / bottom of the page shows at its ends
        if (laid.selected_row < st.scroll + 1) st.scroll = laid.selected_row - 1;
        if (laid.selected_row >= st.scroll + avail - 1) st.scroll = laid.selected_row - avail + 2;
        if (laid.selected_row <= avail - 2) st.scroll = 0;       // the top of the page (its title) fits above the selection: show it
    }
    st.scroll = std::clamp(st.scroll, 0, st.max_scroll);
    v.scrollable = total > avail;

    const std::string lead = spaces(margin);
    // the scrollbar: the last column of every row, a thumb as tall as the visible part is of the page
    const int thumb = std::max(1, std::min(body_rows, (body_rows * avail + total - 1) / std::max(1, total)));
    const int thumb_at = st.max_scroll > 0 ? (st.scroll * (body_rows - thumb) + st.max_scroll / 2) / st.max_scroll : 0;
    auto framed = [&](const Row& r, int line) {
        Row out;
        out.push_back({Role::Text, lead, 0.0f});
        for (const Span& s : r) out.push_back(s);
        const int w = margin + row_width(r);
        if (v.scrollable) {
            if (w < inner - 1) out.push_back({Role::Text, spaces(inner - 1 - w), 0.0f});
            const bool on = line >= thumb_at && line < thumb_at + thumb;
            out.push_back({Role::Scroll, on ? "\xE2\x96\x88" : "\xE2\x96\x91",      // full block / light shade
                           body_rows > 1 ? static_cast<float>(line) / static_cast<float>(body_rows - 1) : 0.0f});
        } else if (w < inner) {
            out.push_back({Role::Text, spaces(inner - w), 0.0f});
        }
        return out;
    };
    for (int line = 0; line < body_rows; ++line) {
        const int i = st.scroll + line;
        v.rows.push_back(framed(i < total ? laid.rows[static_cast<size_t>(i)] : Row(), line));
    }

    // the arrows stay inside the main chapter
    const auto range = chapter_range(st.page);
    v.page_no = st.page - st.min_page + 1;
    v.page_total = last_page() - st.min_page + 1;
    v.can_back = st.page > std::max(range.first, st.min_page);
    v.can_next = st.page < range.second;
    v.toc_page = p.toc;
    return v;
}

int page_count() { return static_cast<int>(doc().pages.size()); }

int page_rows(int page, int cols) {
    const Doc& d = doc();
    if (page < 0 || page >= static_cast<int>(d.pages.size())) return 0;
    const int box_w = std::clamp(cols, 20, kBoxWidth);
    State st;
    st.toc = -1;
    std::vector<std::string> disk(15, std::string(30, 'x'));
    return static_cast<int>(layout(d.pages[static_cast<size_t>(page)], box_w - 2, margin_for(box_w), st, disk).rows.size());
}

int page_title_width(int page) {
    const Doc& d = doc();
    if (page < 0 || page >= static_cast<int>(d.pages.size())) return 0;
    const Page& p = d.pages[static_cast<size_t>(page)];
    std::string t = "MOUSIKI PLUS HELP WIZARD";
    if (!p.border_main.empty()) t += " - " + (p.border_parent.empty() ? "" : p.border_parent + " - ") + p.border_main;
    return cp_len(t);
}

const std::string& problems() { return doc().problems; }

} // namespace wizard
