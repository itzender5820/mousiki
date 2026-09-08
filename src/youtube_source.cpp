#include "youtube_source.h"
#include "process_util.h"
#include <sstream>

namespace muisc {

// yt-dlp --print "%(title)s\t%(artist,uploader)s" "ytsearch1:QUERY"
// gives us a title before we commit to a deterministic cache filename.
static bool probe_title(const std::string& query, std::string& title, std::string& artist) {
    std::string cmd = "yt-dlp --no-warnings --skip-download "
                       "--print \"%(title)s\t%(artist,uploader)s\" "
                       "\"ytsearch1:" + query + "\"";
    ProcResult r = run_capture(cmd);
    if (!r.ok() || r.out.empty()) return false;

    // First line only (in case yt-dlp prints extra diagnostics).
    std::istringstream stream(r.out);
    std::string line;
    std::getline(stream, line);
    auto tab = line.find('\t');
    if (tab == std::string::npos) {
        title = line;
        artist.clear();
    } else {
        title = line.substr(0, tab);
        artist = line.substr(tab + 1);
    }
    if (!title.empty() && title.back() == '\r') title.pop_back();
    if (!artist.empty() && artist.back() == '\r') artist.pop_back();
    return !title.empty();
}

std::optional<SongResult> YoutubeSource::resolve(const std::string& query, std::string* error_out) {
    std::string title, artist;
    if (!probe_title(query, title, artist)) {
        if (error_out) *error_out = "yt-dlp couldn't find/reach a result for: " + query;
        return std::nullopt;
    }

    fs::path cached = cache_.path_for(title, "opus");
    if (cache_.is_cached(title, "opus")) {
        SongResult result{title, artist, cached, true};
        return result;
    }

    // Download straight to the deterministic cache path via -o with a
    // fixed basename (no %(title)s expansion needed since we already
    // resolved it above), forcing opus so the extension matches path_for().
    std::string cmd = "yt-dlp --no-warnings -x --audio-format opus "
                       "-o " + shell_quote((cached.parent_path() / cached.stem()).string() + ".%(ext)s") + " "
                       "\"ytsearch1:" + query + "\"";
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
    std::string cmd = "yt-dlp --no-warnings -x --audio-format opus "
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
