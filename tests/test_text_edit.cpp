// Single-line text editing and the typo-tolerant search score (src/text_edit.h), shared by the player and the radio.
#include "test_framework.h"
#include "text_edit.h"

using namespace muisc;

namespace {
struct Field {
    std::string s;
    size_t caret = 0, anchor = 0;
    bool key(int k, size_t limit = 100) { return edit_text_key(s, caret, anchor, k, limit, nullptr); }
    void type(const std::string& text, size_t limit = 100) { for (unsigned char c : text) key(c, limit); }
};
}

TEST(text_edit_typing_and_backspace_utf8) {
    Field f;
    f.type("ab");
    CHECK_EQ(f.s, std::string("ab"));
    f.type("\xC3\xA4");                    // a-umlaut: two bytes, typed one byte at a time
    CHECK_EQ(f.s, std::string("ab\xC3\xA4"));
    CHECK(f.key(127));                      // one backspace removes the whole character, not half of it
    CHECK_EQ(f.s, std::string("ab"));
    CHECK_EQ(f.caret, static_cast<size_t>(2));
}

TEST(text_edit_home_end_insert_in_the_middle) {
    Field f;
    f.type("world");
    f.key(kKeyHome);
    CHECK_EQ(f.caret, static_cast<size_t>(0));
    f.type("hello ");
    CHECK_EQ(f.s, std::string("hello world"));
    f.key(kKeyEnd);
    CHECK_EQ(f.caret, f.s.size());
}

TEST(text_edit_selection_and_delete) {
    Field f;
    f.type("abcdef");
    f.key(kKeyShiftLeft);
    f.key(kKeyShiftLeft);                  // "ef" marked
    CHECK(f.caret != f.anchor);
    CHECK(f.key(kKeyDelete));              // deleting a selection removes exactly it
    CHECK_EQ(f.s, std::string("abcd"));
    CHECK_EQ(f.caret, f.anchor);
}

TEST(text_edit_limit_is_enforced) {
    Field f;
    f.type("abcdefgh", 5);
    CHECK_EQ(f.s, std::string("abcde"));
}

TEST(text_edit_paint_fits_the_width) {
    for (int w : {1, 3, 8, 20}) {
        const EditPaint p = paint_edit_field("the quick brown fox jumps", 25, 25, w, "", true);
        CHECK(p.cols <= w);                // never wider than the field, whatever the text
        CHECK(p.caret >= 0 && p.caret <= w - 1);
    }
    const EditPaint sel = paint_edit_field("hello", 5, 0, 20, "\x1b[0m", false);
    CHECK(sel.s.find("\x1b[7m") != std::string::npos);   // the marked text is in reverse video
}

TEST(text_edit_block_caret_on_a_wide_character_keeps_the_width) {
    // The caret on a double-width character (Japanese ...) must not make the painted row narrower than it is counted.
    const std::string s = "ab \xE3\x83\x91\xE3\x83\xBC\xE3\x83\xA9";   // "ab パーラ"
    for (size_t caret : {size_t(0), size_t(3), size_t(6), size_t(9)}) {
        const EditPaint p = paint_edit_field(s, caret, caret, 20, "", true);
        CHECK_EQ(display_width(p.s), p.cols);
    }
}

TEST(text_edit_combining_mark_stays_with_its_character) {
    // は + U+3099 (a decomposed ば): one character for the caret, the block cursor and backspace.
    const std::string s = "a\xE3\x81\xAF\xE3\x82\x99" "b";
    CHECK_EQ(utf8_cuts(s).size(), static_cast<size_t>(4));   // 0, after a, after the pair, after b
    for (size_t caret : {size_t(1), size_t(7)}) {
        const EditPaint p = paint_edit_field(s, caret, caret, 20, "", true);
        CHECK_EQ(display_width(p.s), p.cols);
    }
    Field f;
    f.s = s;
    f.caret = f.anchor = 8;
    f.key(127);                                              // backspace removes the 'b'
    f.key(127);                                              // ... then the whole pair, not just the mark
    CHECK_EQ(f.s, std::string("a"));
}

