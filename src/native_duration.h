#pragma once
#include <cstdint>
#include <filesystem>

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

} // namespace muisc
