#include "library_search.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <thread>

#include "par_pool.h"
#include "text_edit.h"

namespace muisc {

namespace {
// ---- the same matching as fuzzy_score(), for a library of tens of thousands of tracks ------------------------------------
// The query is normalised and split once, the targets are normalised once per scan (App::search_.idx), and the edit
// distance stops as soon as a word cannot reach the 0.55 quality floor any more. The results are identical.
inline int popcount64(uint64_t x) {   // portable (no compiler intrinsic)
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<int>((x * 0x0101010101010101ULL) >> 56);
}
inline uint64_t char_bit(char c) { return uint64_t{1} << (static_cast<unsigned char>(c) & 63); }
// Edit distance that gives up above `cutoff`: returns cutoff + 1 then (the exact value is never needed above it).
int levenshtein_capped(std::string_view a, std::string_view b, int cutoff) {   // (an adjacent swap counts as one edit, like levenshtein())
    const int n = static_cast<int>(a.size()), m = static_cast<int>(b.size());
    if (std::abs(n - m) > cutoff) return cutoff + 1;
    thread_local std::vector<int> prev2, prev, cur;
    prev2.assign(static_cast<size_t>(m) + 1, 0);
    prev.assign(static_cast<size_t>(m) + 1, 0);
    cur.assign(static_cast<size_t>(m) + 1, 0);
    for (int j = 0; j <= m; ++j) prev[static_cast<size_t>(j)] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = a[static_cast<size_t>(i) - 1] == b[static_cast<size_t>(j) - 1] ? 0 : 1;
            int v = std::min({prev[static_cast<size_t>(j)] + 1, cur[static_cast<size_t>(j) - 1] + 1, prev[static_cast<size_t>(j) - 1] + cost});
            if (i > 1 && j > 1 && a[static_cast<size_t>(i) - 1] == b[static_cast<size_t>(j) - 2] && a[static_cast<size_t>(i) - 2] == b[static_cast<size_t>(j) - 1])
                v = std::min(v, prev2[static_cast<size_t>(j) - 2] + 1);
            cur[static_cast<size_t>(j)] = v;
            if (v < row_min) row_min = v;
        }
        if (row_min > cutoff + 1) return cutoff + 1;   // (+1: a swap can still lower the next row by one)
        std::swap(prev2, prev);
        std::swap(prev, cur);
    }
    return prev[static_cast<size_t>(m)];
}
// Tier 2 of fuzzy_score(): every query word must find a close word in the target. -1 = no match.
double word_fuzzy_core(const PreparedQuery& pq, const std::string& t_norm) {
    if (pq.words.empty() || t_norm.empty()) return -1.0;
    const std::string_view t(t_norm);
    double total_quality = 0.0;
    bool any_word = false;
    for (size_t qi = 0; qi < pq.words.size(); ++qi) {
        const std::string& qw = pq.words[qi];
        const uint64_t qmask = pq.masks[qi];
        double best_quality = -1.0;
        size_t i = 0;
        while (i < t.size()) {   // the target's words, without copying them
            while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) ++i;
            const size_t start = i;
            uint64_t tmask = 0;
            while (i < t.size() && t[i] != ' ' && t[i] != '\t') { tmask |= char_bit(t[i]); ++i; }
            if (i == start) break;
            any_word = true;
            const std::string_view tw = t.substr(start, i - start);
            const size_t max_len = std::max(qw.size(), tw.size());
            double quality = -1.0;
            // a word typed only as far as it goes ("cov" for "cover3") is a good fit
            if (qw.size() >= 2 && tw.size() > qw.size() && tw.compare(0, qw.size(), qw) == 0)
                quality = 0.80 + 0.15 * static_cast<double>(qw.size()) / static_cast<double>(tw.size());
            const int cutoff = static_cast<int>(0.45 * static_cast<double>(max_len)) + 1;
            // Every distinct character of one word that the other lacks costs an edit (collisions of the hash only
            // lower the count), so this is a lower bound of the distance: no need to compute it when it is too big.
            if (std::max(popcount64(qmask & ~tmask), popcount64(tmask & ~qmask)) <= cutoff)
                quality = std::max(quality, 1.0 - static_cast<double>(levenshtein_capped(qw, tw, cutoff)) / static_cast<double>(max_len));
            // a trailing number of the text's word ("cover3") does not count
            if (tw.size() > 1 && tw.back() >= '0' && tw.back() <= '9' && !(qw.back() >= '0' && qw.back() <= '9')) {
                size_t e = tw.size();
                while (e > 1 && tw[e - 1] >= '0' && tw[e - 1] <= '9') --e;
                const std::string_view tws = tw.substr(0, e);
                const size_t ml = std::max(qw.size(), tws.size());
                const int co = static_cast<int>(0.45 * static_cast<double>(ml)) + 1;
                double q2 = 1.0 - static_cast<double>(levenshtein_capped(qw, tws, co)) / static_cast<double>(ml) - 0.02;
                if (qw.size() >= 2 && tws.size() > qw.size() && tws.compare(0, qw.size(), qw) == 0) q2 = std::max(q2, 0.78 + 0.15 * static_cast<double>(qw.size()) / static_cast<double>(tws.size()));
                quality = std::max(quality, q2);
            }
            if (quality > best_quality) best_quality = quality;
        }
        if (!any_word) return -1.0;
        if (best_quality < 0.55) return -1.0;
        total_quality += best_quality;
    }
    return (total_quality / static_cast<double>(pq.words.size())) * 100.0;
}
} // namespace

