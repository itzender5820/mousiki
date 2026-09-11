#include "youtube_source.h"
#include "process_util.h"
#include <sstream>

namespace muisc {

// yt-dlp --print "%(title)s\t%(artist,uploader)s" "ytsearch1:QUERY"
// gives us a title before we commit to a deterministic cache filename.
static bool probe_title(const std::string& query, std::string& id, std::string& title, std::string& artist) {
    std::string cmd = "yt-dlp -4 --no-warnings --extractor-args \"youtube:player_client=android;player_skip=webpage,configs,js\" --match-filters \"categories *= 'Music' & duration >= 90\" --flat-playlist "
                       "--print \"%(id)s\t%(title)s\t%(uploader)s\" "
                       "\"ytsearch10:" + query + "\"";
    ProcResult r = run_capture(cmd);
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

    fs::path cached = cache_.path_for(title, "opus");
    if (cache_.is_cached(title, "opus")) {
        SongResult result{title, artist, cached, true};
        return result;
    }

    std::string url = "https://www.youtube.com/watch?v=" + id;
    std::string cmd = "yt-dlp -4 --no-warnings --extractor-args \"youtube:player_client=android;player_skip=webpage,configs,js\" -x -f bestaudio/best --audio-format opus --audio-quality 0 "
                       "-o " + shell_quote((cached.parent_path() / cached.stem()).string() + ".%(ext)s") + " "
                       + shell_quote(url);
    ProcResult r = run_capture(cmd, /*merge_stderr=*/true);

    if (!cache_.is_cached(title, "opus")) {
        if (error_out) *error_out = "yt-dlp download failed:\n" + r.out;
        return std::nullopt;
    }

    SongResult result{title, artist, cached, false};
    return result;
}

std::optional<SongResult> YoutubeSource::resolve_by_id(const std::string& video_id, const std::string& title,
                                                         const std::string& artist, std::string* error_out) {
    fs::path cached = cache_.path_for(title, "opus");
    if (cache_.is_cached(title, "opus")) {
        SongResult result{title, artist, cached, true};
        return result;
    }

    std::string url = "https://www.youtube.com/watch?v=" + video_id;
    std::string cmd = "yt-dlp -4 --no-warnings --extractor-args \"youtube:player_client=android;player_skip=webpage,configs,js\" -x -f bestaudio/best --audio-format opus --audio-quality 0 "
                       "-o " + shell_quote((cached.parent_path() / cached.stem()).string() + ".%(ext)s") + " "
                       + shell_quote(url);
    ProcResult r = run_capture(cmd, /*merge_stderr=*/true);

    if (!cache_.is_cached(title, "opus")) {
        if (error_out) *error_out = "yt-dlp download failed:\n" + r.out;
        return std::nullopt;
    }

    SongResult result{title, artist, cached, false};
    return result;
}

} // namespace muisc
