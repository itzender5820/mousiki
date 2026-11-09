// The album cover of the music player: where it is looked for (cover_art.cpp) and the settings that choose it.
#include "test_framework.h"
#include "cover_art.h"
#include "cover_store.h"
#include "path_utf8.h"
#include "process_util.h"
#include "settings.h"

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <cstdlib>

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

namespace {
// The pictures of these tests are made and read with ffmpeg: where there is none (a CI machine), the test does nothing, like the video tests.
bool have_ffmpeg() {
    static const bool ok = [] { const ProcResult r = run_capture({"ffmpeg", "-version"}); return r.exit_code == 0; }();
    return ok;
}
void make_png(const fs::path& p, const char* colour) {
    const std::string cmd = std::string("ffmpeg -loglevel quiet -y -f lavfi -i color=c=") + colour + ":s=64x64 -frames:v 1 \"" + p.string() + "\"";
    if (std::system(cmd.c_str()) != 0) {}
}
const CoverImage* wait_cover(const CoverRequest& rq) {
    for (int i = 0; i < 400; ++i) {
        if (const CoverImage* c = track_cover(rq)) if (c->key.find(std::to_string(rq.store_rev)) != std::string::npos) return c;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return track_cover(rq);
}
}

TEST(cover_track_choice_changes_the_picture_and_same_path_edit_is_seen) {
    if (!have_ffmpeg()) return;   // (no ffmpeg here)
    mt::TempHome home;
    const fs::path d = home.path() / "alb9";
    fs::create_directories(d);
    touch(d / "song.mp3");
    make_png(d / "a.png", "red");
    make_png(d / "b.png", "blue");
    CoverRequest rq;
    rq.path = path_utf8(d / "song.mp3");
    rq.title = "song"; rq.album = "Nine"; rq.search = 0;
    set_cover_track_choice(rq.path, path_utf8(d / "a.png"));
    rq.store_rev = cover_store_revision();
    const CoverImage* c1 = wait_cover(rq);
    CHECK(c1 != nullptr);
    const unsigned char r1 = c1 ? c1->rgb[0] : 0, b1 = c1 ? c1->rgb[2] : 0;
    set_cover_track_choice(rq.path, path_utf8(d / "b.png"));
    rq.store_rev = cover_store_revision();
    const CoverImage* c2 = wait_cover(rq);
    CHECK(c2 != nullptr);
    CHECK(c2 && c2->rgb[2] > c2->rgb[0] && r1 > b1);               // red, then blue
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // (a file is looked at once a second)
    make_png(d / "b.png", "green");                                   // the same file edited, chosen again
    set_cover_track_choice(rq.path, path_utf8(d / "b.png"));
    rq.store_rev = cover_store_revision();
    const CoverImage* c3 = wait_cover(rq);
    CHECK(c3 != nullptr);
    CHECK(c3 && c3->rgb[1] > c3->rgb[2]);
    const CoverImage* p1 = preview_cover(path_utf8(d / "b.png"), false);
    for (int i = 0; i < 200 && !p1; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(25)); p1 = preview_cover(path_utf8(d / "b.png"), false); }
    CHECK(p1 != nullptr);
    // the chosen file edited in place with no new choice: the cover of the track is read again (within a second or two)
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    make_png(d / "b.png", "blue");
    bool seen_blue = false;
    for (int i = 0; i < 200 && !seen_blue; ++i) { const CoverImage* c = track_cover(rq); seen_blue = c && c->rgb[2] > c->rgb[1]; if (!seen_blue) std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
    CHECK(seen_blue);
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    make_png(d / "b.png", "red");
    const CoverImage* p2 = nullptr;
    for (int i = 0; i < 200 && !p2; ++i) { p2 = preview_cover(path_utf8(d / "b.png"), false); if (!p2) std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
    CHECK(p2 && p2->rgb[0] > p2->rgb[1]);   // the preview shows the edited file
}

TEST(thumbnail_fallback_uses_the_cover_first_and_the_video_frame_only_without_one) {
    if (!have_ffmpeg()) return;   // (no ffmpeg here)
    mt::TempHome home;
    const fs::path d = home.path() / "alb10";
    fs::create_directories(d);
    touch(d / "song.mp3");
    const std::string vid = path_utf8(d / "song.mp4");
    if (std::system(("ffmpeg -loglevel quiet -y -f lavfi -i color=c=green:s=64x64:d=1:r=10 -pix_fmt yuv420p \"" + vid + "\"").c_str()) != 0) {}
    CoverRequest rq;
    rq.path = path_utf8(d / "song.mp3");
    rq.title = "song"; rq.album = "Ten"; rq.search = 0;
    rq.snapshot_video = vid; rq.thumb_fallback = true;
    rq.store_rev = cover_store_revision();
    const CoverImage* none = nullptr;
    for (int i = 0; i < 400 && !none; ++i) { none = track_cover(rq); if (!none) std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
    CHECK(none != nullptr);                                            // no cover: a frame of the video (green)
    CHECK(none && none->rgb[1] > none->rgb[0] && none->rgb[1] > none->rgb[2]);
    make_png(d / "cover.png", "red");
    set_cover_track_choice(rq.path, path_utf8(d / "cover.png"));
    rq.store_rev = cover_store_revision();
    const CoverImage* c = wait_cover(rq);
    CHECK(c && c->rgb[0] > c->rgb[1]);                                 // a cover: the cover, not the frame
    set_cover_track_choice(rq.path, kCoverNone);                       // "no cover" on purpose: the disk (the fallback must not use the frame there)
    rq.thumb_fallback = false; rq.snapshot_video.clear();
    rq.store_rev = cover_store_revision();
    CHECK(cover_track_choice(rq.path) == kCoverNone);
}

TEST(cover_track_video_mode_knows_cover_thumbnail_disk_and_fallback) {
    mt::TempHome home;
    set_cover_track_video_mode("/x/a.mp3", 4);
    CHECK_EQ(cover_track_video_mode("/x/a.mp3"), 4);
    set_cover_track_video_mode("/x/a.mp3", 3);   // the plain disk
    CHECK_EQ(cover_track_video_mode("/x/a.mp3"), 3);
    set_cover_track_video_mode("/x/a.mp3", 7);   // not a mode: removed
    CHECK_EQ(cover_track_video_mode("/x/a.mp3"), 0);
    set_cover_track_video_mode("/x/a.mp3", 2);
    CHECK_EQ(cover_track_video_mode("/x/a.mp3"), 2);
    set_cover_track_video_mode("/x/a.mp3", 0);
    CHECK_EQ(cover_track_video_mode("/x/a.mp3"), 0);
}

TEST(a_picture_that_could_not_be_read_is_tried_again_later) {
    if (!have_ffmpeg()) return;   // (no ffmpeg here)
    mt::TempHome home;
    const fs::path d = home.path() / "alb11";
    fs::create_directories(d);
    const fs::path good = d / "good.png", pic = d / "pic.png";
    make_png(good, "red");
    const auto size = fs::file_size(good);
    { std::ofstream o(pic, std::ios::binary); o << std::string(size, 'x'); }   // not a picture, but of the size of one
    const auto when = fs::last_write_time(pic);
    const std::string f = path_utf8(pic);
    const CoverImage* p = nullptr;
    for (int i = 0; i < 100 && preview_cover_loading(f, false); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    p = preview_cover(f, false);
    CHECK(p == nullptr);                                    // cannot be read
    fs::copy_file(good, pic, fs::copy_options::overwrite_existing);
    fs::last_write_time(pic, when);                         // (the same size and time: only the retry can show it)
    std::this_thread::sleep_for(std::chrono::milliseconds(5500));
    for (int i = 0; i < 100 && !p; ++i) { p = preview_cover(f, false); if (!p) std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(p != nullptr);
}

TEST(a_video_in_video_down_sees_the_covers_of_the_download_folder) {
    mt::TempHome home;
    const fs::path dl = home.path() / "dl";
    fs::create_directories(dl / "video_down");
    fs::create_directories(dl / kFetchedCoversDir);
    touch(dl / "video_down" / "Clip.mp4");
    touch(dl / kFetchedCoversDir / "Some Album.jpg");
    touch(dl / kFetchedCoversDir / "Other.png");
    const auto c = cover_candidates(dl / "video_down" / "Clip.mp4", false);
    CHECK_EQ(static_cast<int>(c.size()), 2);
    CHECK(!find_fetched_cover(dl / "video_down", "Some Album").empty());
    CHECK(find_fetched_cover(dl / "video_down", "Nope").empty());
    CHECK(shared_covers_home(dl / "music").empty());
    const fs::path pic = pick_folder_picture(dl / "video_down", {"Some Album", "Clip"}, false, false, true);
    CHECK(!pic.empty());
}

TEST(a_file_outside_the_library_finds_the_fetched_covers_of_the_library_by_album) {
    mt::TempHome home;
    const fs::path lib = home.path() / "lib", other = home.path() / "elsewhere";
    fs::create_directories(lib / "Band" / "Album" / kFetchedCoversDir);
    fs::create_directories(other);
    touch(lib / "Band" / "Album" / kFetchedCoversDir / "Lib Album.jpg");
    set_cover_library_roots({lib});
    CHECK(!cover_outside_library(lib / "Band" / "Album"));
    CHECK(cover_outside_library(other));
    CHECK(!fetched_cover_in_library("Lib Album", true).empty());
    CHECK(fetched_cover_in_library("Nope", true).empty());
    CHECK(!find_fetched_cover(other, "Lib Album").empty());
    CHECK(find_fetched_cover(lib / "Band" / "Album", "Other Album").empty());
    set_cover_library_roots({});
}

TEST(a_video_uses_the_choices_and_covers_of_the_song_it_is_paired_with) {
    mt::TempHome home;
    const fs::path songs = home.path() / "lib" / "Songs", vids = home.path() / "dl" / "video_down";
    fs::create_directories(songs / kFetchedCoversDir);
    fs::create_directories(vids);
    touch(songs / "Tune.mp3");
    touch(vids / "Tune.mp4");
    touch(songs / "pic.jpg");
    CoverRequest rq;
    rq.path = path_utf8(vids / "Tune.mp4");
    rq.title = "Tune"; rq.album = "Tune Album"; rq.search = 0;
    rq.store_rev = cover_store_revision();
    CHECK(resolve_cover_source(rq).none);                                   // not paired: nothing near the video
    rq.pair_path = path_utf8(songs / "Tune.mp3");
    touch(songs / kFetchedCoversDir / "Tune Album.jpg");                    // a cover fetched for the song's album
    CoverSource s = resolve_cover_source(rq);
    CHECK(!s.none);
    CHECK(path_utf8(path_from_utf8(s.file).parent_path().filename()) == kFetchedCoversDir);
    set_cover_track_choice(rq.pair_path, path_utf8(songs / "pic.jpg"));     // the song's own choice serves the video
    rq.store_rev = cover_store_revision();
    CHECK_EQ(resolve_cover_source(rq).file, path_utf8(songs / "pic.jpg"));
    fs::remove(songs / "Tune.mp3");                                         // the song is gone: its choice stays
    rq.store_rev = cover_store_revision();
    CHECK_EQ(resolve_cover_source(rq).file, path_utf8(songs / "pic.jpg"));
    set_cover_track_choice(rq.path, kCoverNone);                            // the video's own choice goes first
    rq.store_rev = cover_store_revision();
    CHECK(resolve_cover_source(rq).none);
}

TEST(a_preview_slot_and_the_playing_slot_do_not_throw_each_other_out) {
    if (!have_ffmpeg()) return;   // (no ffmpeg here)
    mt::TempHome home;
    const fs::path a = home.path() / "albA", b = home.path() / "albB";
    fs::create_directories(a);
    fs::create_directories(b);
    touch(a / "song.mp3");
    touch(b / "song.mp3");
    make_png(a / "cover.png", "red");
    make_png(b / "cover.png", "blue");
    CoverRequest ra, rb;
    ra.path = path_utf8(a / "song.mp3"); ra.title = "song"; ra.album = "A"; ra.search = 0; ra.store_rev = cover_store_revision();
    rb.path = path_utf8(b / "song.mp3"); rb.title = "song"; rb.album = "B"; rb.search = 0; rb.store_rev = cover_store_revision();
    // both are read, then asked in turn like the main screen (the playing track) and the Style Option preview (another track) do every frame
    bool got_a = false, got_b = false;
    for (int i = 0; i < 400 && !(got_a && got_b); ++i) {
        got_a = got_a || track_cover(ra, false) != nullptr;
        got_b = got_b || track_cover(rb, true) != nullptr;
        if (!(got_a && got_b)) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(got_a);
    CHECK(got_b);
    int missing = 0;
    for (int i = 0; i < 60; ++i) {
        if (!track_cover(ra, false)) ++missing;
        if (!track_cover(rb, true)) ++missing;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(missing, 0);   // (with one slot for both, each ask threw the other's picture out and started a new read)
}

TEST(a_style_or_look_change_does_not_make_the_cover_load_again) {
    mt::TempHome home;
    const unsigned rev = cover_store_revision(), srev = cover_style_revision();
    CoverStyle st;
    st.brightness = 40;
    set_cover_track_style("/music/a.mp3", st);
    set_cover_album_look("Album", "/music", 2);
    CHECK_EQ(cover_store_revision(), rev);      // the request of the cover stays the same: the picture on screen is not thrown away
    CHECK(cover_style_revision() != srev);      // (but what the looks are read from is refreshed)
    CHECK_EQ(cover_track_style("/music/a.mp3").brightness, 40);
    CHECK_EQ(cover_album_look("Album", "/music"), 2);
    set_cover_track_choice("/music/a.mp3", kCoverNone);   // a different picture is a change of the store
    CHECK(cover_store_revision() != rev);
}