PreparedQuery prepare_query(const std::string& query) {
    PreparedQuery pq;
    pq.q = normalize_for_search(query);
    pq.words = split_words(pq.q);
    for (const auto& w : pq.words) pq.compact += w;
    for (const auto& w : pq.words) {
        uint64_t m = 0;
        for (char c : w) m |= char_bit(c);
        pq.masks.push_back(m);
    }
    return pq;
}
// Tier 1 of fuzzy_score(): exact substring, earlier position ranks higher. -1 = no substring.
double substring_score(const PreparedQuery& pq, const std::string& t_norm) {
    if (pq.q.empty()) return 0.0;
    const size_t pos = t_norm.find(pq.q);
    return pos == std::string::npos ? -1.0 : 1000.0 - std::min<double>(static_cast<double>(pos), 900.0);
}
double word_fuzzy_score(const PreparedQuery& pq, const std::string& t_norm) {
    double best = word_fuzzy_core(pq, t_norm);
    const int skips = abbreviation_skips(pq.compact, t_norm);   // tier 2b: the query as one run-together word ("testcover" for "test_song-cover3"); the fewer words skipped, the better
    if (skips >= 0) best = std::max(best, std::max(60.0, 90.0 - 5.0 * skips));
    return best;
}
double score_track(const PreparedQuery& pq, const SearchIdx& e, int scope) {
    const std::string& artist = e.artist_n.empty() ? e.folder_artist_n : e.artist_n;
    double score = 0.0;
    if (scope == 1) {
        score = substring_score(pq, artist);
        if (score < 0.0) score = word_fuzzy_score(pq, artist);
    } else if (scope == 2) {
        if (!e.album_n.empty()) {
            score = substring_score(pq, e.album_n);
            if (score < 0.0) score = word_fuzzy_score(pq, e.album_n);
        }
    } else {
        // The best score over the fields. An exact substring (tier 1, 100 .. 1000) always beats a fuzzy one (< 100),
        // so the fuzzy pass only runs when no field contains the query.
        const std::string* fields[4] = {&e.title_n, &artist, &e.tag_title_n, &e.album_n};
        double best = -1.0;
        for (const std::string* f : fields) if (!f->empty()) best = std::max(best, substring_score(pq, *f));
        if (best < 0.0) for (const std::string* f : fields) best = std::max(best, word_fuzzy_score(pq, *f));
        score = best;
    }
    return score;
}

std::vector<std::pair<double, uint32_t>> score_library(const std::vector<SearchIdx>& idx, const PreparedQuery& pq, int scope, int threads) {
    const size_t total = idx.size();
    int n = threads;
    if (n <= 0) {   // (as many as the machine has, at most 4; a small library is scanned by one: the parts would cost more than they save)
        n = std::min(4, static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
        if (total < kSearchParallelMin) n = 1;
        n = static_cast<int>(std::min<size_t>(static_cast<size_t>(n), std::max<size_t>(1, total / 2048)));
    }
    n = static_cast<int>(std::min<size_t>(static_cast<size_t>(std::min(n, 16)), std::max<size_t>(1, total)));
    n = std::max(1, n);
    std::vector<std::vector<std::pair<double, uint32_t>>> parts(static_cast<size_t>(n));
    auto scan = [&](int part) {   // the tracks [a, b) in their order
        const size_t a = total * static_cast<size_t>(part) / static_cast<size_t>(n), b = total * static_cast<size_t>(part + 1) / static_cast<size_t>(n);
        auto& out = parts[static_cast<size_t>(part)];
        for (size_t i = a; i < b; ++i) {
            const double s = score_track(pq, idx[i], scope);
            if (s > 0.0) out.emplace_back(s, static_cast<uint32_t>(i));
        }
    };
    if (n <= 1) scan(0);
    else {
        struct Ctx { decltype(scan)* fn; } c{&scan};
        ParPool::instance().run(n, [](void* p, int i) { (*static_cast<Ctx*>(p)->fn)(i); }, &c);
    }
    if (n == 1) return std::move(parts[0]);
    size_t sum = 0;
    for (const auto& p : parts) sum += p.size();
    std::vector<std::pair<double, uint32_t>> all;
    all.reserve(sum);
    for (auto& p : parts) all.insert(all.end(), p.begin(), p.end());
    return all;
}

}  // namespace muisc
