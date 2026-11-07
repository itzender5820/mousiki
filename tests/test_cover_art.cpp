// The album cover of the music player: where it is looked for (cover_art.cpp) and the settings that choose it.
#include "test_framework.h"
#include "cover_art.h"
#include "cover_store.h"
#include "path_utf8.h"
#include "settings.h"

#include <filesystem>
#include <fstream>

using namespace muisc;
namespace fs = std::filesystem;

namespace {
void touch(const fs::path& p) { std::ofstream(p, std::ios::binary) << "x"; }
}

TEST(cover_name_closeness_ignores_case_punctuation_and_extra_words) {
    CHECK_EQ(name_closeness("Thriller", "thriller"), 1.0);
    CHECK_EQ(name_closeness("Abbey_Road", "abbey road"), 1.0);
    CHECK(name_closeness("Abbey Road - front", "Abbey Road") >= kCoverCloseness);   // the album's name inside a longer one
    CHECK(name_closeness("Thriler", "Thriller") >= kCoverCloseness);                // a typo
    CHECK(name_closeness("Dark Side of the Moon", "dark side moon") >= kCoverCloseness);
    CHECK(name_closeness("Bad", "Thriller") < kCoverCloseness);
    CHECK(name_closeness("cover", "Thriller") < kCoverCloseness);
    CHECK_EQ(name_closeness("", "Thriller"), 0.0);
    CHECK_EQ(name_closeness("!!!", "???"), 0.0);
}

TEST(cover_search_steps_follow_the_setting) {
    using S = CoverStep;
    CHECK(cover_steps(0) == (std::vector<S>{S::Embedded, S::FolderClose, S::FolderGeneric, S::FolderAny}));      // Default
    CHECK(cover_steps(1) == (std::vector<S>{S::FolderClose, S::FolderGeneric, S::Embedded, S::FolderAny}));      // Folder First
    CHECK(cover_steps(2) == (std::vector<S>{S::Embedded, S::FolderClose, S::FolderAny}));                         // Within File First
    CHECK(cover_steps(3) == (std::vector<S>{S::FolderClose, S::FolderGeneric, S::FolderAny}));                    // Image in Folder Only
    CHECK(cover_steps(4) == (std::vector<S>{S::Embedded}));                                         // Within File Only
    CHECK(cover_steps(99) == cover_steps(0));
}

TEST(cover_folder_picture_is_the_closest_named_one_then_a_generic_one) {
    mt::TempHome home;
    const fs::path d = home.path() / "mixed";
    fs::create_directories(d);
    touch(d / "Alpha.jpg");
    touch(d / "Beta Album.png");
    touch(d / "cover.jpg");
    touch(d / "notes.txt");
    CHECK(pick_folder_picture(d, {"Alpha", "x"}, false).filename() == "Alpha.jpg");
    CHECK(pick_folder_picture(d, {"", "beta album (deluxe)"}, false).filename() == "Beta Album.png");   // title or album: either may match
    CHECK(pick_folder_picture(d, {"Gamma", "Gamma Song"}, false).empty());                              // nothing is close
    CHECK(pick_folder_picture(d, {"Gamma"}, true).filename() == "cover.jpg");                           // the generic one
    fs::remove(d / "cover.jpg");
    CHECK(pick_folder_picture(d, {"Gamma"}, true).empty());                                             // no generic one, not just any picture
    CHECK(pick_folder_picture(home.path() / "missing", {"Alpha"}, false).empty());
}

TEST(cover_settings_round_trip_and_defaults) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK(!s.album_cover);       // off until switched on
    CHECK_EQ(s.cover_mode, 2);   // cover behind the disk
    CHECK_EQ(s.cover_search, 0);
    s.album_cover = true;
    s.cover_mode = 1;
    s.cover_search = 3;
    save_settings(s);
    Settings back = load_settings();
    CHECK(back.album_cover);
    CHECK_EQ(back.cover_mode, 1);
    CHECK_EQ(back.cover_search, 3);
    for (int search = 0; search < 5; ++search)
        for (int mode = 0; mode < 5; ++mode) {
            back.cover_search = search;
            back.cover_mode = mode;
            save_settings(back);
            const Settings again = load_settings();
            CHECK_EQ(again.cover_search, search);
            CHECK_EQ(again.cover_mode, mode);
        }
}

TEST(cover_names_from_the_user_report_match) {
    CHECK(name_closeness("test-song-cover", "test_song-cover3") >= kCoverCloseness);
    CHECK(name_closeness("test_cov", "test_song-cover3") >= kCoverCloseness);   // an abbreviated word
}

