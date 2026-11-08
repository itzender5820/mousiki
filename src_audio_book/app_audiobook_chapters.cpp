// The audio book player, part 4 (Phase 3): chapter marks and cutting a file into chapters. ALT+L opens the marks of the chapter that
// plays: [m] sets a mark at the point one is listening at (the title is "Chapter N" until it is renamed), [a] adds one by its time
// (h:mm:ss), [t] changes the time, [r] the title, [x] deletes. The marks are kept in the book's .cue (REM CHAPTER lines). [c] cuts the
// file at the marks: ffmpeg copies the parts losslessly into the book folder, the original stays as <name>.<ending>.mousiki-backup
// until it is deleted on purpose ([SHIFT+d]), and the bookmark and the notes are moved to the new files. Member functions of App.
#include "app.h"
#include "book_state.h"
#include "string_util.h"
#include "text_edit.h"
#include "utf8_util.h"
#include "path_utf8.h"
#include "book_cue.h"
#include "book_notes.h"
#include "book_split.h"
#include "process_util.h"
#include "console_log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include "app_internal.h"

namespace muisc {

namespace {
constexpr const char* kBackupEnding = ".mousiki-backup";


std::string size_text(unsigned long long b) {
    char buf[32];
    if (b >= 1000000000ULL) std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(b) / 1e9);
    else if (b >= 1000000ULL) std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / 1e6);
    else std::snprintf(buf, sizeof buf, "%llu KB", b / 1000ULL);
    return buf;
}
} // namespace

// ---------------------------------------------------------------------
// Opening, saving
// ---------------------------------------------------------------------
void App::book_cm_open() {
    if (book_->dir.empty() || chapters_.empty()) {
        status_line_ = "open a book first (ENTER on a book): chapter marks are set in the chapter that plays";
        return;
    }
    if (book_->cut.active) { status_line_ = "a file is being cut ..."; return; }
    int idx = book_current_chapter();
    if (idx < 0) idx = std::clamp(queue_selected_, 0, static_cast<int>(chapters_.size()) - 1);
    const bool playing_it = idx == book_current_chapter();
    auto go = [this, idx]() {
        if (idx != book_current_chapter()) { book_->prompt_paused = false; book_play_chapter(idx); }   // (the marks are set while listening)
        const fs::path file = chapters_[static_cast<size_t>(idx)].path;
        book_->cm_file = path_utf8(file.filename());
        BookCue cue;
        cue.load(BookCue::path_for(path_from_utf8(book_->dir)));
        book_->cm = chapter_marks_of(cue, book_->cm_file);
        book_->cm_dur = 0.0;
        const ProcResult r = run_capture({"ffprobe", "-v", "error", "-show_entries", "format=duration",
                                          "-of", "default=nw=1:nk=1", path_utf8(file)}, false);
        if (r.ok()) book_->cm_dur = std::max(0.0, std::strtod(r.out.c_str(), nullptr));
        if (book_->cm_dur <= 0.0 && idx == book_current_chapter()) book_->cm_dur = static_cast<double>(total_sec_);
        book_->cm_sel = 0;
        book_->cm_mode = 0;
        book_->cm_edit = -1;
        book_->ov_status.clear();
        book_->ov_kind = BookOv::Chapters;
        text_disengage();
        mode_ = Mode::BookOverlay;
    };
    if (playing_it) go();
    else book_ask_then("Opening the chapter marks of " + stem_of(path_utf8(chapters_[static_cast<size_t>(idx)].path.filename())), go);
}

// The marks of the file into the .cue (the marks of other files and every other line stay).
void App::book_cm_save() {
    const fs::path cp = BookCue::path_for(path_from_utf8(book_->dir));
    BookCue cue;
    cue.load(cp);
    if (cue.empty() && book_->cm.empty()) return;
    chapter_marks_store(cue, book_->cm_file, book_->cm);
    if (!cue.save(cp)) book_->ov_status = "could not write " + path_utf8(cp.filename());
}

