// The meta data editor: the field names and labels, the edit state per file, and the options of the
// online meta fetch (which fields to fill in).
#include "app_dirs.h"
#include "meta_editor.h"
#include "chip_meta.h"
#include "path_utf8.h"
#include "process_util.h"
#include "tiny_json.h"
#include "utf8_util.h"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

using namespace tinyjson;

// =====================================================================
// Field tables
// =====================================================================

const char* meta_field_name(int i) {
    switch (i) {
        case 0: return "filename";
        case 1: return "artist";
        case 2: return "title";
        case 3: return "album";
        case 4: return "year";
        default: return "";
    }
}

const char* meta_field_label(int i) {
    switch (i) {
        case 0: return "FILE";
        case 1: return "ARTIST";
        case 2: return "TITLE";
        case 3: return "ALBUM";
        case 4: return "YEAR";
        default: return "";
    }
}

bool MetaEditEntry::any_edited() const {
    for (bool e : edited) if (e) return true;
    return false;
}

bool MetaEditEntry::any_needs_choice() const {
    for (int i = 0; i < kMetaFieldCount; ++i) if (needs_choice(i)) return true;
    return false;
}

void MetaEditEntry::clear_field(int i) {
    edited[i] = false;
    value[i].clear();
    fetched[i] = false;
    choices[i].clear();
    choice_aux[i].clear();
}

const char* meta_fetch_opt_label(int i) {
    switch (i) {
        case 0: return "missing only";
        case 1: return "all";
        case 2: return "artist";
        case 3: return "title";
        case 4: return "album";
        case 5: return "year";
        default: return "";
    }
}

unsigned meta_fetch_opt_toggle(unsigned opts, int i) {
    if (i < 0 || i >= kMetaFetchOptCount) return opts;
    const unsigned bit = 1u << i;
    const unsigned tags = 0x3Cu;                      // artist | title | album | year
    if (opts & bit) {                                 // unticking
        const unsigned rest = opts & ~bit;
        return rest ? rest : opts;                    // never leave nothing ticked
    }
    if (i == 0 || i == 1) return bit;                 // missing only / all: alone
    return (opts & tags) | bit;                       // a single tag: drops missing only / all, keeps the other tags
}

std::string meta_fetch_opt_summary(unsigned opts) {
    std::string s;
    for (int i = 0; i < kMetaFetchOptCount; ++i) {
        if (!(opts & (1u << i))) continue;
        if (!s.empty()) s += ", ";
        s += meta_fetch_opt_label(i);
    }
    return s.empty() ? std::string("missing only") : s;
}

// =====================================================================
// Autosaved session -- a BACKUP of the edits, never an application of them
// =====================================================================

fs::path meta_session_path() {
    // Same base as snapshot.cpp: $HOME/.cache/mousiki/... (HOME is UTF-8 on
    // Windows, so go through path_from_utf8 rather than fs::path(std::string)).
    const char* home = std::getenv("HOME");
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    return muisc::app_dirs::cache() / "meta_session" / "session.json";
}

bool load_meta_session(std::vector<MetaEditEntry>& entries, std::vector<std::string>& fetch_list,
                       unsigned& fetch_opts) {
    fs::path p = meta_session_path();
    std::error_code ec;
    if (!fs::exists(p, ec)) return false;

    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.empty()) return false;

    Value root;
    if (!parse(text, root) || root.type != Type::Object) return false;

    if (auto* fl = root.find("fetch_list")) {
        if (fl->type == Type::Array) {
            for (const auto& item : fl->arr) {
                if (item.type == Type::String && !item.str.empty()) fetch_list.push_back(item.str);
            }
        }
    }
    if (auto* fo = root.find("fetch_options")) {
        const unsigned v = static_cast<unsigned>(fo->as_number(1));
        if (v != 0 && v < (1u << kMetaFetchOptCount)) fetch_opts = v;
    }
    if (auto* arr = root.find("entries")) {
        if (arr->type == Type::Array) {
            for (const auto& item : arr->arr) {
                if (item.type != Type::Object) continue;
                MetaEditEntry e;
                if (auto* pth = item.find("path")) e.path = pth->as_string();
                if (e.path.empty()) continue;
                if (auto* ed = item.find("edited")) {
                    if (ed->type == Type::Array) {
                        for (const auto& name : ed->arr) {
                            if (name.type != Type::String) continue;
                            for (int i = 0; i < kMetaFieldCount; ++i) {
                                if (name.str == meta_field_name(i)) e.edited[i] = true;
                            }
                        }
                    }
                }
                for (int i = 0; i < kMetaFieldCount; ++i) {
                    if (auto* v = item.find(meta_field_name(i))) e.value[i] = v->as_string();
                }
                if (auto* fe = item.find("fetched")) {
                    if (fe->type == Type::Array) {
                        for (const auto& name : fe->arr) {
                            if (name.type != Type::String) continue;
                            for (int i = 0; i < kMetaFieldCount; ++i) {
                                if (name.str == meta_field_name(i) && e.edited[i]) e.fetched[i] = true;
                            }
                        }
                    }
                }
                for (int pass = 0; pass < 2; ++pass) {
                    if (auto* ch = item.find(pass == 0 ? "choices" : "choice_aux")) {
                        if (ch->type != Type::Object) continue;
                        for (int i = 0; i < kMetaFieldCount; ++i) {
                            auto* list = ch->find(meta_field_name(i));
                            if (!list || list->type != Type::Array) continue;
                            for (const auto& s : list->arr) {
                                if (s.type == Type::String) (pass == 0 ? e.choices[i] : e.choice_aux[i]).push_back(s.str);
                            }
                        }
                    }
                }
                if (e.any_edited()) entries.push_back(std::move(e));
            }
        }
    }
    return !entries.empty() || !fetch_list.empty();
}

