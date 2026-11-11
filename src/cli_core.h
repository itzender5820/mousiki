#pragma once
// The pure parts of the command line (CLI, see app_cli.cpp): splitting a line into commands and words, durations, the aliases and
// scripts on disk (<config folder>/cli/), the conditions of `if`. No App, so the tests can run them.
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace muisc::cli {

std::string trim(const std::string& s);
// "a; b ; \"c;d\"" -> {"a", "b", "\"c;d\""}: split at ';' outside double quotes; empty parts are dropped
std::vector<std::string> split_chain(const std::string& line);
// "play \"beta tune\" now" -> {"play", "beta tune", "now"}
std::vector<std::string> split_words(const std::string& s);
// "\"a b\"" -> "a b"; anything else as it is
std::string unquote(const std::string& s);
// "90", "90s", "10m", "2h", "1h30m", "1.5m" -> seconds (0 < sec <= max_sec); false for anything else
bool parse_duration(const std::string& s, double& sec, double max_sec = 86400.0);
std::string duration_text(double sec);   // 3725 -> "1h02m05s", 75 -> "1m15s", 9 -> "9s"

// Where the scripts and the aliases are kept
std::filesystem::path dir();                                // <config folder>/cli
bool valid_name(const std::string& name);                   // letters, digits, - _ . only, 1..48 characters, no leading dot
std::vector<std::string> script_names();                    // the *.cli files without the extension, sorted
std::filesystem::path script_path(const std::string& name);
bool read_script(const std::string& name, std::vector<std::string>& lines, std::string& error);
bool write_script(const std::string& name, const std::vector<std::string>& lines, std::string& error);

bool rename_script(const std::string& from, const std::string& to, std::string& error);   // refuses an existing name
bool trash_script(const std::string& name, std::string& error);   // moves the file to cli/deleted/ (nothing is erased)
// every word of the query in the name or the text (case does not matter); an empty query matches all
bool script_matches(const std::string& name, const std::string& text, const std::string& query);

// aliases.cli: one line "name = command; command" each
std::map<std::string, std::string> load_aliases();
bool save_aliases(const std::map<std::string, std::string>& a);
// "alias name = a; b": the whole line is the definition (its ';' belong to the alias, the line is not split into commands)
bool is_alias_definition(const std::string& line);

// hooks.cli: "on <event> = <commands>" (also "on <event> <commands>"), one per line, # comments. The commands run (as a script) when the
// event happens; {title} {artist} {album} {state} {volume} {queue} {position} in them are replaced (";" and control characters taken out
// of the values, so a title can never start a second command).
struct Hook { std::string event, chain; };
const std::vector<std::string>& hook_events(bool radio = false);   // the player: track-change pause resume queue-empty idle; the radio: station-change pause resume stream-error idle
std::vector<Hook> parse_hooks(const std::vector<std::string>& lines, bool radio = false);
std::string expand_hook(const std::string& chain, const std::map<std::string, std::string>& values);
std::string hook_safe_value(const std::string& v);
// At most `max_fires` runs in `window` seconds (a hook that triggers itself -- `on pause = play` -- is stopped by this, not by hoping)
class HookLimiter {
public:
    bool allow(std::chrono::steady_clock::time_point now, int max_fires = 8, int window_sec = 10);
private:
    std::deque<std::chrono::steady_clock::time_point> fires_;
};

// What `if` can look at
struct State {
    bool has_track = false, paused = false, muted = false;
    int volume = 0;
    size_t queue = 0;
    double position = 0, duration = 0;
};
// "playing", "paused", "stopped", "muted", "queue-empty", "volume>50" ... (a leading "!" or "not" negates); ok=false: not a condition
bool eval_condition(const std::string& cond, const State& st, bool& ok);

// The line of a script as it is run: comments (# ...) and blank lines give ""; a leading "-" (keep going after an error) is returned in `soft`
std::string script_line(const std::string& raw, bool& soft);

// --- running commands (App::cli_run in app_cli.cpp) ---
enum class Origin { Typed, Script, Remote, Network };   // what a person typed / a script or the startup file / `mousiki --cmd`, the phone, a hook / the web server (/api/cli): the same as Remote, minus quit, set, edit, alias, force ...
struct Job {
    std::deque<std::string> lines;      // still to run (a typed line is split at ';', a script is its lines)
    Origin origin = Origin::Typed;
    std::string reply_id;               // a `mousiki --query` waits for reply-<id>.txt
    std::vector<std::string> out;       // the replies so far
    bool failed = false;
    bool waiting = false;
    std::chrono::steady_clock::time_point wait_until{};
    int executed = 0;                   // the lines run (a script that calls itself ends at 1000)
    std::string label;                  // the first line, for the log
    bool to_overlay = false;            // the replies also go to the script overlay's output rows
};
struct Timer {
    std::chrono::steady_clock::time_point at{};
    std::string line;
    Origin origin = Origin::Typed;
    int id = 0;
};

}  // namespace muisc::cli
