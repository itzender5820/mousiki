#include "online_source.h"
#include "process_util.h"
#include <sstream>

namespace muisc {

// Same tiny flat-JSON string extractor used in lyrics_fetcher.cpp — kept
// local here since yt-dlp's per-line JSON objects are the only thing that
// needs it in this file, not worth a shared JSON dependency for two spots.
static bool json_get_string(const std::string& json, const std::string& key, std::string& out) {
    std::string needle = "\"" + key + "\"";
    size_t kpos = json.find(needle);
    if (kpos == std::string::npos) return false;
    size_t colon = json.find(':', kpos + needle.size());
    if (colon == std::string::npos) return false;
    size_t qstart = json.find('"', colon);
    if (qstart == std::string::npos) return false;
    size_t i = qstart + 1;
    std::string raw;
    while (i < json.size()) {
        if (json[i] == '\\' && i + 1 < json.size()) { raw += json[i]; raw += json[i + 1]; i += 2; continue; }
        if (json[i] == '"') break;
        raw += json[i];
        ++i;
    }
    // unescape the common cases
    std::string clean;
    for (size_t j = 0; j < raw.size(); ++j) {
        if (raw[j] == '\\' && j + 1 < raw.size()) {
            char n = raw[j + 1];
            if (n == 'n') { clean += ' '; ++j; continue; }
            if (n == '"' || n == '\\' || n == '/') { clean += n; ++j; continue; }
        }
        clean += raw[j];
    }
    out = clean;
    return true;
}

std::vector<OnlineResult> OnlineSource::search(const std::string& query, int count) {
    std::vector<OnlineResult> results;
    std::string cmd = "yt-dlp --no-warnings --flat-playlist -j "
                       "\"ytsearch" + std::to_string(count) + ":" + query + "\"";
    ProcResult r = run_capture(cmd);
    if (r.out.empty()) return results;

    std::istringstream stream(r.out);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line[0] != '{') continue;
        OnlineResult item;
        std::string uploader;
        json_get_string(line, "id", item.video_id);
        json_get_string(line, "title", item.title);
        if (!json_get_string(line, "uploader", uploader)) {
            json_get_string(line, "channel", uploader);
        }
        item.uploader = uploader;
        if (!item.video_id.empty() && !item.title.empty()) results.push_back(std::move(item));
    }
    return results;
}

} // namespace muisc
