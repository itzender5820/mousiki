// The chapters a single audio file carries itself (an .m4b audio book, an .mp3 with ID3 CHAP frames, a Matroska audio file ...), read with
// ffprobe -show_chapters. The audio book player lists them in the chapter pane like separate files; the pure part (parsing and cleaning
// up the list) is here and unit tested.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace muisc {

struct EmbeddedChapter {
    double start = 0.0;
    double end = 0.0;      // seconds; the last one ends where the file ends
    std::string title;
};

// ffprobe's `-show_chapters -of json` answer -> the chapters, in time order. Untitled ones are called "Chapter N" (N = their number in the
// list); a chapter that is shorter than a second (or has no length) is dropped and its neighbour takes over its time, so the list covers
// the file without gaps. Fewer than two chapters: empty (a single chapter is just the file).
std::vector<EmbeddedChapter> parse_embedded_chapters(const std::string& ffprobe_json, double file_duration = 0.0);

// Runs ffprobe on the file (a few milliseconds for the header of an .m4b). Empty when ffprobe is missing, fails or the file has no
// chapters. Results are remembered per file (path, size and time of last change), so opening the same book again costs nothing.
std::vector<EmbeddedChapter> read_embedded_chapters(const std::filesystem::path& file);

// Which files are worth asking: an .m4b / .mka always (that is what they are for); any other file only when the book is a single file or
// two (a folder of 40 .mp3 chapters is not probed one by one every time it opens).
bool embedded_chapters_worth_asking(const std::filesystem::path& file, size_t files_in_book);

} // namespace muisc