TEST(fuzzy_exact_beats_typo) {
    const double exact = fuzzy_score("another love", "Tom Odell - Another Love");
    const double typo = fuzzy_score("anogher lobe", "Tom Odell - Another Love");
    CHECK(exact >= 100.0);
    CHECK(typo > 0.0 && typo < 100.0);
    CHECK(exact > typo);
}

TEST(fuzzy_no_match_and_empty_query) {
    CHECK(fuzzy_score("zzzzqq", "Another Love") < 0.0);
    CHECK_EQ(fuzzy_score("", "anything"), 0.0);
}

TEST(fuzzy_separators_fold_to_spaces) {
    CHECK(fuzzy_score("x files", "X-Files") >= 900.0);          // "X-Files" is found by "x files"
    CHECK(fuzzy_score("a b", "a_b") >= 900.0);
}

TEST(fuzzy_earlier_position_ranks_higher) {
    CHECK(fuzzy_score("love", "love me do") > fuzzy_score("love", "all you need is love"));
}

TEST(fuzzy_run_together_words_find_the_file) {
    CHECK(fuzzy_score("testcover", "test_song-cover3") > 0.0);        // test + cover, "song" skipped
    CHECK(fuzzy_score("testcov", "test_song-cover3") > 0.0);
    CHECK(fuzzy_score("test cov", "test_song-cover3") > 0.0);         // a word typed only as far as it goes
    CHECK(fuzzy_score("testcover", "test cover") > fuzzy_score("testcover", "test_song-cover3"));   // fewer skipped words rank higher
    CHECK(fuzzy_score("testcover", "Another Love") < 0.0);
    CHECK(fuzzy_score("bohrhap", "Bohemian Rhapsody") > 0.0);
}

TEST(fuzzy_accents_and_swaps) {
    CHECK(fuzzy_score("arzte", "Die Ärzte") >= 900.0);                 // an umlaut is its plain letter
    CHECK(fuzzy_score("beyonce", "Beyoncé - Halo") >= 900.0);
    CHECK(fuzzy_score("strasse", "Straße") >= 900.0);                  // ß = ss
    CHECK(fuzzy_score("teh beatles", "The Beatles") > 0.0);            // swapped letters
}