// A mark at `sec`. The start (0:00:00, "Chapter 1") is added with the first mark; the standard titles follow the order.
void App::book_cm_insert(double sec, const std::string& title, bool ask_title) {
    sec = std::max(0.0, std::round(sec * 10.0) / 10.0);
    if (book_->cm_dur > 0 && sec >= book_->cm_dur - 1.0) { book_->ov_status = "that is the end of the file: a chapter needs at least a second"; return; }
    for (const ChapterMark& m : book_->cm)
        if (std::fabs(m.sec - sec) < 1.0) { book_->ov_status = "there is a chapter mark at " + clock_hms(m.sec) + " already"; return; }
    if (book_->cm.empty() && sec >= 1.0) book_->cm.push_back({book_->cm_file, 0.0, default_title(1)});
    book_->cm.push_back({book_->cm_file, sec, title});
    order_marks(book_->cm);
    int at = 0;
    for (size_t i = 0; i < book_->cm.size(); ++i) if (std::fabs(book_->cm[i].sec - sec) < 0.05) at = static_cast<int>(i);
    book_->cm_sel = at;
    book_cm_save();
    book_->ov_status = "chapter mark set at " + clock_hms(sec);
    if (ask_title) {   // the title field, filled with the standard title: ENTER keeps it
        book_->cm_mode = 1;
        book_->cm_edit = at;
        book_->cm_text = book_->cm[static_cast<size_t>(at)].title;
        edit_owner_ = "bookcm";
        edit_caret_ = book_->cm_text.size();
        edit_anchor_ = 0;   // selected: typing replaces the standard title, ENTER keeps it
    }
}

