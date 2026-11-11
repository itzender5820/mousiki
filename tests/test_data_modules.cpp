// Data and text modules that the player relies on but that had no tests: the small JSON reader/writer, the lyrics text
// helpers, UTF-8 helpers, saved playlists and the listening history.
#include <fstream>

#include "history.h"
#include "lyrics_fetcher.h"
#include "playlist_manager.h"
#include "path_utf8.h"
#include "test_framework.h"
#include "TextSanitizer.h"
#include "tiny_json.h"
#include "utf8_util.h"

using namespace muisc;
namespace tj = muisc::tinyjson;

// ---- tiny_json --------------------------------------------------------------------------------------------------

TEST(json_round_trip_keeps_every_type) {
    tj::Value root = tj::Value::make_obj();
    root.set("name", tj::Value::make_str("Mousiki \"Plus\"\nline two\\end"));
    root.set("count", tj::Value::make_num(42));
    root.set("ratio", tj::Value::make_num(0.25));
    root.set("on", tj::Value::make_bool(true));
    root.set("off", tj::Value::make_bool(false));
    tj::Value list = tj::Value::make_arr();
    list.arr.push_back(tj::Value::make_num(1));
    list.arr.push_back(tj::Value::make_str("two"));
    root.set("list", list);

    tj::Value back;
    CHECK(tj::parse(tj::write(root), back));
    CHECK(back.type == tj::Type::Object);
    CHECK_EQ(back.find("name")->as_string(), std::string("Mousiki \"Plus\"\nline two\\end"));
    CHECK_NEAR(back.find("count")->as_number(), 42, 1e-9);
    CHECK_NEAR(back.find("ratio")->as_number(), 0.25, 1e-9);
    CHECK(back.find("on")->as_bool());
    CHECK(!back.find("off")->as_bool(true));
    CHECK_EQ(back.find("list")->arr.size(), static_cast<size_t>(2));
    CHECK_EQ(back.find("list")->arr[1].as_string(), std::string("two"));
}

TEST(json_set_replaces_and_find_misses_cleanly) {
    tj::Value o = tj::Value::make_obj();
    o.set("a", tj::Value::make_num(1));
    o.set("a", tj::Value::make_num(2));          // same key: replaced, not duplicated
    CHECK_EQ(o.obj.size(), static_cast<size_t>(1));
    CHECK_NEAR(o.find("a")->as_number(), 2, 1e-9);
    CHECK(o.find("missing") == nullptr);
    CHECK_EQ(tj::Value::make_num(5).as_string("fallback"), std::string("fallback"));   // wrong type: the default
    CHECK_NEAR(tj::Value::make_str("x").as_number(7), 7, 1e-9);
}

TEST(json_malformed_input_never_crashes) {
    // A half-written snapshot or history file must fail to parse, not crash.
    const char* bad[] = {"", "{", "[1,2", "{\"a\":}", "{\"a\" 1}", "\"unterminated", "{\"a\":1,}x", "nul", "[1 2]", "{1:2}"};
    for (const char* text : bad) {
        tj::Value v;
        (void)tj::parse(text, v);                 // the result may be either, it only must not throw or crash
    }
    tj::Value v;
    CHECK(!tj::parse("{\"a\":", v));
    CHECK(!tj::parse("[1,2", v));
    CHECK(!tj::parse("\"unterminated", v));
}

TEST(json_empty_containers_and_nesting) {
    tj::Value v;
    CHECK(tj::parse("{\"a\":[],\"b\":{},\"c\":[[1],[2,[3]]]}", v));
    CHECK(v.find("a")->arr.empty());
    CHECK(v.find("b")->obj.empty());
    CHECK_EQ(v.find("c")->arr.size(), static_cast<size_t>(2));
    CHECK_NEAR(v.find("c")->arr[1].arr[1].arr[0].as_number(), 3, 1e-9);
    CHECK(tj::parse("  \n{ \"k\" :\t\"v\" }  ", v));   // white space everywhere
    CHECK_EQ(v.find("k")->as_string(), std::string("v"));
}

