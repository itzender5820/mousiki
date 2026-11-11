// Saved playlists: lists, renames and removes the playlist files in the playlist folder and rewrites
// track paths when files move.
#include "playlist_manager.h"
#include "path_utf8.h"
#include "settings.h"   // write_text_atomic()
#include <algorithm>
#include <fstream>
#include <system_error>

namespace muisc {

namespace {

std::string ascii_lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// Strips a trailing \r (files may have been saved/edited on Windows) and
// \n (getline already consumes \n itself, but this stays defensive) from
// one line read out of a playlist file.
void strip_eol(std::string& line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
}

} // namespace

std::string PlaylistManager::sanitize_name(const std::string& name) {
    std::string safe = name;
    for (char& c : safe) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    // Trailing dots/spaces are invalid/get silently stripped on Windows --
    // strip them here too so the name we show matches the file we wrote.
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) safe.pop_back();
    while (!safe.empty() && safe.front() == ' ') safe.erase(safe.begin());
    if (safe.empty()) safe = "playlist";
    return safe;
}

std::vector<PlaylistSummary> PlaylistManager::list(const fs::path& dir) {
    std::vector<PlaylistSummary> out;
    for (const bool imported : {false, true}) {
        const fs::path folder = imported ? dir / kImportedDir : dir;
        std::error_code ec;
        if (!fs::exists(folder, ec) || !fs::is_directory(folder, ec)) continue;
        std::error_code iter_ec;   // (an explicit loop: the range-for's operator++ throws on an I/O error half way)
        const fs::directory_iterator iter_end;
        for (fs::directory_iterator it(folder, iter_ec); !iter_ec && it != iter_end; it.increment(iter_ec)) {
            const fs::directory_entry& entry = *it;
            std::error_code fec;
            if (!entry.is_regular_file(fec) || fec) continue;
            fs::path p = entry.path();
            if (ascii_lower(path_utf8(p.extension())) != ".txt") continue;

            size_t count = 0;
            std::ifstream in(p);
            std::string line;
            while (std::getline(in, line)) {
                strip_eol(line);
                if (line.empty() || line[0] == '#') continue;
                ++count;
            }
            PlaylistSummary sum{path_utf8(p.stem()), count};
            sum.imported = imported;
            out.push_back(std::move(sum));
        }
    }

    std::sort(out.begin(), out.end(), [](const PlaylistSummary& a, const PlaylistSummary& b) {
        return ascii_lower(a.name) < ascii_lower(b.name);
    });
    return out;
}

int PlaylistManager::rename_paths(const fs::path& dir,
                                  const std::vector<std::pair<std::string, std::string>>& renames) {
    if (renames.empty()) return 0;
    int changed = 0;
    for (const fs::path& folder : {dir, dir / kImportedDir}) {
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) continue;
    std::error_code iter_ec;
    const fs::directory_iterator iter_end;
    for (fs::directory_iterator it(folder, iter_ec); !iter_ec && it != iter_end; it.increment(iter_ec)) {
        const fs::directory_entry& entry = *it;
        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec) continue;
        fs::path p = entry.path();
        if (ascii_lower(path_utf8(p.extension())) != ".txt") continue;
        std::vector<std::string> lines;
        bool touched = false;
        {
            std::ifstream in(p);
            std::string line;
            while (std::getline(in, line)) {
                strip_eol(line);
                for (const auto& r : renames) {
                    if (line == r.first) { line = r.second; touched = true; break; }
                }
                lines.push_back(std::move(line));
            }
        }
        if (!touched) continue;
        fs::path tmp = p;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            if (!out.is_open()) continue;
            for (const auto& l : lines) out << l << "\n";
        }
        fs::rename(tmp, p, ec);
        if (ec) { fs::remove(tmp, ec); continue; }
        ++changed;
    }
    }
    return changed;
}

std::optional<Playlist> PlaylistManager::load(const fs::path& dir, const std::string& name) {
    fs::path file = dir / path_from_utf8(sanitize_name(name) + ".txt");
    bool imported = false;
    std::ifstream in(file);
    if (!in.is_open()) {   // not a created one: maybe a given (imported) one
        in.clear();
        file = dir / kImportedDir / path_from_utf8(sanitize_name(name) + ".txt");
        in.open(file);
        if (!in.is_open()) return std::nullopt;
        imported = true;
    }

    Playlist pl;
    pl.name = name;
    pl.imported = imported;
    std::error_code ec;
    std::string line;
    while (std::getline(in, line)) {
        strip_eol(line);
        if (line.empty() || line[0] == '#') continue;
        fs::path p = path_from_utf8(line);
        PlaylistTrack t;
        t.path = p;
        t.title = path_utf8(p.stem());
        t.artist = path_utf8(p.parent_path().filename());
        t.missing = !fs::exists(p, ec);
        pl.tracks.push_back(std::move(t));
    }
    return pl;
}

bool PlaylistManager::save(const fs::path& dir, const Playlist& pl, std::string* error) {
    std::error_code ec;
    const fs::path folder = pl.imported ? dir / kImportedDir : dir;
    fs::create_directories(folder, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }

    fs::path file = folder / path_from_utf8(sanitize_name(pl.name) + ".txt");
    std::string text = "# mousiki playlist: " + pl.name + "\n";
    for (const auto& t : pl.tracks) text += path_utf8(t.path) + "\n";
    if (!write_text_atomic(file, text)) {   // (temp file + rename: a crash never leaves a cut-off playlist)
        if (error) *error = "could not open file for writing";
        return false;
    }
    return true;
}

bool PlaylistManager::remove(const fs::path& dir, const std::string& name, std::string* error) {
    fs::path file = dir / path_from_utf8(sanitize_name(name) + ".txt");
    std::error_code ec;
    if (!fs::exists(file, ec)) file = dir / kImportedDir / path_from_utf8(sanitize_name(name) + ".txt");
    if (!fs::exists(file, ec)) {
        if (error) *error = "no such playlist";
        return false;
    }
    if (!fs::remove(file, ec) || ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

} // namespace muisc