void save_meta_session(const std::vector<MetaEditEntry>& entries,
                       const std::vector<std::string>& fetch_list,
                       unsigned fetch_opts) {
    fs::path p = meta_session_path();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    Value root = Value::make_obj();
    root.set("version", Value::make_num(1));

    Value fl = Value::make_arr();
    for (const auto& s : fetch_list) fl.arr.push_back(Value::make_str(s));
    root.set("fetch_list", fl);
    root.set("fetch_options", Value::make_num(static_cast<double>(fetch_opts)));

    Value arr = Value::make_arr();
    for (const auto& e : entries) {
        Value v = Value::make_obj();
        v.set("path", Value::make_str(e.path));
        Value edited = Value::make_arr();
        for (int i = 0; i < kMetaFieldCount; ++i) {
            if (e.edited[i]) edited.arr.push_back(Value::make_str(meta_field_name(i)));
        }
        v.set("edited", edited);
        for (int i = 0; i < kMetaFieldCount; ++i) v.set(meta_field_name(i), Value::make_str(e.value[i]));
        Value fetched = Value::make_arr();
        Value choices = Value::make_obj(), aux = Value::make_obj();
        for (int i = 0; i < kMetaFieldCount; ++i) {
            if (e.fetched[i]) fetched.arr.push_back(Value::make_str(meta_field_name(i)));
            if (!e.choices[i].empty()) {
                Value l = Value::make_arr();
                for (const auto& s : e.choices[i]) l.arr.push_back(Value::make_str(s));
                choices.set(meta_field_name(i), l);
                if (!e.choice_aux[i].empty()) {
                    Value a = Value::make_arr();
                    for (const auto& s : e.choice_aux[i]) a.arr.push_back(Value::make_str(s));
                    aux.set(meta_field_name(i), a);
                }
            }
        }
        v.set("fetched", fetched);
        v.set("choices", choices);
        v.set("choice_aux", aux);
        arr.arr.push_back(std::move(v));
    }
    root.set("entries", arr);

    // Same write-then-rename sequence snapshot.cpp uses: a crash mid-write
    // must not be able to leave a truncated, unparseable session.json behind
    // (that would silently destroy exactly the work this file exists to keep).
    fs::path tmp = p;
    tmp += ".tmp";
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out.is_open()) return;
    out << write(root);
    out.close();
    fs::rename(tmp, p, ec);
    if (ec) {
        std::ofstream direct(p, std::ios::trunc | std::ios::binary);
        if (direct.is_open()) direct << write(root);
        std::error_code ignored;
        fs::remove(tmp, ignored);
    }
}

void delete_meta_session() {
    std::error_code ec;
    fs::remove(meta_session_path(), ec);
}

// =====================================================================
// Applying the session to the files (Ctrl+Shift+S)
// =====================================================================

namespace {

bool ascii_iequal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// Turns a typed file name into something Windows will actually accept, and
// (crucially) into a name that still carries the file's ORIGINAL extension:
// the FILE field edits the name, not the format, so typing "song.txt" or just
// "song" both end up as "song.txt.mp3"/"song.mp3" rather than silently
// turning an audio file into something the player can no longer open.
std::string normalize_file_name(std::string name, const std::string& ext) {
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '.')) name.pop_back();
    if (name.empty()) return std::string();

    for (char& c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || u < 32) {
            c = '_';
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    if (name.empty()) return std::string();

    if (!ext.empty()) {
        bool has_ext = name.size() >= ext.size() &&
                       ascii_iequal(name.substr(name.size() - ext.size()), ext);
        if (!has_ext) name += ext;
    }
    return name;
}

std::string first_line(std::string s) {
    size_t nl = s.find('\n');
    if (nl != std::string::npos) s.resize(nl);
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
    if (s.size() > 160) s.resize(160);
    return s;
}


} // namespace

