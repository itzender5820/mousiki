// Album covers: finds the best picture for a track (inside the file or a close-named picture in its
// folder), decodes it with ffmpeg, quantises the colours and scales it to the disk's place.
#include "cover_art.h"
#include "bg_tasks.h"
#include <set>
#include "book_art.h"
#include "cover_store.h"
#include "console_log.h"
#include "path_utf8.h"
#include "process_util.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cctype>
#include <chrono>
#include <cmath>
#include <future>
#include <map>
#include <mutex>
#include <filesystem>
#include <memory>
#include <sstream>

namespace muisc {

// ---- scaling ---------------------------------------------------------------------------------------------------------------
void quantize_colours(const std::vector<unsigned char>& rgb, const std::vector<unsigned char>& alpha, int n,
                      std::array<std::array<uint8_t, 3>, 256>& pal, std::vector<unsigned char>& hue) {
    const size_t px = alpha.size();
    std::vector<unsigned int> hist(32768, 0);
    for (size_t i = 0; i < px; ++i)
        if (alpha[i] >= 3) ++hist[(static_cast<size_t>(rgb[i * 3] >> 3) << 10) | (static_cast<size_t>(rgb[i * 3 + 1] >> 3) << 5) | (rgb[i * 3 + 2] >> 3)];
    std::vector<int> bins;
    for (int b = 0; b < 32768; ++b) if (hist[static_cast<size_t>(b)]) bins.push_back(b);
    auto chan = [](int b, int c) { return c == 0 ? (b >> 10) & 31 : c == 1 ? (b >> 5) & 31 : b & 31; };
    std::vector<std::vector<int>> boxes;
    if (!bins.empty()) boxes.push_back(bins);
    while (static_cast<int>(boxes.size()) < n) {
        int best = -1, best_c = 0, best_range = 0;
        for (size_t k = 0; k < boxes.size(); ++k) {
            if (boxes[k].size() < 2) continue;
            for (int c = 0; c < 3; ++c) {
                int lo = 31, hi = 0;
                for (int b : boxes[k]) { lo = std::min(lo, chan(b, c)); hi = std::max(hi, chan(b, c)); }
                if (hi - lo > best_range) { best_range = hi - lo; best = static_cast<int>(k); best_c = c; }
            }
        }
        if (best < 0) break;
        std::vector<int> v = std::move(boxes[static_cast<size_t>(best)]);
        std::sort(v.begin(), v.end(), [&](int a, int b) { return chan(a, best_c) < chan(b, best_c); });
        unsigned long long total = 0, acc = 0;
        for (int b : v) total += hist[static_cast<size_t>(b)];
        size_t cut = 1;
        for (size_t i = 0; i + 1 < v.size(); ++i) { acc += hist[static_cast<size_t>(v[i])]; cut = i + 1; if (acc * 2 >= total) break; }
        boxes[static_cast<size_t>(best)] = std::vector<int>(v.begin(), v.begin() + static_cast<long>(cut));
        boxes.emplace_back(v.begin() + static_cast<long>(cut), v.end());
    }
    std::vector<unsigned char> of_bin(32768, 0);
    for (size_t k = 0; k < boxes.size(); ++k) {
        double sum[3] = {0, 0, 0}, cnt = 0;
        for (int b : boxes[k]) {
            const double c = hist[static_cast<size_t>(b)];
            for (int ch = 0; ch < 3; ++ch) sum[ch] += c * ((chan(b, ch) << 3) | (chan(b, ch) >> 2));
            cnt += c;
            of_bin[static_cast<size_t>(b)] = static_cast<unsigned char>(k);
        }
        for (int ch = 0; ch < 3; ++ch) pal[k][static_cast<size_t>(ch)] = static_cast<uint8_t>(cnt > 0 ? std::lround(sum[ch] / cnt) : 0);
    }
    hue.assign(px, 0);
    for (size_t i = 0; i < px; ++i)
        if (alpha[i] >= 3) hue[i] = of_bin[(static_cast<size_t>(rgb[i * 3] >> 3) << 10) | (static_cast<size_t>(rgb[i * 3 + 1] >> 3) << 5) | (rgb[i * 3 + 2] >> 3)];
}


// Chromatic aberration: the red plane is scaled up a little, the blue one down, round the middle -- more and more towards the corners
// (the same lens as the video's, VideoPlayer::apply_grade).
static void chroma_planes(std::vector<unsigned char>& red, std::vector<unsigned char>& blue, int w, int h, int level) {
    if (level <= 0 || w < 4 || h < 4 || red.size() < static_cast<size_t>(w) * h || blue.size() < static_cast<size_t>(w) * h) return;
    const float k = level == 1 ? 0.0045f : level == 2 ? 0.009f : 0.016f;
    const std::vector<unsigned char> r0 = red, b0 = blue;
    const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
    auto bil = [&](const std::vector<unsigned char>& p, float x, float y) {
        x = std::clamp(x, 0.0f, static_cast<float>(w - 1)); y = std::clamp(y, 0.0f, static_cast<float>(h - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y), x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
        const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
        auto v = [&](int xx, int yy) { return static_cast<float>(p[static_cast<size_t>(yy) * w + xx]); };
        return static_cast<unsigned char>(std::clamp((v(x0, y0) * (1 - tx) + v(x1, y0) * tx) * (1 - ty) + (v(x0, y1) * (1 - tx) + v(x1, y1) * tx) * ty + 0.5f, 0.0f, 255.0f));
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float dx = (x - cx) / std::max(1.0f, cx), dy = (y - cy) / std::max(1.0f, cy);
            const float g = 1.0f + k * (0.4f + 0.6f * (dx * dx + dy * dy));   // (covers are small: a little less than the video's reach)
            red[static_cast<size_t>(y) * w + x] = bil(r0, cx + (x - cx) * g, cy + (y - cy) * g);
            blue[static_cast<size_t>(y) * w + x] = bil(b0, cx + (x - cx) / g, cy + (y - cy) / g);
        }
}

const CoverScaled& cover_scaled(const CoverImage& c, int max_w, int max_h, bool colour, int contrast, int chroma) {
    static SizeCache<CoverScaled, 4> cache;
    contrast = std::clamp(contrast, 0, 150);
    const std::string key = c.key + "|" + std::to_string(max_w) + "x" + std::to_string(max_h) + (colour ? "c" : "g") + std::to_string(contrast) + (colour && chroma > 0 ? "x" + std::to_string(chroma) : std::string());
    // more contrast: values move away from the middle (0 % = unchanged, 100 % = 2.5 times the distance from the middle grey, 150 % = 3.25 times)
    const double factor = 1.0 + contrast / 100.0 * 1.5;
    auto punch = [&](unsigned char v) { return static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround((v - 127.5) * factor + 127.5)), 0, 255)); };
    if (const CoverScaled* hit = cache.find(key)) return *hit;
    CoverScaled& cs = cache.fresh();
    cs.key = key;
    if (!colour) {
        cs.grey = BookArt::scale_grey(c.level, c.w, c.h, max_w, max_h, cs.w, cs.h);
        int aw = 0, ah = 0;
        cs.alpha = BookArt::scale_grey(c.alpha, c.w, c.h, max_w, max_h, aw, ah);
        if (contrast > 0)
            for (unsigned char& v : cs.grey) if (v) v = std::max<unsigned char>(1, punch(v));
        return cs;
    }
    const size_t n = static_cast<size_t>(c.w) * c.h;
    std::vector<unsigned char> chn[4];
    int ow = 0, oh = 0;
    for (int k = 0; k < 4; ++k) {
        std::vector<unsigned char> plane(n);
        for (size_t i = 0; i < n; ++i) plane[i] = k < 3 ? c.rgb[i * 3 + static_cast<size_t>(k)] : c.alpha[i];
        chn[k] = BookArt::scale_grey(plane, c.w, c.h, max_w, max_h, ow, oh);
    }
    cs.w = ow; cs.h = oh;
    chroma_planes(chn[0], chn[2], ow, oh, chroma);
    const size_t m = static_cast<size_t>(ow) * oh;
    std::vector<unsigned char> rgb(m * 3);
    for (size_t i = 0; i < m; ++i) for (int k = 0; k < 3; ++k) rgb[i * 3 + static_cast<size_t>(k)] = contrast > 0 ? punch(chn[k][i]) : chn[k][i];
    cs.level = chn[3];
    quantize_colours(rgb, cs.level, 240, cs.pal, cs.hue);
    return cs;
}