#include "translit.h"
TEST(translit_japanese_styles) {
    translit_set_style(false, false);
    CHECK_EQ(transliterate("こえ を きかせて"), std::string("koe o kikasete"));
    CHECK_EQ(transliterate("コーヒー"), std::string("koohii"));
    translit_set_style(true, true);
    CHECK_EQ(transliterate("こえ を きかせて"), std::string("koe wo kikasete"));
    CHECK_EQ(transliterate("コーヒー"), std::string("k\xC5\x8Dh\xC4\xAB"));
    CHECK_EQ(transliterate("きえぬよう"), std::string("kienuy\xC5\x8D"));
    CHECK_EQ(transliterate("おおきい"), std::string("\xC5\x8Dkii"));
    translit_set_style(false, false);
}
TEST(translit_sokuon_and_particle_wa) {
    translit_set_style(false, false);
    CHECK_EQ(transliterate("いって"), std::string("itte"));
    CHECK_EQ(transliterate("いつて"), std::string("itte"));            // a full-size tsu where a small one belongs (lyrics sites)
    CHECK_EQ(transliterate("わかつて"), std::string("wakatte"));
    CHECK_EQ(transliterate("まつたく"), std::string("mattaku"));
    CHECK_EQ(transliterate("かつて"), std::string("katsute"));         // "formerly" stays
    CHECK_EQ(transliterate("つて"), std::string("tsute"));
    CHECK_EQ(transliterate("は いけない"), std::string("wa ikenai"));   // the particle on its own
    CHECK_EQ(transliterate("ここでは いけない"), std::string("kokodewa ikenai"));
    CHECK_EQ(transliterate("ここではない"), std::string("kokodewanai"));
    CHECK_EQ(transliterate("にはとり"), std::string("nihatori"));      // inside a word: ha
    CHECK_EQ(transliterate("はは"), std::string("haha"));
    CHECK_EQ(transliterate("へ いく"), std::string("e iku"));
    CHECK_EQ(transliterate("いて も"), std::string("ite mo"));
}
TEST(translit_hanzi_default_modes) {
    // 東京 is the same in Japanese and Chinese: LatinHanziDefault decides
    translit_set_default_hanzi(0);
    CHECK_EQ(transliterate("\xE6\x9D\xB1\xE4\xBA\xAC"), std::string("\xE6\x9D\xB1\xE4\xBA\xAC"));   // leave
    translit_set_default_hanzi(1);
    CHECK(transliterate("\xE6\x9D\xB1\xE4\xBA\xAC") != std::string("\xE6\x9D\xB1\xE4\xBA\xAC"));  // pinyin
    translit_set_default_hanzi(2);
    CHECK_EQ(transliterate("\xE6\x9D\xB1\xE4\xBA\xAC"), std::string("\xE6\x9D\xB1\xE4\xBA\xAC"));   // Japanese: stays without a kanji reader
    CHECK_EQ(transliterate("\xE6\x9D\xB1\xE4\xBA\xAC\xE3\x81\xAE\xE7\xA9\xBA"), std::string("\xE6\x9D\xB1\xE4\xBA\xAC no \xE7\xA9\xBA"));   // kana in the text: Japanese in every mode
    translit_set_default_hanzi(0);
}
TEST(translit_japanese_kana_to_hepburn) {
    CHECK_EQ(transliterate("さくら"), std::string("sakura"));
    CHECK_EQ(transliterate("ありがとう"), std::string("arigatou"));
    CHECK_EQ(transliterate("きょう"), std::string("kyou"));
    CHECK_EQ(transliterate("シャンプー"), std::string("shanpuu"));
    CHECK_EQ(transliterate("ラーメン"), std::string("raamen"));
    CHECK_EQ(transliterate("がっこう"), std::string("gakkou"));
    CHECK_EQ(transliterate("まっちゃ"), std::string("matcha"));
    CHECK_EQ(transliterate("しんあい"), std::string("shin'ai"));
    CHECK_EQ(transliterate("ファイト"), std::string("faito"));
    CHECK_EQ(transliterate("ティー"), std::string("tii"));
    CHECK_EQ(transliterate("Hello ひかり 2"), std::string("Hello hikari 2"));
}

TEST(translit_hangul_to_revised_romanization) {
    CHECK_EQ(transliterate("안녕하세요"), std::string("annyeonghaseyo"));
    CHECK_EQ(transliterate("한국어"), std::string("hangugeo"));
    CHECK_EQ(transliterate("사랑해"), std::string("saranghae"));
    CHECK_EQ(transliterate("김치"), std::string("gimchi"));
}

TEST(translit_chinese_to_pinyin_and_kanji_stay) {
    CHECK_EQ(transliterate("谢谢你"), std::string("xi\xC3\xA8 xi\xC3\xA8 n\xC7\x90"));   // xiè xiè nǐ (simplified-only characters: Chinese)
    CHECK_EQ(transliterate("周杰伦 Jay"), std::string("zh\xC5\x8Du ji\xC3\xA9 l\xC3\xBAn Jay"));   // (伦 is simplified-only)
    CHECK_EQ(transliterate("你好"), std::string("你好"));                             // the same in every writing: left alone without a hint
    CHECK_EQ(transliterate("愛してる"), std::string("\xE6\x84\x9B shiteru"));      // Japanese text: the kanji stays
    CHECK_EQ(transliterate("plain ascii"), std::string("plain ascii"));
    CHECK_EQ(with_latin_spelling("さくら"), std::string("さくら sakura"));
    CHECK_EQ(with_latin_spelling("abc"), std::string("abc"));
    CHECK(fuzzy_score("sakura", with_latin_spelling("さくら")) >= 900.0);
    CHECK(fuzzy_score("zhou jielun", with_latin_spelling("周杰伦")) > 0.0);   // (Chinese: 伦 only exists in the simplified writing)
}

