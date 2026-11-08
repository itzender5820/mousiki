// The radio's listening history: records the lines and songs heard on the radio and aggregates them
// into the same kind of statistics as the music player.
#include "radio_history.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#if defined(_WIN32)
#include "win_compat.h"
#endif
#include "date_util.h"

namespace muisc::radio {

namespace fs = std::filesystem;

namespace {

constexpr long long kSessionGapSec = 30 * 60;
constexpr double kMinKeepSec = 3.0;      // lines heard for less than this are channel hopping, not listening

std::string clean(std::string s) {
    for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        const size_t t = line.find('\t', pos);
        if (t == std::string::npos) { out.push_back(line.substr(pos)); break; }
        out.push_back(line.substr(pos, t - pos));
        pos = t + 1;
    }
    return out;
}

double to_d(const std::string& s) { try { return std::stod(s); } catch (...) { return 0.0; } }
long long to_ll(const std::string& s) { try { return std::stoll(s); } catch (...) { return 0; } }

// Same session bookkeeping as the player's HistoryArchive::fold(), applied to explicit state.
struct Sessions {
    int closed = 0;
    double sum = 0.0;
    bool open = false;
    long long start = 0, end = 0;
    void add(const HistoryEntry& e) {
        if (e.at <= 0) return;
        const long long e_end = e.at + static_cast<long long>(e.listened + 0.5);
        if (!open || e.at - end > kSessionGapSec) {
            if (open) { ++closed; sum += static_cast<double>(std::max<long long>(0, end - start)); }
            open = true; start = e.at; end = e_end;
        } else {
            start = std::min(start, e.at); end = std::max(end, e_end);
        }
    }
};

void add_time_buckets(const HistoryEntry& e, std::array<double, 24>& hours, std::array<double, 7>& weekdays) {
    if (e.at <= 0) return;
    const std::tm tm = local_tm(e.at);
    hours[static_cast<size_t>(std::clamp(tm.tm_hour, 0, 23))] += e.listened;
    weekdays[static_cast<size_t>((tm.tm_wday + 6) % 7)] += e.listened;   // Monday first
}

} // namespace

void HistoryArchive::fold(const HistoryEntry& e) {
    ++entries;
    listened += e.listened;
    auto& c = channels[e.channel];
    c.first += e.listened;
    c.second += 1;
    if (e.at > 0) per_day[day_key(e.at)] += e.listened;
    add_time_buckets(e, hours, weekdays);
    Sessions s{sessions_closed, session_sum_sec, session_open, sess_start, sess_end};
    s.add(e);
    sessions_closed = s.closed; session_sum_sec = s.sum; session_open = s.open; sess_start = s.start; sess_end = s.end;
}

// ---------------------------------------------------------------------------------------------------------------
void RadioHistory::open(const fs::path& dir) {
    close_live();
    dir_ = dir;
    closed_.clear();
    archive_ = HistoryArchive();
    std::error_code ec;
    // Every mode has its own history file; the radio's is named like the player's with "_radio" added. Files written by
    // older versions (history.txt / archive.txt) are renamed once.
    for (const char* base : {"history", "archive"}) {
        const fs::path old_f = dir / (std::string(base) + ".txt"), new_f = dir / (std::string(base) + "_radio.txt");
        if (fs::exists(old_f, ec) && !fs::exists(new_f, ec)) fs::rename(old_f, new_f, ec);
    }
    {
        std::ifstream in(dir / "archive_radio.txt");
        std::string line;
        while (std::getline(in, line)) {
            const auto f = split_tabs(line);
            if (f.empty()) continue;
            if (f[0] == "E" && f.size() >= 3) { archive_.entries = to_ll(f[1]); archive_.listened = to_d(f[2]); }
            else if (f[0] == "C" && f.size() >= 4) archive_.channels[f[3]] = {to_d(f[1]), static_cast<int>(to_ll(f[2]))};
            else if (f[0] == "D" && f.size() >= 3) archive_.per_day[f[1]] = to_d(f[2]);
            else if (f[0] == "H" && f.size() >= 3) { const long long h = to_ll(f[1]); if (h >= 0 && h < 24) archive_.hours[static_cast<size_t>(h)] = to_d(f[2]); }
            else if (f[0] == "W" && f.size() >= 3) { const long long d = to_ll(f[1]); if (d >= 0 && d < 7) archive_.weekdays[static_cast<size_t>(d)] = to_d(f[2]); }
            else if (f[0] == "S" && f.size() >= 6) {
                archive_.sessions_closed = static_cast<int>(to_ll(f[1])); archive_.session_sum_sec = to_d(f[2]);
                archive_.session_open = f[3] == "1"; archive_.sess_start = to_ll(f[4]); archive_.sess_end = to_ll(f[5]);
            }
        }
    }
    {
        std::ifstream in(dir / "history_radio.txt");
        std::string line;
        while (std::getline(in, line)) {
            const auto f = split_tabs(line);
            if (f.size() < 5) continue;
            HistoryEntry e;
            e.at = to_ll(f[0]); e.listened = to_d(f[1]); e.channel = f[2]; e.artist = f[3]; e.title = f[4];
            closed_.push_back(e);
        }
    }
    if (closed_.size() > kMaxHistoryEntries) {      // an older / hand-extended file: fold the excess, never drop it silently
        while (closed_.size() > kMaxHistoryEntries) { archive_.fold(closed_.front()); closed_.pop_front(); }
        rewrite_files();
    }
}

