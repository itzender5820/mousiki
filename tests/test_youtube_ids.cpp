// The checks in front of yt-dlp: only a real video id or an http(s) address is passed on (after `--`).
#include "test_framework.h"
#include "youtube_source.h"

using namespace muisc;

TEST(youtube_video_ids) {
    CHECK(valid_video_id("dQw4w9WgXcQ"));
    CHECK(valid_video_id("a_b-c1"));
    CHECK(!valid_video_id(""));
    CHECK(!valid_video_id("short"));
    CHECK(!valid_video_id("--exec=rm"));                      // two dashes are fine in an id, but "=" is not
    CHECK(!valid_video_id("dQw4w9WgXcQ&list=x"));
    CHECK(!valid_video_id("https://evil.example/x"));
    CHECK(!valid_video_id("id with space"));
    CHECK(!valid_video_id(std::string(33, 'a')));
}

TEST(youtube_track_urls) {
    CHECK(valid_track_url("https://soundcloud.com/artist/track"));
    CHECK(valid_track_url("http://artist.bandcamp.com/track/x"));
    CHECK(!valid_track_url("file:///etc/passwd"));
    CHECK(!valid_track_url("--exec://x"));
    CHECK(!valid_track_url("https://x.example/a b"));
    CHECK(!valid_track_url("https://x.example/a\nb"));
    CHECK(!valid_track_url(std::string("https://x.example/") + std::string(2100, 'a')));
}
