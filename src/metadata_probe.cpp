// Track metadata via ffprobe: reads duration and tags (artist, title, album, year) for the lists and
// the metadata panel, and every tag of a file for the queue search.
#include "metadata_probe.h"
#include "local_source.h"
#include "chiptune.h"
#include "chip_meta.h"
#include "process_util.h"
#include "string_util.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>
#include <system_error>

namespace muisc {

static std::string to_upper(std::string s) {
    return ascii_upper_str(std::move(s));
}

double probe_duration_seconds(const fs::path& file) {
    ProcResult r = run_capture({"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0",
                                path_utf8(file)});
    if (r.out.empty()) return -1.0;
    try {
        return std::stod(r.out);
    } catch (...) {
        return -1.0;
    }
}

static RowMeta probe_row_meta_impl(const fs::path& file) {
    RowMeta rm;
    // Pulls title/album alongside artist now (still one ffprobe call, so
    // no extra subprocess cost) so the library-wide search can match
    // against embedded tags, not just the artist column and the
    // filename-derived title. stream_tags is also requested: ffmpeg's
    // Ogg Vorbis/Opus muxers expose title/artist/album as TAGS ON THE
    // AUDIO STREAM rather than on the container ("format") -- format_tags
    // alone comes back completely empty for those two formats even
    // though the file plainly has metadata (confirmed against real
    // ffmpeg-muxed .ogg/.opus files: format_tags is empty, stream_tags
    // has everything). Keeping format_tags too covers everything else
    // (MP3, FLAC, M4A, ...), where it's the one that's populated.
    ProcResult r = run_capture({"ffprobe", "-v", "error",
                                "-show_entries", "format=duration:format_tags=artist,title,album,date:stream_tags=artist,title,album,date",
                                "-of", "default=noprint_wrappers=1", path_utf8(file)});
    // A real probe attempt happened either way -- mark it resolved even on
    // an empty/failed result so callers don't keep retrying an untagged or
    // unreadable file forever. Only the fields actually parsed below get
    // filled in; everything else stays at its default (empty/-1).
    rm.tags_resolved = true;
    if (is_sid_file(file)) {   // ffprobe cannot read SID: the header has name / author / release year
        const SidInfo si = read_sid_header(file);
        if (si.ok) {
            if (si.name != "<?>") rm.title = clean_tag_text(si.name);
            if (si.author != "<?>") rm.artist = clean_tag_text(si.author);
            if (si.released.size() >= 4 && std::isdigit(static_cast<unsigned char>(si.released[0]))) rm.year = si.released.substr(0, 4);
        }
        return rm;
    }
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
        val = clean_tag_text(val);   // malformed UTF-8 / control characters in a tag would be measured wider than drawn
        if (val.empty()) continue;

        if (key == "duration") {
            if (const auto n_ = muisc::parse_double(val)) { rm.duration_sec = *n_; }
        } else if (key == "TAG:artist") {
            // First-wins: with stream_tags also requested, a file with
            // several streams (e.g. an attached-picture "video" stream
            // alongside the audio) can print more than one TAG:artist
            // line. format_tags is listed first and is the authoritative
            // one when present; don't let a later, possibly-blank or
            // irrelevant stream's tags clobber it.
            if (rm.artist.empty()) rm.artist = clean_tag_text(val);
        } else if (key == "TAG:title") {
            if (rm.title.empty()) rm.title = clean_tag_text(val);
        } else if (key == "TAG:album") {
            if (rm.album.empty()) rm.album = clean_tag_text(val);
        } else if (key == "TAG:date") {
            // Containers store the year very differently (ID3's TYER="1999",
            // ID3v2.4's TDRC="1999-05-01", Vorbis' DATE, MP4's ©day) --
            // ffprobe just reports whatever it found, so keep only the
            // leading 4-digit year. Same rule probe_metadata() applies to
            // its own md.year, so the editor and the metadata panel agree.
            if (rm.year.empty()) { const std::string y = clean_tag_text(val); rm.year = (y.size() >= 4) ? y.substr(0, 4) : y; }
        }
    }
    return rm;
}