TEST(json_negative_and_exponent_numbers) {
    tj::Value v;
    CHECK(tj::parse("[-3, 2.5e2, 0.001]", v));
    CHECK_NEAR(v.arr[0].as_number(), -3, 1e-9);
    CHECK_NEAR(v.arr[1].as_number(), 250, 1e-9);
    CHECK_NEAR(v.arr[2].as_number(), 0.001, 1e-12);
}

// ---- UTF-8 helpers ------------------------------------------------------------------------------------------------

TEST(utf8_ascii_case_folding_leaves_other_bytes_alone) {
    CHECK_EQ(ascii_lower_str("Hello WORLD"), std::string("hello world"));
    CHECK_EQ(ascii_upper_str("Hello world"), std::string("HELLO WORLD"));
    const std::string cjk = "\xE4\xB8\xAD\xE6\x96\x87 ABC";   // two CJK characters and ASCII
    CHECK_EQ(ascii_lower_str(cjk), std::string("\xE4\xB8\xAD\xE6\x96\x87 abc"));   // the multi-byte part is untouched
    CHECK(ascii_alnum('a') && ascii_alnum('Z') && ascii_alnum('7'));
    CHECK(!ascii_alnum('-') && !ascii_alnum(0xE4));
}

TEST(utf8_sequence_lengths_and_decoding) {
    CHECK_EQ(utf8_seq_len('a'), 1);
    CHECK_EQ(utf8_seq_len(0xC3), 2);
    CHECK_EQ(utf8_seq_len(0xE4), 3);
    CHECK_EQ(utf8_seq_len(0xF0), 4);
    CHECK_EQ(utf8_seq_len(0x80), 1);                  // a stray continuation byte counts as one
    const std::string s = "a\xC3\xA4\xE4\xB8\xAD\xF0\x9F\x8E\xB5";   // a, a-umlaut, CJK, musical note emoji
    size_t i = 0;
    CHECK_EQ(utf8_decode(s, i), static_cast<uint32_t>('a'));
    CHECK_EQ(utf8_decode(s, i), static_cast<uint32_t>(0xE4));
    CHECK_EQ(utf8_decode(s, i), static_cast<uint32_t>(0x4E2D));
    CHECK_EQ(utf8_decode(s, i), static_cast<uint32_t>(0x1F3B5));
    CHECK_EQ(i, s.size());
}

TEST(utf8_decode_does_not_read_past_a_truncated_sequence) {
    const std::string cut = "\xE4\xB8";                // a 3-byte lead with only one more byte
    size_t i = 0;
    (void)utf8_decode(cut, i);
    CHECK(i <= cut.size());
}

// ---- lyrics text ----------------------------------------------------------------------------------------------------

TEST(lyrics_sanitizer_removes_the_hangul_filler_only) {
    const std::string filler = "\xE3\x85\xA4";           // U+3164
    CHECK_EQ(sanitize_lyric_text("a" + filler + "b"), std::string("ab"));
    CHECK_EQ(sanitize_lyric_text("plain text"), std::string("plain text"));
    CHECK_EQ(sanitize_lyric_text("\xE4\xB8\xAD\xE6\x96\x87"), std::string("\xE4\xB8\xAD\xE6\x96\x87"));
    CHECK_EQ(sanitize_lyric_text(filler), std::string());
}

TEST(lyrics_plain_lines_word_tags_without_spaces_still_separate_the_words) {
    // enhanced LRC as some services write it: <time>word<time>word, no space anywhere
    const std::vector<std::string> lines = lyrics_plain_lines(
        "[00:34.279]<00:34.279>Talking<00:35.429>away\n"
        "[00:51.514]<00:51.514>Take<00:52.914>on<00:54.330>me<00:55.697>(Take<00:56.297>on<00:56.849>me)\n");
    CHECK_EQ(lines.size(), static_cast<size_t>(2));
    CHECK_EQ(lines[0], std::string("Talking away"));
    CHECK_EQ(lines[1], std::string("Take on me (Take on me)"));
}

