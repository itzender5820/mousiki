// YouTube lookups: asks yt-dlp for the id, title and uploader of a search or a video id, for the
// online downloads.
#include "youtube_source.h"
#include "path_utf8.h"
#include "process_util.h"
#include <sstream>

namespace muisc {

bool valid_video_id(const std::string& id) {
    if (id.size() < 6 || id.size() > 32) return false;   // (YouTube's are 11 characters)
    for (unsigned char c : id)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-')) return false;
    return true;
}

bool valid_track_url(const std::string& url) {
    if (url.size() > 2048 || (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)) return false;
    for (unsigned char c : url) if (c <= 32 || c == 127) return false;   // no spaces / control characters
    return true;
}

// One download of the best audio into <stem>.<ext>: the address goes last, after `--` (so it can never be read as an option).
static ProcResult yt_dlp_download(const std::vector<std::string>& options, const std::string& address) {
    std::vector<std::string> argv = {"yt-dlp"};
    argv.insert(argv.end(), options.begin(), options.end());
    argv.push_back("--");
    argv.push_back(address);
    return run_capture(argv, /*merge_stderr=*/true);
}

// yt-dlp --print "%(title)s\t%(artist,uploader)s" "ytsearch1:QUERY"
// gives us a title before we commit to a deterministic cache filename.
static bool probe_title(const std::string& query, std::string& id, std::string& title, std::string& artist) {
    ProcResult r = run_capture({"yt-dlp", "-4", "--no-warnings",
                                "--extractor-args", "youtube:player_client=android;player_skip=webpage,configs,js",
                                "--match-filters", "categories *= 'Music' & duration >= 90",
                                "--flat-playlist", "--print", "%(id)s\t%(title)s\t%(uploader)s",
                                "ytsearch10:" + query});
    if (!r.ok() || r.out.empty()) return false;

    // First line only (in case yt-dlp prints extra diagnostics).
    std::istringstream stream(r.out);
    std::string line;
    std::getline(stream, line);
    
    auto t1 = line.find('\t');
    if (t1 == std::string::npos) return false;
    auto t2 = line.find('\t', t1 + 1);
    
    id = line.substr(0, t1);
    if (t2 == std::string::npos) {
        title = line.substr(t1 + 1);
        artist.clear();
    } else {
        title = line.substr(t1 + 1, t2 - t1 - 1);
        artist = line.substr(t2 + 1);
    }
    
    if (!id.empty() && id.back() == '\r') id.pop_back();
    if (!title.empty() && title.back() == '\r') title.pop_back();
    if (!artist.empty() && artist.back() == '\r') artist.pop_back();
    return !title.empty() && !id.empty();
}

std::optional<SongResult> YoutubeSource::resolve(const std::string& query, std::string* error_out) {
    std::string id, title, artist;
    if (!probe_title(query, id, title, artist)) {
        if (error_out) *error_out = "yt-dlp couldn't find/reach a result for: " + query;
        return std::nullopt;
    }

    const std::string key = cache_.cache_key(title, id);   // (a different song with the same title has its own file)
    fs::path cached = cache_.path_for(key, "opus");
    if (cache_.is_cached(key, "opus")) {
        SongResult result{title, artist, cached, true};
        return result;
    }

    if (!valid_video_id(id)) {
        if (error_out) *error_out = "yt-dlp returned something that is no video id";
        return std::nullopt;
    }
    const std::string url = "https://www.youtube.com/watch?v=" + id;
    ProcResult r = yt_dlp_download({"-4", "--no-warnings", "--extractor-args", "youtube:player_client=android;player_skip=webpage,configs,js",
                                    "-x", "-f", "bestaudio/best", "--audio-format", "opus", "--audio-quality", "0",
                                    "-o", path_utf8(cached.parent_path() / cached.stem()) + ".%(ext)s"}, url);

    if (!cache_.is_cached(key, "opus")) {
        if (error_out) *error_out = "yt-dlp download failed:\n" + r.out;
        return std::nullopt;
    }

    cache_.record_audio_download(key, id);
    cache_.record_download("youtube", id);   // shared with yt-dlp runs outside Mousiki (history/downloaded_history.txt)
    SongResult result{title, artist, cached, false};
    return result;
}

std::optional<SongResult> YoutubeSource::resolve_by_id(const std::string& video_id, const std::string& title,
                                                         const std::string& artist, std::string* error_out) {
    if (video_id.find("://") != std::string::npos) {
        if (!valid_track_url(video_id)) {   // (http / https only, no spaces: never something yt-dlp could read as an option)
            if (error_out) *error_out = "not a track address";
            return std::nullopt;
        }
        // SoundCloud / Bandcamp: the id is the track's page URL. The audio is kept as the site delivers it (MP3, Opus,
        // AAC ...) -- no second lossy conversion -- so the cached file has whatever extension that is.
        static const char* const kExts[] = {"opus", "mp3", "m4a", "aac", "ogg", "flac", "wav", "webm"};
        auto find_cached = [&]() -> std::optional<fs::path> {
            for (const char* e : kExts) if (cache_.is_cached(title, e)) return cache_.path_for(title, e);
            return std::nullopt;
        };
        if (auto hit = find_cached()) return SongResult{title, artist, *hit, true};
        const fs::path stem = cache_.path_for(title, "opus");
        ProcResult r = yt_dlp_download({"--no-warnings", "-x", "-f", "bestaudio/best",
                                       "-o", path_utf8(stem.parent_path() / stem.stem()) + ".%(ext)s"}, video_id);
        if (auto got = find_cached()) return SongResult{title, artist, *got, false};
        if (error_out) *error_out = "yt-dlp download failed:\n" + r.out;
        return std::nullopt;
    }
    const std::string key = cache_.cache_key(title, video_id);   // (a different song with the same title has its own file)
    fs::path cached = cache_.path_for(key, "opus");
    if (cache_.is_cached(key, "opus")) {
        SongResult result{title, artist, cached, true};
        return result;
    }

    if (!valid_video_id(video_id)) {
        if (error_out) *error_out = "not a video id: " + video_id.substr(0, 40);
        return std::nullopt;
    }
    const std::string url = "https://www.youtube.com/watch?v=" + video_id;
    ProcResult r = yt_dlp_download({"-4", "--no-warnings", "--extractor-args", "youtube:player_client=android;player_skip=webpage,configs,js",
                                    "-x", "-f", "bestaudio/best", "--audio-format", "opus", "--audio-quality", "0",
                                    "-o", path_utf8(cached.parent_path() / cached.stem()) + ".%(ext)s"}, url);

    if (!cache_.is_cached(key, "opus")) {
        if (error_out) *error_out = "yt-dlp download failed:\n" + r.out;
        return std::nullopt;
    }

    cache_.record_audio_download(key, video_id);
    cache_.record_download("youtube", video_id);   // shared with yt-dlp runs outside Mousiki (history/downloaded_history.txt)
    SongResult result{title, artist, cached, false};
    return result;
}

} // namespace muisc