TEST(translit_script_is_told_from_the_characters_and_the_context) {
    CHECK(script_of("さくら") == Script::Japanese);
    CHECK(script_of("桜") == Script::Japanese);              // a character only Japanese uses
    CHECK(script_of("爱 们 这") == Script::Chinese);          // simplified-only characters
    CHECK(script_of("東京") == Script::Unknown);             // the same in all writings
    CHECK_EQ(transliterate("東京"), std::string("東京"));     // unknown: left alone
    CHECK_EQ(transliterate("们"), std::string("men"));       // (pinyin without a tone: neutral tone)
    // a track decides for its other tags: kana in the title makes the artist Japanese; the artist is remembered for other tracks
    translit_learn("ありがとう", "周杰倫", "");
    CHECK_EQ(transliterate("周杰倫"), std::string("周杰倫"));
    translit_learn("", "李榮浩", "我们");                       // simplified-only 们 in the album: Chinese
    CHECK_EQ(transliterate("李榮浩"), std::string("l\xC7\x90 r\xC3\xB3ng h\xC3\xA0o"));
    // the reading dictionary and names written both ways
    CHECK_EQ(transliterate("米津玄師"), std::string("Yonezu Kenshi"));
    CHECK_EQ(transliterate("夜に駆ける - 米津玄師"), std::string("\xE5\xA4\x9C ni \xE9\xA7\x86 keru - Yonezu Kenshi"));
    translit_learn("", "Fujii Kaze (藤井 風)", "");
    CHECK_EQ(transliterate("Fujii Kaze (藤井 風)"), std::string("Fujii Kaze"));
    CHECK_EQ(transliterate("藤井 風"), std::string("Fujii Kaze"));
}

#include "name_lookup.h"
TEST(name_lookup_answer_lines_are_read) {
    const NameLookupResult ok = parse_name_lookup_line("{\"ok\": true, \"latin\": \"Jay Chou\"}");
    CHECK(ok.ok);
    CHECK_EQ(ok.latin, std::string("Jay Chou"));
    CHECK(!parse_name_lookup_line("{\"ok\": false, \"notfound\": true}").ok);
    CHECK(parse_name_lookup_line("{\"ok\": false, \"notfound\": true}").notfound);
    const NameLookupResult bad = parse_name_lookup_line("{\"ok\": false, \"error\": \"not reachable\"}");
    CHECK(!bad.ok && !bad.notfound && !bad.error.empty());
}

TEST(translit_lookup_queue_and_answers) {
    translit_learn("", "\xE9\x99\x88\xE5\xA5\x95\xE8\xBF\x85", "\xE4\xBB\xAC");   // 陈奕迅, simplified-only 们 in the album: Chinese
    const auto pending = translit_take_pending(5);
    bool found = false;
    for (const auto& n : pending) found = found || n == "\xE9\x99\x88\xE5\xA5\x95\xE8\xBF\x85";
    CHECK(found);
    translit_lookup_found("\xE9\x99\x88\xE5\xA5\x95\xE8\xBF\x85", "Eason Chan");
    CHECK_EQ(transliterate("\xE9\x99\x88\xE5\xA5\x95\xE8\xBF\x85"), std::string("Eason Chan"));
}

#include "settings.h"
TEST(translit_with_the_font_map_does_not_recurse_on_kept_kanji) {
    // kanji stay in Japanese text: the result still holds CJK bytes, and must not be converted again (that recursed forever)
    set_translit_enabled(true);
    const std::unordered_map<char, std::pair<std::string, std::string>> none;
    CHECK_EQ(apply_font_map("\xE6\x84\x9B\xE3\x81\x97\xE3\x81\xA6\xE3\x82\x8B", none), std::string("\xE6\x84\x9B shiteru"));
    set_translit_enabled(false);
}