TEST(lyrics_plain_lines_strip_timestamps_tags_and_blank_runs) {
    const std::string lrc =
        "[ar:Some Artist]\n[ti:Some Title]\n[offset:+250]\n"
        "[00:12.30]First line\r\n"
        "[00:15.00]<00:15.00>Second <00:15.50>line\n"
        "\n\n\n"
        "[01:02.5]Third   line   with  gaps\n";
    const std::vector<std::string> lines = lyrics_plain_lines(lrc);
    CHECK_EQ(lines.size(), static_cast<size_t>(4));
    CHECK_EQ(lines[0], std::string("First line"));
    CHECK_EQ(lines[1], std::string("Second line"));
    CHECK_EQ(lines[2], std::string(""));                // a run of blank lines is one
    CHECK_EQ(lines[3], std::string("Third line with gaps"));
}

TEST(lyrics_read_lines_keep_the_times_and_line_up_with_the_plain_lines) {
    const std::string lrc =
        "[ar:Some Artist]\n[offset:+250]\n"
        "[00:12.30]First line\n"
        "[00:15.00]<00:15.00>Second <00:15.50>line\n"
        "\n\n"
        "[01:02.5]Third line\n"
        "plain line without a time\n";
    double delay = 99.0;
    const std::vector<LyricLine> l = lyrics_read_lines(lrc, &delay);
    const std::vector<std::string> plain = lyrics_plain_lines(lrc);
    CHECK_EQ(l.size(), plain.size());                    // one entry per plain line, the same text
    for (size_t i = 0; i < l.size() && i < plain.size(); ++i) CHECK_EQ(l[i].full_text, plain[i]);
    CHECK_EQ(l.size(), static_cast<size_t>(5));
    CHECK(std::abs(l[0].start_time - 12.3) < 0.001);
    CHECK(std::abs(l[1].start_time - 15.0) < 0.001);
    CHECK_EQ(l[1].words.size(), static_cast<size_t>(2));   // the word times of the enhanced line
    CHECK(std::abs(l[1].words[1].first - 15.5) < 0.001);
    CHECK(l[2].full_text.empty());
    CHECK(std::abs(l[3].start_time - 62.5) < 0.001);
    CHECK(l[4].start_time < 0.0);                        // no time stamp
    CHECK(std::abs(delay - (-0.25)) < 0.001);            // [offset:+250] = the lyrics come 250 ms earlier
}

TEST(lyrics_plain_lines_handle_bom_and_edges) {
    CHECK(lyrics_plain_lines("").empty());
    CHECK(lyrics_plain_lines("[ar:x]\n[ti:y]\n").empty());
    const std::vector<std::string> v = lyrics_plain_lines("\xEF\xBB\xBF[00:01.00]After the BOM\n\n\n");
    CHECK_EQ(v.size(), static_cast<size_t>(1));          // BOM removed, trailing blanks dropped
    CHECK_EQ(v[0], std::string("After the BOM"));
    const std::vector<std::string> w = lyrics_plain_lines("\n\n[00:01.00]late start");
    CHECK_EQ(w.size(), static_cast<size_t>(1));          // leading blanks dropped
}

// ---- saved playlists --------------------------------------------------------------------------------------------------

TEST(playlist_names_become_safe_file_names) {
    CHECK_EQ(PlaylistManager::sanitize_name("Road trip"), std::string("Road trip"));
    CHECK_EQ(PlaylistManager::sanitize_name("a/b\\c:d*e?f\"g<h>i|j"), std::string("a_b_c_d_e_f_g_h_i_j"));
    CHECK_EQ(PlaylistManager::sanitize_name("  name. . "), std::string("name"));    // Windows drops trailing dots and spaces
    CHECK_EQ(PlaylistManager::sanitize_name(""), std::string("playlist"));
    CHECK_EQ(PlaylistManager::sanitize_name("..."), std::string("playlist"));
}