static TrackMetadata probe_metadata_impl(const fs::path& file, const std::string& fallback_name,
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

    // SID tunes: ffprobe cannot read them, the PSID header has the name, author and release line.
    if (is_sid_file(file)) {
        const SidInfo si = read_sid_header(file);
        if (si.ok) {
            if (!si.name.empty() && si.name != "<?>") md.name = clean_tag_text(si.name);
            if (!si.author.empty() && si.author != "<?>") md.artist = clean_tag_text(si.author);
            if (si.released.size() >= 4 && std::isdigit(static_cast<unsigned char>(si.released[0]))) md.year = si.released.substr(0, 4);
            md.format = si.rsid ? "RSID" : "PSID";
            md.type = "C64 SID" + std::string(si.songs > 1 ? " (" + std::to_string(si.songs) + " tunes)" : "");
        }
        return md;
    }

    // format_tags for MP3 / FLAC / M4A, stream_tags for Ogg Vorbis / Opus (their
    // tags live on the audio stream, see probe_row_meta). The first value of a
    // kind wins, and the key is compared case-insensitively: Vorbis comments
    // come back as TAG:DATE / TAG:ALBUM, ID3 as TAG:date.
    ProcResult r = run_capture({"ffprobe", "-v", "error",
                                "-show_entries", "format_tags=artist,date,year,title,album:stream_tags=artist,date,year,title,album:stream=sample_rate,codec_name",
                                "-of", "default=noprint_wrappers=1", path_utf8(file)});
    if (!r.ok() && r.out.empty()) return md;

    std::istringstream stream(r.out);
    std::string line;
    const bool is_video_ext = LocalSource::is_video_file(file);
    if (is_video_ext) md.type = "video + audio";
    bool got_title = false, got_artist = false, got_album = false, got_year = false, got_rate = false, got_codec = false;
    while (std::getline(stream, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        if (val.empty() || val == "N/A") continue;
        val = clean_tag_text(val);   // malformed UTF-8 / control characters in a tag would be measured wider than drawn
        if (val.empty()) continue;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (key == "sample_rate") {
            if (!got_rate) { md.sampling = val + "KHz"; got_rate = true; }   // matches the mockup's (unconventional) unit label
        } else if (key == "codec_name") {
            if (!got_codec || is_video_ext) { md.format = to_upper(val); got_codec = true; }   // (a video file: the last stream is the sound)
        } else if (key == "tag:title") {
            if (!got_title) { md.name = val; got_title = true; }
        } else if (key == "tag:artist") {
            if (!got_artist) { md.artist = val; got_artist = true; }
        } else if (key == "tag:album") {
            if (!got_album) { md.album = val; got_album = true; }
        } else if (key == "tag:date" || key == "tag:year") {
            if (!got_year) { md.year = val.substr(0, 4); got_year = true; }
        }
    }
    // Tracker modules and game music: ffprobe reports the decoded PCM ("PCM_F32LE"); the file type says more.
    if (is_tracker_file(file) || is_gme_file(file)) {
        md.format = to_upper(path_utf8(file.extension()).substr(1));
        md.type = is_tracker_file(file) ? "tracker module" : "game music";
    }
    return md;
}

// Tracker modules and chiptunes: what the side file next to them holds is put over what the file itself reports.
std::string probe_all_tag_text(const fs::path& file) {
    // no names after format_tags / stream_tags: ffprobe prints every tag it finds (stream tags too: Ogg / Opus keep them there)
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-show_entries", "format_tags:stream_tags",
                                      "-of", "default=noprint_wrappers=1", path_utf8(file)});
    std::string all;
    std::istringstream stream(r.out);
    std::string line;
    while (std::getline(stream, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || line.compare(0, 4, "TAG:") != 0) continue;
        std::string val = line.substr(eq + 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        if (val.empty() || val == "N/A" || val.size() > 400) continue;   // (a whole lyrics text is not worth searching here)
        val = clean_tag_text(val);
        if (val.empty()) continue;
        if (!all.empty()) all += " | ";
        all += val;
    }
    return all;
}

RowMeta probe_row_meta(const fs::path& file) {
    RowMeta rm = probe_row_meta_impl(file);
    chipmeta::overlay(file, rm.artist, rm.title, rm.album, rm.year);
    return rm;
}

TrackMetadata probe_metadata(const fs::path& file, const std::string& fallback_name,
                              const std::string& fallback_artist, const std::string& location_label) {
    TrackMetadata md = probe_metadata_impl(file, fallback_name, fallback_artist, location_label);
    if (chipmeta::applies(file)) {
        std::string artist = md.artist == "-" ? std::string() : md.artist, title = md.name, album = md.album, year = md.year;
        chipmeta::overlay(file, artist, title, album, year);
        if (!artist.empty()) md.artist = artist;
        md.name = title;
        md.album = album;
        md.year = year;
    }
    return md;
}

} // namespace muisc
