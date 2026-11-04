// Playlist import: finding the tracks of a foreign list in the own library (file name first, then fuzzy title + artist).
#include "test_framework.h"
#include "playlist_match.h"

using namespace muisc;

static MatchTrack lt(const std::string& path, const std::string& stem, const std::string& folder,
                     const std::string& tag_title = "", const std::string& tag_artist = "") {
    MatchTrack t;
    t.path = path; t.stem = stem; t.folder = folder; t.tag_title = tag_title; t.tag_artist = tag_artist;
    return t;
}

static MatchEntry en(const std::string& path, const std::string& artist = "", const std::string& title = "") {
    MatchEntry e;
    e.path = path; e.extinf_artist = artist; e.extinf_title = title;
    return e;
}

TEST(match_similarity_basics) {
    CHECK_NEAR(text_similarity("Hello World", "hello  world!"), 1.0, 1e-9);
    CHECK_NEAR(text_similarity("Beyoncé", "Beyonce"), 1.0, 1e-9);
    CHECK_NEAR(text_similarity("Song (Remastered 2011)", "Song"), 1.0, 1e-9);
    CHECK_NEAR(text_similarity("Kenshi Yonezu", "Yonezu Kenshi"), 1.0, 1e-9);   // order of the words
    CHECK(text_similarity("abcd", "abce") >= 0.75);
    CHECK(text_similarity("abcd", "wxyz") < 0.1);
    CHECK_NEAR(text_similarity("", "x"), 0.0, 1e-9);
}

TEST(match_similarity_transliterated) {
    CHECK_NEAR(text_similarity("米津玄師", "Kenshi Yonezu"), 1.0, 1e-9);        // the built-in reading of the name
    CHECK_NEAR(text_similarity("さくら", "Sakura"), 1.0, 1e-9);                   // kana
    CHECK_NEAR(text_similarity("사랑", "Sarang"), 1.0, 1e-9);                      // Hangul
    CHECK_NEAR(text_similarity("ラーメン", "ramen"), 1.0, 0.25);                  // long vowel mark: close, not necessarily exact
    CHECK(text_similarity("ラーメン", "ramen") >= 0.75);
}

TEST(match_by_file_name) {
    std::vector<MatchTrack> lib = {
        lt("/m/Other/Another.mp3", "Another", "Other"),
        lt("/m/Queen/01 - Bohemian Rhapsody.flac", "01 - Bohemian Rhapsody", "Queen"),
        lt("/m/Comp/Bohemian Rhapsody.mp3", "Bohemian Rhapsody", "Comp"),
    };
    PlaylistMatcher m(lib);
    // another drive, another extension, no track number: the name still fits; the folder name decides between the two
    MatchResult r = m.find(en("D:\\Music\\Queen\\Bohemian Rhapsody.mp3"));
    CHECK(r.kind == MatchKind::FileName);
    CHECK_EQ(r.index, 1);
    r = m.find(en("/x/Comp/bohemian_rhapsody!.ogg"));   // punctuation / case: the same word only after folding? "_" is a separator
    CHECK(r.kind == MatchKind::FileName);
    CHECK_EQ(r.index, 2);
    r = m.find(en("/x/y/Nothing Like It.mp3"));
    CHECK(r.kind == MatchKind::None);
}

TEST(match_file_name_other_artist_is_vetoed) {
    std::vector<MatchTrack> lib = {lt("/m/A/Intro.mp3", "Intro", "A", "Intro", "The xx")};
    PlaylistMatcher m(lib);
    CHECK(m.find(en("/x/Intro.mp3", "Metallica", "Intro")).kind == MatchKind::None);
    CHECK(m.find(en("/x/Intro.mp3", "The XX", "Intro")).kind == MatchKind::FileName);
}