TEST(playlist_save_load_list_and_remove) {
    mt::TempHome home;
    const fs::path dir = home.path() / "playlists";
    CHECK(PlaylistManager::list(dir).empty());              // no folder yet: empty, not an error
    CHECK(!PlaylistManager::load(dir, "nothing").has_value());

    const fs::path song = home.path() / "music" / "Artist One" / "Song A.mp3";
    fs::create_directories(song.parent_path());
    { std::ofstream f(song, std::ios::binary); f << "x"; }

    Playlist pl;
    pl.name = "Zebra mix";
    PlaylistTrack ok;
    ok.path = song;
    PlaylistTrack gone;
    gone.path = home.path() / "music" / "Artist Two" / "Vanished.mp3";
    pl.tracks = {ok, gone};
    std::string err;
    CHECK(PlaylistManager::save(dir, pl, &err));

    Playlist second;
    second.name = "alpha";
    CHECK(PlaylistManager::save(dir, second, &err));

    const auto loaded = PlaylistManager::load(dir, "Zebra mix");
    CHECK(loaded.has_value());
    CHECK_EQ(loaded->tracks.size(), static_cast<size_t>(2));
    CHECK_EQ(loaded->tracks[0].title, std::string("Song A"));
    CHECK_EQ(loaded->tracks[0].artist, std::string("Artist One"));
    CHECK(!loaded->tracks[0].missing);
    CHECK(loaded->tracks[1].missing);                       // kept in the list, but marked
    CHECK_EQ(loaded->tracks[1].title, std::string("Vanished"));

    const auto all = PlaylistManager::list(dir);
    CHECK_EQ(all.size(), static_cast<size_t>(2));
    CHECK_EQ(all[0].name, std::string("alpha"));            // alphabetical, case-insensitive
    CHECK_EQ(all[1].name, std::string("Zebra mix"));
    CHECK_EQ(all[1].track_count, static_cast<size_t>(2));
    CHECK_EQ(all[0].track_count, static_cast<size_t>(0));

    CHECK(PlaylistManager::remove(dir, "alpha", &err));
    CHECK(!PlaylistManager::remove(dir, "alpha", &err));    // already gone
    CHECK(!err.empty());
    CHECK_EQ(PlaylistManager::list(dir).size(), static_cast<size_t>(1));
}

TEST(saving_a_playlist_replaces_the_file_without_leaving_a_temp_file) {
    mt::TempHome home;
    const fs::path dir = home.path() / "playlists";
    Playlist pl;
    pl.name = "Atomic";
    PlaylistTrack t;
    t.path = home.path() / "a.mp3";
    pl.tracks = {t};
    std::string err;
    CHECK(PlaylistManager::save(dir, pl, &err));
    pl.tracks.push_back(t);
    CHECK(PlaylistManager::save(dir, pl, &err));            // over the existing one
    auto back = PlaylistManager::load(dir, "Atomic");
    CHECK(back.has_value());
    if (back) CHECK_EQ(back->tracks.size(), static_cast<size_t>(2));
    int files = 0;
    for (const auto& e : fs::directory_iterator(dir)) { ++files; CHECK(e.path().extension() == ".txt"); }   // no "Atomic.txt.tmp" left
    CHECK_EQ(files, 1);
}

TEST(playlist_file_ignores_comments_and_blank_lines) {
    mt::TempHome home;
    const fs::path dir = home.path() / "playlists";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "hand.txt", std::ios::binary);
        f << "# mousiki playlist: hand\r\n\r\n" << (home.path() / "a" / "One.mp3").string() << "\r\n"
          << "# a comment\n" << (home.path() / "b" / "Two.mp3").string() << "\n";
    }
    const auto pl = PlaylistManager::load(dir, "hand");
    CHECK(pl.has_value());
    CHECK_EQ(pl->tracks.size(), static_cast<size_t>(2));    // comment and blank lines skipped, CRLF handled
    CHECK_EQ(pl->tracks[0].title, std::string("One"));
    CHECK_EQ(pl->tracks[1].title, std::string("Two"));
}

TEST(playlist_names_with_reserved_characters_round_trip) {
    mt::TempHome home;
    const fs::path dir = home.path() / "playlists";
    Playlist pl;
    pl.name = "who/what?";
    CHECK(PlaylistManager::save(dir, pl));
    CHECK(fs::exists(dir / "who_what_.txt"));
    CHECK(PlaylistManager::load(dir, "who/what?").has_value());   // the same name finds the same file
}

