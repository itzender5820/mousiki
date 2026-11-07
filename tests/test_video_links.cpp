// The hand-made pairs of track and video: storage, following renames / moves, mending, and the yt-dlp command of a download.
#include "test_framework.h"
#include "video_links.h"
#include "cover_store.h"
#include "online_source.h"
#include "app_internal.h"
#include "settings.h"
#include "cache_manager.h"

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

TEST(video_links_mend_tracks_finds_a_moved_song_by_its_name) {
    LinkFile lf;
    VideoLinks& v = VideoLinks::get();
    const fs::path moved = lf.dir / "newplace" / "Song One.MP3";
    std::error_code ec;
    fs::create_directories(moved.parent_path(), ec);
    { std::ofstream(moved) << "x"; }
    v.set("/old/song one.mp3", "/v/one.mp4");            // the song moved (outside the app): its link follows
    v.set("/old/lost.mp3", "/v/lost.mp4");               // no candidate: stays (the drive may be unplugged)
    v.mark_none("/old/Song Two.mp3");                    // the "no video" mark follows too
    const fs::path moved2 = lf.dir / "newplace" / "song two.mp3";
    { std::ofstream(moved2) << "x"; }
    CHECK_EQ(v.mend_tracks({moved.string(), moved2.string()}), 2);
    CHECK_EQ(v.video_for(moved.string()), std::string("/v/one.mp4"));
    CHECK(v.is_none(moved2.string()));
    CHECK_EQ(v.video_for("/old/lost.mp3"), std::string("/v/lost.mp4"));
    CHECK_EQ(v.mend_tracks({"/a/dup.mp3", "/b/dup.mp3"}), 0);   // two candidates: no guess
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

TEST(video_downloads_are_remembered_and_found_again) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_vdl_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "video_down", ec);
    CacheManager c;
    c.set_download_dir(dir);
    CHECK(c.find_video_download("dQw4w9WgXcQ").empty());   // never downloaded
    const fs::path f = dir / "video_down" / "Some Song.mp4";
    { std::ofstream(f, std::ios::binary) << "video"; }
    c.record_video_download("dQw4w9WgXcQ", f.string());
    CHECK(c.find_video_download("dQw4w9WgXcQ") == f);      // found by the file list
    std::ifstream in(c.video_archive_path());
    std::string line;
    std::getline(in, line);
    CHECK_EQ(line, std::string("youtube dQw4w9WgXcQ"));    // in yt-dlp's archive format (for down_vid)
    c.record_video_download("dQw4w9WgXcQ", f.string());     // twice: still one archive line, one list line
    int n = 0;
    { std::ifstream a(c.video_archive_path()); while (std::getline(a, line)) ++n; }
    CHECK_EQ(n, 1);
    fs::remove(f, ec);
    CHECK(c.find_video_download("dQw4w9WgXcQ").empty());   // the file is gone: it can be fetched again
    const fs::path g = dir / "video_down" / "Other Title [abcDEF12345].mp4";   // a file made by down_vid
    { std::ofstream(g, std::ios::binary) << "video"; }
    CHECK(c.find_video_download("abcDEF12345") == g);
    CHECK(c.find_video_download("bad id!").empty());
    fs::remove_all(dir, ec);
}

TEST(video_links_no_video_mark_hides_the_name_match_and_persists) {
    LinkFile lf;
    VideoLinks& v = VideoLinks::get();
    v.set("/m/a.mp3", "/m/clip.mp4");
    CHECK(v.has("/m/a.mp3"));
    v.mark_none("/m/a.mp3");                          // the connection was removed on purpose
    CHECK(!v.has("/m/a.mp3"));
    CHECK(v.video_for("/m/a.mp3").empty());
    CHECK(v.is_none("/m/a.mp3"));
    CHECK(v.all().empty());                           // (the mark is no pair)
    CHECK(v.tracks_for("-").empty());
    v.reload();
    CHECK(v.is_none("/m/a.mp3"));                     // it stays across a restart
    v.remove("/m/a.mp3");                             // [DEL] again: the automatic pairing is back
    CHECK(!v.is_none("/m/a.mp3"));
    v.set("/m/a.mp3", "/m/clip.mp4");                 // a new connection replaces the mark
    v.mark_none("/m/a.mp3");
    v.set("/m/a.mp3", "/m/other.mp4");
    CHECK(!v.is_none("/m/a.mp3"));
    CHECK_EQ(v.video_for("/m/a.mp3"), std::string("/m/other.mp4"));
}

TEST(cover_track_video_mode_is_kept_apart_from_the_picture_choice) {
    set_cover_track_choice("/m/zz_modetest.mp3", "/m/cover.png");
    CHECK_EQ(cover_track_video_mode("/m/zz_modetest.mp3"), 0);
    set_cover_track_video_mode("/m/zz_modetest.mp3", 2);              // the video thumbnail
    CHECK_EQ(cover_track_video_mode("/m/zz_modetest.mp3"), 2);
    CHECK_EQ(cover_track_choice("/m/zz_modetest.mp3"), std::string("/m/cover.png"));   // the chosen picture is still there
    set_cover_track_video_mode("/m/zz_modetest.mp3", 1);              // and back to the cover
    CHECK_EQ(cover_track_choice("/m/zz_modetest.mp3"), std::string("/m/cover.png"));
    set_cover_track_video_mode("/m/zz_modetest.mp3", 0);
    CHECK_EQ(cover_track_video_mode("/m/zz_modetest.mp3"), 0);
    set_cover_track_choice("/m/zz_modetest.mp3", "");
}

TEST(video_links_several_tracks_can_share_one_video) {
    LinkFile lf;
    VideoLinks& L = VideoLinks::get();
    L.set("/m/a.mp3", "/v/clip.mp4");
    L.set("/m/b.mp3", "/v/clip.mp4");
    L.set("/m/c.mp3", "/v/clip.mp4");
    CHECK_EQ(L.video_for("/m/a.mp3"), std::string("/v/clip.mp4"));
    CHECK_EQ(L.video_for("/m/b.mp3"), std::string("/v/clip.mp4"));
    CHECK_EQ(L.tracks_for("/v/clip.mp4").size(), size_t(3));
    // taking one track's connection away leaves the others (and "no video" for one track is its own mark)
    L.remove("/m/a.mp3");
    L.mark_none("/m/b.mp3");
    CHECK(L.video_for("/m/a.mp3").empty());
    CHECK(L.video_for("/m/b.mp3").empty());
    CHECK_EQ(L.video_for("/m/c.mp3"), std::string("/v/clip.mp4"));
    // renaming the video moves every pair, and the pairs survive a reload from disk
    L.set("/m/a.mp3", "/v/clip.mp4");
    L.rename_paths({{"/v/clip.mp4", "/v/renamed.mp4"}});
    CHECK_EQ(L.video_for("/m/a.mp3"), std::string("/v/renamed.mp4"));
    CHECK_EQ(L.video_for("/m/c.mp3"), std::string("/v/renamed.mp4"));
    L.reload();
    CHECK_EQ(L.tracks_for("/v/renamed.mp4").size(), size_t(2));
    CHECK(L.is_none("/m/b.mp3"));
    // renaming one track only touches its own pair
    L.rename_paths({{"/m/a.mp3", "/m/a2.mp3"}});
    CHECK_EQ(L.video_for("/m/a2.mp3"), std::string("/v/renamed.mp4"));
    CHECK_EQ(L.video_for("/m/c.mp3"), std::string("/v/renamed.mp4"));
}
