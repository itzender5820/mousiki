// Online search: runs the search through yt-dlp or the helper script and parses the results into
// OnlineResult entries.
#include "online_source.h"
#include "process_util.h"
#include "tiny_json.h"
#include "utf8_util.h"
#include <cctype>
#include <sstream>
#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

// Shared by both search paths -- yt-dlp's flat-playlist -j output and
// fast_yt_search.py's output are both one JSON object per line, and
// fast_yt_search.py deliberately mirrors yt-dlp's field names for exactly
// this reason (see its own comment).
static std::vector<OnlineResult> parse_json_lines(const std::string& out) {
    std::vector<OnlineResult> results;
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] != '{') continue;
        tinyjson::Value obj;
        if (!tinyjson::parse(line, obj) || obj.type != tinyjson::Type::Object) continue;
        OnlineResult item;
        item.video_id = tinyjson::string_field(obj, "id");
        item.title = clean_tag_text(tinyjson::string_field(obj, "title"));   // (from the web: no control characters)
        std::string uploader = tinyjson::string_field(obj, "uploader");
        if (uploader.empty()) uploader = tinyjson::string_field(obj, "channel");
        item.uploader = clean_tag_text(uploader);
        item.duration_sec = tinyjson::number_field(obj, "duration", item.duration_sec);
        if (!item.video_id.empty() && !item.title.empty()) results.push_back(std::move(item));
    }
    return results;
}

static std::vector<OnlineResult> search_via_ytdlp(const std::string& query, int count) {
    // The query is user-typed text. It is passed as one argument, so no shell
    // ever sees it: `cost $5` or `"Heroes" (live)` is searched exactly as typed.
    ProcResult r = run_capture({"yt-dlp", "-4", "--no-warnings", "--match-filters", "categories *= 'Music' & duration >= 90",
                                "--flat-playlist", "-j", "ytsearch" + std::to_string(count) + ":" + query});
    return parse_json_lines(r.out);
}

static std::vector<OnlineResult> search_via_fast_script(const std::string& query, int count,
                                                          const std::string& script_path) {
    Args cmd = python_command();
    if (cmd.empty()) return {};
    cmd.push_back(script_path);
    cmd.push_back(query);
    cmd.push_back(std::to_string(count));
    ProcResult r = run_capture(cmd);
    return parse_json_lines(r.out);
}

std::vector<OnlineResult> OnlineSource::search(const std::string& query, int count,
                                                const std::string& fast_script_path) {
    if (!fast_script_path.empty()) {
        auto results = search_via_fast_script(query, count, fast_script_path);
        if (!results.empty()) return results;
        // Empty covers every failure mode uniformly (script missing,
        // Python missing, network error, or a genuine zero-result
        // query) -- yt-dlp gets a full attempt in every one of them.
    }
    return search_via_ytdlp(query, count);
}

std::vector<OnlineResult> OnlineSource::search_videos(const std::string& query, int count) {
    ProcResult r = run_capture({"yt-dlp", "-4", "--no-warnings", "--ignore-errors", "--flat-playlist", "-j", "ytsearch" + std::to_string(count) + ":" + query});
    return parse_json_lines(r.out);
}

// One JSON object per line with a page URL instead of a video id (SoundCloud via yt-dlp, Bandcamp via the script):
// the URL becomes the result's id (see OnlineResult).
static std::vector<OnlineResult> parse_url_lines(const std::string& out) {
    std::vector<OnlineResult> results;
    std::istringstream stream(out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] != '{') continue;
        tinyjson::Value obj;
        if (!tinyjson::parse(line, obj) || obj.type != tinyjson::Type::Object) continue;
        OnlineResult item;
        std::string url = tinyjson::string_field(obj, "webpage_url");
        if (url.find("://") == std::string::npos) url = tinyjson::string_field(obj, "url");
        item.title = clean_tag_text(tinyjson::string_field(obj, "title"));   // (from the web: no control characters)
        std::string uploader = tinyjson::string_field(obj, "uploader");
        if (uploader.empty()) uploader = tinyjson::string_field(obj, "artist");
        item.uploader = clean_tag_text(uploader);
        item.duration_sec = tinyjson::number_field(obj, "duration", item.duration_sec);
        if (url.find("://") == std::string::npos || item.title.empty()) continue;
        item.video_id = url;
        results.push_back(std::move(item));
    }
    return results;
}