const CoverRgb& cover_scaled_rgb(const CoverImage& c, int max_w, int max_h, int contrast, int chroma) {
    static SizeCache<CoverRgb, 3> cache;
    contrast = std::clamp(contrast, 0, 150);
    const std::string key = c.key + "|" + std::to_string(max_w) + "x" + std::to_string(max_h) + "|" + std::to_string(contrast) + (chroma > 0 ? "x" + std::to_string(chroma) : std::string());
    if (const CoverRgb* hit = cache.find(key)) return *hit;
    const double factor = 1.0 + contrast / 100.0 * 1.5;
    CoverRgb& cs = cache.fresh();
    cs.key = key;
    const size_t n = static_cast<size_t>(c.w) * c.h;
    int ow = 0, oh = 0;
    std::vector<unsigned char> chn[3];
    for (int k = 0; k < 3; ++k) {
        std::vector<unsigned char> plane(n);
        for (size_t i = 0; i < n; ++i) plane[i] = c.rgb[i * 3 + static_cast<size_t>(k)];
        chn[k] = BookArt::scale_grey(plane, c.w, c.h, max_w, max_h, ow, oh);
    }
    cs.alpha = BookArt::scale_grey(c.alpha, c.w, c.h, max_w, max_h, ow, oh);
    cs.w = ow; cs.h = oh;
    chroma_planes(chn[0], chn[2], ow, oh, chroma);
    const size_t m = static_cast<size_t>(ow) * oh;
    cs.rgb.resize(m * 3);
    for (size_t i = 0; i < m; ++i)
        for (int k = 0; k < 3; ++k) {
            const int v = chn[k][i];
            cs.rgb[i * 3 + static_cast<size_t>(k)] = contrast > 0 ? static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround((v - 127.5) * factor + 127.5)), 0, 255))
                                                                   : static_cast<unsigned char>(v);
        }
    return cs;
}

