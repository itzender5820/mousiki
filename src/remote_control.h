#pragma once
// Remote control: `mousiki --cmd next`, `mousiki --cmd volume 60`, `mousiki --status` ... talk to a RUNNING Mousiki (the
// player or the radio, whichever is active) through a small folder, <config dir>/control/:
//
//   running      written by the program (pid + mode), refreshed every 2 s; a client treats it as "not running" when older than 10 s
//   *.cmd        one command per file, written by the client (tmp file + rename, so a half-written command is never read);
//                the program reads them in name order and deletes them
//   status       what is playing now, rewritten about once a second (key=value lines)
//
// Plain files instead of a socket or pipe: the same code on Linux, macOS and Windows, no extra libraries, and anything that can
// run a command (a window manager's key binding, AutoHotkey, a macOS Shortcut, a script, SSH) can control the program.
// The program only accepts the fixed command names below, with a short numeric argument.
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc::remote {

struct Command {
    std::string name;   // one of known_commands()
    std::string arg;    // "" or a short number such as "60", "+10", "-30"; for name "cli": the command line (text)
    std::string id;     // "cli" only: the reply is written to reply-<id>.txt when this is not empty
    bool from_net = false;   // "cli" only: it came over the network (the web server): the restricted set of commands
};

// ---- inside the program -------------------------------------------------------------------------------------------------
void start(const char* mode);                 // "player" | "radio": create the folder, drop stale commands, mark the program as running
void stop();                                  // remove the marker (quit, or switching to the other mode)
void tick(const char* mode);                  // keeps the marker fresh; call every frame (cheap)
void reply(const std::string& id, bool ok, const std::string& text);   // answers a "cli" command that came with an id
std::vector<Command> poll();                  // the commands that arrived since the last call (at most 16; throttled to ~10 Hz)
void publish_status(const std::vector<std::pair<std::string, std::string>>& kv);   // throttled to ~1 Hz; call every frame
double number(const std::string& arg, double fallback);   // "+10" -> 10, "" -> fallback

// ---- the command line ---------------------------------------------------------------------------------------------------
bool is_client_call(int argc, char** argv);   // --cmd / --cli / --query / --run / --scripts / --status / --remote-help
int client_main(int argc, char** argv);       // exit code: 0 sent, 1 nothing running, 2 wrong usage

std::vector<std::string> known_commands();
std::filesystem::path dir();                  // exposed for tests

} // namespace muisc::remote