TEST(translit_a_folder_shared_by_many_artists_names_none_of_them) {
    for (int a = 0; a < 2; ++a) {
        const std::string artist = a == 0 ? "\xE5\xAE\x87\xE5\xA4\x9A\xE7\x94\xB0" : "\xE5\xB9\xB3\xE4\xBA\x95";
        for (int t = 0; t < 4; ++t)
            translit_learn("x", artist, "", "/m/Youtube/f" + std::to_string(a * 10 + t) + ".mp3");
    }
    CHECK_EQ(transliterate("\xE5\xB9\xB3\xE4\xBA\x95"), std::string("\xE5\xB9\xB3\xE4\xBA\x95"));
}

TEST(translit_latin_name_beside_the_kana_name_shows_the_latin_one) {
    CHECK_EQ(transliterate("Kaneko Ayano \xE3\x82\xAB\xE3\x83\x8D\xE3\x82\xB3\xE3\x82\xA2\xE3\x83\xA4\xE3\x83\x8E"), std::string("Kaneko Ayano"));
    CHECK_EQ(transliterate("PARANMAUM \xE3\x83\x91\xE3\x83\xBC\xE3\x83\xA9\xE3\x83\xB3\xE3\x83\x9E\xE3\x82\xA6\xE3\x83\xA0"), std::string("PARANMAUM"));
    CHECK_EQ(transliterate("HAKU \xE3\x83\x8F\xE3\x82\xAF\xE3\x80\x82 (Jap.)"), std::string("HAKU (Jap.)"));
    // a translation that only sits beside a title is not a spelling of it
    CHECK_EQ(transliterate("Mono No Aware \xE3\x81\x8B\xE3\x82\x80\xE3\x81\x8B\xE3\x82\x82\xE3\x81\x97\xE3\x81\x8B\xE3\x82\x82! I might stumble"), std::string("Mono No Aware kamukamoshikamo! I might stumble"));
    // an artist tag with kanji: learned from the tag
    translit_learn("x", "Anri \xE6\x9D\x8F\xE9\x87\x8C", "");
    CHECK_EQ(transliterate("Anri \xE6\x9D\x8F\xE9\x87\x8C"), std::string("Anri"));
    CHECK_EQ(transliterate("\xE6\x9D\x8F\xE9\x87\x8C"), std::string("Anri"));
}

TEST(translit_decomposed_kana_marks_and_particles) {
    CHECK_EQ(transliterate("\xE3\x83\x88\xE3\x82\x99\xE3\x83\xAD\xE3\x83\x95\xE3\x82\x9A"), std::string("doropu"));   // ドロプ written as ト+゙ ロ フ+゚
    CHECK_EQ(transliterate("\xE6\x97\xA5\xE6\x9C\xAC\xE3\x81\xAE\xE3\x82\xB7\xE3\x83\x86\xE3\x82\xA3"), std::string("\xE6\x97\xA5\xE6\x9C\xAC no shiti"));   // 日本のシティ
}

TEST(translit_kanji_waiting_counts_the_lines_the_reader_has_not_answered) {
    const std::string a = "\xE6\x98\x9F\xE3\x81\x8C\xE9\x99\x8D\xE3\x82\x8A\xE3\x81\xA6\xE3\x81\x8F\xE3\x82\x8B \xE5\xA4\x9C";   // 星が降りてくる 夜
    const std::vector<std::string> lines = {a, "", "plain latin"};
    CHECK_EQ(translit_kanji_waiting(lines), static_cast<size_t>(1));      // asked now, not answered
    CHECK_EQ(translit_kanji_waiting(lines), static_cast<size_t>(1));      // (and not asked twice)
    translit_take_kanji_pending(50);                                     // taken by the reader: still waiting
    CHECK_EQ(translit_kanji_waiting(lines), static_cast<size_t>(1));
    translit_kanji_found(a, "hoshi ga orite kuru yoru");
    CHECK_EQ(translit_kanji_waiting(lines), static_cast<size_t>(0));
}