TEST(imported_playlists_live_in_their_own_subfolder) {
    mt::TempHome home;
    const fs::path dir = home.path() / "playlists";
    Playlist mine; mine.name = "Mine";
    Playlist given; given.name = "Given"; given.imported = true;
    CHECK(PlaylistManager::save(dir, mine));
    CHECK(PlaylistManager::save(dir, given));
    CHECK(fs::exists(dir / "Mine.txt"));
    CHECK(fs::exists(dir / PlaylistManager::kImportedDir / "Given.txt"));
    CHECK(!fs::exists(dir / "Given.txt"));
    const auto all = PlaylistManager::list(dir);
    CHECK_EQ(all.size(), static_cast<size_t>(2));
    for (const auto& s : all) CHECK_EQ(s.imported, s.name == "Given");
    auto back = PlaylistManager::load(dir, "Given");
    CHECK(back.has_value());
    CHECK(back->imported);
    CHECK(!PlaylistManager::load(dir, "Mine")->imported);
    CHECK(PlaylistManager::remove(dir, "Given"));
    CHECK(!fs::exists(dir / PlaylistManager::kImportedDir / "Given.txt"));
    CHECK_EQ(PlaylistManager::list(dir).size(), static_cast<size_t>(1));
}

// ---- listening history --------------------------------------------------------------------------------------------------

namespace {
HistoryPlay play(const std::string& id, const std::string& title, double len, long long at) {
    HistoryPlay p;
    p.id = id;
    p.title = title;
    p.artist = "Artist";
    p.len_sec = len;
    p.started_at = at;
    return p;
}
}

TEST(history_format_helpers) {
    CHECK_EQ(format_mmss(0), std::string("0:00"));
    CHECK_EQ(format_mmss(222), std::string("3:42"));
    CHECK_EQ(format_mmss(3723), std::string("1:02:03"));
    CHECK_EQ(format_mmss(-5), std::string("0:00"));
    CHECK_EQ(format_len(42), std::string("42s"));
    CHECK_EQ(format_len(2520), std::string("42m"));
    CHECK_EQ(format_len(4320), std::string("1h 12m"));
}

TEST(history_track_ids_unify_local_and_stream_plays) {
    CHECK_EQ(history_track_id(true, "/music/a.mp3", "ignored"), std::string("/music/a.mp3"));
    CHECK_EQ(history_track_id(false, "", "abc123"), std::string("yt:abc123"));
    CHECK_EQ(history_track_id(false, "", ""), std::string());      // nothing to aggregate by
}

TEST(history_live_play_counts_only_what_was_heard) {
    HistoryStore h;
    CHECK(!h.live());
    h.add_listened(5);                                            // no live play: ignored
    h.begin_play(play("a", "Song", 100, 1000));
    CHECK(h.live());
    h.add_listened(30);
    h.add_listened(-4);                                           // bad frame deltas are ignored
    h.add_listened(0);
    CHECK_NEAR(h.live_listened(), 30, 1e-9);
    h.add_listened(500);                                          // never more than the track length
    CHECK_NEAR(h.live_listened(), 100, 1e-9);
    h.finish_live(true);
    CHECK(!h.live());
    CHECK(h.plays()[0].finished);
    h.add_listened(10);                                           // closed: no change any more
    CHECK_NEAR(h.plays()[0].listened_sec, 100, 1e-9);
}

TEST(history_is_newest_first_and_top_counts_replays) {
    HistoryStore h;
    for (int i = 0; i < 3; ++i) {
        h.begin_play(play("a", "Often", 60, 1000 + i * 100));
        h.add_listened(60);
        h.finish_live(true);
    }
    h.begin_play(play("b", "Once", 60, 2000));
    h.add_listened(20);
    h.finish_live(false);
    CHECK_EQ(h.plays().size(), static_cast<size_t>(4));
    CHECK_EQ(h.plays()[0].id, std::string("b"));                  // newest first

    const auto most = history_top(h.plays(), true);
    CHECK_EQ(most.size(), static_cast<size_t>(2));
    CHECK_EQ(most[0].id, std::string("a"));
    CHECK_EQ(most[0].plays, 3);
    CHECK_NEAR(most[0].listened_sec, 180, 1e-9);
    const auto least = history_top(h.plays(), false);
    CHECK_EQ(least[0].id, std::string("b"));                      // least played on top

    const HistoryStats st = history_stats(h.plays());
    CHECK_EQ(st.plays, 4);
    CHECK_EQ(st.finished, 3);
    CHECK_EQ(st.skipped, 1);
    CHECK_EQ(st.replays, 2);                                      // plays beyond the first of a title
    CHECK_NEAR(st.completion, 0.75, 1e-9);
    CHECK_NEAR(st.total_sec, 200, 1e-9);
}

