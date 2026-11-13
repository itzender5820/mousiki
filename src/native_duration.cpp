// In-process header parsing: reads the duration and the basic tags of MP3, FLAC, Ogg/Opus and MP4
// files without starting ffprobe, so the lists fill in without blocking the screen.
#include "native_duration.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <vector>
#if defined(_WIN32)
#include "win_compat.h"
#include <io.h>
#else
#include <unistd.h>
#endif

namespace muisc {

namespace {

// Thin wrappers so the parsers below stay identical on every platform.
//
// Two Windows-only details matter here. fs::path::value_type is wchar_t, so
// the POSIX `open(path.c_str(), ...)` would not even compile -- and routing
// through path.string() instead would mangle any non-ASCII filename, which is
// exactly what the yt-dlp cache is full of. _wopen takes the native wide path
// directly. And _O_BINARY is mandatory: without it the CRT translates CRLF
// inside what are binary audio headers, corrupting every offset.
int open_for_probe(const fs::path& path) {
#if defined(_WIN32)
    return _wopen(path.c_str(), _O_RDONLY | _O_BINARY);
#else
    return ::open(path.c_str(), O_RDONLY);
#endif
}

long long file_size_of(int fd) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0) return -1;
    return static_cast<long long>(st.st_size);
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return static_cast<long long>(st.st_size);
#endif
}

void close_probe(int fd) {
#if defined(_WIN32)
    _close(fd);
#else
    ::close(fd);
#endif
}

// Positional read. Windows has no pread; win_pread() emulates it with an
// OVERLAPPED offset, which -- like the real thing -- leaves the descriptor's
// own file pointer untouched.
long long pread_at(int fd, void* buf, size_t count, long long offset) {
#if defined(_WIN32)
    return static_cast<long long>(muisc::win_pread(fd, buf, count, offset));
#else
    return static_cast<long long>(::pread(fd, buf, count, static_cast<off_t>(offset)));
#endif
}

// Normalises a raw "year" tag to the 4-digit year the meta editor shows and
// edits. The same underlying value arrives as "1999" (ID3v2.3 TYER, Vorbis
// DATE), "1999-05-01" (ID3v2.4 TDRC, MP4 ©day) or occasionally something
// free-text -- keeping only a leading all-digit run of >= 4 characters
// matches what probe_row_meta()/probe_metadata() do with ffprobe's answer,
// so a file looks the same whether it was resolved here or by ffprobe.
std::string year_from_tag(std::string v) {
    size_t start = v.find_first_not_of(" \t");
    if (start == std::string::npos) return std::string();
    v.erase(0, start);
    if (v.size() >= 4 && std::isdigit(static_cast<unsigned char>(v[0])) &&
        std::isdigit(static_cast<unsigned char>(v[1])) &&
        std::isdigit(static_cast<unsigned char>(v[2])) &&
        std::isdigit(static_cast<unsigned char>(v[3]))) {
        return v.substr(0, 4);
    }
    return v;
}

} // namespace