TEST(cover_folder_picture_takes_every_regular_format) {
    mt::TempHome home;
    const fs::path d = home.path() / "formats";
    fs::create_directories(d);
    for (const char* ext : {".jpg", ".JPEG", ".png", ".webp", ".bmp", ".gif", ".tiff", ".avif", ".heic", ".jxl", ".jp2", ".tga", ".ico"}) {
        const fs::path f = d / (std::string("Thriller") + ext);
        touch(f);
        CHECK(pick_folder_picture(d, {"Thriller"}, false) == f);
        fs::remove(f);
    }
    touch(d / "Thriller.cr2");   // camera RAW: not a cover
    touch(d / "Thriller.txt");
    CHECK(pick_folder_picture(d, {"Thriller"}, false).empty());
}

TEST(cover_any_picture_is_the_alphabetical_last_resort) {
    mt::TempHome home;
    const fs::path d = home.path() / "any";
    fs::create_directories(d);
    touch(d / "zebra.png");
    touch(d / "Moon.jpg");
    CHECK(pick_folder_picture(d, {"Gamma"}, false, true).filename() == "Moon.jpg");
}

TEST(cover_contrast_is_saved_in_steps_of_five) {
    mt::TempHome home;
    Settings s = load_settings();
    CHECK_EQ(s.cover_contrast, 0);
    s.cover_contrast = 35;
    save_settings(s);
    CHECK_EQ(load_settings().cover_contrast, 35);
}

TEST(cover_store_names_and_candidates) {
    mt::TempHome home;
    CHECK_EQ(cover_name_key("Abbey_Road!"), std::string("abbey road"));
    CHECK_EQ(cover_file_name("AC/DC: Back in Black?"), std::string("AC_DC_ Back in Black_"));
    const fs::path d = home.path() / "album";
    fs::create_directories(d / kFetchedCoversDir);
    touch(d / "song.mp3");
    touch(d / "front.jpg");
    touch(d / kFetchedCoversDir / "Thriller.jpg");
    const auto c = cover_candidates(d / "song.mp3", true);
    CHECK_EQ(c.size(), static_cast<size_t>(3));
    CHECK(c[0].kind == CoverCandidate::Embedded);
    CHECK(c[1].kind == CoverCandidate::Folder);
    CHECK(c[2].kind == CoverCandidate::Fetched);
    CHECK(find_fetched_cover(d, "thriller").filename() == "Thriller.jpg");
    CHECK(find_fetched_cover(d, "Bad").empty());
    // a close name is also found in the fetched covers folder
    CHECK(pick_folder_picture(d, {"Thriller"}, false, false, true).filename() == "Thriller.jpg");
    CHECK(pick_folder_picture(d, {"Thriller"}, false, false, false).empty());
    // ... but only by the ALBUM's name: a track title that resembles another album's fetched cover must not pick it
    CHECK(pick_folder_picture(d, {"4REAL", "Thriller"}, false, false, true).empty());
}

TEST(cover_choices_are_kept_and_deleting_removes_them) {
    mt::TempHome home;
    const unsigned rev = cover_store_revision();
    set_cover_track_choice("/m/a.mp3", "/m/x.jpg");
    set_cover_album_choice("Abbey Road", "/m/y.jpg");
    CHECK_EQ(cover_track_choice("/m/a.mp3"), std::string("/m/x.jpg"));
    CHECK_EQ(cover_album_choice("abbey_road"), std::string("/m/y.jpg"));   // the album name is a key
    CHECK(cover_store_revision() > rev);
    const fs::path d = home.path() / "alb";
    fs::create_directories(d);
    touch(d / "a.jpg");
    CoverCandidate c{CoverCandidate::Folder, d / "a.jpg", "a.jpg"};
    set_cover_track_choice("/m/b.mp3", path_utf8(c.file));
    CHECK(delete_cover_candidate(c));
    CHECK(!fs::exists(d / "a.jpg"));
    CHECK_EQ(cover_track_choice("/m/b.mp3"), std::string());
    CoverCandidate e{CoverCandidate::Embedded, d / "x.mp3", "inside the file"};
    std::string why;
    CHECK(!delete_cover_candidate(e, &why));
    CHECK(!why.empty());
    set_cover_track_choice("/m/a.mp3", "");
    set_cover_album_choice("Abbey Road", "");
}

TEST(cover_fetch_answer_line_is_parsed) {
    const CoverFetchResult ok = parse_cover_fetch_line("{\"ok\": true, \"file\": \"/m/fetched_covers/A.jpg\", \"release\": \"A\", \"artist\": \"B\", \"match\": 0.93}");
    CHECK(ok.ok);
    CHECK_EQ(ok.file, std::string("/m/fetched_covers/A.jpg"));
    CHECK(ok.match > 0.9);
    const CoverFetchResult bad = parse_cover_fetch_line("{\"ok\": false, \"error\": \"no release called like that on MusicBrainz\"}");
    CHECK(!bad.ok);
    CHECK(bad.error.find("MusicBrainz") != std::string::npos);
    CHECK(!parse_cover_fetch_line("garbage").ok);
}

