// A WAV file is read in-process (native_duration.h): the duration from the `fmt ` / `data` chunks, the tags from a LIST / INFO chunk. Anything unusual is
// declined (resolved = false, duration 0) so that ffprobe still decides it.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "native_duration.h"
#include "test_framework.h"

using namespace muisc;
namespace fs = std::filesystem;

namespace {
void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF); }
void put16(std::string& s, uint32_t v) { for (int i = 0; i < 2; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF); }

std::string chunk(const std::string& id, const std::string& body) {
    std::string c = id;
    put32(c, static_cast<uint32_t>(body.size()));
    c += body;
    if (body.size() & 1) c += '\0';
    return c;
}
std::string fmt_chunk(uint32_t tag, uint32_t channels, uint32_t rate, uint32_t bits) {
    std::string b;
    put16(b, tag); put16(b, channels); put32(b, rate); put32(b, rate * channels * bits / 8); put16(b, channels * bits / 8); put16(b, bits);
    return chunk("fmt ", b);
}
std::string info_chunk(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string b = "INFO";
    for (const auto& f : fields) b += chunk(f.first, f.second + std::string(1, '\0'));
    return chunk("LIST", b);
}
std::string riff(const std::string& chunks) {
    std::string r = "RIFF";
    put32(r, static_cast<uint32_t>(4 + chunks.size()));
    r += "WAVE";
    r += chunks;
    return r;
}
fs::path write_file(const fs::path& dir, const std::string& name, const std::string& bytes) {
    const fs::path p = dir / name;
    std::ofstream o(p, std::ios::binary);
    o.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return p;
}
}  // namespace

TEST(native_wav_reads_the_duration_and_the_info_tags) {
    mt::TempHome home;
    const std::string audio(44100 * 4 * 3 + 100, '\x11');   // 3 seconds and a bit of 16-bit stereo at 44.1 kHz
    // tags before the audio
    const fs::path a = write_file(home.path(), "a.wav", riff(fmt_chunk(1, 2, 44100, 16) + info_chunk({{"INAM", "Blue Moon"}, {"IART", "Ella"}, {"IPRD", "Songs"}, {"ICRD", "1999-05-01"}}) + chunk("data", audio)));
    CHECK_EQ(probe_duration_native(a), 3u);
    const NativeId3Tags ta = probe_wav_native(a);
    CHECK(ta.resolved);
    CHECK_EQ(ta.title, std::string("Blue Moon"));
    CHECK_EQ(ta.artist, std::string("Ella"));
    CHECK_EQ(ta.album, std::string("Songs"));
    CHECK_EQ(ta.year, std::string("1999"));
    // tags after the audio (where most programs write them)
    const fs::path b = write_file(home.path(), "b.wav", riff(fmt_chunk(1, 2, 44100, 16) + chunk("data", audio) + info_chunk({{"INAM", "Odd"}, {"IART", "Me"}})));
    CHECK_EQ(probe_duration_native(b), 3u);
    const NativeId3Tags tb = probe_wav_native(b);
    CHECK(tb.resolved);
    CHECK_EQ(tb.title, std::string("Odd"));   // (an odd length text is padded to an even size)
    CHECK_EQ(tb.artist, std::string("Me"));
    CHECK(tb.album.empty());
}

TEST(native_wav_without_tags_is_resolved_with_empty_fields) {
    mt::TempHome home;
    const std::string audio(48000 * 2 * 2, '\x05');   // 2 seconds, 16-bit mono at 48 kHz -> 192000 bytes
    const fs::path p = write_file(home.path(), "plain.wav", riff(fmt_chunk(1, 1, 48000, 16) + chunk("data", audio.substr(0, 48000 * 2 * 2))));
    CHECK_EQ(probe_duration_native(p), 2u);
    const NativeId3Tags t = probe_wav_native(p);
    CHECK(t.resolved);
    CHECK(t.title.empty() && t.artist.empty() && t.album.empty() && t.year.empty());
}

TEST(native_wav_declines_what_it_does_not_fully_understand) {
    mt::TempHome home;
    const std::string audio(44100 * 4, '\x22');
    // an ID3 tag chunk: ffprobe reads that one
    const fs::path id3 = write_file(home.path(), "id3.wav", riff(fmt_chunk(1, 2, 44100, 16) + chunk("data", audio) + chunk("id3 ", std::string("ID3\x03\0\0\0\0\0\0", 10))));
    CHECK(!probe_wav_native(id3).resolved);
    // a data chunk with no size (a streamed file) or more than the file has (a cut file)
    std::string streamed = "RIFF";
    put32(streamed, 0xFFFFFFFFu);
    streamed += "WAVE" + fmt_chunk(1, 2, 44100, 16) + "data";
    put32(streamed, 0xFFFFFFFFu);
    streamed += audio;
    const fs::path st = write_file(home.path(), "streamed.wav", streamed);
    CHECK(!probe_wav_native(st).resolved);
    CHECK_EQ(probe_duration_native(st), 0u);
    std::string cut = riff(fmt_chunk(1, 2, 44100, 16) + chunk("data", audio));
    cut.resize(cut.size() - 1000);
    const fs::path ct = write_file(home.path(), "cut.wav", cut);
    CHECK(!probe_wav_native(ct).resolved);
    // not a WAV at all / empty / a RIFF of another kind
    CHECK(!probe_wav_native(write_file(home.path(), "junk.wav", std::string(500, 'x'))).resolved);
    CHECK(!probe_wav_native(write_file(home.path(), "empty.wav", "")).resolved);
    std::string avi = "RIFF";
    put32(avi, 8);
    avi += "AVI LIST";
    CHECK(!probe_wav_native(write_file(home.path(), "avi.wav", avi)).resolved);
    CHECK(!probe_wav_native(home.path() / "missing.wav").resolved);
    // a chunk that runs out of the file
    std::string junk_header = "junk";
    put32(junk_header, 100000);   // (a chunk header that promises 100000 bytes the file does not have)
    const fs::path bg = write_file(home.path(), "big.wav", riff(fmt_chunk(1, 2, 44100, 16) + junk_header));
    CHECK(!probe_wav_native(bg).resolved);
}

TEST(native_wav_knows_no_duration_for_a_format_with_an_average_byte_rate) {
    mt::TempHome home;
    const std::string audio(44100 * 4, '\x33');
    // MS ADPCM (format 2): the bytes per second are only an average, ffprobe has the exact length -- the tags are still read here
    const fs::path p = write_file(home.path(), "adpcm.wav", riff(fmt_chunk(2, 2, 44100, 4) + info_chunk({{"INAM", "Squeeze"}}) + chunk("data", audio)));
    CHECK_EQ(probe_duration_native(p), 0u);
    const NativeId3Tags t = probe_wav_native(p);
    CHECK(t.resolved);
    CHECK_EQ(t.title, std::string("Squeeze"));
    // float samples (format 3) are exact
    const fs::path f = write_file(home.path(), "float.wav", riff(fmt_chunk(3, 2, 44100, 32) + chunk("data", std::string(44100 * 8 * 2, '\x01'))));
    CHECK_EQ(probe_duration_native(f), 2u);
}