void RadioHistory::append_line(const HistoryEntry& e) const {
    if (dir_.empty()) return;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    std::ofstream out(dir_ / "history_radio.txt", std::ios::app);
    if (out) out << e.at << '\t' << static_cast<long long>(e.listened + 0.5) << '\t' << clean(e.channel) << '\t' << clean(e.artist) << '\t' << clean(e.title) << '\n';
}

void RadioHistory::rewrite_files() const {
    if (dir_.empty()) return;
    std::error_code ec;
    fs::create_directories(dir_, ec);
    {
        const fs::path tmp = dir_ / "history_radio.txt.tmp";
        std::ofstream out(tmp, std::ios::trunc);
        for (const auto& e : closed_)
            out << e.at << '\t' << static_cast<long long>(e.listened + 0.5) << '\t' << clean(e.channel) << '\t' << clean(e.artist) << '\t' << clean(e.title) << '\n';
        out.close();
        fs::rename(tmp, dir_ / "history_radio.txt", ec);
    }
    {
        const fs::path tmp = dir_ / "archive_radio.txt.tmp";
        std::ofstream out(tmp, std::ios::trunc);
        out << "E\t" << archive_.entries << '\t' << archive_.listened << '\n';
        for (const auto& kv : archive_.channels) out << "C\t" << kv.second.first << '\t' << kv.second.second << '\t' << clean(kv.first) << '\n';
        for (const auto& kv : archive_.per_day) out << "D\t" << kv.first << '\t' << kv.second << '\n';
        for (size_t h = 0; h < 24; ++h) out << "H\t" << h << '\t' << archive_.hours[h] << '\n';
        for (size_t d = 0; d < 7; ++d) out << "W\t" << d << '\t' << archive_.weekdays[d] << '\n';
        out << "S\t" << archive_.sessions_closed << '\t' << archive_.session_sum_sec << '\t' << (archive_.session_open ? 1 : 0)
            << '\t' << archive_.sess_start << '\t' << archive_.sess_end << '\n';
        out.close();
        fs::rename(tmp, dir_ / "archive_radio.txt", ec);
    }
}

void RadioHistory::commit(const HistoryEntry& e) {
    if (e.listened < kMinKeepSec) return;
    closed_.push_back(e);
    if (closed_.size() > kMaxHistoryEntries) {
        archive_.fold(closed_.front());      // the oldest line leaves the window, its numbers stay
        closed_.pop_front();
        rewrite_files();
    } else {
        append_line(e);
    }
}

void RadioHistory::close_live() {
    if (!live_on_) return;
    live_on_ = false;
    commit(live_);
}

void RadioHistory::tick(const RadioStatus& st, double dt, long long now) {
    const bool audible = st.state == StreamState::Live;
    const bool tuned = st.state != StreamState::Idle && st.state != StreamState::Failed && !st.tuned_name.empty();
    if (!tuned) { close_live(); return; }
    if (st.state == StreamState::Connecting && !live_on_) return;   // nothing heard yet
    const std::string& channel = st.tuned_name;
    const std::string& artist = st.info.artist;
    const std::string& title = st.info.title;
    if (live_on_) {
        const bool same_channel = live_.channel == channel;
        const bool same_song = live_.artist == artist && live_.title == title;
        if (!same_channel) {
            close_live();
        } else if (!same_song) {
            // The title usually arrives a moment after the first audio: a line that had no song yet and was only just
            // opened becomes that song instead of leaving an empty line behind.
            if (live_.artist.empty() && live_.title.empty() && live_.listened < 20.0) { live_.artist = artist; live_.title = title; }
            else close_live();
        }
    }
    if (!live_on_) {
        if (!audible) return;
        live_ = HistoryEntry();
        live_.at = now; live_.channel = channel; live_.artist = artist; live_.title = title;
        live_on_ = true;
    }
    if (audible && !st.muted && dt > 0.0 && dt < 5.0) live_.listened += dt;
}