namespace {

uint32_t read_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
uint64_t read_be64(const uint8_t* p) {
    return (uint64_t(read_be32(p)) << 32) | uint64_t(read_be32(p + 4));
}
uint32_t read_le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t read_le64(const uint8_t* p) {
    return uint64_t(read_le32(p)) | (uint64_t(read_le32(p + 4)) << 32);
}

// MP4/M4A/AAC 'mvhd' atom.
uint32_t parse_mp4_duration(int fd, long long file_size) {
    long long offset = 0;
    uint8_t hdr[8];
    while (offset + 8 <= file_size) {
        if (pread_at(fd, hdr, 8, offset) != 8) break;
        uint32_t atom_size = read_be32(hdr);
        if (atom_size == 0) break;

        if (std::memcmp(hdr + 4, "moov", 4) == 0) {
            long long sub_offset = offset + 8;
            long long moov_end = offset + atom_size;
            while (sub_offset + 8 <= moov_end) {
                uint8_t sub_hdr[8];
                if (pread_at(fd, sub_hdr, 8, sub_offset) != 8) break;
                uint32_t sub_size = read_be32(sub_hdr);
                if (sub_size == 0) break;

                if (std::memcmp(sub_hdr + 4, "mvhd", 4) == 0) {
                    uint8_t mvhd[32];
                    if (pread_at(fd, mvhd, sizeof(mvhd), sub_offset + 8) >= 24) {
                        uint8_t version = mvhd[0];
                        if (version == 0) {
                            uint32_t timescale = read_be32(mvhd + 12);
                            uint32_t duration = read_be32(mvhd + 16);
                            if (timescale > 0) return duration / timescale;
                        } else if (version == 1) {
                            uint32_t timescale = read_be32(mvhd + 20);
                            uint64_t duration = read_be64(mvhd + 24);
                            if (timescale > 0) return static_cast<uint32_t>(duration / timescale);
                        }
                    }
                    return 0;
                }
                sub_offset += sub_size;
            }
        }
        offset += atom_size;
    }
    return 0;
}

// OGG/Opus/Vorbis: last page's granule position.
uint32_t parse_ogg_duration(int fd, long long file_size) {
    if (file_size < 4096) return 0;
    long long seek_pos = (file_size > 65536) ? (file_size - 65536) : 0;
    size_t read_len = static_cast<size_t>(file_size - seek_pos);

    std::vector<uint8_t> buf(read_len);
    if (pread_at(fd, buf.data(), read_len, seek_pos) != static_cast<long long>(read_len)) return 0;

    for (long long i = static_cast<long long>(read_len) - 14; i >= 0; --i) {
        if (std::memcmp(buf.data() + i, "OggS", 4) == 0) {
            uint64_t granule = read_le64(buf.data() + i + 6);
            if (granule > 0 && granule != static_cast<uint64_t>(-1)) {
                return static_cast<uint32_t>(granule / 48000); // standard Opus 48kHz clock
            }
        }
    }
    return 0;
}

// MP3: first valid frame header, Xing/Info VBR field if present, else
// bitrate-based estimate from remaining file size.
uint32_t parse_mp3_duration(int fd, long long file_size) {
    static const int bitrate_tbl[2][3][16] = {
        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
    static const int freq_tbl[3][4] = {
        {44100, 48000, 32000, 0}, {22050, 24000, 16000, 0}, {11025, 12000, 8000, 0}};

    long long offset = 0;
    uint8_t id3_hdr[10];
    if (pread_at(fd, id3_hdr, 10, 0) == 10 && std::memcmp(id3_hdr, "ID3", 3) == 0) {
        uint32_t tag_size = ((id3_hdr[6] & 0x7F) << 21) | ((id3_hdr[7] & 0x7F) << 14) |
                             ((id3_hdr[8] & 0x7F) << 7) | (id3_hdr[9] & 0x7F);
        offset = 10 + tag_size;
    }

    uint8_t scan_buf[8192];
    long long read_bytes = pread_at(fd, scan_buf, sizeof(scan_buf), offset);
    if (read_bytes < 4) return 0;

    for (long long i = 0; i < read_bytes - 4; ++i) {
        if (scan_buf[i] == 0xFF && (scan_buf[i + 1] & 0xE0) == 0xE0) {
            uint8_t b1 = scan_buf[i + 1], b2 = scan_buf[i + 2], b3 = scan_buf[i + 3];
            int ver_idx = (b1 >> 3) & 0x03;
            int lay_idx = (b1 >> 1) & 0x03;
            int br_idx = (b2 >> 4) & 0x0F;
            int sr_idx = (b2 >> 2) & 0x03;
            int channel_mode = (b3 >> 6) & 0x03;
            if (ver_idx == 1 || lay_idx == 0 || br_idx == 0x0F || sr_idx == 3) continue;

            int v_slot = (ver_idx == 3) ? 0 : 1;
            int l_slot = (lay_idx == 3) ? 0 : (lay_idx == 2 ? 1 : 2);
            int freq_ver = (ver_idx == 3) ? 0 : (ver_idx == 2 ? 1 : 2);
            int bitrate_kbps = bitrate_tbl[v_slot][l_slot][br_idx];
            int sample_rate = freq_tbl[freq_ver][sr_idx];
            if (bitrate_kbps == 0 || sample_rate == 0) continue;

            int xing_offset = (ver_idx == 3) ? (channel_mode == 3 ? 17 : 32) : (channel_mode == 3 ? 9 : 17);
            if (i + 4 + xing_offset + 12 < read_bytes) {
                const uint8_t* xing_ptr = scan_buf + i + 4 + xing_offset;
                if (std::memcmp(xing_ptr, "Xing", 4) == 0 || std::memcmp(xing_ptr, "Info", 4) == 0) {
                    uint32_t flags = read_be32(xing_ptr + 4);
                    if (flags & 0x01) {
                        uint32_t total_frames = read_be32(xing_ptr + 8);
                        int samples_per_frame = (lay_idx == 3) ? 384 : ((ver_idx == 3) ? 1152 : 576);
                        return static_cast<uint32_t>((uint64_t(total_frames) * samples_per_frame) / sample_rate);
                    }
                }
            }
            long long audio_bytes = (file_size > offset) ? (file_size - offset) : 0;
            return static_cast<uint32_t>((audio_bytes * 8) / (bitrate_kbps * 1000));
        }
    }
    return 0;
}

// FLAC STREAMINFO block.
uint32_t parse_flac_duration(int fd) {
    uint8_t buf[42];
    if (pread_at(fd, buf, 42, 0) != 42 || std::memcmp(buf, "fLaC", 4) != 0 || (buf[4] & 0x7F) != 0) return 0;
    uint32_t sample_rate = (uint32_t(buf[18]) << 12) | (uint32_t(buf[19]) << 4) | (uint32_t(buf[20]) >> 4);
    uint64_t total_samples = (uint64_t(buf[20] & 0x0F) << 32) | (uint64_t(buf[21]) << 24) |
                              (uint64_t(buf[22]) << 16) | (uint64_t(buf[23]) << 8) | uint64_t(buf[24]);
    return (sample_rate > 0) ? static_cast<uint32_t>(total_samples / sample_rate) : 0;
}

// ISO-8859-1 (Latin-1): every byte is that exact Unicode codepoint, so this
// is just the standard 1-byte-in, 1-or-2-bytes-out UTF-8 encoding rule.
std::string latin1_to_utf8(const uint8_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c < 0x80) out += static_cast<char>(c);
        else { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return out;
}

// UTF-16 (either endianness, BOM already stripped by the caller) to UTF-8.
// Handles surrogate pairs for completeness, even though a title/artist/
// album tag needing a codepoint outside the BMP is essentially unheard of.
std::string utf16_to_utf8(const uint8_t* p, size_t n, bool big_endian) {
    std::string out;
    auto unit = [&](size_t i) -> uint32_t {
        return big_endian ? (uint32_t(p[i]) << 8 | p[i + 1]) : (uint32_t(p[i + 1]) << 8 | p[i]);
    };
    size_t i = 0;
    while (i + 1 < n) {
        uint32_t cp = unit(i);
        i += 2;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n) {
            uint32_t lo = unit(i);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        // a surrogate left without its partner cannot be encoded as UTF-8 (it would come out as three invalid
        // bytes, measured and drawn differently): it becomes one U+FFFD
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// Decodes one ID3v2 text-frame payload (encoding byte + raw text, exactly
// as stored -- no frame-header stripping, no null-terminator trimming
// beyond what's needed to drop trailing padding) into a UTF-8 std::string.
std::string decode_id3_text(const uint8_t* payload, size_t len) {
    if (len == 0) return {};
    uint8_t encoding = payload[0];
    const uint8_t* text = payload + 1;
    size_t text_len = len - 1;
    std::string out;
    switch (encoding) {
        case 0x00: { // ISO-8859-1
            size_t nul = 0; while (nul < text_len && text[nul] != 0) ++nul;
            out = latin1_to_utf8(text, nul);
            break;
        }
        case 0x03: { // UTF-8 (2.4 only)
            size_t nul = 0; while (nul < text_len && text[nul] != 0) ++nul;
            out.assign(reinterpret_cast<const char*>(text), nul);
            break;
        }
        case 0x01: { // UTF-16 with BOM
            bool big_endian = false;
            size_t start = 0;
            if (text_len >= 2 && text[0] == 0xFE && text[1] == 0xFF) { big_endian = true; start = 2; }
            else if (text_len >= 2 && text[0] == 0xFF && text[1] == 0xFE) { big_endian = false; start = 2; }
            size_t nul = start;
            while (nul + 1 < text_len && !(text[nul] == 0 && text[nul + 1] == 0)) nul += 2;
            out = utf16_to_utf8(text + start, nul - start, big_endian);
            break;
        }
        case 0x02: { // UTF-16BE, no BOM (2.4 only)
            size_t nul = 0;
            while (nul + 1 < text_len && !(text[nul] == 0 && text[nul + 1] == 0)) nul += 2;
            out = utf16_to_utf8(text, nul, /*big_endian=*/true);
            break;
        }
        default:
            return {}; // unknown encoding byte -- leave for ffprobe to sort out
    }
    return out;
}

} // namespace

NativeId3Tags probe_id3v2_native(const fs::path& path) {
    NativeId3Tags result;
    int fd = open_for_probe(path);
    if (fd < 0) return result;

    uint8_t hdr[10];
    if (pread_at(fd, hdr, 10, 0) != 10 || std::memcmp(hdr, "ID3", 3) != 0) {
        close_probe(fd); // no ID3v2 tag at all -- caller falls back to ffprobe (may still have ID3v1)
        return result;
    }
    uint8_t major = hdr[3];
    uint8_t header_flags = hdr[5];
    uint32_t tag_size = ((hdr[6] & 0x7F) << 21) | ((hdr[7] & 0x7F) << 14) |
                         ((hdr[8] & 0x7F) << 7) | (hdr[9] & 0x7F);

    // Bail on anything this simple walker doesn't handle rather than risk
    // misparsing: unsupported major version, whole-tag unsynchronisation
    // (bit 0x80) or an extended header (bit 0x40) both shift every frame
    // offset in a way a plain frame walk doesn't account for.
    if ((major != 3 && major != 4) || (header_flags & 0xC0) != 0) {
        close_probe(fd);
        return result;
    }
    // Cap the read: text frames are always written before any embedded
    // cover art in practice, and a multi-megabyte APIC frame is exactly
    // the kind of tag this should stay fast in front of.
    constexpr uint32_t kReadCap = 2 * 1024 * 1024;
    uint32_t to_read = std::min(tag_size, kReadCap);
    std::vector<uint8_t> body(to_read);
    if (to_read > 0 && pread_at(fd, body.data(), to_read, 10) != static_cast<long long>(to_read)) {
        close_probe(fd);
        return result;
    }
    close_probe(fd);

    size_t off = 0;
    while (off + 10 <= body.size()) {
        const uint8_t* fh = body.data() + off;
        if (fh[0] == 0) break; // padding reached
        char frame_id[5] = {static_cast<char>(fh[0]), static_cast<char>(fh[1]),
                             static_cast<char>(fh[2]), static_cast<char>(fh[3]), 0};
        uint32_t frame_size;
        if (major == 4) {
            frame_size = ((fh[4] & 0x7F) << 21) | ((fh[5] & 0x7F) << 14) |
                         ((fh[6] & 0x7F) << 7) | (fh[7] & 0x7F);
        } else {
            frame_size = read_be32(fh + 4);
        }
        uint8_t frame_flags2 = fh[9]; // format-flags byte (2.3 and 2.4 agree on which byte this is)
        off += 10;
        if (frame_size == 0 || off + frame_size > body.size()) break; // malformed / truncated by the read cap

        // Compression (0x08) or encryption (0x04) rearrange or hide the
        // payload in ways worth just skipping rather than parsing wrong.
        bool skip_frame = (frame_flags2 & 0x0C) != 0;
        const uint8_t* payload = body.data() + off;
        size_t payload_len = frame_size;
        if (!skip_frame) {
            // Grouping (0x40) prepends a 1-byte group id; a data-length
            // indicator (0x01, 2.4 only) prepends a 4-byte size. Neither
            // affects finding title/artist/album text, just where it starts.
            size_t extra = 0;
            if (frame_flags2 & 0x40) extra += 1;
            if (major == 4 && (frame_flags2 & 0x01)) extra += 4;
            if (extra < payload_len) { payload += extra; payload_len -= extra; }
            else payload_len = 0;

            if (std::strcmp(frame_id, "TIT2") == 0) result.title = decode_id3_text(payload, payload_len);
            else if (std::strcmp(frame_id, "TPE1") == 0) result.artist = decode_id3_text(payload, payload_len);
            else if (std::strcmp(frame_id, "TALB") == 0) result.album = decode_id3_text(payload, payload_len);
            // TYER carries the year in ID3v2.3, TDRC the full recording
            // date in ID3v2.4 -- only one of the two is ever present, so
            // first-wins is the right merge rule here.
            else if ((std::strcmp(frame_id, "TYER") == 0 || std::strcmp(frame_id, "TDRC") == 0) &&
                     result.year.empty()) {
                result.year = year_from_tag(decode_id3_text(payload, payload_len));
            }
        }
        off += frame_size;
    }

    result.resolved = true; // walked the tag to completion, whatever it did or didn't contain
    return result;
}

// ---------------------------------------------------------------------
// FLAC / Ogg Vorbis / Opus: shared "Vorbis comment" list format --
// vendor_length(4 LE) + vendor string + comment_count(4 LE), then that
// many length-prefixed "KEY=value" (UTF-8) entries. Returns false on any
// truncation (including one caused by a caller-side read cap) rather than
// report a partial answer that might be missing exactly the field being
// looked for.
// ---------------------------------------------------------------------
namespace {
bool parse_vorbis_comment_list(const uint8_t* data, size_t len, NativeId3Tags& out) {
    if (len < 4) return false;
    size_t off = 0;
    uint32_t vendor_len = read_le32(data + off); off += 4;
    if (off + vendor_len > len) return false;
    off += vendor_len;
    if (off + 4 > len) return false;
    uint32_t count = read_le32(data + off); off += 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (off + 4 > len) return false;
        uint32_t clen = read_le32(data + off); off += 4;
        if (off + clen > len) return false;
        const char* p = reinterpret_cast<const char*>(data + off);
        size_t eq = 0;
        while (eq < clen && p[eq] != '=') ++eq;
        if (eq < clen) {
            std::string key(p, eq);
            for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            std::string val(p + eq + 1, clen - eq - 1);
            if (key == "TITLE" && out.title.empty()) out.title = val;
            else if (key == "ARTIST" && out.artist.empty()) out.artist = val;
            else if (key == "ALBUM" && out.album.empty()) out.album = val;
            // DATE is the Vorbis/FLAC standard for the year; YEAR is the
            // (common) tagger convention. First-wins, like the others.
            else if ((key == "DATE" || key == "YEAR") && out.year.empty()) out.year = year_from_tag(val);
        }
        off += clen;
    }
    return true; // fully parsed, whether or not any of the three keys were present
}

} // namespace

NativeId3Tags probe_flac_native(const fs::path& path) {
    NativeId3Tags result;
    int fd = open_for_probe(path);
    if (fd < 0) return result;

    uint8_t magic[4];
    if (pread_at(fd, magic, 4, 0) != 4 || std::memcmp(magic, "fLaC", 4) != 0) {
        close_probe(fd);
        return result;
    }
    // Text tags are always written well before any embedded cover art in
    // practice; capping here keeps this fast without needing to actually
    // read a multi-megabyte PICTURE block that might follow.
    constexpr uint32_t kReadCap = 2 * 1024 * 1024;
    long long offset = 4;
    for (int guard = 0; guard < 64; ++guard) { // sane bound on metadata block count
        uint8_t bhdr[4];
        if (pread_at(fd, bhdr, 4, offset) != 4) { close_probe(fd); return result; } // truncated/corrupt
        bool is_last = (bhdr[0] & 0x80) != 0;
        uint8_t block_type = bhdr[0] & 0x7F;
        uint32_t block_len = (uint32_t(bhdr[1]) << 16) | (uint32_t(bhdr[2]) << 8) | uint32_t(bhdr[3]);
        offset += 4;
        if (block_type == 4) { // VORBIS_COMMENT
            uint32_t to_read = std::min(block_len, kReadCap);
            std::vector<uint8_t> buf(to_read);
            if (to_read > 0 && pread_at(fd, buf.data(), to_read, offset) != static_cast<long long>(to_read)) {
                close_probe(fd);
                return result;
            }
            close_probe(fd);
            if (to_read < block_len) return result; // capped -- bail rather than risk a missed field
            if (!parse_vorbis_comment_list(buf.data(), buf.size(), result)) return result;
            result.resolved = true;
            return result;
        }
        offset += block_len;
        if (is_last) break;
    }
    close_probe(fd);
    result.resolved = true; // walked every metadata block, genuinely no VORBIS_COMMENT block present
    return result;
}

namespace {

// Reassembles the first two logical packets (identification header, then
// comment header) out of an Ogg bitstream's pages, following the lacing
// values in each page's segment table so a packet that spans multiple
// pages (large embedded cover art pushing the comment packet past one
// page) is still reconstructed correctly rather than truncated at a page
// boundary. Returns false (bail to ffprobe) on anything that doesn't look
// like a well-formed Ogg stream, or if two full packets aren't available
// within the read cap.
bool collect_first_two_ogg_packets(int fd, long long file_size,
                                    std::vector<uint8_t>& packet0, std::vector<uint8_t>& packet1) {
    constexpr long long kCap = 2 * 1024 * 1024;
    long long offset = 0;
    int packet_index = 0;
    std::vector<uint8_t> current;
    while (offset + 27 <= file_size && offset < kCap) {
        uint8_t page_hdr[27];
        if (pread_at(fd, page_hdr, 27, offset) != 27) return false;
        if (std::memcmp(page_hdr, "OggS", 4) != 0) return false;
        uint8_t num_segments = page_hdr[26];
        std::vector<uint8_t> seg_table(num_segments);
        if (num_segments > 0 && pread_at(fd, seg_table.data(), num_segments, offset + 27) != num_segments)
            return false;
        long long pos = offset + 27 + num_segments;

        size_t i = 0;
        while (i < seg_table.size()) {
            long long packet_start = pos;
            long long packet_len = 0;
            bool terminated = false;
            while (i < seg_table.size()) {
                uint8_t seg = seg_table[i++];
                packet_len += seg;
                pos += seg;
                if (seg < 255) { terminated = true; break; }
            }
            if (packet_len > 0) {
                size_t old_size = current.size();
                current.resize(old_size + static_cast<size_t>(packet_len));
                if (pread_at(fd, current.data() + old_size, static_cast<size_t>(packet_len), packet_start) != packet_len)
                    return false;
            }
            if (terminated) {
                if (packet_index == 0) { packet0 = std::move(current); current.clear(); packet_index = 1; }
                else if (packet_index == 1) { packet1 = std::move(current); return true; }
            }
            // else: packet continues on the next page -- keep accumulating into `current`
        }
        offset = pos; // next page begins right after this page's segment data
    }
    return false; // ran out of pages/cap before completing two packets
}

} // namespace

NativeId3Tags probe_ogg_native(const fs::path& path) {
    NativeId3Tags result;
    int fd = open_for_probe(path);
    if (fd < 0) return result;
    long long file_size = file_size_of(fd);
    if (file_size <= 0) { close_probe(fd); return result; }

    std::vector<uint8_t> packet0, packet1;
    bool ok = collect_first_two_ogg_packets(fd, file_size, packet0, packet1);
    close_probe(fd);
    if (!ok) return result;

    if (packet0.size() >= 8 && std::memcmp(packet0.data(), "OpusHead", 8) == 0) {
        if (packet1.size() < 8 || std::memcmp(packet1.data(), "OpusTags", 8) != 0) return result;
        if (!parse_vorbis_comment_list(packet1.data() + 8, packet1.size() - 8, result)) return result;
        result.resolved = true;
        return result;
    }
    if (packet0.size() >= 7 && packet0[0] == 0x01 && std::memcmp(packet0.data() + 1, "vorbis", 6) == 0) {
        if (packet1.size() < 7 || packet1[0] != 0x03 || std::memcmp(packet1.data() + 1, "vorbis", 6) != 0)
            return result;
        if (!parse_vorbis_comment_list(packet1.data() + 7, packet1.size() - 7, result)) return result;
        result.resolved = true;
        return result;
    }
    return result; // some other codec in an Ogg container (Theora, FLAC-in-Ogg, Speex, ...) -- fall back
}

namespace {

// One "box"/atom's content region, referencing into an already-read buffer
// -- never owns memory, never copies.
struct AtomRef {
    const uint8_t* data = nullptr;
    uint64_t size = 0;
};

// Finds a direct child atom by 4-character code within [buf, buf+len).
// Handles the 64-bit "extended size" header (size field == 1) and the
// "extends to end of parent" convention (size field == 0); anything that
// doesn't fit within the given range stops the walk rather than read past
// it.
AtomRef find_atom(const uint8_t* buf, uint64_t len, const char name[4]) {
    uint64_t off = 0;
    while (off + 8 <= len) {
        uint32_t sz32 = read_be32(buf + off);
        uint64_t hdr_size = 8;
        uint64_t atom_size;
        if (sz32 == 1) {
            if (off + 16 > len) break;
            atom_size = read_be64(buf + off + 8);
            hdr_size = 16;
        } else if (sz32 == 0) {
            atom_size = len - off;
        } else {
            atom_size = sz32;
        }
        if (atom_size < hdr_size || off + atom_size > len) break;
        if (std::memcmp(buf + off + 4, name, 4) == 0) return { buf + off + hdr_size, atom_size - hdr_size };
        off += atom_size;
    }
    return {};
}

} // namespace

NativeId3Tags probe_mp4_native(const fs::path& path) {
    NativeId3Tags result;
    int fd = open_for_probe(path);
    if (fd < 0) return result;
    long long file_size = file_size_of(fd);
    if (file_size <= 0) { close_probe(fd); return result; }

    // Walk top-level atoms looking for 'moov', skipping over everything
    // else (in particular 'mdat', the actual audio data, which can be the
    // large majority of the file) by header alone -- never reading their
    // contents.
    constexpr uint64_t kMoovCap = 4 * 1024 * 1024;
    long long offset = 0;
    std::vector<uint8_t> moov_buf;
    while (offset + 8 <= file_size) {
        uint8_t hdr[16];
        long long hdr_read = pread_at(fd, hdr, 16, offset);
        if (hdr_read < 8) break;
        uint32_t sz32 = read_be32(hdr);
        uint64_t hdr_size = 8;
        uint64_t atom_size;
        if (sz32 == 1) {
            if (hdr_read < 16) break;
            atom_size = read_be64(hdr + 8);
            hdr_size = 16;
        } else if (sz32 == 0) {
            atom_size = static_cast<uint64_t>(file_size - offset);
        } else {
            atom_size = sz32;
        }
        if (atom_size < hdr_size) break;
        if (std::memcmp(hdr + 4, "moov", 4) == 0) {
            uint64_t content_len = atom_size - hdr_size;
            uint64_t to_read = std::min<uint64_t>(content_len, kMoovCap);
            moov_buf.resize(to_read);
            if (to_read > 0 &&
                pread_at(fd, moov_buf.data(), to_read, offset + static_cast<long long>(hdr_size)) !=
                    static_cast<long long>(to_read)) {
                close_probe(fd);
                return result; // truncated read -- bail to ffprobe
            }
            if (to_read < content_len) { close_probe(fd); return result; } // moov bigger than our cap -- bail
            break;
        }
        offset += static_cast<long long>(atom_size);
    }
    close_probe(fd);
    if (moov_buf.empty()) return result; // no 'moov' found at all -- not a container we understand here

    AtomRef udta = find_atom(moov_buf.data(), moov_buf.size(), "udta");
    if (!udta.data) { result.resolved = true; return result; } // valid moov, no iTunes-style tags at all
    AtomRef meta = find_atom(udta.data, udta.size, "meta");
    if (!meta.data) { result.resolved = true; return result; }
    if (meta.size < 4) { result.resolved = true; return result; } // 'meta' is a full box: 4 bytes version+flags first
    AtomRef ilst = find_atom(meta.data + 4, meta.size - 4, "ilst");
    if (!ilst.data) { result.resolved = true; return result; }

    auto extract_text = [&](const char tag[4]) -> std::string {
        AtomRef item = find_atom(ilst.data, ilst.size, tag);
        if (!item.data) return {};
        AtomRef data_atom = find_atom(item.data, item.size, "data");
        if (!data_atom.data || data_atom.size < 8) return {};
        uint32_t type_indicator = read_be32(data_atom.data);
        if (type_indicator != 1) return {}; // not plain UTF-8 text -- skip rather than misdecode
        return std::string(reinterpret_cast<const char*>(data_atom.data + 8),
                            static_cast<size_t>(data_atom.size - 8));
    };

    result.title = extract_text("\xA9" "nam");
    result.artist = extract_text("\xA9" "ART");
    result.album = extract_text("\xA9" "alb");
    result.year = year_from_tag(extract_text("\xA9" "day")); // MP4/M4A stores the year in ©day
    result.resolved = true;
    return result;
}

namespace {

// A WAV file (RIFF / WAVE): the chunk list is walked by its headers alone (the audio is never read), for the `fmt ` chunk (the duration) and a
// LIST / INFO chunk (title, artist, album, year). `ok` only when the whole list was walked and nothing unusual was met: a chunk that runs out of
// the file, a `data` chunk with no size or a size the file does not have (a streamed or cut file), an ID3 tag chunk (ffprobe reads that one), a
// huge LIST, more than 256 chunks. Everything else is left to ffprobe, as before.
struct WavInfo {
    bool ok = false;
    uint32_t seconds = 0;   // 0: unknown (a format whose bytes per second are only an average, or no `fmt `)
    std::string title, artist, album, year;
};

WavInfo walk_wav(int fd, long long file_size) {
    WavInfo w;
    uint8_t hdr[12];
    if (file_size < 12 || pread_at(fd, hdr, 12, 0) != 12 || std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return w;
    const uint32_t riff = read_le32(hdr + 4);
    const long long limit = (riff >= 4 && riff != 0xFFFFFFFFu && 8 + static_cast<long long>(riff) <= file_size) ? 8 + static_cast<long long>(riff) : file_size;
    long long pos = 12;
    uint32_t tag = 0, byte_rate = 0;
    long long data_bytes = -1;
    int n = 0;
    for (; n < 256 && pos + 8 <= limit; ++n) {
        uint8_t ch[8];
        if (pread_at(fd, ch, 8, pos) != 8) return w;
        const uint32_t sz = read_le32(ch + 4);
        const long long body = pos + 8;
        if (std::memcmp(ch, "data", 4) == 0) {
            if (sz == 0 || sz == 0xFFFFFFFFu || body + static_cast<long long>(sz) > limit) return w;
            data_bytes = sz;
        } else {
            if (body + static_cast<long long>(sz) > limit) return w;
            if (std::memcmp(ch, "fmt ", 4) == 0) {
                uint8_t f[16];
                if (sz < 16 || pread_at(fd, f, 16, body) != 16) return w;
                tag = uint32_t(f[0]) | (uint32_t(f[1]) << 8);
                byte_rate = read_le32(f + 8);
            } else if (std::memcmp(ch, "id3 ", 4) == 0 || std::memcmp(ch, "ID3 ", 4) == 0) {
                return w;
            } else if (std::memcmp(ch, "LIST", 4) == 0 && sz >= 4) {
                if (sz > 65536) return w;
                std::vector<uint8_t> b(sz);
                if (pread_at(fd, b.data(), sz, body) != static_cast<long long>(sz)) return w;
                if (std::memcmp(b.data(), "INFO", 4) == 0) {
                    size_t p = 4;
                    while (p + 8 <= b.size()) {
                        const uint32_t isz = read_le32(&b[p + 4]);
                        if (p + 8 + static_cast<size_t>(isz) > b.size()) break;
                        size_t len = 0;
                        while (len < isz && b[p + 8 + len] != 0) ++len;   // (a text ends at its first NUL)
                        const std::string text(reinterpret_cast<const char*>(&b[p + 8]), len);
                        if (std::memcmp(&b[p], "INAM", 4) == 0 && w.title.empty()) w.title = text;
                        else if (std::memcmp(&b[p], "IART", 4) == 0 && w.artist.empty()) w.artist = text;
                        else if (std::memcmp(&b[p], "IPRD", 4) == 0 && w.album.empty()) w.album = text;
                        else if (std::memcmp(&b[p], "ICRD", 4) == 0 && w.year.empty()) w.year = year_from_tag(text);
                        p += 8 + static_cast<size_t>(isz) + (isz & 1);
                    }
                }
            }
        }
        pos = body + static_cast<long long>(sz) + (sz & 1);
    }
    if (n >= 256 && pos + 8 <= limit) return w;   // (too many chunks: not a normal file)
    w.ok = true;
    if (byte_rate > 0 && data_bytes >= 0 && (tag == 1 || tag == 3 || tag == 0xFFFE))   // PCM / float / extensible: the bytes per second are exact
        w.seconds = static_cast<uint32_t>(std::min<long long>(data_bytes / byte_rate, 0xFFFFFFFFLL));
    return w;
}

}  // namespace

uint32_t probe_duration_native(const fs::path& path) {
    int fd = open_for_probe(path);
    if (fd < 0) return 0;
    long long size = file_size_of(fd);
    if (size <= 0) {
        close_probe(fd);
        return 0;
    }
    struct { long long st_size; } st{size};   // keeps the parser calls below unchanged

    uint32_t duration = 0;
    std::string ext = ascii_lower_str(path_utf8(path.extension()));

    if (ext == ".m4a" || ext == ".m4b" || ext == ".mp4" || ext == ".aac") duration = parse_mp4_duration(fd, st.st_size);
    else if (ext == ".opus" || ext == ".ogg") duration = parse_ogg_duration(fd, st.st_size);
    else if (ext == ".mp3") duration = parse_mp3_duration(fd, st.st_size);
    else if (ext == ".flac") duration = parse_flac_duration(fd);
    else if (ext == ".wav") duration = walk_wav(fd, st.st_size).seconds;

    close_probe(fd);
    return duration;
}

NativeId3Tags probe_wav_native(const fs::path& path) {
    NativeId3Tags result;
    int fd = open_for_probe(path);
    if (fd < 0) return result;
    const long long size = file_size_of(fd);
    const WavInfo w = size > 0 ? walk_wav(fd, size) : WavInfo();
    close_probe(fd);
    if (!w.ok) return result;
    result.resolved = true;   // (no INFO list = a file with no tags: that is the answer)
    result.title = w.title;
    result.artist = w.artist;
    result.album = w.album;
    result.year = w.year;
    return result;
}

} // namespace muisc