TEST(translit_lyrics_line_with_a_latin_remark_is_not_a_name_pair) {
    // パラッパ x4 (wow-oh): a line of a song, not "Name (Latin name)": the whole line is spelled
    const std::string r = transliterate("\xE3\x83\x91\xE3\x83\xA9\xE3\x83\x83\xE3\x83\x91 \xE3\x83\x91\xE3\x83\xA9\xE3\x83\x83\xE3\x83\x91 \xE3\x83\x91\xE3\x83\xA9\xE3\x83\x83\xE3\x83\x91 (wow-oh)");
    CHECK(r.find("parappa") != std::string::npos);
    CHECK(r.find("wow-oh") != std::string::npos);
}

TEST(translit_reader_long_vowels_follow_the_macron_setting) {
    const std::string t = "\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAF\xE3\x82\xB3\xE3\x83\xBC\xE3\x83\x92\xE3\x83\xBC";   // 今日はコーヒー
    transliterate(t);
    translit_kanji_found(t, "ky\xC3\xB4 wa k\xC5\x8Dh\xC4\xAB");   // what the reader writes: ô = o+u, ō ī = doubled vowels
    translit_set_style(false, false);
    CHECK_EQ(transliterate(t), std::string("kyou wa koohii"));
    translit_set_style(true, false);
    CHECK_EQ(transliterate(t), std::string("ky\xC5\x8D wa k\xC5\x8Dh\xC4\xAB"));
    translit_set_style(false, false);
}

TEST(translit_kanji_in_japanese_text_is_offered_to_a_reader_and_its_answer_is_shown) {
    const std::string t = "\xE5\xA4\x9C\xE3\x81\xAB\xE9\xA7\x86\xE3\x81\x91\xE3\x82\x8B \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88";   // 夜に駆ける テスト
    transliterate(t);
    const auto pending = translit_take_kanji_pending(50);
    bool found = false;
    for (const auto& n : pending) found = found || n == t;
    CHECK(found);
    translit_kanji_found(t, "yoru ni kakeru tesuto");
    CHECK_EQ(transliterate(t), std::string("yoru ni kakeru tesuto"));
    transliterate("\xE6\x9D\xB1\xE4\xBA\xAC");   // 東京 alone: could be Chinese, never offered
    for (const auto& n : translit_take_kanji_pending(50)) CHECK(n != "\xE6\x9D\xB1\xE4\xBA\xAC");
}

#include "lyrics_translate.h"
TEST(lyrics_export_text_layers_original_translit_translation) {
    const std::vector<std::string> lines = {"さくら", "", "Hello", "hello"};
    const std::vector<std::string> tr = {"cherry blossom", "", "Hallo", "Hello"};
    translit_set_style(false, false);
    CHECK_EQ(muisc::lyrics_export_text(lines, nullptr, false), std::string("さくら\n\nHello\nhello\n"));    // the original only: as it is
    CHECK_EQ(muisc::lyrics_export_text(lines, &tr, false),
             std::string("さくら\ncherry blossom\n\nHello\nHallo\n\nhello\n"));                                  // (a translation equal to its line is left out)
    CHECK_EQ(muisc::lyrics_export_text(lines, &tr, true),
             std::string("さくら\nsakura\ncherry blossom\n\nHello\nHallo\n\nhello\n"));
    CHECK(muisc::lyrics_export_translit_differs(lines));
    CHECK(!muisc::lyrics_export_translit_differs({"Hello", "world"}));
    const std::vector<std::string> short_tr = {"x"};
    CHECK_EQ(muisc::lyrics_export_text({"a", "b"}, &short_tr, false), std::string("a\nb\n"));                 // a translation of another length is not used
}