bool apply_meta_entry(const MetaEditEntry& e, std::string* err, std::string* new_path) {
    auto fail = [&](const std::string& msg) {
        if (err) *err = msg;
        return false;
    };
    if (new_path) new_path->clear();

    fs::path old_path = path_from_utf8(e.path);
    std::error_code ec;
    if (!fs::exists(old_path, ec)) return fail("file not found: " + e.path);

    const std::string ext = path_utf8(old_path.extension());

    // --- 1. tags: remux with -c copy (audio untouched) into a sibling temp
    // file, then rename over the original. ffmpeg cannot write into its own
    // input, and writing straight to the real file would risk truncating it
    // if anything failed midway.
    bool want_tags = e.edited[static_cast<int>(MetaField::Artist)] ||
                     e.edited[static_cast<int>(MetaField::Title)] ||
                     e.edited[static_cast<int>(MetaField::Album)] ||
                     e.edited[static_cast<int>(MetaField::Year)];
    if (want_tags && chipmeta::applies(old_path)) {
        // Tracker modules and chiptunes: ffmpeg cannot write them. The fields the file can hold go into its header, the
        // others into the side file next to it (see chip_meta.h).
        std::string werr;
        if (!chipmeta::save_tags(old_path, e, &werr)) return fail(werr);
    } else if (want_tags) {
        fs::path tmp = old_path.parent_path() /
                       (path_utf8(old_path.stem()) + ".mousiki-tmp" + ext);
        // -map 0 keeps every stream (including embedded cover art); explicit
        // -map_metadata 0 copies the existing tags so only the keys passed
        // below are replaced -- an edit to ARTIST can't wipe the album.
        Args cmd = {"ffmpeg", "-y", "-v", "error", "-i", path_utf8(old_path), "-map", "0", "-map_metadata", "0", "-c", "copy"};
        auto add_meta = [&](int field, const char* key) {
            if (!e.edited[field]) return;
            cmd.push_back("-metadata");
            cmd.push_back(std::string(key) + "=" + e.value[field]);
        };
        add_meta(static_cast<int>(MetaField::Artist), "artist");
        add_meta(static_cast<int>(MetaField::Title), "title");
        add_meta(static_cast<int>(MetaField::Album), "album");
        add_meta(static_cast<int>(MetaField::Year), "date"); // containers all call the year "date"
        cmd.push_back(path_utf8(tmp));

        ProcResult r = run_capture(cmd, /*merge_stderr=*/true);
        if (!r.ok()) {
            std::error_code ignore;
            fs::remove(tmp, ignore);
            std::string detail = first_line(r.out);
            return fail("ffmpeg failed" + (detail.empty() ? std::string() : ": " + detail));
        }
        fs::rename(tmp, old_path, ec);
        if (ec) {
            std::error_code ignore;
            fs::remove(tmp, ignore);
            return fail("could not replace the file: " + ec.message() +
                        " (is it playing right now?)");
        }
    }

    // --- 2. rename ---
    fs::path final_path = old_path;
    if (e.edited[static_cast<int>(MetaField::FileName)]) {
        std::string target_name = normalize_file_name(e.value[static_cast<int>(MetaField::FileName)], ext);
        if (!target_name.empty() && target_name != path_utf8(old_path.filename())) {
            fs::path new_path_file = old_path.parent_path() / path_from_utf8(target_name);
            if (fs::exists(new_path_file, ec)) return fail("a file named \"" + target_name + "\" already exists");
            std::error_code rename_ec;
            fs::rename(old_path, new_path_file, rename_ec);
            if (rename_ec) return fail("rename failed: " + rename_ec.message());
            if (chipmeta::applies(old_path)) chipmeta::rename_entry(old_path.parent_path(), path_utf8(old_path.filename()), target_name);
            final_path = new_path_file;
        }
    }

    if (new_path) *new_path = path_utf8(final_path);
    if (err) err->clear();
    return true;
}

// =====================================================================
// AcoustID (scripts/fetch_meta.py)
// =====================================================================

void MetaFetchCancel::request() {
    requested_ = true;
    std::lock_guard<std::mutex> lk(mu_);
    if (child_) child_->kill();
}

void MetaFetchCancel::attach(ChildProcess* child) {
    std::lock_guard<std::mutex> lk(mu_);
    child_ = child;
    if (child_ && requested_.load()) child_->kill();
}

