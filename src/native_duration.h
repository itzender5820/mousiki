// In-process header parsing (see native_duration.cpp): duration and tag probes for common formats,
// used before falling back to ffprobe.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace muisc {

namespace fs = std::filesystem;

// In-process binary-header duration parsing for MP3/FLAC/M4A/MP4/AAC/OGG/
// Opus — a handful of pread() syscalls against the file header, no
// subprocess involved at all. This is what makes it safe to call
// synchronously on the main/render thread: worst case it's a few
// microseconds of disk I/O, never a process spawn that can stall or hang.
//
// Returns 0 if the format isn't one of the above, or the header couldn't
// be parsed (corrupt/unusual file) — callers should treat 0 as "unknown,
// fall back to something else" rather than a real zero-length track.
uint32_t probe_duration_native(const fs::path& path);

// Result of a native (no-subprocess) ID3v2 tag read — see
// probe_id3v2_native() below.
struct NativeId3Tags {
    // True only when a well-formed ID3v2.3/2.4 tag was actually walked
    // frame-by-frame to completion. An untagged-but-otherwise-fine MP3 (no
    // "ID3" magic at offset 0 at all) is NOT "resolved" here -- some
    // untagged-by-ID3v2 files still carry an ID3v1 trailer or other tag
    // this lightweight reader doesn't look at, so callers should fall back
    // to ffprobe in that case rather than assume the file has no metadata.
    // `resolved == true` with every field empty means "a real ID3v2 tag
    // was fully parsed and genuinely has none of these three frames" --
    // that IS a final answer, no fallback needed.
    bool resolved = false;
    std::string title;
    std::string artist;
    std::string album;
    std::string year;    // "YYYY" (or a free-text year), empty if the tag is absent
};

// Reads TIT2/TPE1/TALB/TYER (and the ID3v2.4 TDRC date) straight out of an
// MP3's ID3v2 header -- no `ffprobe`
// subprocess at all, just the same handful of pread() calls
// probe_duration_native() already does for MP3 duration. This is what lets
// an 800-track library's metadata become fully searchable in well under a
// second instead of however long it takes to spawn that many subprocesses.
//
// Deliberately conservative: bails out (resolved=false) rather than guess
// on anything this simple frame walker doesn't fully understand --
// per-tag or per-frame unsynchronisation, an extended header, compressed or
// encrypted frames, or a tag_size that doesn't fit the file. Every one of
// those is rare in practice (most taggers -- Mp3tag, kid3, iTunes, foobar2000
// -- don't produce them), and callers fall back to ffprobe for exactly
// those files, so correctness never depends on this parser being complete,
// only fast for the common case.
NativeId3Tags probe_id3v2_native(const fs::path& path);

// Same idea as probe_id3v2_native(), for the other formats native duration
// parsing already understands. Each is equally conservative: any tag
// layout it doesn't fully recognize (an oversized/truncated comment list,
// an unrecognized codec inside an Ogg container, a non-standard MP4 tag
// location) comes back as resolved=false so the caller falls back to
// ffprobe rather than risk a wrong or incomplete answer.
NativeId3Tags probe_flac_native(const fs::path& path);   // FLAC: Vorbis comment metadata block
NativeId3Tags probe_ogg_native(const fs::path& path);    // Ogg Vorbis or Opus: Vorbis-comment packet
NativeId3Tags probe_mp4_native(const fs::path& path);    // M4A/MP4/AAC: moov/udta/meta/ilst atoms
NativeId3Tags probe_wav_native(const fs::path& path);    // WAV: the RIFF chunk list, a LIST/INFO chunk (INAM, IART, IPRD, ICRD); resolved with no fields = a WAV without tags

} // namespace muisc
