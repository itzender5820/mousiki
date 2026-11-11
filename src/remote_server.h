#pragma once
// The remote APP: a small web server inside Mousiki (off by default; Settings -> GENERAL -> REMOTE / UPDATES -> Remote App) that the
// Android app -- or any phone browser -- talks to over the home network.
//
//   GET /                      the remote page (the Android app shows the same page)
//   GET /api/hello?pin=        checks the PIN; {"ok":true,"version":...,"mode":...}
//   GET /api/screen?pin=&cols=&rows=
//                              the phone screen, drawn by Mousiki itself (its disk, boxes, buttons, the colours of the theme
//                              in use): {"rev":..,"mode":..,"cols":..,"rows":..,"lines":[ANSI lines],"zones":[{x,y,w,h,cmd,arg}]}
//   GET /api/image?pin=&key=   the PNG the screen names under "image" (the audio book cover); image/png
//   GET /api/cmd?pin=&name=&arg=   one remote command (the names of `mousiki --cmd`)
//   GET /api/cli?pin=&line=&wait=   one command line (the language of `mousiki --cli`) and its reply, as JSON {"ok","reply","lines"}; only
//                              when config.txt has RemoteAppCli=true, and with a restricted set (no quit / set / edit / alias / force)
//   GET /api/status?pin=       what is playing (the keys of `mousiki --status`) as JSON
//
// Every /api/ call needs the PIN (shown in the settings, `mousiki --remote-info`); after 10 wrong PINs the API answers
// nothing for a minute. Only the fixed command names are accepted, exactly like the file-based remote control.
// The server lives for the whole process (player and radio); both modes only hand it the screen and read its commands.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "remote_control.h"

namespace muisc::remote::net {

struct Config {
    bool enabled = false;
    int port = 4747;
    std::string pin;       // 4-8 digits
    std::string hosts;     // extra names the server answers to (config.txt RemoteAppHosts=a,b): see host_ok()
    bool cli = false;      // GET /api/cli?line= (a command line with a reply): off by default, config.txt RemoteAppCli=true
};

void configure(const Config& c);      // starts / stops / restarts the server to match (cheap when nothing changed)
void shutdown();                      // at program exit
bool running();
std::string last_error();             // why it is not running ("" when it is, or when it is off)
std::string lan_address();            // best guess of this computer's address in the home network ("" when unknown)
struct Interface { std::string ip, name; bool vpn = false, virt = false; };
std::vector<Interface> lan_interfaces();   // this computer's IPv4 addresses, the most likely home-network one first
bool vpn_active();                    // an adapter that looks like a VPN is up (it can hide the home network from the phone)
std::string open_windows_firewall(int port);   // Windows: an inbound rule for the port (asks for administrator rights)
std::string url();                    // "http://<lan address>:<port>" ("" when off)
std::string make_pin();               // a fresh random 6-digit PIN

// ---- the program's side ------------------------------------------------------------------------------------------------
std::vector<Command> take_commands(); // commands that came in over the network (called by remote::poll())
// The answer to a command line that came in over the network (the id starts with "net-"): wakes the HTTP request that waits for it.
void deliver_reply(const std::string& id, bool ok, const std::string& text);

// A phone has asked for the screen within the last 3 s; cols / rows: the size it asked for. The program then builds the
// screen (at most ~12 times a second) and hands it over with set_screen().
bool screen_wanted(int& cols, int& rows);
// The phone's lists (playlists, stations): entries per page for a screen of H lines (two lines per entry; the frame,
// the note and the three boxes take the rest).
inline int phone_list_rows(int H) { return H - 10 > 6 ? (H - 10) / 2 : 3; }
// The player's LISTS screen: one line per entry, the local list on top (about two thirds), the saved playlists below; the
// frame, the note, two pane headers, the UP / BACK / DOWN boxes and the CLEAR QUEUE box take 12 lines.
inline void phone_lists_rows(int H, int& tracks, int& playlists) {
    const int avail = std::max(6, H - 12);
    playlists = std::max(3, avail / 3);
    tracks = std::max(3, avail - playlists);
}
struct Zone { int x = 0, y = 0, w = 0, h = 0; std::string cmd, arg; };
// `text_color`: the palette number of text without a colour of its own (the list text colour; "" = the page's default)
void set_screen(const char* mode, int cols, int rows, const std::vector<std::string>& lines, const std::vector<Zone>& zones,
                const std::string& text_color = "");
void set_status(const std::vector<std::pair<std::string, std::string>>& kv);
// A picture (a PNG) the phone draws at full resolution over a rectangle of cells of the screen -- the audio book's cover. The
// screen JSON names it ("image":{x,y,w,h,key}) and the phone fetches GET /api/image?key= once per key. Set before set_screen().
bool picture_has(const std::string& key);
void picture_store(const std::string& key, const std::string& png);
void set_picture(bool active, int x, int y, int w, int h, const std::string& key);

// ---- helpers (exposed for the tests) -------------------------------------------------------------------------------------
// Parses "GET /path?a=1&b=x%20y HTTP/1.1" into the path and the decoded query; false when it is not a GET/HEAD request line.
bool parse_request_line(const std::string& line, std::string& path, std::vector<std::pair<std::string, std::string>>& query);
// The answer to one request (status line, headers and body), without any socket: what the server thread sends back.
// `host`: the value of the request'"'"'s Host header ("" = none given: HTTP/1.0 clients and the tests); a request whose Host is not allowed (see
// host_ok) gets 400 and nothing else.
std::string handle_request(const std::string& request_line, const std::string& peer, const std::string& host = std::string());
// DNS rebinding: a web page can make its own domain name point at this computer and then send requests to the server with the browser
// as the carrier; such a request still carries the page's domain in Host. So Host must be an IP address (the phone, the app, curl on an
// address), "localhost", or one of the names in `extra` (comma separated, case does not matter, a port is ignored).
bool host_ok(const std::string& host, const std::string& extra);
bool pin_ok(const std::string& pin);   // the PIN rule: 4-8 digits
// True for an address of the home network or this computer (127.x, 10.x, 172.16-31.x, 192.168.x, 169.254.x, 100.64-127.x): the
// server answers only those; anything else (a public address forwarded in by a router) is closed at once.
bool lan_peer(const std::string& ip);

} // namespace muisc::remote::net