TEST(history_plays_without_an_id_are_not_aggregated) {
    HistoryStore h;
    h.begin_play(play("", "Nameless", 60, 1000));
    h.finish_live(true);
    CHECK(history_top(h.plays(), true).empty());
}

TEST(history_saves_and_loads_and_survives_a_corrupt_file) {
    mt::TempHome home;
    const std::string dir = (home.path() / "hist").string();
    {
        HistoryStore h;
        h.set_dir(dir);
        CHECK(!h.file_exists());
        h.begin_play(play("a", "Kept", 90, 5000));
        h.add_listened(45);
        h.finish_live(false);
        h.save();
        CHECK(h.file_exists());
    }
    {
        HistoryStore h;
        h.set_dir(dir);
        h.load();
        CHECK_EQ(h.plays().size(), static_cast<size_t>(1));
        CHECK_EQ(h.plays()[0].title, std::string("Kept"));
        CHECK_NEAR(h.plays()[0].listened_sec, 45, 1e-9);
        CHECK(!h.plays()[0].finished);
        CHECK(!h.live());                                          // a loaded record is never "live"
    }
    {
        std::ofstream f(fs::path(dir) / "history.json", std::ios::binary | std::ios::trunc);
        f << "{\"plays\":[{\"id\":";                               // half-written
    }
    HistoryStore broken;
    broken.set_dir(dir);
    broken.load();                                                  // must not throw
    CHECK(broken.plays().empty());
}

TEST(history_old_plays_fold_into_the_archive_without_losing_counts) {
    HistoryStore h;
    const int total = 1005;                                        // more than the window of individual records
    for (int i = 0; i < total; ++i) {
        h.begin_play(play("t" + std::to_string(i % 5), "Title " + std::to_string(i % 5), 60, 100000 + i * 100));
        h.add_listened(60);
        h.finish_live(true);
    }
    CHECK(h.plays().size() < static_cast<size_t>(total));         // the window is capped ...
    CHECK(!h.archive().empty());                                   // ... and the rest is in the archive
    const auto top = history_top(h.plays(), true, &h.archive());
    int sum = 0;
    for (const auto& r : top) sum += r.plays;
    CHECK_EQ(sum, total);                                          // nothing counted is lost
    const HistoryStats st = history_stats(h.plays(), &h.archive());
    CHECK_EQ(st.plays, total);
    CHECK_NEAR(st.total_sec, 60.0 * total, 1e-6);
}

TEST(history_time_of_day_buckets) {
    CHECK_EQ(history_tod_bucket(0), -1);                           // unknown start time
    CHECK_EQ(history_tod_bucket(-5), -1);
}

TEST(playlist_paths_follow_renamed_files) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_pl_rename";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Playlist pl;
    pl.name = "mix";
    for (const char* f : {"/music/a.mp3", "/music/b.mp3"}) {
        PlaylistTrack t;
        t.path = f;
        pl.tracks.push_back(t);
    }
    std::string err;
    CHECK(PlaylistManager::save(dir, pl, &err));
    CHECK_EQ(PlaylistManager::rename_paths(dir, {{"/music/b.mp3", "/music/Better.mp3"}}), 1);
    auto back = PlaylistManager::load(dir, "mix");
    CHECK(back.has_value());
    CHECK_EQ(back->tracks.size(), static_cast<size_t>(2));
    CHECK_EQ(path_utf8(back->tracks[0].path), std::string("/music/a.mp3"));
    CHECK_EQ(path_utf8(back->tracks[1].path), std::string("/music/Better.mp3"));
    CHECK_EQ(PlaylistManager::rename_paths(dir, {{"/nowhere", "/x"}}), 0);   // no match: nothing rewritten
    fs::remove_all(dir);
}
