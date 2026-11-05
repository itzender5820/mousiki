#pragma once
// Radio mode -- station list.
//
// Part of the self-contained radio module (everything under radio/). Nothing
// in here knows about the music player's audio path.
#include <string>
#include <vector>

namespace muisc::radio {

struct Station {
    std::string name;
    std::string genre;
    std::string country;
    std::string codec_hint;   // "AAC", "MP3", ... -- shown until the stream itself tells us
    int bitrate_hint = 0;     // kbps, 0 = unknown
    std::string url;          // http(s) stream URL, or "lavfi:<name>" for the offline test signals
    bool favorite = false;    // "*" in stations.txt: only the DEFAULT presets (used until presets.txt exists)
    double dial_mhz = 0.0;    // decorative position on the frequency band (87.5 .. 108.0)
    std::string preset_name;  // optional second, shorter name shown in the PRESETS pane (SHIFT+c in the big stations overlay); empty = use `name`

    const std::string& preset_label() const { return preset_name.empty() ? name : preset_name; }
    bool synthetic() const { return url.rfind("lavfi:", 0) == 0; }
    // An http:// or https:// address (any case). Stations from Radio Browser must be one (anyone can add a station there: no
    // file: paths, no devices, no ffmpeg options); for such a station ffmpeg / ffprobe only get the web protocols.
    static bool web_url(const std::string& u) {
        auto starts = [&](const char* p) { size_t i = 0; for (; p[i]; ++i) if (i >= u.size() || (u[i] | 0x20) != p[i]) return false; return true; };
        return starts("http://") || starts("https://");
    }
};

// Loads stations.txt (see radio/stations.txt for the format). Search order:
//   1. $HOME/.config/mousiki/stations.txt   (%USERPROFILE%\.config\mousiki\ on Windows)
//   2. ./stations.txt next to the executable's working directory
//   3. the built-in defaults
// `source_out`, if given, receives a short description of what was used.
std::vector<Station> load_stations(std::string* source_out = nullptr);

// Adds `st` to the station list ON DISK (the list in memory is the caller's business). `all` is the
// in-memory list INCLUDING the new station. If a stations.txt exists (same search order as above) the one
// line is appended to it; if none exists yet (the built-in defaults are in use) a new
// ~/.config/mousiki/stations.txt is written with the whole of `all`, so the defaults are not lost.
// Returns false and fills `err` if nothing could be written; `where` receives the file's path.
// Where the data files live (PATHS tab). Empty / never called = the default ~/.config/mousiki layout. When a file is
// switched to a new place and does not exist there yet, the old one is copied (not moved) so nothing is lost.
void set_storage_files(const std::string& stations_file, const std::string& lists_file, const std::string& presets_file);
// The preset names live in preset_names.txt (one `url<TAB>name` line per station that has one) in the same folder as
// the stations file, so they move with it when the PATHS tab changes the folder. load_stations() fills Station::preset_name.
std::string preset_names_path();
bool save_preset_names(const std::vector<Station>& all);
// Writes the WHOLE in-memory list to the stations file (after a station was deleted, edited or came back): the file load_stations()
// used, else a new one. The comment lines at the top of an existing file are kept. Returns false and fills `err` on failure.
bool save_all_stations(const std::vector<Station>& all, std::string* where = nullptr, std::string* err = nullptr);
bool append_station_to_file(const Station& st, const std::vector<Station>& all, std::string* where = nullptr, std::string* err = nullptr);

// True when two stream URLs are the same stream spelled differently: scheme (http / https), host case, a trailing "/"
// and tracking parameters are ignored ("...stream.mp3" == "...stream.mp3?aggregator=web"). If one of the two has no
// query at all, any query on the other is ignored; two real queries must agree (sorted, tracking parameters dropped).
bool same_stream_url(const std::string& a, const std::string& b);

// The built-in list (also what a missing stations.txt falls back to).
std::vector<Station> default_stations();

// Decorative "dial" frequency for a station that does not specify one:
// a stable hash of its name mapped onto 87.5 .. 108.0 MHz in 0.1 steps.
double dial_for_name(const std::string& name);

// ---- presets -------------------------------------------------------------------------------
// 16 preset slots, recalled with these keys in this order (slot 0 = "1" ... slot 15 = "g").
constexpr int kPresetCount = 16;
inline constexpr char kPresetKeys[kPresetCount + 1] = "1234567890ertdfg";

// Slot index for a key press, or -1 if the key is not a preset key.
int preset_slot_for_key(int key);

// Where the presets live: $HOME/.config/mousiki/presets.txt (%USERPROFILE%\.config\mousiki\ on Windows),
// or ./presets.txt when no home directory is known.
std::string presets_path();

// A named PRESET: one set of kPresetCount slots (station indices, -1 = empty, slot i <-> kPresetKeys[i]).
// The user can keep several and switch between them (SELECT PRESET pane / Shift+Left/Right).
struct PresetBank {
    std::string name;
    std::vector<int> slots = std::vector<int>(kPresetCount, -1);
};

struct PresetStore {
    std::vector<PresetBank> banks;   // never empty after load_presets()
    int active = 0;                  // index into `banks`: the preset the keys 1234567890ertdfg recall from
};

// Longest preset name, in characters.
constexpr int kPresetNameMax = 24;

// Trims a typed name, drops control characters, turns [ ] | (file syntax) into ( ) /, caps it at
// kPresetNameMax characters. Returns "" if nothing usable is left.
std::string clean_preset_name(const std::string& raw);

// ---- station lists --------------------------------------------------------------------------
// A STATION LIST is a named, ordered collection of stations ("the order they were added"), built and
// managed in the STATION LISTS menu (Shift+K). Unlike a preset it has no slots and no size limit.
// Items are indices into the station list in memory; on disk they are stored as `url | station name`.
constexpr int kListNameMax = 25;

struct StationList {
    std::string name;
    std::vector<int> items;   // station indices, in the order they were added
};

// Same cleaning as clean_preset_name(), but capped at kListNameMax characters.
std::string clean_list_name(const std::string& raw);

// $HOME/.config/mousiki/stationlists.txt (%USERPROFILE%\.config\mousiki\ on Windows), or ./stationlists.txt.
std::string station_lists_path();

// Reads stationlists.txt:
//   # comment
//   [List name]
//   https://stream.example.com/aac | Station name
// Stations are matched by URL, then by name; one that has disappeared from stations.txt is dropped.
// Returns an empty vector when the file does not exist.
std::vector<StationList> load_station_lists(const std::vector<Station>& stations);

// Writes stationlists.txt (creating the folder). Returns false if it could not be written.
bool save_station_lists(const std::vector<Station>& stations, const std::vector<StationList>& lists);

// Reads presets.txt (format below) and matches the stations by URL, then by name. Always returns at least
// one preset. With no presets.txt the stations marked "*" in stations.txt fill the first preset ("Default").
//
//   @active Name           <- optional, which preset was selected last
//   [Default]              <- a preset; the lines up to the next [..] are its slots
//   1 | url | station name
//   [Morning]
//   2 | url | station name
//
// A presets.txt from before presets had names (slot lines without any [..] header) is read as "Default".
PresetStore load_presets(const std::vector<Station>& stations);

// Writes presets.txt (creating the folder). Returns false if it could not be written.
bool save_presets(const std::vector<Station>& stations, const PresetStore& store);

} // namespace muisc::radio
