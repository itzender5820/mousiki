// Online search (see online_source.cpp): OnlineResult is one search hit and OnlineSource runs the
// search.
#pragma once
#include <string>
#include <vector>

namespace muisc {

struct OnlineResult {
    // A YouTube video id, or -- for SoundCloud / Bandcamp results -- the track's page URL ("https://soundcloud.com/...",
    // "https://artist.bandcamp.com/track/..."): every place that keeps an online track (queue, history, playlists)
    // stores this one string, and YoutubeSource::resolve_by_id() downloads either kind.
    std::string video_id;
    std::string title;
    std::string uploader;
    double duration_sec = -1.0; // -1 = unknown (not every extractor/flat-listing includes it)
};

class OnlineSource {
public:
    // `ytsearch<count>:query` with --flat-playlist so this only lists
    // results (fast, no per-video metadata fetch) — actual download only
    // happens once the user picks one (see YoutubeSource::resolve_by_id).
    //
    // When `fast_script_path` names a real file (scripts/fast_yt_search.py,
    // resolved once at startup -- see find_fast_search_script() in
    // app.cpp), that script is tried first: it hits YouTube's search
    // endpoint directly over HTTP instead of shelling out to yt-dlp,
    // which is the heavier of the two paths -- yt-dlp is a general-
    // purpose extractor for hundreds of sites and pays for that
    // generality in startup time, on every keystroke-triggered search.
    // yt-dlp is used as the fallback whenever the fast path comes back
    // empty, for any reason: the script wasn't found, Python isn't on
    // PATH, the network call failed, or the query genuinely has zero
    // results. That last case makes the fallback nearly free in the
    // common case (a real query already found something) while still
    // giving yt-dlp's own retry/extraction logic a chance on the ones
    // that aren't. Pass an empty path to skip the fast path entirely.
    //
    // The two are not quite equivalent: the fast path can't replicate
    // yt-dlp's "categories *= 'Music'" half of its result filter (that
    // needs a second request per video, which defeats the purpose), only
    // the "duration >= 90s" half. In practice that means the fast path
    // may occasionally surface a non-music video yt-dlp would have
    // filtered out; it will not surface shorts or live streams, both of
    // which are dropped by duration in the script itself.
    std::vector<OnlineResult> search(const std::string& query, int count = 15,
                                      const std::string& fast_script_path = "");

    // Lists every video in a YouTube playlist (or, harmlessly, just the
    // one video if given a plain video URL) via yt-dlp --flat-playlist,
    // same fast listing-only approach as search() -- nothing is
    // downloaded here, just enumerated so the caller can queue all of
    // them. `error_out`, if given, is filled in on failure (empty
    // result, non-zero exit, or an unparseable link).
    std::vector<OnlineResult> list_playlist(const std::string& url, std::string* error_out = nullptr);

    // Videos for the Video view (SEARCH YT): the same flat listing, without the music-only filter (a video can be a concert, a lyric
    // video ...). Titles, channels and lengths only; the preview picture is https://i.ytimg.com/vi/<id>/mqdefault.jpg.
    std::vector<OnlineResult> search_videos(const std::string& query, int count = 10);

    // SoundCloud (/sc:): yt-dlp's "scsearch" (SoundCloud's own search API).
    std::vector<OnlineResult> search_soundcloud(const std::string& query, int count = 15);
    // Bandcamp (/b:): yt-dlp has no Bandcamp search, so scripts/bandcamp_search.py asks Bandcamp's search directly
    // (its search API, the search page as a fallback) and prints the tracks it finds as JSON lines.
    std::vector<OnlineResult> search_bandcamp(const std::string& query, int count, const std::string& script_path);
};

} // namespace muisc
