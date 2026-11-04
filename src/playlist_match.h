#pragma once
// Finds the tracks of an imported playlist (M3U / M3U8) in the user's own library when the paths of the list do not exist here -- the
// list comes from someone else's computer. Two steps, in this order:
//   1. FILE NAME: the name of the file without its folder and extension ("01 - Song.flac" = "Song.mp3"; case, accents, spaces and
//      punctuation do not count; a leading track number is ignored). Japanese / Korean / Chinese names also match by their Latin
//      spelling (translit.h). With several files of that name the one whose folder name matches (then whose artist matches) wins.
//   2. TITLE AND ARTIST, fuzzy: both must be at least 75 % alike (kMatchThreshold). The list's side comes from its #EXTINF line
//      ("Artist - Title") and from its file name ("Artist - Title.mp3", folder name as the artist); the library's side from the tags
//      (when they have been read) and from the file name / folder. The Latin spelling of Japanese / Korean / Chinese text is compared
//      as well, so "米津玄師" finds "Yonezu Kenshi" and the other way round (the order of the words in a name does not matter).
//      A list entry with no artist anywhere is taken only on a title that is at least 90 % alike and only when exactly one track of
//      the library fits.
#include <string>
#include <vector>

namespace muisc {

constexpr double kMatchThreshold = 0.75;     // title AND artist
constexpr double kMatchTitleOnly = 0.90;     // an entry without any artist

// A track of the library.
struct MatchTrack {
    std::string path;          // UTF-8
    std::string stem;          // file name without extension
    std::string folder;        // name of its folder
    std::string tag_title;     // from the tags; empty when unknown (not read yet / untagged)
    std::string tag_artist;
};

// An entry of the playlist whose file was not found.
struct MatchEntry {
    std::string path;          // UTF-8, as written in the list
    std::string extinf_title;  // from "#EXTINF:123,Artist - Title"; empty when there is no such line
    std::string extinf_artist;
};

enum class MatchKind { None, FileName, TitleArtist };

struct MatchResult {
    MatchKind kind = MatchKind::None;
    int index = -1;            // into the vector the matcher was built from
    double score = 0.0;        // 1.0 for a file name; the mean of title and artist similarity for the fuzzy step
};

class PlaylistMatcher {
public:
    explicit PlaylistMatcher(const std::vector<MatchTrack>& library);
    MatchResult find(const MatchEntry& e) const;
    ~PlaylistMatcher();
    PlaylistMatcher(const PlaylistMatcher&) = delete;
    PlaylistMatcher& operator=(const PlaylistMatcher&) = delete;
private:
    struct Impl;
    Impl* d_;
};

// Similarity of two texts, 0..1 (1 - edit distance / length of the longer, on letters and digits only: case, accents, punctuation,
// bracketed parts like "(Remastered)" ignored). The Latin spelling of CJK text is tried too; the better value counts.
double text_similarity(const std::string& a, const std::string& b);

}  // namespace muisc
