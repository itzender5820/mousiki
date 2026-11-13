// The matching of the library search (the local list: "/" in the main screen), for a library of tens of thousands of tracks.
//
// The same matching as fuzzy_score() (text_edit.h) -- the results are identical -- but the query is normalised and split once, the targets are
// normalised once per scan (SearchIdx), and the edit distance stops as soon as a word cannot reach the quality floor any more. The scan over all
// tracks is shared between a few threads (par_pool.h) when the library is large: the tracks are cut into parts in their order and the parts are put
// together again in that order, so the result is the same as with one thread (tested).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

// One row of the search index (parallel to the list of local tracks): the strings a search compares, already lowercased / separator-folded,
// and the cache key. The tag fields are filled in as the background sweep resolves them (tags_seen).
struct SearchIdx {
    std::string key;
    std::string title_n, folder_artist_n;
    std::string tag_title_n, artist_n, album_n;
    bool tags_seen = false;
};

struct PreparedQuery {
    std::string q;
    std::vector<std::string> words;
    std::vector<uint64_t> masks;   // per word: which characters it contains (hashed to 64 bits)
    std::string compact;           // the words run together (for the abbreviation match)
};
PreparedQuery prepare_query(const std::string& query);

// Tier 1 of fuzzy_score(): exact substring, earlier position ranks higher. -1 = no substring.
double substring_score(const PreparedQuery& pq, const std::string& t_norm);
// Tiers 2 and 2b of fuzzy_score(): every query word must find a close word in the target (or the run-together abbreviation). -1 = no match.
double word_fuzzy_score(const PreparedQuery& pq, const std::string& t_norm);

// The score of one track. scope: 0 everything (title, artist, tag title, album), 1 the artist only, 2 the album only. <= 0: not a match.
double score_track(const PreparedQuery& pq, const SearchIdx& e, int scope);

// The tracks that match (score > 0) as {score, index into `idx`}, in the order of the index. `threads`: 0 = as many as the machine has (at most 4),
// 1 = one thread (with 0 a library of fewer than kSearchParallelMin tracks is scanned by one thread: the parts would cost more than they save).
std::vector<std::pair<double, uint32_t>> score_library(const std::vector<SearchIdx>& idx, const PreparedQuery& pq, int scope, int threads = 0);
constexpr size_t kSearchParallelMin = 6000;

}  // namespace muisc
