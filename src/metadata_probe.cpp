#include "metadata_probe.h"
#include "process_util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <system_error>

namespace muisc {

static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

double probe_duration_seconds(const fs::path& file) {
    std::string cmd = "ffprobe -v error -show_entries format=duration -of csv=p=0 "
                       + shell_quote(file.string());
    ProcResult r = run_capture(cmd);
    if (r.out.empty()) return -1.0;
    try {
        return std::stod(r.out);
    } catch (...) {
        return -1.0;
    }
}

RowMeta probe_row_meta(const fs::path& file) {
    RowMeta rm;
    std::string cmd = "ffprobe -v error -show_entries format=duration:format_tags=artist "
                       "-of default=noprint_wrappers=1 " + shell_quote(file.string());
    ProcResult r = run_capture(cmd);
    if (r.out.empty()) return rm;

    std::istringstream stream(r.out);
    std::string line;
    while (std::getline(stream, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        if (val.empty() || val == "N/A") continue;

        if (key == "duration") {
            try { rm.duration_sec = std::stod(val); } catch (...) {}
        } else if (key == "TAG:artist") {
            rm.artist = val;
        }
    }
    return rm;
}

TrackMetadata probe_metadata(const fs::path& file, const std::string& fallback_name,
                              const std::string& fallback_artist, const std::string& location_label) {
    TrackMetadata md;
    md.name = fallback_name;
    md.artist = fallback_artist.empty() ? "-" : fallback_artist;
    md.location = location_label;

    std::error_code ec;
    auto bytes = fs::file_size(file, ec);
    if (!ec) {
        double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        std::ostringstream oss;
        oss.precision(2);
        oss << std::fixed << mb << "MB";
        md.file_size = oss.str();
    }

    std::string cmd = "ffprobe -v error "
                       "-show_entries format=duration:format_tags=artist,date,title:stream=sample_rate,codec_name "
                       "-of default=noprint_wrappers=1 " + shell_quote(file.string());
    ProcResult r = run_capture(cmd);
    if (!r.ok() && r.out.empty()) return md;

    std::istringstream stream(r.out);
    std::string line;
    while (std::getline(stream, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        if (val.empty() || val == "N/A") continue;

        if (key == "sample_rate") {
            md.sampling = val + "KHz"; // matches the mockup's (unconventional) unit label
        } else if (key == "codec_name") {
            md.format = to_upper(val);
        } else if (key == "TAG:title") {
            md.name = val;
        } else if (key == "TAG:artist") {
            md.artist = val;
        } else if (key == "TAG:date") {
            md.year = val.substr(0, 4);
        }
    }
    return md;
}

} // namespace muisc
