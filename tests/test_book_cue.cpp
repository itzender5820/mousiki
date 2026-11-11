// The .cue file of an audio book: header lines (narrator, publisher, year), BOM and line endings, unknown lines kept.
#include "test_framework.h"
#include "book_cue.h"
#include "path_utf8.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace muisc;
namespace fs = std::filesystem;

namespace {
fs::path temp_dir(const char* name) {
    const fs::path d = fs::temp_directory_path() / (std::string("mousiki_cue_test_") + name);
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}
std::string slurp(const fs::path& f) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST(book_cue_path_is_folder_name) {
    const fs::path p = BookCue::path_for(fs::path("/x/Books/Dune"));
    CHECK_EQ(path_utf8(p.filename()), std::string("Dune.cue"));
    CHECK_EQ(path_utf8(p.parent_path().filename()), std::string("Dune"));
}

TEST(book_cue_set_get_and_remove) {
    const fs::path d = temp_dir("setget");
    const fs::path f = BookCue::path_for(d);
    BookCue c;
    CHECK(!c.load(f));   // no file yet
    c.set("REM NARRATOR", "Ann \"Reader\"");   // a quote cannot be escaped in CUE: it becomes '
    c.set("REM PUBLISHER", "Penguin");
    c.set("REM DATE", "2019");
    CHECK(c.save(f));
    BookCue r;
    CHECK(r.load(f));
    CHECK_EQ(r.get("REM NARRATOR"), std::string("Ann 'Reader'"));
    CHECK_EQ(r.get("REM PUBLISHER"), std::string("Penguin"));
    CHECK_EQ(r.get("REM DATE"), std::string("2019"));
    CHECK(slurp(f).find("REM DATE 2019\n") != std::string::npos);   // a year is written bare
    r.set("REM PUBLISHER", "");   // empty = the line goes
    CHECK_EQ(r.get("REM PUBLISHER"), std::string());
    CHECK(r.save(f));
    CHECK(slurp(f).find("PUBLISHER") == std::string::npos);
    CHECK(!fs::exists(fs::path(f).concat(".tmp")));   // saved through a temp file that was renamed over the cue
    std::error_code ec;
    fs::remove_all(d, ec);
}

TEST(book_cue_keeps_unknown_lines_bom_and_crlf) {
    const fs::path d = temp_dir("keep");
    const fs::path f = BookCue::path_for(d);
    {
        std::ofstream o(f, std::ios::binary);
        o << "\xEF\xBB\xBF" "REM GENRE Fiction\r\nPERFORMER \"Jane\"\r\nFILE \"a.mp3\" MP3\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\nREM NARRATOR \"in a track, not the header\"\r\n";
    }
    BookCue c;
    CHECK(c.load(f));
    CHECK_EQ(c.get("PERFORMER"), std::string("Jane"));
    CHECK_EQ(c.get("REM NARRATOR"), std::string());   // only the header (before the first FILE line) counts
    c.set("REM NARRATOR", "Ann");   // goes into the header, before the FILE line
    CHECK(c.save(f));
    const std::string s = slurp(f);
    CHECK(s.compare(0, 3, "\xEF\xBB\xBF") == 0);                // the BOM stays
    CHECK(s.find("REM NARRATOR \"Ann\"\r\nFILE") != std::string::npos);   // CRLF stays, header placement
    CHECK(s.find("INDEX 01 00:00:00") != std::string::npos);    // the rest is untouched
    CHECK(s.find("REM GENRE Fiction") != std::string::npos);
    std::error_code ec;
    fs::remove_all(d, ec);
}