TEST(cover_source_follows_track_choice_before_album_choice_before_search) {
    mt::TempHome home;
    const fs::path d = home.path() / "alb2";
    fs::create_directories(d);
    touch(d / "song.mp3");
    touch(d / "one.jpg");
    touch(d / "two.jpg");
    CoverRequest rq;
    rq.path = path_utf8(d / "song.mp3");
    rq.title = "song";
    rq.album = "Nowhere Album";
    rq.search = 0;
    set_cover_album_choice("Nowhere Album", path_utf8(d / "one.jpg"));
    rq.store_rev = cover_store_revision();
    CoverSource a = resolve_cover_source(rq);
    CHECK(a.chosen);
    CHECK_EQ(a.file, path_utf8(d / "one.jpg"));
    set_cover_track_choice(rq.path, path_utf8(d / "two.jpg"));       // the track beats the album
    rq.store_rev = cover_store_revision();
    CHECK_EQ(resolve_cover_source(rq).file, path_utf8(d / "two.jpg"));
    set_cover_track_choice(rq.path, kCoverNone);                      // "no cover"
    rq.store_rev = cover_store_revision();
    CHECK(resolve_cover_source(rq).none);
    set_cover_track_choice(rq.path, "");
    set_cover_album_choice("Nowhere Album", "");
}

TEST(album_covers_lists_each_picture_once) {
    mt::TempHome home;
    const fs::path d = home.path() / "alb3";
    fs::create_directories(d / kFetchedCoversDir);
    touch(d / "t.mp3");
    touch(d / "Nice Album.jpg");
    touch(d / kFetchedCoversDir / "Nice Album.jpg");
    touch(d / "unrelated.jpg");
    AlbumCovers ac = album_covers(d, "Nice Album", d / "t.mp3", false, false);
    CHECK_EQ(static_cast<int>(ac.files.size()), 2);
    AlbumCovers none = album_covers(d, "Other", d / "t.mp3", false, false);
    CHECK_EQ(none.count(), 0);
}

TEST(cover_look_per_album) {
    mt::TempHome home;
    CHECK_EQ(cover_album_look("Some Album", "/music/a"), -1);           // none chosen
    set_cover_album_look("Some Album", "/music/a", 4);
    CHECK_EQ(cover_album_look("some_album", "/elsewhere"), 4);         // the album name decides (spelling ignored), not the folder
    set_cover_album_look("", "/music/loose", 0);                        // no album tag: the folder
    CHECK_EQ(cover_album_look("-", "/music/loose"), 0);
    CHECK_EQ(cover_album_look("", "/music/other"), -1);
    set_cover_album_look("Some Album", "/music/a", -1);                 // back to Settings
    CHECK_EQ(cover_album_look("Some Album", "/music/a"), -1);
    set_cover_album_look("", "/music/loose", -1);
}

TEST(video_track_option_mp4_audio_is_kept_per_track) {
    mt::TempHome home;
    CHECK_EQ(cover_track_video_opts("/music/a.mp3").mp4audio, -1);     // default: the song's own audio
    VideoTrackOpts o;
    o.mp4audio = 1;
    set_cover_track_video_opts("/music/a.mp3", o);
    CHECK_EQ(cover_track_video_opts("/music/a.mp3").mp4audio, 1);
    CHECK_EQ(cover_track_video_opts("/music/b.mp3").mp4audio, -1);     // other tracks keep the default
    o.brightness = 60;                                                 // the other values around it stay intact
    set_cover_track_video_opts("/music/a.mp3", o);
    CHECK_EQ(cover_track_video_opts("/music/a.mp3").brightness, 60);
    CHECK_EQ(cover_track_video_opts("/music/a.mp3").mp4audio, 1);
    set_cover_track_video_opts("/music/a.mp3", VideoTrackOpts{});      // all default: the entry is gone
    CHECK_EQ(cover_track_video_opts("/music/a.mp3").mp4audio, -1);
}

TEST(cover_scaled_keeps_several_sizes) {
    CoverImage c;
    c.key = "test-cover"; c.w = 64; c.h = 32; c.ok = true;
    c.level.assign(static_cast<size_t>(c.w) * c.h, 128);
    c.alpha.assign(c.level.size(), 255);
    c.rgb.assign(c.level.size() * 3, 100);
    const CoverScaled& a = cover_scaled(c, 40, 20, false, 0);
    const CoverScaled& b = cover_scaled(c, 20, 10, false, 0);
    CHECK(&a != &b);                                                    // two sizes, two slots
    CHECK(&cover_scaled(c, 40, 20, false, 0) == &a);                    // the first size is still there (not made again)
    CHECK_EQ(a.w, 40);
    CHECK_EQ(b.w, 20);
}