#include "lyrics_fetcher.h"
#include <fstream>
TEST(lyrics_translations_are_kept_per_language_and_service) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_tr_per_service";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path track = dir / "Song.mp3";
    { std::ofstream(track) << "x"; }
    LyricsVersion v;
    v.kind = LyricsVersion::Kind::Embedded;
    const std::vector<std::string> plain = {"a", "b"};
    auto make = [&](const char* svc, const char* l1) {
        LyricsTranslation t;
        t.ok = true; t.from = "ja"; t.service = svc; t.lines = {l1, "second"};
        return t;
    };
    CHECK(save_lyrics_translation(track, v, "en", plain, make("google", "G1")));
    CHECK(save_lyrics_translation(track, v, "en", plain, make("mymemory", "M1")));
    auto list = list_lyrics_translations(track, v, plain);
    CHECK_EQ(list.size(), static_cast<size_t>(2));          // both are kept
    LyricsTranslation got;
    CHECK(load_lyrics_translation(track, v, "en", plain, got, "google"));
    CHECK_EQ(got.lines[0], std::string("G1"));
    CHECK(load_lyrics_translation(track, v, "en", plain, got, "mymemory"));
    CHECK_EQ(got.lines[0], std::string("M1"));
    CHECK(save_lyrics_translation(track, v, "en", plain, make("google", "G2")));   // the same service again: only its own is replaced
    list = list_lyrics_translations(track, v, plain);
    CHECK_EQ(list.size(), static_cast<size_t>(2));
    CHECK(load_lyrics_translation(track, v, "en", plain, got, "google"));
    CHECK_EQ(got.lines[0], std::string("G2"));
    CHECK(load_lyrics_translation(track, v, "en", plain, got, "mymemory"));
    CHECK_EQ(got.lines[0], std::string("M1"));
    // a file in the older naming ("<name>.de.txt", the service only inside) is still found, and a new save of that service replaces it
    {
        LyricsTranslation x = make("", "alt");
        CHECK(save_lyrics_translation(track, v, "de", plain, x));
        const fs::path legacy = lyrics_translation_file(track, v, "de");
        std::string body;
        { std::ifstream f(legacy, std::ios::binary); body.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
        body.replace(body.find("svc=-"), 5, "svc=google");
        { std::ofstream f(legacy, std::ios::binary | std::ios::trunc); f << body; }
        list = list_lyrics_translations(track, v, plain);
        CHECK_EQ(list.size(), static_cast<size_t>(3));
        CHECK(load_lyrics_translation(track, v, "de", plain, got, "google"));
        CHECK_EQ(got.lines[0], std::string("alt"));
        CHECK(!load_lyrics_translation(track, v, "de", plain, got, "mymemory"));
        CHECK(save_lyrics_translation(track, v, "de", plain, make("google", "neu")));
        CHECK(!fs::exists(legacy));
        CHECK(load_lyrics_translation(track, v, "de", plain, got, "google"));
        CHECK_EQ(got.lines[0], std::string("neu"));
        CHECK(delete_lyrics_translation(track, v, "de", "google"));
        CHECK(!load_lyrics_translation(track, v, "de", plain, got, "google"));
    }
    fs::remove_all(dir, ec);
}