// ---- the search order ------------------------------------------------------------------------------------------------------
std::vector<CoverStep> cover_steps(int search) {
    switch (search) {
        // (every search but Within File Only ends with FolderAny: any picture of the folder, by name, when nothing else gave one)
        case 1: return {CoverStep::FolderClose, CoverStep::FolderGeneric, CoverStep::Embedded, CoverStep::FolderAny};     // Folder First
        case 2: return {CoverStep::Embedded, CoverStep::FolderClose, CoverStep::FolderAny};                                // Within File First
        case 3: return {CoverStep::FolderClose, CoverStep::FolderGeneric, CoverStep::FolderAny};                           // Image in Folder Only
        case 4: return {CoverStep::Embedded};                                                                                       // Within File Only (never looks at the folder)
        default: return {CoverStep::Embedded, CoverStep::FolderClose, CoverStep::FolderGeneric, CoverStep::FolderAny};     // Default
    }
}

// ---- how close two names are -----------------------------------------------------------------------------------------------
namespace {

// Lower case, everything that is not a letter or digit (ASCII) becomes a single space; UTF-8 bytes stay as they are.
std::string normalise(const std::string& s) {
    std::string o;
    bool space = true;
    for (unsigned char c : s) {
        if (c >= 0x80) { o += static_cast<char>(c); space = false; }
        else if (std::isalnum(c)) { o += static_cast<char>(std::tolower(c)); space = false; }
        else if (!space) { o += ' '; space = true; }
    }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

double lev_ratio(const std::string& a, const std::string& b) {
    if (a.empty() && b.empty()) return 1.0;
    const size_t la = std::min<size_t>(a.size(), 96), lb = std::min<size_t>(b.size(), 96);
    std::vector<int> prev(lb + 1), cur(lb + 1);
    for (size_t j = 0; j <= lb; ++j) prev[j] = static_cast<int>(j);
    for (size_t i = 1; i <= la; ++i) {
        cur[0] = static_cast<int>(i);
        for (size_t j = 1; j <= lb; ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return 1.0 - static_cast<double>(prev[lb]) / static_cast<double>(std::max(la, lb));
}

std::vector<std::string> words(const std::string& n) {
    std::vector<std::string> w;
    std::istringstream in(n);
    std::string t;
    while (in >> t) w.push_back(t);
    return w;
}

} // namespace

double name_closeness(const std::string& a, const std::string& b) {
    const std::string na = normalise(a), nb = normalise(b);
    if (na.empty() || nb.empty()) return 0.0;
    if (na == nb) return 1.0;
    double best = lev_ratio(na, nb);
    // one name inside the other, as whole words ("abbey road front" holds "abbey road"): the shorter the difference, the closer
    const std::string& s = na.size() <= nb.size() ? na : nb;
    const std::string& l = na.size() <= nb.size() ? nb : na;
    if (s.size() >= 3 && (" " + l + " ").find(" " + s + " ") != std::string::npos)
        best = std::max(best, 0.70 + 0.25 * static_cast<double>(s.size()) / static_cast<double>(l.size()));
    // words: how many of them have a near partner in the other name (a typo or a plural does not matter)
    const std::vector<std::string> wa = words(na), wb = words(nb);
    if (!wa.empty() && !wb.empty()) {
        std::vector<bool> used(wb.size(), false);
        int matched = 0;
        for (const std::string& x : wa) {
            int pick = -1;
            double pr = 0.8;
            for (size_t j = 0; j < wb.size(); ++j) {
                if (used[j]) continue;
                double r = lev_ratio(x, wb[j]);
                // an abbreviation ("cov" for "cover") counts as a near partner too
                const std::string& sh = x.size() <= wb[j].size() ? x : wb[j];
                const std::string& lg = x.size() <= wb[j].size() ? wb[j] : x;
                if (sh.size() >= 3 && lg.compare(0, sh.size(), sh) == 0) r = std::max(r, 0.85);
                if (r >= pr) { pr = r; pick = static_cast<int>(j); }
            }
            if (pick >= 0) { used[static_cast<size_t>(pick)] = true; ++matched; }
        }
        best = std::max(best, 0.95 * 2.0 * matched / static_cast<double>(wa.size() + wb.size()));
    }
    return std::clamp(best, 0.0, 0.99);
}

namespace {

std::string lower_ext(const fs::path& f) {
    std::string e = path_utf8(f.extension());
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool is_picture_file(const fs::path& f) {
    const std::string e = lower_ext(f);
    for (const char* x : kCoverPictureExtensions) if (e == x) return true;
    return false;
}

} // namespace

thread_local bool g_cover_may_wait = false;   // (the background reader of a cover may wait for the library's index; the screen never does)

fs::path pick_folder_picture(const fs::path& dir, const std::vector<std::string>& names, bool generic, bool any, bool fetched) {
    std::error_code ec;
    std::vector<fs::path> pics;
    std::set<fs::path> from_fetched;   // pictures of fetched_covers: they belong to ONE album each, so only the album's name may pick them
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && is_picture_file(it->path())) pics.push_back(it->path());
    if (fetched && !generic && !any) {   // the covers fetched for this folder (named like their album)
        ec.clear();
        for (fs::directory_iterator it(dir / kFetchedCoversDir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && is_picture_file(it->path())) { pics.push_back(it->path()); from_fetched.insert(it->path()); }
        const fs::path home = shared_covers_home(dir);   // a video in video_down: the covers fetched for the songs of the download folder too
        if (!home.empty()) {
            ec.clear();
            for (fs::directory_iterator it(home / kFetchedCoversDir, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec) && is_picture_file(it->path())) { pics.push_back(it->path()); from_fetched.insert(it->path()); }
        }
        if (!names.empty() && !names[0].empty() && cover_outside_library(dir)) {   // no folder of its own (video_down, a cache folder): the fetched covers of the library, by the album's name
            const fs::path lib = fetched_cover_in_library(names[0], g_cover_may_wait);
            if (!lib.empty()) { pics.push_back(lib); from_fetched.insert(lib); }
        }
    }
    if (pics.empty()) return {};
    std::sort(pics.begin(), pics.end());
    if (any) return pics.front();
    if (generic) {
        static const char* const kNames[] = {"cover", "folder", "front", "albumart", "album", "thumb"};
        for (const char* n : kNames)
            for (const fs::path& f : pics) {
                std::string stem = path_utf8(f.stem());
                for (char& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (stem == n) return f;
            }
        return {};
    }
    fs::path best;
    double best_score = 0.0;
    for (const fs::path& f : pics) {
        const std::string stem = path_utf8(f.stem());
        const bool fetched_pic = from_fetched.count(f) > 0;
        for (size_t ni = 0; ni < names.size(); ++ni) {
            const std::string& n = names[ni];
            if (n.empty() || (fetched_pic && ni != 0)) continue;   // (names[0] is the album; a track title must not pick another album's fetched cover)
            const double sc = name_closeness(stem, n);
            if (sc > best_score) { best_score = sc; best = f; }
        }
    }
    ConsoleLog::instance().log_basic("album cover: closest picture in " + path_utf8(dir) + " is " + (best.empty() ? std::string("(none)") : path_utf8(best.filename())) +
                                     " (match " + std::to_string(static_cast<int>(best_score * 100)) + "%, needs " + std::to_string(static_cast<int>(kCoverCloseness * 100)) + "%)");
    return best_score >= kCoverCloseness ? best : fs::path();
}

// ---- decoding --------------------------------------------------------------------------------------------------------------
namespace {

// ffmpeg writes the picture as one PAM image (a tiny header, then RGBA bytes) at most 640 x 640; the grey level of it is
// 0.15 + 0.85 * luminance, times the alpha -- the same as the audio book cover's.
} // namespace
// A picture that ffmpeg gave nothing for: its complaint goes into the console log, once per file (the previews try again later).
static void note_unreadable_picture(const fs::path& file, bool embedded, double seek) {
    static std::mutex mu;
    static std::set<std::string> told;
    const std::string key = path_utf8(file);
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!told.insert(key).second) return;
    }
    Args cmd = {"ffmpeg", "-v", "error", "-nostdin"};
    if (seek > 0.0) { char b[32]; std::snprintf(b, sizeof b, "%.2f", seek); cmd.push_back("-ss"); cmd.push_back(b); }
    cmd.insert(cmd.end(), {"-i", key});
    if (embedded) cmd.push_back("-an");
    cmd.insert(cmd.end(), {"-frames:v", "1", "-f", "null", "-"});
    const ProcResult r = run_capture(cmd, true);
    std::string msg = r.out;
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    const size_t nl = msg.find('\n');
    ConsoleLog::instance().log_basic("picture: ffmpeg gave no picture for " + key + " (exit " + std::to_string(r.exit_code) + "): " + (msg.empty() ? std::string("no message (no video stream?)") : (nl == std::string::npos ? msg : msg.substr(0, nl)).substr(0, 300)));
}

bool decode_cover_file(const fs::path& file, bool embedded, CoverImage& d, double seek) {
    Args cmd = {"ffmpeg", "-v", "error"};
    if (seek > 0.0) { char b[32]; std::snprintf(b, sizeof b, "%.2f", seek); cmd.push_back("-ss"); cmd.push_back(b); }
    cmd.push_back("-i"); cmd.push_back(path_utf8(file));
    if (embedded) cmd.push_back("-an");
    cmd.insert(cmd.end(), {"-vf", "scale=w=640:h=640:force_original_aspect_ratio=decrease", "-frames:v", "1",
                           "-f", "image2pipe", "-vcodec", "pam", "-pix_fmt", "rgba", "-"});
    std::string o;   // (read straight from the process: run_capture would write the picture's bytes into the console log)
    if (std::unique_ptr<ChildProcess> child = spawn_capture(cmd, false)) {
        std::vector<char> buf(1 << 16);
        std::ptrdiff_t n;
        while ((n = child->read(buf.data(), buf.size())) > 0) o.append(buf.data(), static_cast<size_t>(n));
        child->wait();
    }
    if (o.size() < 16 || o.compare(0, 2, "P7") != 0) {
        note_unreadable_picture(file, embedded, seek);
        return false;
    }
    const size_t hdr = o.find("ENDHDR\n");
    if (hdr == std::string::npos) return false;
    int w = 0, h = 0;
    {
        std::istringstream in(o.substr(0, hdr));
        std::string tok;
        while (in >> tok) {
            if (tok == "WIDTH") in >> w;
            else if (tok == "HEIGHT") in >> h;
        }
    }
    const size_t at = hdr + 7;
    if (w < 2 || h < 2 || o.size() < at + static_cast<size_t>(w) * h * 4) return false;
    d.w = w; d.h = h;
    d.level.assign(static_cast<size_t>(w) * h, 0);
    d.rgb.assign(static_cast<size_t>(w) * h * 3, 0);
    d.alpha.assign(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < d.level.size(); ++i) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(o.data()) + at + i * 4;
        const double lum = (0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]) / 255.0;
        const double v = (0.15 + 0.85 * lum) * (p[3] / 255.0) * 255.0;
        d.rgb[i * 3] = p[0]; d.rgb[i * 3 + 1] = p[1]; d.rgb[i * 3 + 2] = p[2]; d.alpha[i] = p[3];
        d.level[i] = p[3] ? static_cast<unsigned char>(std::clamp(static_cast<int>(std::lround(v)), 1, 255)) : 0;
    }
    d.ok = true;
    return true;
}

namespace {
// the names the folder's pictures are compared with: the album, the title and the file's own name without a track number
std::vector<std::string> cover_search_names(const CoverRequest& rq, const fs::path& file) {
    std::vector<std::string> names;
    names.push_back(rq.album);
    names.push_back(rq.title);
    std::string stem = path_utf8(file.stem());
    size_t i = 0;
    while (i < stem.size() && (std::isdigit(static_cast<unsigned char>(stem[i])) || stem[i] == ' ' || stem[i] == '.' || stem[i] == '-' || stem[i] == '_')) ++i;
    names.push_back(i < stem.size() ? stem.substr(i) : stem);
    return names;
}
bool decode_picture(const fs::path& file, bool embedded, CoverImage& d) { return decode_cover_file(file, embedded, d); }
}  // namespace
bool decode_video_snapshot(const fs::path& video, CoverImage& d) {
    double dur = 0.0;
    const ProcResult r = run_capture({"ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path_utf8(video)});
    if (r.ok()) { try { dur = std::stod(r.out); } catch (...) { dur = 0.0; } }
    const double at = dur > 2.0 ? std::min(30.0, dur * 0.1) : 0.0;
    if (decode_cover_file(video, false, d, at)) return true;
    return at > 0.0 && decode_cover_file(video, false, d, 0.0);
}
namespace {

CoverImage load_track_cover(CoverRequest rq, std::string key) {
    g_cover_may_wait = true;
    CoverImage d;
    if (bg_stopping().load()) { d.key = key; return d; }   // (the program ends: nothing is read)
    d.key = key;
    const fs::path file = path_from_utf8(rq.path);
    const fs::path dir = file.parent_path();
    d.dir = path_utf8(dir);
    d.album = rq.album;
    // the names the folder's pictures are compared with: the album, the title and the file's own name without a track number
    const std::vector<std::string> names = cover_search_names(rq, file);
    std::string tried;
    // Settings -> Cover, Disk or Thumbnail: a frame of the track's video first; the usual search (the chosen cover, the file, the folder,
    // the disk) is the fallback when it cannot be read.
    if (!rq.snapshot_video.empty() && !rq.thumb_fallback) {
        CoverImage t;
        t.key = key;
        if (decode_video_snapshot(path_from_utf8(rq.snapshot_video), t)) {
            d = std::move(t);
            d.dir = path_utf8(dir); d.album = rq.album;
            d.source = path_utf8(path_from_utf8(rq.snapshot_video).filename()) + " (video snapshot)";
            ConsoleLog::instance().log_basic("album cover: using a frame of " + rq.snapshot_video);
            return d;
        }
        tried += "the video's frame could not be read; ";
    }
    // What the user chose beats the search (except with "Within File Only", which looks at the files alone): first the picture chosen
    // for this track, then the one chosen for its album name.
    if (rq.search != 4) {
        for (int pass = 0; pass < 3; ++pass) {   // the track's own choice, the choice of the song it is paired with (a video), the album's choice
            if (pass == 1 && rq.pair_path.empty()) continue;
            const std::string choice = pass == 0 ? cover_track_choice(rq.path) : pass == 1 ? cover_track_choice(rq.pair_path) : cover_album_choice(rq.album);
            if (choice.empty()) continue;
            if (choice == kCoverNone) {   // "no cover": the disk
                d.per_track = pass <= 1;
                d.note = "no cover chosen for this " + std::string(pass <= 1 ? "track" : "album");
                ConsoleLog::instance().log_basic("album cover: none (chosen) for " + rq.path);
                return d;
            }
            CoverImage t;
            t.key = key;
            const bool emb = choice == kCoverEmbedded;
            const fs::path pic = emb ? (pass == 1 ? path_from_utf8(rq.pair_path) : file) : path_from_utf8(choice);
            if (decode_picture(pic, emb, t)) {
                d = std::move(t);
                d.dir = path_utf8(dir); d.album = rq.album;
                d.per_track = pass <= 1;
                d.source = emb ? path_utf8(file.filename()) + " (inside the file)" : path_utf8(pic.filename());
                ConsoleLog::instance().log_basic("album cover: using the chosen " + std::string(emb ? "picture inside " : "") + rq.path + (emb ? "" : " -> " + path_utf8(pic)));
                return d;
            }
            tried += "the chosen picture could not be read; ";
        }
    }
    for (CoverStep step : cover_steps(rq.search)) {
        CoverImage t;
        t.key = key;
        if (step == CoverStep::Embedded) {
            if (decode_picture(file, true, t)) {
                d = std::move(t);
                d.dir = path_utf8(dir); d.album = rq.album;
                d.source = path_utf8(file.filename()) + " (inside the file)";
                ConsoleLog::instance().log_basic("album cover: using the picture inside " + rq.path);
                return d;
            }
            tried += "no picture inside the file";
        } else {
            const fs::path pic = pick_folder_picture(dir, names, step == CoverStep::FolderGeneric, step == CoverStep::FolderAny, step == CoverStep::FolderClose);
            if (!pic.empty() && decode_picture(pic, false, t)) {
                d = std::move(t);
                d.dir = path_utf8(dir); d.album = rq.album;
                d.source = path_utf8(pic.filename());
                ConsoleLog::instance().log_basic("album cover: using " + path_utf8(pic));
                return d;
            }
            tried += pic.empty() ? (step == CoverStep::FolderClose ? "no closely named picture in the folder" : step == CoverStep::FolderAny ? "no picture in the folder" : "no cover.jpg / folder.jpg in the folder")
                                 : "ffmpeg could not read " + path_utf8(pic.filename());
        }
        tried += "; ";
    }
    if (!rq.pair_path.empty()) {   // a video: what the song it is paired with would get (its folder, its fetched covers, the picture inside it)
        CoverRequest r2 = rq;
        r2.path = rq.pair_path; r2.pair_path.clear(); r2.snapshot_video.clear(); r2.thumb_fallback = false;
        CoverImage d2 = load_track_cover(r2, key);
        if (d2.ok) { d2.per_track = false; return d2; }
    }
    if (rq.thumb_fallback && !rq.snapshot_video.empty()) {   // no cover anywhere: a frame of the track's video
        CoverImage t;
        t.key = key;
        if (decode_video_snapshot(path_from_utf8(rq.snapshot_video), t)) {
            d = std::move(t);
            d.dir = path_utf8(dir); d.album = rq.album;
            d.source = path_utf8(path_from_utf8(rq.snapshot_video).filename()) + " (video snapshot)";
            ConsoleLog::instance().log_basic("album cover: no cover, using a frame of " + rq.snapshot_video);
            return d;
        }
        tried += "the video's frame could not be read; ";
    }
    if (tried.size() >= 2) tried.resize(tried.size() - 2);
    d.note = tried.empty() ? "nothing to search" : tried;
    ConsoleLog::instance().log_basic("album cover: none for " + rq.path + " (" + d.note + ")");
    return d;
}

// "size:mtime" of a picture file, read at most once a second per file: a picture that was edited in place (same name) is a new picture.
std::string file_stamp(const std::string& file) {
    if (file.empty()) return std::string();
    struct Hit { std::chrono::steady_clock::time_point at; std::string stamp; };
    static std::mutex mu;
    static std::map<std::string, Hit> memo;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(mu);
    auto it = memo.find(file);
    if (it != memo.end() && now - it->second.at < std::chrono::seconds(1)) return it->second.stamp;
    std::error_code ec;
    const fs::path p = path_from_utf8(file);
    const auto sz = fs::file_size(p, ec);
    const auto mt = ec ? fs::file_time_type() : fs::last_write_time(p, ec);
    std::string st = ec ? std::string("-") : std::to_string(static_cast<unsigned long long>(sz)) + ":" + std::to_string(static_cast<long long>(mt.time_since_epoch().count()));
    if (memo.size() > 256) memo.clear();
    memo[file] = {now, st};
    return st;
}

std::string request_key(const CoverRequest& rq) {
    std::string pic;   // the chosen picture file (track, else album): edited in place = a new request
    if (rq.search != 4) {
        std::string c = cover_track_choice(rq.path);
        if (c.empty()) c = cover_album_choice(rq.album);
        if (!c.empty() && c != kCoverNone && c != kCoverEmbedded) pic = file_stamp(c);
    }
    return pic + "\n" + rq.path + "\n" + rq.title + "\n" + rq.album + "\n" + std::to_string(rq.search) + "\n" + std::to_string(rq.store_rev) + "\n" + rq.snapshot_video + (rq.thumb_fallback ? "\nfb" : "") + "\n" + rq.pair_path;
}

// the single slot: the cover that is ready, and the one being read
struct TrackCoverState {
    CoverImage have;
    std::shared_future<CoverImage> pending;
    std::string pending_key;
};
TrackCoverState& state(bool preview = false) {
    static TrackCoverState playing, shown;   // (the preview of an overlay has a slot of its own, see track_cover())
    return preview ? shown : playing;
}

} // namespace

const CoverImage* track_cover(const CoverRequest& rq, bool preview) {
    if (rq.path.empty()) return nullptr;
    TrackCoverState& s = state(preview);
    const std::string key = request_key(rq);
    if (s.pending.valid() && s.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        s.have = s.pending.get();
        s.pending = {};
        s.pending_key.clear();
    }
    if (s.have.key == key) return s.have.ok ? &s.have : nullptr;
    if (!s.pending.valid()) {
        s.pending_key = key;
        s.pending = bg_async([rq, key]() { return load_track_cover(rq, key); });
    }
    // the next track of the same album (in the same folder) is being read: the cover on screen is the one it will get
    const std::string dir = path_utf8(path_from_utf8(rq.path).parent_path());
    if (s.have.ok && !s.have.per_track && !rq.album.empty() && s.have.album == rq.album && s.have.dir == dir) return &s.have;
    return nullptr;
}

std::string cover_note(const CoverRequest& rq) {
    TrackCoverState& s = state();
    if (s.have.key == request_key(rq) && !s.have.ok) return s.have.note;
    return "";
}

std::string cover_source() {
    const TrackCoverState& s = state();
    return s.have.ok ? s.have.source : std::string();
}

// Which picture a track would get (the same order as load_track_cover, without decoding): for the overlays' previews.
CoverSource resolve_cover_source(const CoverRequest& rq) {
    CoverSource out;
    const fs::path file = path_from_utf8(rq.path);
    const fs::path dir = file.parent_path();
    if (rq.search != 4) {
        for (int pass = 0; pass < 3; ++pass) {   // the track's own choice, the choice of the song it is paired with (a video), the album's choice
            if (pass == 1 && rq.pair_path.empty()) continue;
            const std::string choice = pass == 0 ? (rq.ignore_track_choice ? std::string() : cover_track_choice(rq.path))
                                     : pass == 1 ? (rq.ignore_track_choice ? std::string() : cover_track_choice(rq.pair_path)) : cover_album_choice(rq.album);
            if (choice.empty()) continue;
            out.chosen = true;
            if (choice == kCoverNone) { out.none = true; out.label = "no cover chosen"; return out; }
            std::error_code ec;
            if (choice == kCoverEmbedded) { out.file = pass == 1 ? rq.pair_path : rq.path; out.embedded = true; out.label = "inside the file (chosen)"; return out; }
            if (fs::exists(path_from_utf8(choice), ec)) { out.file = choice; out.label = path_utf8(path_from_utf8(choice).filename()) + " (chosen)"; return out; }
            out.chosen = false;
        }
    }
    const std::vector<std::string> names = cover_search_names(rq, file);
    for (CoverStep step : cover_steps(rq.search)) {
        if (step == CoverStep::Embedded) {
            if (has_embedded_picture(file)) { out.file = rq.path; out.embedded = true; out.label = "inside the file"; return out; }
        } else {
            const fs::path pic = pick_folder_picture(dir, names, step == CoverStep::FolderGeneric, step == CoverStep::FolderAny, step == CoverStep::FolderClose);
            if (!pic.empty()) { out.file = path_utf8(pic); out.label = path_utf8(pic.filename()); return out; }
        }
    }
    if (!rq.pair_path.empty()) {   // a video: what the song it is paired with would get
        CoverRequest r2 = rq;
        r2.path = rq.pair_path; r2.pair_path.clear();
        const CoverSource s2 = resolve_cover_source(r2);
        if (!s2.none && !s2.file.empty()) return s2;
    }
    out.none = true;
    out.label = "no cover found";
    return out;
}

// ---- previews (the meta editor's cover pane, the cover overlays) ---------------------------------------------------------
namespace {

struct Preview {
    std::shared_future<CoverImage> fut;
    std::shared_ptr<std::atomic<bool>> dropped = std::make_shared<std::atomic<bool>>(false);   // set when the slot is thrown away: the read is not started (or not finished) any more
    std::chrono::steady_clock::time_point born = std::chrono::steady_clock::now();
};
std::map<std::string, Preview>& previews() {
    static std::map<std::string, Preview> m;
    return m;
}
std::vector<std::string>& preview_order() {
    static std::vector<std::string> v;
    return v;
}

Preview& preview_slot(const std::string& file, bool embedded, bool video = false) {
    const std::string key = std::string(video ? "V|" : embedded ? "E|" : "F|") + file + "|" + file_stamp(file);
    auto& m = previews();
    auto it = m.find(key);
    if (it != m.end()) {
        // a picture that could not be read is tried again after a few seconds: the file may have been busy or half written (a download
        // that was still running, a drive that woke up) and a failure must not stay for the whole session
        Preview& old = it->second;
        const bool failed = old.fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready && !old.fut.get().ok;
        if (!failed || std::chrono::steady_clock::now() - old.born < std::chrono::seconds(5)) return old;
        old.dropped->store(true);
        m.erase(it);
        auto& ord = preview_order();
        ord.erase(std::remove(ord.begin(), ord.end(), key), ord.end());
    }
    auto& order = preview_order();
    if (order.size() >= 32) {
        auto old = m.find(order.front());
        if (old != m.end()) { old->second.dropped->store(true); m.erase(old); }
        order.erase(order.begin());
    }   // (a running read keeps its own state alive)
    order.push_back(key);
    Preview p;
    p.fut = bg_async([file, embedded, video, key, dropped = p.dropped]() {
        CoverImage d;
        d.key = key;
        BgSlot slot;   // (at most three ffmpeg runs at a time; scrolling through the list does not start thirty)
        if (!slot.ok() || dropped->load() || bg_stopping().load()) return d;   // thrown away or the program ends: nothing to read
        if (video) decode_video_snapshot(path_from_utf8(file), d);   // a frame of the video (a tenth into it)
        else decode_cover_file(path_from_utf8(file), embedded, d);
        return d;
    });
    return m.emplace(key, std::move(p)).first->second;
}

} // namespace

const CoverImage* preview_cover(const std::string& file, bool embedded) {
    Preview& p = preview_slot(file, embedded);
    if (p.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    const CoverImage& d = p.fut.get();
    return d.ok ? &d : nullptr;
}

const CoverImage* preview_video_frame(const std::string& video) {
    Preview& p = preview_slot(video, false, true);
    if (p.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    const CoverImage& d = p.fut.get();
    return d.ok ? &d : nullptr;
}

bool preview_video_frame_loading(const std::string& video) {
    Preview& p = preview_slot(video, false, true);
    return p.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

bool preview_cover_loading(const std::string& file, bool embedded) {
    Preview& p = preview_slot(file, embedded);
    return p.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

} // namespace muisc