TEST(match_by_title_and_artist_fuzzy) {
    std::vector<MatchTrack> lib = {
        lt("/m/a/x1.mp3", "x1", "a", "Wonderwall", "Oasis"),
        lt("/m/a/x2.mp3", "x2", "a", "Wonderwall", "Some Cover Band"),
        lt("/m/b/Radiohead - Creep.mp3", "Radiohead - Creep", "b"),
    };
    PlaylistMatcher m(lib);
    // tags: a typo in the title (still >= 75 %), artist spelled slightly differently
    MatchResult r = m.find(en("/z/q.mp3", "Oasis", "Wonderwal"));
    CHECK(r.kind == MatchKind::TitleArtist);
    CHECK_EQ(r.index, 0);
    CHECK(r.score >= 0.75);
    // from the file names: "Artist - Title" and the library's tag-less file
    r = m.find(en("/z/Radiohead - Creep (Remastered).mp3"));   // the bracket is ignored: already the same file name
    CHECK(r.kind == MatchKind::FileName);
    CHECK_EQ(r.index, 2);
    r = m.find(en("/z/Radiohed - Creap.mp3"));                 // typos in both: the fuzzy step
    CHECK(r.kind == MatchKind::TitleArtist);
    CHECK_EQ(r.index, 2);
    // the artist does not fit -> not matched
    r = m.find(en("/z/q.mp3", "Metallica", "Wonderwall"));
    CHECK(r.kind == MatchKind::None);
    // the title is too far off
    r = m.find(en("/z/q.mp3", "Oasis", "Supersonic"));
    CHECK(r.kind == MatchKind::None);
}

TEST(match_title_only_needs_one_clear_candidate) {
    std::vector<MatchTrack> lib = {
        lt("/m/a/1.mp3", "1", "a", "Unique Song Name", "Artist One"),
        lt("/m/a/2.mp3", "2", "a", "Common Title", "Artist One"),
        lt("/m/a/3.mp3", "3", "a", "Common Title", "Artist Two"),
    };
    PlaylistMatcher m(lib);
    CHECK(m.find(en("/z/Unique Song Name.wav")).index == 0);          // only one track fits
    CHECK(m.find(en("/z/Common Title.wav")).kind == MatchKind::None);  // two fit: no guess
}

TEST(match_cjk_through_latin_spelling) {
    std::vector<MatchTrack> lib = {
        lt("/m/j/1.mp3", "1", "j", "Lemon", "Kenshi Yonezu"),
        lt("/m/k/2.mp3", "2", "k", "Sarang", "Kim"),
        lt("/m/j/3.mp3", "3", "j", "さくら", "森山直太朗"),
    };
    PlaylistMatcher m(lib);
    // the list has the Japanese writing, the library the Latin one
    MatchResult r = m.find(en("/z/a.mp3", "米津玄師", "Lemon"));
    CHECK(r.kind == MatchKind::TitleArtist);
    CHECK_EQ(r.index, 0);
    // the other way round: Hangul title in the list
    r = m.find(en("/z/a.mp3", "Kim", "사랑"));
    CHECK(r.kind == MatchKind::TitleArtist);
    CHECK_EQ(r.index, 1);
    // kana title in the library, Latin in the list
    r = m.find(en("/z/a.mp3", "森山直太朗", "Sakura"));
    CHECK(r.kind == MatchKind::TitleArtist);
    CHECK_EQ(r.index, 2);
}

TEST(match_artist_two_folders_up) {
    std::vector<MatchTrack> lib = {lt("/m/Alpha Artist/First Album/Beta Song.wav", "Beta Song", "First Album")};
    PlaylistMatcher m(lib);
    CHECK(m.find(en("C:\\x\\Alpha Artist - Beta Song (Live).mp3")).kind == MatchKind::TitleArtist);   // "Artist - Title"; the bracket does not count
    CHECK(m.find(en("/q/Other/Beta Sing.mp3", "Alpha Artist", "Beta Sing")).kind == MatchKind::TitleArtist);
    CHECK(m.find(en("/q/Alpha Artist/Some Album/Beta Sing.mp3")).kind == MatchKind::TitleArtist);   // the list's own Artist/Album/Song layout
}