TEST(a_lyrics_file_can_be_assigned_to_a_track_by_hand) {
    const fs::path dir = fs::temp_directory_path() / "mousiki_assign_lyrics";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "other", ec);
    const fs::path track = dir / "Song.mp3";
    { std::ofstream(track) << "x"; }
    const fs::path a = dir / "other" / "mine.lrc", b = dir / "other" / "second.lrc", e = dir / "other" / "empty.lrc";
    { std::ofstream(a, std::ios::binary) << "[00:01.00]hello\n[00:02.00]world\n"; }
    { std::ofstream(b, std::ios::binary) << "[00:01.00]other text\n"; }
    { std::ofstream(e, std::ios::binary) << "  \n"; }
    std::string err;
    CHECK(!assign_lyrics_file(track, dir / "nothing.lrc", &err));      // no such file
    CHECK(!err.empty());
    CHECK(!assign_lyrics_file(track, e, &err));                        // nothing in it
    CHECK(assign_lyrics_file(track, a, &err));
    auto v = list_lyrics_versions(track);
    CHECK_EQ(v.size(), static_cast<size_t>(1));                        // the copy in use
    CHECK(v[0].active);
    CHECK(assign_lyrics_file(track, a, &err));                         // the same text again: nothing changes
    CHECK_EQ(list_lyrics_versions(track).size(), static_cast<size_t>(1));
    CHECK(assign_lyrics_file(track, b, &err));                         // another one: the first is put aside, not lost
    v = list_lyrics_versions(track);
    CHECK_EQ(v.size(), static_cast<size_t>(2));
    CHECK(lyrics_added_manually(track));
    CHECK_EQ(lyrics_manual_origin_name(track), b.filename().string());   // the copy remembers the file it came from
    CHECK(unassign_lyrics_file(track, &err));                          // the first one comes back, the source file stays
    CHECK(fs::exists(b));
    CHECK(lyrics_added_manually(track));                               // it is the first manual one
    CHECK(unassign_lyrics_file(track, &err));                          // nothing older: the track has no saved lyrics now
    CHECK(!lyrics_added_manually(track));
    CHECK(!unassign_lyrics_file(track, &err));                         // nothing manual left
    fs::remove_all(dir, ec);
}

#include <sstream>
TEST(the_kanji_cache_starts_again_when_the_readers_rules_change) {
    const fs::path file = fs::temp_directory_path() / "mousiki_kanji_cache_rules.txt";
    std::error_code ec;
    fs::remove(file, ec);
    const std::string k1 = "\xE6\x9C\xAA\xE6\x9D\xA5\xE3\x81\xAE\xE7\xA9\xBA\xE4\xB8\x80";   // 未来の空一
    const std::string k2 = "\xE6\x9C\xAA\xE6\x9D\xA5\xE3\x81\xAE\xE7\xA9\xBA\xE4\xBA\x8C";   // 未来の空二
    const std::string k3 = "\xE6\x9C\xAA\xE6\x9D\xA5\xE3\x81\xAE\xE7\xA9\xBA\xE4\xB8\x89";   // 未来の空三
    auto content = [&] { std::ifstream in(file, std::ios::binary); std::ostringstream o; o << in.rdbuf(); return o.str(); };
    { std::ofstream(file, std::ios::binary) << k1 << "\tmirai no sora ichi\n"; }                 // an older file: no rules line
    translit_set_kanji_cache_file(file.string(), "rules-A");
    CHECK_EQ(content(), std::string("#rules rules-A\n"));                                         // emptied, now named
    CHECK(transliterate(k1) != "mirai no sora ichi");                                             // (its reading is gone)
    translit_kanji_found(k2, "mirai no sora ni");                                                 // a new reading is kept under the rules line
    CHECK_EQ(content(), "#rules rules-A\n" + k2 + "\tmirai no sora ni\n");
    translit_set_kanji_cache_file(file.string(), "rules-A");                                      // the same rules: kept
    CHECK_EQ(content(), "#rules rules-A\n" + k2 + "\tmirai no sora ni\n");
    translit_set_kanji_cache_file(file.string(), "rules-B");                                      // other rules: made again
    CHECK_EQ(content(), std::string("#rules rules-B\n"));
    fs::remove(file, ec);
    translit_kanji_found(k3, "mirai no sora san");                                                // a file that is not there yet starts with its rules line
    CHECK_EQ(content(), "#rules rules-B\n" + k3 + "\tmirai no sora san\n");
    translit_set_kanji_cache_file(std::string());
    fs::remove(file, ec);
}
