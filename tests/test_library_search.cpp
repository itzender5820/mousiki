// The library search (library_search.h): the same matching as fuzzy_score(), and the parallel scan gives the result of the single-thread scan.
#include <string>
#include <vector>

#include "library_search.h"
#include "test_framework.h"
#include "text_edit.h"

using namespace muisc;

namespace {
std::vector<SearchIdx> synthetic_library(size_t n, uint32_t seed) {
    static const char* words[] = {"love", "night", "fire", "heart", "dream", "blue", "dance", "summer", "river", "shadow", "gold", "moon", "city", "road", "rain",
                                  "wild", "stone", "angel", "ocean", "storm", "light", "ghost", "silver", "paper", "winter", "thunder", "velvet", "echo", "mirror", "garden"};
    uint32_t s = seed;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return s >> 8; };
    auto w = [&]() { return std::string(words[rnd() % 30]); };
    std::vector<SearchIdx> idx(n);
    for (auto& e : idx) {
        e.title_n = w() + " " + w() + " " + w();
        e.folder_artist_n = w() + " " + w();
        e.artist_n = rnd() % 4 ? w() + " " + w() : "";
        e.tag_title_n = rnd() % 2 ? w() + " " + w() : "";
        e.album_n = rnd() % 3 ? w() + " " + w() + " " + w() : "";
        e.key = "/m/" + e.title_n;
    }
    return idx;
}
}  // namespace

TEST(library_search_scan_with_several_threads_gives_the_single_thread_result) {
    const std::vector<SearchIdx> idx = synthetic_library(9000, 42);
    for (const char* q : {"love", "l", "love night", "silvr", "ocen strm", "xyzq", "thunder gold"}) {
        for (int scope = 0; scope < 3; ++scope) {
            const PreparedQuery pq = prepare_query(q);
            const auto one = score_library(idx, pq, scope, 1);
            for (int threads : {0, 2, 3, 4, 7}) {
                const auto many = score_library(idx, pq, scope, threads);
                CHECK_EQ(many.size(), one.size());
                bool same = many.size() == one.size();
                for (size_t i = 0; same && i < one.size(); ++i) same = many[i].first == one[i].first && many[i].second == one[i].second;
                CHECK(same);
            }
        }
    }
}

TEST(library_search_scan_handles_tiny_and_empty_libraries_with_forced_threads) {
    const PreparedQuery pq = prepare_query("love");
    CHECK(score_library({}, pq, 0, 4).empty());
    const std::vector<SearchIdx> three = synthetic_library(3, 1);
    const auto a = score_library(three, pq, 0, 1), b = score_library(three, pq, 0, 8);
    CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
}

TEST(library_search_matches_like_fuzzy_score) {
    const std::vector<std::string> targets = {"Another Love", "test_song-cover3", "Bohemian Rhapsody", "the quiet night", "cover3", "Sunday Morning Coming Down",
                                              "Rock'n'Roll Suicide", "88 Lines about 44 Women", "Ünïcödé Tëst", "a"};
    const std::vector<std::string> queries = {"another love", "anogher lobe", "testcover", "cov", "bohemian", "rhapsodie", "quiet nite", "sunday coming", "roll", "44", "zzz", "unicode"};
    for (const auto& t : targets) {
        SearchIdx e;
        e.title_n = normalize_for_search(t);
        for (const auto& q : queries) {
            const double want = fuzzy_score(q, t);
            const double got = score_track(prepare_query(q), e, 0);
            if (want > 0.0 || got > 0.0) CHECK_NEAR(got, want, 1e-9);
            else CHECK(got <= 0.0 && want <= 0.0);
        }
    }
}

TEST(library_search_scopes_look_at_the_artist_or_the_album_only) {
    SearchIdx e;
    e.title_n = "midnight city";
    e.folder_artist_n = "m83";
    e.album_n = "hurry up we are dreaming";
    const PreparedQuery city = prepare_query("city"), m83 = prepare_query("m83"), dream = prepare_query("dreaming");
    CHECK(score_track(city, e, 0) > 0.0);    // everything: the title
    CHECK(score_track(city, e, 1) <= 0.0);   // artist only: not in the artist
    CHECK(score_track(city, e, 2) <= 0.0);   // album only: not in the album
    CHECK(score_track(m83, e, 1) > 0.0);     // (the folder's artist stands in while no tag artist is known)
    CHECK(score_track(dream, e, 2) > 0.0);
    e.artist_n = "m83 band";                  // a tag artist takes the place of the folder's
    CHECK(score_track(m83, e, 1) > 0.0);
    SearchIdx noalbum;
    noalbum.title_n = "x";
    CHECK(score_track(dream, noalbum, 2) <= 0.0);
}

TEST(library_search_exact_substring_beats_a_fuzzy_match_and_earlier_beats_later) {
    SearchIdx exact, fuzzy, late;
    exact.title_n = "silver rain";
    fuzzy.title_n = "silvr rain";
    late.title_n = "the long road of silver";
    const PreparedQuery q = prepare_query("silver");
    const double se = score_track(q, exact, 0), sf = score_track(q, fuzzy, 0), sl = score_track(q, late, 0);
    CHECK(se > sl);
    CHECK(sl > sf);
    CHECK(sf > 0.0 && sf < 100.0);
}