std::vector<fs::path> App::book_backup_files() const {
    std::vector<fs::path> out;
    std::error_code ec;
    if (book_->dir.empty()) return out;
    for (const auto& e : fs::directory_iterator(path_from_utf8(book_->dir), ec)) {
        const std::string n = path_utf8(e.path().filename());
        const std::string end = kBackupEnding;
        if (n.size() > end.size() && n.compare(n.size() - end.size(), end.size(), end) == 0) out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------
bool App::book_cm_key(int key) {
    const bool arrow = last_key_was_arrow();
    auto say = [&](const std::string& s) { book_->ov_status = s; };
    if (book_->cut.active) return true;   // the cutting runs: keys wait
    const int n = static_cast<int>(book_->cm.size());

    if (book_->cm_mode >= 1 && book_->cm_mode <= 3) {   // a field
        if (!arrow && key == 27) {   // a new mark keeps its standard title; everything else is left as it was
            book_->cm_mode = 0;
            text_disengage();
            return true;
        }
        if (!arrow && (key == '\r' || key == '\n')) {
            const int mode = book_->cm_mode;
            book_->cm_mode = 0;
            text_disengage();
            if (mode == 1 && book_->cm_edit >= 0 && book_->cm_edit < n) {
                const std::string t = trim_ws(book_->cm_text);
                const double at = book_->cm[static_cast<size_t>(book_->cm_edit)].sec;
                book_->cm[static_cast<size_t>(book_->cm_edit)].title = t;   // (empty: the standard title comes back)
                order_marks(book_->cm);
                for (size_t i = 0; i < book_->cm.size(); ++i) if (std::fabs(book_->cm[i].sec - at) < 0.05) book_->cm_sel = static_cast<int>(i);
                book_cm_save();
                say("title set");
            } else if (mode == 2 && book_->cm_edit >= 0 && book_->cm_edit < n) {
                double s = 0;
                if (!parse_clock(book_->cm_text, s)) { say("not a time: write h:mm:ss (or m:ss)"); return true; }
                s = std::round(s * 10.0) / 10.0;
                if (s < 1.0 || (book_->cm_dur > 0 && s >= book_->cm_dur - 1.0)) { say("a chapter starts between 0:00:01 and the end of the file"); return true; }
                for (int i = 0; i < n; ++i)
                    if (i != book_->cm_edit && std::fabs(book_->cm[static_cast<size_t>(i)].sec - s) < 1.0) { say("there is a chapter mark at " + clock_hms(book_->cm[static_cast<size_t>(i)].sec) + " already"); return true; }
                book_->cm[static_cast<size_t>(book_->cm_edit)].sec = s;
                order_marks(book_->cm);
                for (size_t i = 0; i < book_->cm.size(); ++i) if (std::fabs(book_->cm[i].sec - s) < 0.05) book_->cm_sel = static_cast<int>(i);
                book_cm_save();
                say("time set to " + clock_hms(s));
            } else if (mode == 3) {
                double s = 0;
                if (!parse_clock(book_->cm_text, s)) { say("not a time: write h:mm:ss (or m:ss)"); return true; }
                book_cm_insert(s, std::string(), true);
            }
            return true;
        }
        edit_focus("bookcm", book_->cm_text);
        edit_text_key(book_->cm_text, edit_caret_, edit_anchor_, key, 120, &book_->ov_status);
        return true;
    }
    if (book_->cm_mode >= 4) {   // questions
        const int mode = book_->cm_mode;
        book_->cm_mode = 0;
        const bool trim_cut = mode == 5 && !arrow && (key == 't' || key == 'T');   // [t]: cut and trim the blank audio at the edges to 2 s
        if (arrow || (key != 'y' && key != 'Y' && !trim_cut)) { say("cancelled"); return true; }
        if (mode == 4 && book_->cm_sel >= 0 && book_->cm_sel < n) {
            book_->cm.erase(book_->cm.begin() + book_->cm_sel);
            if (book_->cm.size() == 1) book_->cm.clear();   // a lone start is no division
            order_marks(book_->cm);
            book_->cm_sel = std::clamp(book_->cm_sel, 0, std::max(0, static_cast<int>(book_->cm.size()) - 1));
            book_cm_save();
            say("chapter mark deleted");
        } else if (mode == 5) {
            book_cut_start(trim_cut);
        } else if (mode == 6) {
            unsigned long long bytes = 0;
            int count = 0;
            for (const fs::path& f : book_backup_files()) {
                std::error_code ec;
                const auto sz = fs::file_size(f, ec);
                if (fs::remove(f, ec)) { ++count; if (!ec) bytes += sz; }
            }
            say(count > 0 ? "backup deleted (" + std::to_string(count) + (count == 1 ? " file, " : " files, ") + size_text(bytes) + " free again)" : "no backup to delete");
        }
        return true;
    }

    // the list. The playback keys work as everywhere: Left / Right seek (held down: faster and faster), SPACE pauses.
    if (arrow && (key == 'A' || key == 'B') && last_key_was_shifted()) {   // SHIFT+arrow / PgUp / PgDn: a page (no wrapping)
        if (n) book_->cm_sel = std::clamp(book_->cm_sel + (key == 'A' ? -10 : 10), 0, n - 1);
        return true;
    }
    if (arrow && key == 'A') { if (n) book_->cm_sel = (book_->cm_sel + n - 1) % n; return true; }
    if (arrow && key == 'B') { if (n) book_->cm_sel = (book_->cm_sel + 1) % n; return true; }
    if (arrow && key == 'C') { book_seek(1); return true; }
    if (arrow && key == 'D') { book_seek(-1); return true; }
    if (arrow) return true;
    {
        const int lk = (key == '\r') ? '\n' : key;
        std::string act = resolve_hotkey_action(lk);
        if (act.empty() && lk >= 'a' && lk <= 'z') act = resolve_hotkey_action(lk - 32);
        if (act == "HKeySeekForward") { book_seek(1); return true; }
        if (act == "HKeySeekBackward") { book_seek(-1); return true; }
        if (act == "HKeyTogglePlayPause") { act_play_pause(); return true; }
        if (act == "HKeyIncreaseVolume") { act_volume_change(+5); return true; }
        if (act == "HKeyDecreaseVolume") { act_volume_change(-5); return true; }
        if (act == "HKeyLyricsTiming") { mode_ = Mode::Browse; return true; }   // the key that opened it closes it
    }
    const bool this_file_plays = has_track_ && current_is_local_ && path_utf8(current_path_.filename()) == book_->cm_file && path_utf8(current_path_.parent_path()) == book_->dir;
    if (key == 27 || key == 'q') { mode_ = Mode::Browse; return true; }
    if (key == 'm' || key == 'M') {   // a mark where one is listening
        if (!this_file_plays) { say("play this file first: a mark is set at the point you are listening to"); return true; }
        book_cm_insert(player_.poll_elapsed(), std::string(), false);
        return true;
    }
    if (key == 'a') {   // a mark at a typed time
        book_->cm_mode = 3;
        book_->cm_edit = -1;
        book_->cm_text = this_file_plays ? clock_hms(player_.poll_elapsed()) : std::string("0:00:00");
        edit_owner_ = "bookcm";
        edit_caret_ = book_->cm_text.size();
        edit_anchor_ = 0;   // selected: typing replaces it
        return true;
    }
    if (key == 'D') {   // SHIFT+d: delete the backup
        const auto files = book_backup_files();
        if (files.empty()) { say("this book has no backup"); return true; }
        book_->cm_mode = 6;
        return true;
    }
    if (key == 'c') {
        std::string err;
        const std::string ext = path_utf8(path_from_utf8(book_->cm_file).extension());
        if (plan_split(book_->cm, book_->cm_dur, ext, &err).empty()) { say(err.empty() ? "nothing to cut" : err); return true; }
        book_->cm_mode = 5;
        return true;
    }
    if (n == 0 || book_->cm_sel < 0 || book_->cm_sel >= n) { say("[m] sets the first chapter mark where you are listening, [a] by its time"); return true; }
    const ChapterMark& cur = book_->cm[static_cast<size_t>(book_->cm_sel)];
    if (key == 'r') {
        book_->cm_mode = 1;
        book_->cm_edit = book_->cm_sel;
        book_->cm_text = cur.title;
        edit_owner_ = "bookcm";
        edit_caret_ = book_->cm_text.size();
        edit_anchor_ = 0;
        return true;
    }
    if (key == 't') {
        if (book_->cm_sel == 0) { say("the first chapter always starts at 0:00:00"); return true; }
        book_->cm_mode = 2;
        book_->cm_edit = book_->cm_sel;
        book_->cm_text = clock_hms(cur.sec);
        edit_owner_ = "bookcm";
        edit_caret_ = book_->cm_text.size();
        edit_anchor_ = 0;   // selected: typing replaces it
        return true;
    }
    if (key == 'x' || key == kKeyDelete || key == 127) {
        if (book_->cm_sel == 0 && n > 1) { say("the first chapter always starts at 0:00:00 (delete the others, or this one goes with the last)"); return true; }
        book_->cm_mode = 4;
        return true;
    }
    if (key == '\r' || key == '\n') {   // plays from the mark
        if (!this_file_plays) { say("this file is not playing"); return true; }
        player_.seek_relative(cur.sec - player_.poll_elapsed());
        if (player_.is_paused()) player_.resume();
        say("playing from " + clock_hms(cur.sec));
        return true;
    }
    return true;
}

// ---------------------------------------------------------------------
// Cutting
// ---------------------------------------------------------------------
void App::book_cut_start(bool trim) {
    const fs::path dir = path_from_utf8(book_->dir);
    const std::string ext = path_utf8(path_from_utf8(book_->cm_file).extension());
    std::string err;
    BookCutJob job;
    job.parts = plan_split(book_->cm, book_->cm_dur, ext, &err);
    if (job.parts.empty()) { book_->ov_status = err.empty() ? "nothing to cut" : err; return; }
    job.src = dir / path_from_utf8(book_->cm_file);
    job.dir = book_->dir;
    job.file = book_->cm_file;
    std::error_code ec;
    for (const SplitPart& p : job.parts)
        if (fs::exists(dir / path_from_utf8(p.file), ec)) { book_->ov_status = "the file " + p.file + " exists already: rename or move it first"; return; }
    if (load_in_progress_.load()) { book_->ov_status = "the chapter is still loading: try again in a moment"; return; }
    bool plays_it = has_track_ && current_is_local_ && path_utf8(current_path_) == path_utf8(job.src);
    if (has_track_ && current_is_local_ && !plays_it) { std::error_code eq; plays_it = fs::equivalent(current_path_, job.src, eq) && !eq; }
    if (!plays_it && has_track_ && current_is_local_) plays_it = current_path_.filename() == job.src.filename() && current_path_.parent_path().filename() == job.src.parent_path().filename();   // (another spelling of the same folder)
    if (plays_it) {   // the player must let go of the file
        device_stop();   // on the device worker (see App::device_run())
        // The long files are decoded by ffmpeg processes that read the file for as long as anybody holds the track (the player, and
        // the app's own reference): letting go of it ends them, and the file can be moved.
        if (current_pcm_) current_pcm_->cancelled.store(true);   // (the decode threads end and close the file; another holder of the track would keep them alive)
        current_pcm_.reset();
        has_track_ = false;
        history_end_current_play();
    }
    book_->seek_pending = false;
    job.active = true;
    if (trim) {   // ffmpeg looks for blank stretches first (in the background: a long file takes a while), the parts are trimmed with the result
        job.trim = job.detecting = true;
        const Args cmd = {"ffmpeg", "-nostats", "-v", "info", "-i", path_utf8(job.src), "-vn",
                          "-af", "silencedetect=noise=-50dB:d=2.5", "-f", "null", "-"};
        job.detect = std::async(std::launch::async, [cmd]() { return run_capture(cmd, true).out; });
    }
    book_->cut = std::move(job);
    book_->ov_status = trim ? "looking for blank audio ..." : "cutting 0/" + std::to_string(book_->cut.parts.size()) + " ...";
}

void App::book_cut_step() {
    BookCutJob& job = book_->cut;
    if (job.detecting) {   // waiting for the blank stretches; then the parts shrink to 2 s of blank at their edges
        if (job.detect.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const std::string log = job.detect.get();
        job.detecting = false;
        const int n = trim_silence(job.parts, parse_silences(log, book_->cm_dur), book_->cm_dur, 2.0);
        ConsoleLog::instance().log_basic("book cut: " + std::to_string(n) + " part" + (n == 1 ? "" : "s") + " trimmed to 2 s of blank audio");
        book_->ov_status = "cutting 0/" + std::to_string(job.parts.size()) + " ...  (" + std::to_string(n) + (n == 1 ? " part" : " parts") + " trimmed)";
        return;
    }
    if (job.next >= job.parts.size()) { book_cut_finish(); return; }
    const SplitPart& p = job.parts[job.next];
    const fs::path out = path_from_utf8(job.dir) / path_from_utf8(p.file);
    char t[40];
    std::snprintf(t, sizeof t, "%.3f", p.start);
    Args cmd = {"ffmpeg", "-y", "-v", "error", "-ss", std::string(t), "-i", path_utf8(job.src)};
    if (p.end >= 0) { std::snprintf(t, sizeof t, "%.3f", p.end - p.start); cmd.push_back("-t"); cmd.push_back(t); }
    cmd.insert(cmd.end(), {"-map", "0", "-map_chapters", "-1", "-map_metadata", "0", "-c", "copy"});
    cmd.insert(cmd.end(), {"-metadata", "title=" + p.title});
    cmd.insert(cmd.end(), {"-metadata", "track=" + std::to_string(job.next + 1) + "/" + std::to_string(job.parts.size())});
    cmd.push_back(path_utf8(out));
    const ProcResult r = run_capture(cmd, true);
    std::error_code ec;
    job.made.push_back(out);
    if (!r.ok() || !fs::exists(out, ec) || fs::file_size(out, ec) == 0) {
        job.error = "ffmpeg could not write " + p.file;
        for (const fs::path& f : job.made) fs::remove(f, ec);   // nothing is left half done; the original was not touched
        job.active = false;
        book_->ov_status = "cutting failed: " + job.error + " -- the original is unchanged";
        status_line_ = book_->ov_status;
        return;
    }
    ++job.next;
    book_->ov_status = "cutting " + std::to_string(job.next) + "/" + std::to_string(job.parts.size()) + " ...";
    if (job.next >= job.parts.size()) book_cut_finish();
}

// The parts are all there: the original becomes the backup, the .cue and the notes follow the new files.
void App::book_cut_finish() {
    BookCutJob& job = book_->cut;
    std::error_code ec;
    fs::path backup = job.src;
    backup += kBackupEnding;
    for (int i = 1; fs::exists(backup, ec) && i < 100; ++i) { backup = job.src; backup += std::string(kBackupEnding) + "." + std::to_string(i); }
    // A program that still holds the file (Mousiki's own reading of its tags and length, the player's decoder letting go, a virus
    // scanner, a sync client) is waited for: one try every 0.4 s for up to 90 s, while the screen stays alive.
    {
        const auto now = std::chrono::steady_clock::now();
        if (!job.waiting) { job.waiting = true; job.wait_start = now; job.last_try = now - std::chrono::seconds(1); }
        if (now - job.last_try < std::chrono::milliseconds(400)) return;
        job.last_try = now;
        ec.clear();
        fs::rename(job.src, backup, ec);
        if (ec && now - job.wait_start < std::chrono::seconds(90)) {
            const long secs = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(now - job.wait_start).count());
            book_->ov_status = "the original is still in use (" + ec.message() + ") -- waiting for it to be let go, " + std::to_string(secs) + " s ...";
            return;
        }
    }
    if (ec) {   // rename refused: copy the file aside and remove the original (a copy only needs the file to be readable)
        const std::error_code why = ec;
        std::error_code ec2;
        fs::copy_file(job.src, backup, fs::copy_options::overwrite_existing, ec2);
        if (!ec2) {
            fs::remove(job.src, ec2);
            if (ec2) { std::error_code ec3; fs::remove(backup, ec3); }
        }
        ec = ec2 ? why : std::error_code();
        if (ec) book_->cut_error = why.message();
    }
    if (ec) {
        const std::string reason = book_->cut_error;
        for (const fs::path& f : job.made) fs::remove(f, ec);
        job.active = false;
        book_->ov_status = "could not move the original aside, even after waiting 90 s: " + reason + " (" + path_utf8(job.src.filename()) +
                          ") -- nothing changed. Close whatever else has the file open (a preview, a virus scanner, a sync client) and cut again.";
        status_line_ = book_->ov_status;
        return;
    }
    // the .cue: the marks are used up; the bookmark moves to the part that holds its place
    const fs::path cp = BookCue::path_for(path_from_utf8(job.dir));
    BookCue cue;
    cue.load(cp);
    chapter_marks_store(cue, job.file, {});
    std::vector<std::string> marks_out;
    int moved_marks = 0;
    for (const std::string& raw : cue.get_all_raw("REM BOOKMARK")) {
        BookMark m;
        if (parse_bookmark(raw, m) && m.file == job.file) {
            std::string f;
            double s = 0;
            if (remap_position(job.parts, m.sec, f, s)) { m.file = f; m.sec = s; ++moved_marks; marks_out.push_back(format_bookmark(m)); continue; }
        }
        marks_out.push_back(raw);
    }
    cue.set_all_raw("REM BOOKMARK", marks_out);
    {   // the auto-mark follows the cut as well
        BookMark am;
        if (parse_bookmark(cue.get_raw("REM AUTOMARK"), am) && am.file == job.file) {
            std::string f;
            double sc = 0;
            if (remap_position(job.parts, am.sec, f, sc)) { am.file = f; am.sec = sc; cue.set_raw("REM AUTOMARK", format_bookmark(am)); }
        }
    }
    if (!cue.empty() && !cue.save(cp)) job.error = "could not write " + path_utf8(cp.filename());
    // the notes likewise
    const fs::path nf = book_notes::path_for(path_from_utf8(job.dir));
    std::vector<BookNote> notes;
    int moved_notes = 0;
    if (book_notes::load(nf, notes)) {
        for (BookNote& nt : notes) {
            if (nt.file != job.file) continue;
            std::string f;
            double s = 0;
            if (remap_position(job.parts, nt.sec, f, s)) { nt.file = f; nt.sec = s; ++moved_notes; }
        }
        if (moved_notes > 0 && !book_notes::save(nf, notes) && job.error.empty()) job.error = "could not write " + path_utf8(nf.filename());
    }
    const size_t count = job.parts.size();
    const std::string backup_name = path_utf8(backup.filename());
    job.active = false;
    book_->cm.clear();
    book_bookmark_load();
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        row_meta_cache_.clear();
    }
    search_idx_.clear();
    book_->meta_path.clear();
    rescan_library();   // the new files show up as the chapters
    std::string msg = "cut into " + std::to_string(count) + " chapters; the original is kept as " + backup_name + " (ALT+L, SHIFT+d deletes it)";
    if (moved_marks > 0) msg += "; the bookmark moved";
    if (moved_notes > 0) msg += "; " + std::to_string(moved_notes) + (moved_notes == 1 ? " note" : " notes") + " moved";
    if (!job.error.empty()) msg += "; " + job.error;
    status_line_ = msg;
    book_->ov_status = msg;
    mode_ = Mode::Browse;
}

// ---------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------
std::vector<std::string> App::build_book_cm_panel(int panel_w, int panel_h) const {
    const int W = panel_w, inner = W - 4;
    const std::string border = ansi_for(settings_.border_color, false);
    const std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m";
    const Framer framed{border + settings_.box_vertical + R, inner};
    const std::string head = header_sgr(settings_), leg = legend_sgr(settings_);
    const std::string plain_ink = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
    std::vector<std::string> lines;
    lines.push_back(box_top("Chapter marks - " + truncate_str(book_->cm_file, std::max(8, W - 28)), W, border));

    const bool this_file = has_track_ && current_is_local_ && path_utf8(current_path_.filename()) == book_->cm_file && path_utf8(current_path_.parent_path()) == book_->dir;
    const double pos = this_file ? player_.poll_elapsed() : -1.0;
    const int n = static_cast<int>(book_->cm.size());
    {   // where one is
        std::string info = this_file ? std::string(player_.is_paused() ? "paused at " : "playing at ") + clock_hms(pos) : std::string("not playing");
        info += " of " + (book_->cm_dur > 0 ? clock_hms(book_->cm_dur) : std::string("?"));
        info += n >= 2 ? "   -   " + std::to_string(n) + " chapters when cut" : "   -   no chapter marks yet";
        const std::string t = truncate_str(info, inner);
        lines.push_back(framed(head + t, display_width(t)));
    }
    const int time_w = 9, len_w = 9;
    const int title_w = std::max(10, inner - 2 - 4 - time_w - len_w - 2);
    auto cols = [&](const std::string& mk, const std::string& num, const std::string& tm, const std::string& title, const std::string& len) {
        std::string s = mk + " " + pad_right(num, 3) + pad_right(tm, time_w) + pad_right(truncate_str(apply_font_map(title, settings_.font_map), title_w), title_w) + "  " + pad_right(len, len_w);
        return pad_right(truncate_str(s, inner), inner);
    };
    lines.push_back(framed(head + cols(" ", "#", "TIME", "CHAPTER TITLE", "LENGTH"), inner));
    auto key_name = [&](const char* action, const char* arrow, const char* fallback) {   // arrow keys read as arrows
        const std::string k = hotkey_text(action, fallback);
        return k.find("ARROW") != std::string::npos ? std::string(arrow) : k;
    };
    const std::vector<LegendGroup> groups = {
        {"Marks", {"[m] Mark here", "[a] Add by time", "[t] Time", "[r] Title", "[x] Delete"}},
        {"Listen", {"[" + key_name("HKeySeekBackward", "\u2190", "LEFT") + key_name("HKeySeekForward", "\u2192", "RIGHT") + "] Seek (held: faster and faster)\001Seek",
                    "[" + key_name("HKeyTogglePlayPause", "SPACE", "SPACE") + "] Pause", "[\u2191\u2193] Select", "[ENTER] Play from the mark\001Play"}},
        {"Cut", {"[c] Cut the file at the marks\001Cut", "[SHIFT+d] Delete the backup\001Backup", "[ESC] Close"}}};
    const std::vector<LegendRow> lrows = book_overlay_legend(W, groups, 3);
    // the question or the status below the list: as many rows as it needs (three at most), never cut off
    std::string st_text;
    if (book_->cm_mode == 4) st_text = "Delete this chapter mark?   [y] yes   [n] no";
    else if (book_->cm_mode == 5) {
        const std::string ext = path_utf8(path_from_utf8(book_->cm_file).extension());
        st_text = "Cut into " + std::to_string(plan_split(book_->cm, book_->cm_dur, ext).size()) + " chapters? Playback stops; the original is kept as backup; bookmark and notes move on.   [y] yes   [t] yes, and trim blank audio at the edges to 2 s   [n] no";
    } else if (book_->cm_mode >= 6) {
        unsigned long long bytes = 0;
        const auto files = book_backup_files();
        for (const fs::path& f : files) { std::error_code ec; bytes += fs::file_size(f, ec); }
        st_text = "Delete the backup (" + std::to_string(files.size()) + (files.size() == 1 ? " file, " : " files, ") + size_text(bytes) + ") for good? The uncut original is gone then.   [y] yes   [n] no";
    } else if (book_->cm_mode == 0) {
        st_text = book_->ov_status;
        if (st_text.empty()) {
            const auto files = book_backup_files();
            if (!files.empty()) st_text = "this book has a backup of an uncut original (" + std::to_string(files.size()) + (files.size() == 1 ? " file" : " files") + "): SHIFT+d deletes it";
        }
    }
    std::vector<std::string> st_lines;
    if (!st_text.empty()) {
        st_lines = wrap_words(st_text, std::max(8, inner));
        if (st_lines.size() > 3) { st_lines.resize(3); st_lines.back() = truncate_str(st_lines.back(), std::max(8, inner)); }
    }
    const int st_rows = book_->cm_mode >= 1 && book_->cm_mode <= 3 ? 1 : std::max<int>(1, static_cast<int>(st_lines.size()));
    const int body = std::max(3, panel_h - 4 - st_rows - static_cast<int>(lrows.size()));   // top, info, header, status rows, bottom border, the legend
    int playing_row = -1;
    for (int i = 0; i < n; ++i) if (pos >= 0 && pos >= book_->cm[static_cast<size_t>(i)].sec - 0.05) playing_row = i;
    int first = 0;
    if (book_->cm_sel >= first + body) first = book_->cm_sel - body + 1;
    for (int i = 0; i < body; ++i) {
        const int idx = first + i;
        if (idx >= n) { lines.push_back(framed("", 0)); continue; }
        const ChapterMark& m = book_->cm[static_cast<size_t>(idx)];
        const double end = idx + 1 < n ? book_->cm[static_cast<size_t>(idx) + 1].sec : book_->cm_dur;
        const std::string len = end > 0 ? clock_hms(std::max(0.0, end - m.sec)) : std::string("-");
        const std::string plain = cols(idx == playing_row ? "▶" : " ", std::to_string(idx + 1), clock_hms(m.sec), m.title, len);
        const bool sel = idx == book_->cm_sel;
        lines.push_back(framed((sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color) : (idx == playing_row ? head + bg_ansi_for(settings_.list_inactive_bg_color) : plain_ink)) + plain, inner));
    }
    if (n == 0) {
        const std::string t = "No chapter marks. Listen, and press [m] where a chapter begins -- or [a] to type a time (h:mm:ss).";
        lines[3] = framed(leg + truncate_str(t, inner), display_width(truncate_str(t, inner)));
    }
    {   // the status / field / question row
        std::string body_s;
        int used = 0;
        const std::string hb = bg_ansi_for(settings_.header_color);
        const std::string ask_sgr = hb.empty() ? std::string("\x1b[7m") : hb + "\x1b[30m";
        if (book_->cm_mode >= 1 && book_->cm_mode <= 3) {
            const std::string label = book_->cm_mode == 1 ? "Title: " : book_->cm_mode == 2 ? "Time (h:mm:ss): " : "New mark at (h:mm:ss): ";
            size_t c = book_->cm_text.size();
            size_t a = c;
            if (edit_owner_ == "bookcm") { c = edit_caret_; a = edit_anchor_; }
            const EditPaint p = paint_edit_field(book_->cm_text, c, a, std::max(4, inner - display_width(label)), change_sgr(settings_), true);
            body_s = head + label + change_sgr(settings_) + p.s;
            used = display_width(label) + p.cols;
        } else if (book_->cm_mode >= 4) {
            for (size_t i = 0; i < st_lines.size(); ++i) {
                const std::string row = ask_sgr + pad_right(st_lines[i], inner);
                if (i + 1 < st_lines.size()) lines.push_back(framed(row, inner)); else { body_s = row; used = inner; }
            }
        } else {
            for (size_t i = 0; i < st_lines.size(); ++i) {
                if (i + 1 < st_lines.size()) lines.push_back(framed(leg + st_lines[i], display_width(st_lines[i])));
                else { body_s = leg + st_lines[i]; used = display_width(st_lines[i]); }
            }
        }
        lines.push_back(framed(body_s, used));
    }
    lines.push_back(box_bottom(W, "", border_bottom));
    for (size_t i = 0; i < lrows.size(); ++i) lines.push_back(paint_legend(settings_, lrows, i, border, W, false));
    return lines;
}

} // namespace muisc
