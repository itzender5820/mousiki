// The hand-made pairs of track and video: storage, following renames / moves, mending, and the yt-dlp command of a download.
#include "test_framework.h"
#include "video_links.h"
#include "online_source.h"
#include "app_internal.h"
#include "settings.h"

#include <filesystem>
#include <fstream>

using namespace muisc;
namespace fs = std::filesystem;

namespace {
struct LinkFile {
    fs::path dir, file;
    LinkFile() {
        dir = fs::temp_directory_path() / "mousiki_links_test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        file = dir / "video_links.txt";
        VideoLinks::get().use_file(file);
    }
    ~LinkFile() { std::error_code ec; fs::remove_all(dir, ec); }
};
}  // namespace

TEST(video_links_set_get_remove_and_persist) {
    LinkFile lf;
    VideoLinks& v = VideoLinks::get();
    CHECK_EQ(v.size(), static_cast<size_t>(0));
    v.set("/m/a.mp3", "/v/a.mp4");
    v.set("/m/b.mp3", "/v/a.mp4");
    CHECK_EQ(v.video_for("/m/a.mp3"), std::string("/v/a.mp4"));
    CHECK_EQ(v.tracks_for("/v/a.mp4").size(), static_cast<size_t>(2));
    v.reload();                                           // read back from the file
    CHECK_EQ(v.video_for("/m/b.mp3"), std::string("/v/a.mp4"));
    CHECK(v.remove("/m/a.mp3"));
    CHECK(!v.has("/m/a.mp3"));
    v.set("/m/b.mp3", "");                                // an empty video removes the pair
    CHECK_EQ(v.size(), static_cast<size_t>(0));
    CHECK(!fs::exists(lf.file));                           // nothing left: no file
}

TEST(video_links_follow_renamed_files_and_folders) {
    LinkFile lf;
    VideoLinks& v = VideoLinks::get();
    v.set("/m/old.mp3", "/v/clip.mp4");
    v.set("/m/x.mp3", "/v/old.mp4");
    CHECK_EQ(v.rename_paths({{"/m/old.mp3", "/m/new.mp3"}, {"/v/old.mp4", "/v/new.mp4"}}), 2);   // both sides of the pairs
    CHECK_EQ(v.video_for("/m/new.mp3"), std::string("/v/clip.mp4"));
    CHECK_EQ(v.video_for("/m/x.mp3"), std::string("/v/new.mp4"));
    CHECK(!v.has("/m/old.mp3"));
    CHECK_EQ(v.rename_folder("/v", "/videos2"), 2);
    CHECK_EQ(v.video_for("/m/new.mp3"), std::string("/videos2/clip.mp4"));
    CHECK_EQ(v.rename_folder("/zzz", "/q"), 0);
    v.reload();
    CHECK_EQ(v.video_for("/m/x.mp3"), std::string("/videos2/new.mp4"));   // saved
}

TEST(video_links_mend_finds_a_moved_video_by_its_name) {
    LinkFile lf;
    VideoLinks& v = VideoLinks::get();
    const fs::path moved = lf.dir / "elsewhere" / "Clip.MP4";
    std::error_code ec;
    fs::create_directories(moved.parent_path(), ec);
    { std::ofstream(moved) << "x"; }
    v.set("/m/a.mp3", "/gone/clip.mp4");                    // the video is no longer there; the same name (any case) is somewhere else
    v.set("/m/b.mp3", "/gone/other.mp4");                   // no candidate: stays as it is (the drive may just be unplugged)
    CHECK_EQ(v.mend({moved.string()}), 1);
    CHECK_EQ(v.video_for("/m/a.mp3"), moved.string());
    CHECK_EQ(v.video_for("/m/b.mp3"), std::string("/gone/other.mp4"));
    // two candidates with the same name: no guess
    v.set("/m/c.mp3", "/gone/dup.mp4");
    CHECK_EQ(v.mend({"/p/dup.mp4", "/q/dup.mp4"}), 0);
}

TEST(video_download_command_and_settings) {
    const auto a = video_download_args("abc123", fs::path("/dl/video_down"), "Song Name", 720);
    CHECK_EQ(a[0], std::string("yt-dlp"));
    CHECK(std::find(a.begin(), a.end(), "--no-playlist") != a.end());
    CHECK(std::find(a.begin(), a.end(), "bv*[height<=720]+ba/b[height<=720]/b") != a.end());
    CHECK_EQ(a.back(), std::string("https://www.youtube.com/watch?v=abc123"));
    bool has_out = false;
    for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == "-o") has_out = a[i + 1].find("video_down") != std::string::npos && a[i + 1].find("Song Name.%(ext)s") != std::string::npos;
    CHECK(has_out);
    const auto b = video_download_args("x", fs::path("/d"), "n", 99999);   // the height is kept in a sane range
    CHECK(std::find(b.begin(), b.end(), "bv*[height<=2160]+ba/b[height<=2160]/b") != b.end());
}

namespace muisc { const OnlineResult* video_fetch_pick(const std::vector<OnlineResult>& hits, double song_seconds); }

TEST(video_fetch_pick_prefers_the_closest_length) {
    std::vector<muisc::OnlineResult> hits(3);
    hits[0].video_id = "a"; hits[0].duration_sec = 600;   // a long live set
    hits[1].video_id = "b"; hits[1].duration_sec = 215;
    hits[2].video_id = "c"; hits[2].duration_sec = 200;
    const muisc::OnlineResult* p = muisc::video_fetch_pick(hits, 205);
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->video_id, std::string("c"));
    p = muisc::video_fetch_pick(hits, -1);                 // length unknown: the first hit
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->video_id, std::string("a"));
    p = muisc::video_fetch_pick(hits, 30);                 // nothing near: the first hit
    if (p) CHECK_EQ(p->video_id, std::string("a"));
    CHECK(muisc::video_fetch_pick({}, 100) == nullptr);
}
