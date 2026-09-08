#pragma once
#include <string>
#include <vector>

namespace muisc {

struct OnlineResult {
    std::string video_id;
    std::string title;
    std::string uploader;
};

class OnlineSource {
public:
    // `ytsearch<count>:query` with --flat-playlist so this only lists
    // results (fast, no per-video metadata fetch) — actual download only
    // happens once the user picks one (see YoutubeSource::resolve_by_id).
    std::vector<OnlineResult> search(const std::string& query, int count = 15);
};

} // namespace muisc
