// book_types.h -- the small types of the audio book player (tag job, time stamps, split job), shared by app.h and book_state.h.
#pragma once
#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <utility>
#include <vector>
#include "book_cue.h"
#include "book_split.h"

namespace muisc {

namespace fs = std::filesystem;

// --- audio book tags (SHIFT+m): an editor only, nothing is fetched. One chapter at a time; every row is written with ffmpeg -c copy ---
constexpr int kBookTagCount = 7;   // file name, book title, author, narrator, chapter, publisher + year, genre

using BookTagVals = std::array<std::string, kBookTagCount>;

struct BookTagJob {
    bool active = false;
    std::vector<std::pair<fs::path, std::array<std::pair<bool, std::string>, 7>>> todo;   // file, per row: (changed, new value)
    size_t next = 0;
    int ok = 0;
    std::string error;
    std::vector<std::pair<fs::path, fs::path>> renames;   // file name changes (old, new), done last in two phases
    fs::path cue_file;                // <book folder>/<folder>.cue
    std::vector<std::pair<std::string, std::string>> cue_set;   // header lines to write into it (narrator, publisher, year, ...)
    bool cue_written = false;
    int renamed = 0;
};

// --- bookmark and notes (app_audiobook_notes.cpp) ---
// The overlay of the book mode is one Mode (BookOverlay) with several faces.
enum class BookOv { Tags, Ask, Resume, Notes, Marks, Chapters, DelMark, Continue, Pick };

using BookBookmark = BookMark;            // file, second, when it was set (REM BOOKMARK lines in the .cue)

// the bookmark list (SHIFT+k: the bookmark of every book; app_audiobook_marks.cpp)
// A time stamp of a book. Three kinds: the auto-mark (kind 0; the program writes it as REM AUTOMARK while one listens), a regular
// bookmark (1; REM BOOKMARK, as many as one likes) and a note (2; a bookmark with a text, in <folder>_notes.txt).
struct BookStamp {
    int kind = 1;                         // 0 auto-mark, 1 bookmark, 2 note
    std::string dir, book, file, title;   // book folder, its name, the chapter's file name, the note's title ("" = none)
    double sec = 0.0;
    long long when = 0;                   // seconds since 1970 (0 = unknown; notes have none)
    int num = 0;                          // position among its kind in the book's file (1 based): "Bookmark 2", "Note 3"
    BookNote note;                        // kind 2: the whole note (for the undo; carries the dates it was set and last changed)
};

struct BookCutJob {
    bool active = false;
    fs::path src;
    std::string dir, file;
    std::vector<SplitPart> parts;
    size_t next = 0;
    std::vector<fs::path> made;
    std::string error;
    bool trim = false;                    // trim the blank audio at the edges of the parts to 2 s
    bool detecting = false;               // ffmpeg's silencedetect runs over the file (in `detect`)
    std::future<std::string> detect;
    bool waiting = false;                 // the original is being waited for (it is still in use)
    std::chrono::steady_clock::time_point wait_start{}, last_try{};
};

} // namespace muisc
