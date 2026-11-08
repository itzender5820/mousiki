// book_state.h -- the audio book player's state, kept apart from app.h so that a change here only rebuilds the files that include this header
// and not the whole program. App holds it behind a pointer (App::book_).
#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <deque>
#include <memory>
#include <thread>
#include <future>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "book_types.h"
#include "book_cue.h"
#include "book_notes.h"
#include "book_split.h"
#include "local_source.h"

namespace muisc {

namespace fs = std::filesystem;

struct BookState {
    std::string cut_error;           // why the original could not be moved aside (the system's own words)
    std::string dir;                    // the folder of the open book ("" = none)
    std::chrono::steady_clock::time_point seek_last{}, seek_start{};
    int seek_dir = 0, seek_run = 0;
    std::string nav;                    // the folder the books pane shows the content of ("" = the AUDIO BOOK PATH itself)
    double anim_t = 0.0;                // seconds of the book animation (advances while a chapter plays)
    double idle_t = 0.0;                // seconds of the stack animation (advances while no book is loaded)
    std::string meta_path;              // the loaded chapter's tags below belong to this file
    BookTagVals meta_vals{};
    int ov_chapter = 0;                 // the chapter the tag editor shows
    BookTagVals ov_val{}, ov_orig{};
    int ov_row = 0;
    bool ov_editing = false;
    bool ov_confirm = false;            // ESC with unsaved changes: Y save / N discard
    std::string ov_status;
    BookTagJob job;
    bool ov_rename_whole = true;        // the question is about the whole book ([a]) or about this chapter and the numbers moving to the front ([s])
    bool ov_rename_ask = false;         // SHIFT-free [a] with a changed file name: asks first, the chapter numbers are added
    BookOv ov_kind = BookOv::Tags;
    BookBookmark bookmark;              // the newest bookmark of the open book (the one the resume choices are about)
    std::vector<BookMark> marks;        // all bookmarks of the open book (file order)
    BookMark resume_mark;               // the bookmark the resume choices are about
    std::vector<BookStamp> mrows;
    std::vector<BookStamp> undo;        // recent deletions, newest last (CTRL+SHIFT+u brings them back)
    BookMark automark;                  // the auto-mark of the open book
    std::chrono::steady_clock::time_point am_last{};
    std::string am_last_file;
    double am_last_sec = -100.0;
    bool am_hold = false;
    std::string am_hold_file;
    double am_hold_sec = 0.0;
    std::vector<BookStamp> pick_rows;
    int pick_sel = 0, pick_idx = -1;
    BookMark cont_mark;                 // the auto-mark that the continue question is about
    int cont_idx = -1;
    std::string resume_label = "bookmark";   // what the resume choices are about (header text)
    int msel = 0, mscroll = 0;
    int notes_sort = 0;                 // SHIFT+t in the notes overlay: 0 chapter order (furthest on top), 1 recently edited first
    int mfilter = 0;                    // SHIFT+t in the list: 0 all, 1 notes by the date they were set, 2 notes by the date of the last change
    int mmode = 0;                      // 0 the list, 1 "discard this bookmark?"
    std::string cm_file;                // the file that is marked (its name in dir)
    double cm_dur = 0.0;                // its length in seconds (0 = unknown)
    std::vector<ChapterMark> cm;        // its chapter marks, by time (kept in the .cue as REM CHAPTER lines)
    int cm_sel = 0;
    int cm_mode = 0;                    // 0 the list, 1 title field, 2 time field, 3 time field of a new mark, 4 delete the mark?, 5 cut?, 6 delete the backups?
    std::string cm_text;                // the field
    int cm_edit = -1;                   // index of the mark whose title / time is typed
    BookCutJob cut;
    std::vector<BookNote> notes;        // the open book's notes (<folder>_notes.txt)
    std::function<void()> then;         // what happens after the answer
    bool prompt_paused = false;         // the question paused a running chapter
    std::string ask_file;               // where the question is about
    double ask_sec = 0.0;
    int resume_idx = -1, resume_sel = 0;
    std::string ask_what;               // the line under the question: what the user is about to do
    std::string note_dir;               // the folder of the book the note editor writes to
    std::chrono::steady_clock::time_point seek_at{};
    BookMark del_mark;                  // the bookmark that the question is about
    bool seek_pending = false;          // a chapter is loading and has to go to seek_sec when it is ready
    std::string seek_file;
    double seek_sec = 0.0;
    double prev_elapsed = 0.0;          // for noticing that playback passed a note
    bool note_edit = false;
    bool note_typing = true;            // true: keys type; false: SHIFT+s saves, ENTER types again, ESC discards
    bool note_discard_ask = false;
    std::vector<std::string> note_lines;   // [0] the title, then the text lines
    std::string note_file;
    double note_sec = 0.0;
    int note_row = 0;                   // the line with the caret (edit_caret_ = the byte in it)
    int note_arow = 0;                  // the line with the other end of the marked text (edit_anchor_ = the byte in it)
    int note_vscroll = 0;       // the first visible row of the (soft-wrapped) text
    int note_wrap_w = 40;       // the width the text was wrapped to last (Up / Down move by those rows)
    int note_replace = -1;              // index in notes when an existing note is edited
    std::string note_status;
    bool note_shown = false;            // the field shows a note (not the visualization)
    BookNote note_view;
    bool note_auto = false;             // shown by playback passing it (goes away by itself)
    std::chrono::steady_clock::time_point note_auto_at{};
    int viz_before = -1;                // the view of the field (0 sphere, 1 oscilloscope, 2 spectrogram, 3 lyrics) before a note took its place
    int notes_sel = 0, notes_scroll = 0;
    int notes_mode = 0;                 // 0 the list, 1 export path, 2 import path, 3 delete question, 4 a note is written / changed in the right pane
    std::string notes_path;
};

} // namespace muisc