MetaFetchOutcome run_acoustid_fetch(const std::vector<MetaFetchRequest>& reqs,
                                    const fs::path& script,
                                    const std::function<void(const MetaFetchResult&)>& on_result,
                                    MetaFetchCancel* cancel) {
    MetaFetchOutcome out;
    if (reqs.empty()) {
        out.error = "nothing to fetch";
        return out;
    }
    std::error_code ec;
    if (!fs::exists(script, ec)) {
        out.script_missing = true;
        out.error = "fetch script not found: " + path_utf8(script);
        return out;
    }
    // No key check here: the AcoustID application key is hard-coded in the
    // script (API_KEY), so it is present by construction. If a fork empties
    // it, the script reports that once for the whole batch instead of one
    // failure per queued file.

    Args cmd = python_command();
    if (cmd.empty()) {
        out.python_missing = true;
        out.error = "python not found (Python 3 is required for AcoustID lookups)";
        return out;
    }

    // One request file per batch: a plain argv-only interface keeps the whole
    // command line free of quoting/length problems, and titles with quotes,
    // ampersands or non-ASCII text survive intact.
    fs::path req_file = meta_session_path().parent_path() / "fetch_request.json";
    fs::create_directories(req_file.parent_path(), ec);
    {
        Value root = Value::make_obj();
        Value arr = Value::make_arr();
        for (const auto& r : reqs) {
            Value v = Value::make_obj();
            v.set("path", Value::make_str(r.path));
            v.set("title", Value::make_str(r.title));
            v.set("artist", Value::make_str(r.artist));
            Value want = Value::make_arr();
            for (int i = 1; i < kMetaFieldCount; ++i) {
                if (r.want[i]) want.arr.push_back(Value::make_str(meta_field_name(i)));
            }
            v.set("want", want);
            arr.arr.push_back(std::move(v));
        }
        root.set("requests", arr);
        std::ofstream outf(req_file, std::ios::trunc | std::ios::binary);
        if (!outf.is_open()) {
            out.error = "could not write " + path_utf8(req_file);
            return out;
        }
        outf << write(root);
    }

    cmd.push_back(path_utf8(script));
    cmd.push_back(path_utf8(req_file));
    auto child = spawn_capture(cmd, /*merge_stderr=*/true);
    if (!child) {
        fs::remove(req_file, ec);
        out.error = "could not start " + command_text(cmd);
        return out;
    }

    if (cancel) cancel->attach(child.get());   // a cancel that came in before this point kills it right away

    auto handle_line = [&](std::string line) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) return;
        if (line[0] != '{') return; // stderr noise / progress chatter -- only JSON objects are results
        Value v;
        if (!parse(line, v) || v.type != Type::Object) return;
        if (v.find("done")) return; // trailer line, counts are ours to keep

        MetaFetchResult r;
        if (auto* p = v.find("path")) r.path = p->as_string();
        if (auto* ok = v.find("ok")) r.ok = ok->as_bool(false);
        if (r.ok) {
            // FileName (0) is deliberately never filled in: AcoustID has
            // no opinion about file names.
            for (int i = 1; i < kMetaFieldCount; ++i) {
                if (auto* val = v.find(meta_field_name(i))) {
                    r.set[i] = true;
                    r.value[i] = val->as_string();
                }
                // "<field>_choices": [{"value": "...", "aux": "..."}, ...] -- every candidate, best first.
                if (auto* cl = v.find(std::string(meta_field_name(i)) + "_choices")) {
                    if (cl->type == Type::Array) {
                        for (const auto& item : cl->arr) {
                            if (item.type != Type::Object) continue;
                            auto* cv = item.find("value");
                            if (!cv || cv->as_string().empty()) continue;
                            r.choices[i].push_back(cv->as_string());
                            auto* ca = item.find("aux");
                            r.choice_aux[i].push_back(ca ? ca->as_string() : std::string());
                        }
                    }
                }
            }
            if (auto* n = v.find("note")) r.note = n->as_string();
            ++out.ok_count;
        } else {
            if (auto* e = v.find("error")) r.error = e->as_string();
            if (auto* d = v.find("detail")) {
                if (!r.error.empty()) r.error += ": ";
                r.error += d->as_string();
            }
            if (r.error.empty()) r.error = "lookup failed";
            ++out.fail_count;
            if (out.error.empty()) out.error = r.error;
        }
        if (on_result) on_result(r);
    };

    std::string buffer;
    char chunk[4096];
    for (;;) {
        std::ptrdiff_t n = child->read(chunk, sizeof chunk);
        if (n <= 0) break;
        buffer.append(chunk, static_cast<size_t>(n));
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            handle_line(buffer.substr(0, pos));
            buffer.erase(0, pos + 1);
        }
    }
    if (!buffer.empty()) handle_line(buffer);
    int rc = child->wait();
    if (cancel) cancel->attach(nullptr);
    out.cancelled = cancel && cancel->requested();
    fs::remove(req_file, ec);

    if (rc != 0 && out.ok_count == 0 && out.fail_count == 0) {
        // No JSON line at all was produced: the script died before it could
        // report per-request results (bad interpreter, syntax error, ...).
        out.error = "metadata fetch failed (exit code " + std::to_string(rc) + ")";
    }
    return out;
}

} // namespace muisc