const HistoryEntry& RadioHistory::newest(size_t i) const {
    if (live_on_) {
        if (i == 0) return live_;
        --i;
    }
    return closed_[closed_.size() - 1 - std::min(i, closed_.size() - 1)];
}

std::vector<HistoryChannelRow> RadioHistory::top_channels(bool most_first) const {
    std::map<std::string, std::pair<double, int>> sum = archive_.channels;
    auto add = [&](const HistoryEntry& e) { auto& c = sum[e.channel]; c.first += e.listened; c.second += 1; };
    for (const auto& e : closed_) add(e);
    if (live_on_) add(live_);
    std::vector<HistoryChannelRow> rows;
    for (const auto& kv : sum) rows.push_back({kv.first, kv.second.first, kv.second.second});
    std::stable_sort(rows.begin(), rows.end(), [&](const HistoryChannelRow& a, const HistoryChannelRow& b) {
        return most_first ? a.listened > b.listened : a.listened < b.listened;
    });
    return rows;
}

HistoryStats RadioHistory::stats(long long now) const {
    HistoryStats s;
    std::map<std::string, double> per_day = archive_.per_day;
    s.hours = archive_.hours;
    s.weekdays = archive_.weekdays;
    Sessions sess{archive_.sessions_closed, archive_.session_sum_sec, archive_.session_open, archive_.sess_start, archive_.sess_end};
    double total = archive_.listened;
    long long entries = archive_.entries;
    auto add = [&](const HistoryEntry& e) {
        ++entries;
        total += e.listened;
        if (e.at > 0) per_day[day_key(e.at)] += e.listened;
        add_time_buckets(e, s.hours, s.weekdays);
        sess.add(e);
    };
    for (const auto& e : closed_) add(e);
    if (live_on_) add(live_);
    s.entries = static_cast<int>(entries);
    s.total_sec = total;
    s.sessions = sess.closed + (sess.open ? 1 : 0);
    const double sum = sess.sum + (sess.open ? static_cast<double>(std::max<long long>(0, sess.end - sess.start)) : 0.0);
    s.avg_session_sec = s.sessions > 0 ? sum / s.sessions : 0.0;
    s.songs_per_session = s.sessions > 0 ? static_cast<double>(entries) / s.sessions : 0.0;
    s.days = 0;
    double day_total = 0.0;
    for (const auto& kv : per_day) if (kv.second > 0.0) { ++s.days; day_total += kv.second; }
    s.avg_day_sec = s.days > 0 ? day_total / s.days : 0.0;
    const std::string today = day_key(now);
    const auto it = per_day.find(today);
    s.today_sec = it == per_day.end() ? 0.0 : it->second;
    // Average per weekday: every weekday of the calendar from the first day with radio up to today counts, so a
    // Monday without radio pulls the Monday average down (it is "an average Monday", not "an average Monday with radio").
    {
        const auto first_it = std::find_if(per_day.begin(), per_day.end(), [](const auto& kv) { return kv.second > 0.0; });
        long long first = 0, last = 0;
        if (first_it != per_day.end() && civil_day_number(first_it->first, first) &&
            civil_day_number(std::max(today, per_day.rbegin()->first), last) && last >= first) {
            const long long n = last - first + 1;
            const int first_wd = monday_first_weekday(first);
            for (int k = 0; k < 7; ++k) {
                // the k-th weekday after the first day occurs ceil((n - k) / 7) times in the range
                const long long cnt = n > k ? (n - k + 6) / 7 : 0;
                s.weekday_count[static_cast<size_t>((first_wd + k) % 7)] = static_cast<int>(cnt);
            }
        }
        for (size_t d = 0; d < 7; ++d)
            s.weekday_avg[d] = s.weekday_count[d] > 0 ? s.weekdays[d] / s.weekday_count[d] : 0.0;
    }
    const auto rows = top_channels(true);
    s.channels = static_cast<int>(rows.size());
    if (!rows.empty() && rows.front().listened > 0.0) s.top_channel = rows.front().channel;
    double best = 0.0;
    for (int h = 0; h < 24; ++h) if (s.hours[static_cast<size_t>(h)] > best) { best = s.hours[static_cast<size_t>(h)]; s.busiest_hour = h; }
    return s;
}

} // namespace muisc::radio