// A SoundCloud track whose every stream is a preview (the 30-second "snippet" SoundCloud plays for Go+ tracks and
// some label releases): yt-dlp marks those streams with "preview" in their format id. No stream at all (DRM, blocked
// in this country) cannot be played either.
static bool soundcloud_preview_only(const std::string& line) {
    const std::string key = "\"format_id\": \"";
    size_t at = 0;
    int all = 0, preview = 0;
    while ((at = line.find(key, at)) != std::string::npos) {
        at += key.size();
        const size_t end = line.find('"', at);
        if (end == std::string::npos) break;
        ++all;
        if (line.substr(at, end - at).find("preview") != std::string::npos) ++preview;
        at = end;
    }
    return all == 0 || preview == all;
}

std::vector<OnlineResult> OnlineSource::search_soundcloud(const std::string& query, int count) {
    // Not --flat-playlist: only the full listing has the streams, and only they tell a 30-second preview from the
    // whole track (the duration shown is the full one either way). Somewhat slower; a few more results are asked
    // for, since previews are dropped.
    const int ask = count + count / 2;
    ProcResult r = run_capture({"yt-dlp", "--no-warnings", "--ignore-errors", "-j", "scsearch" + std::to_string(ask) + ":" + query});
    std::string kept;
    std::istringstream stream(r.out);
    std::string line;
    while (std::getline(stream, line))
        if (!line.empty() && line[0] == '{' && !soundcloud_preview_only(line)) kept += line + "\n";
    auto results = parse_url_lines(kept);
    if (static_cast<int>(results.size()) > count) results.resize(static_cast<size_t>(count));
    return results;
}

std::vector<OnlineResult> OnlineSource::search_bandcamp(const std::string& query, int count, const std::string& script_path) {
    if (script_path.empty()) return {};
    Args cmd = python_command();
    if (cmd.empty()) return {};
    cmd.push_back(script_path);
    cmd.push_back(query);
    cmd.push_back(std::to_string(count));
    ProcResult r = run_capture(cmd);
    return parse_url_lines(r.out);
}

std::vector<OnlineResult> OnlineSource::list_playlist(const std::string& url, std::string* error_out) {
    std::vector<OnlineResult> results;
    std::string trimmed = url;
    // Trim incidental whitespace a paste often carries.
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r' || trimmed.back() == '\n')) trimmed.pop_back();
    if (trimmed.empty()) {
        if (error_out) *error_out = "empty link";
        return results;
    }
    if (trimmed.find("http://") != 0 && trimmed.find("https://") != 0) {
        if (error_out) *error_out = "not a URL -- paste a youtube.com/playlist?list=... link";
        return results;
    }

    // --flat-playlist -j lists every entry (id/title/uploader) without
    // resolving/downloading any of them -- same fast enumeration
    // approach as search() above. Works for a playlist URL (many
    // entries) and degrades gracefully to a single entry for a plain
    // video URL.
    ProcResult r = run_capture({"yt-dlp", "-4", "--no-warnings", "--flat-playlist", "-j", trimmed});
    if (!r.ok() || r.out.empty()) {
        if (error_out) *error_out = "yt-dlp couldn't list that link (exit " + std::to_string(r.exit_code) + ")";
        return results;
    }

    std::istringstream stream(r.out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] != '{') continue;
        tinyjson::Value obj;
        if (!tinyjson::parse(line, obj) || obj.type != tinyjson::Type::Object) continue;
        OnlineResult item;
        item.video_id = tinyjson::string_field(obj, "id");
        item.title = clean_tag_text(tinyjson::string_field(obj, "title"));   // (from the web: no control characters)
        std::string uploader = tinyjson::string_field(obj, "uploader");
        if (uploader.empty()) uploader = tinyjson::string_field(obj, "channel");
        item.uploader = clean_tag_text(uploader);
        item.duration_sec = tinyjson::number_field(obj, "duration", item.duration_sec);
        if (!item.video_id.empty()) {
            if (item.title.empty()) item.title = item.video_id;
            results.push_back(std::move(item));
        }
    }
    if (results.empty() && error_out) *error_out = "no videos found in that playlist";
    return results;
}

} // namespace muisc
