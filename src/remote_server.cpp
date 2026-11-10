// The remote app's web server: a small HTTP server on the local network that serves the phone page,
// takes PIN-protected commands and returns the current screen as text.
#include "remote_server.h"
#include "tiny_json.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <shellapi.h>
using sock_t = SOCKET;
#define MOUSIKI_BAD_SOCK INVALID_SOCKET
#define mousiki_close_sock closesocket
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
#define MOUSIKI_BAD_SOCK (-1)
#define mousiki_close_sock ::close
#endif

#include "version.h"

namespace muisc::remote::net {

namespace {

// the remote page (generated from remote_app/web/index.html by tools/web_to_inc.py)
#include "remote_page.inc"

using Clock = std::chrono::steady_clock;

std::mutex g_mu;
Config g_cfg;                       // what is running
std::string g_error;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_running{false};

std::vector<Command> g_commands;
Clock::time_point g_screen_asked{};
int g_want_cols = 44, g_want_rows = 30;
std::string g_screen_json;
// The pictures the phone draws over the screen (PNGs, by key): the last few, because a turning cover makes a new one every few
// hundredths of a second and the phone asks for a picture a moment after it saw the screen that names it -- with only the newest kept,
// most of those requests came too late (404) and the disk turned in jerks.
std::deque<std::pair<std::string, std::string>> g_pics;
std::string g_pic_json;            // ,"image":{...} for the screen JSON ("" = none)
std::string g_mode = "player";
long long g_rev = 0;
std::string g_status_json = "{}";

// Wrong PINs are counted per address that sends them: 10 in a row lock THAT address out for a minute, so another device on the
// network cannot lock the phone out by guessing. At most kMaxGuards addresses are remembered (the oldest quiet one goes).
struct PinGuard { int bad = 0; Clock::time_point locked_until{}; Clock::time_point last{}; };
std::map<std::string, PinGuard> g_guards;
constexpr size_t kMaxGuards = 256;
PinGuard& guard_for(const std::string& peer, Clock::time_point now) {
    auto it = g_guards.find(peer);
    if (it == g_guards.end() && g_guards.size() >= kMaxGuards) {
        auto oldest = g_guards.begin();
        for (auto i = g_guards.begin(); i != g_guards.end(); ++i)
            if (i->second.locked_until < now && i->second.last < oldest->second.last) oldest = i;
        g_guards.erase(oldest);
    }
    PinGuard& g = g_guards[peer];
    g.last = now;
    return g;
}

std::string page() {
    std::string s;
    for (const char* const* p = kRemotePage; *p; ++p) s += *p;
    return s;
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            out += static_cast<char>(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else out += s[i];
    }
    return out;
}

std::string get(const std::vector<std::pair<std::string, std::string>>& q, const char* key) {
    for (const auto& [k, v] : q) if (k == key) return v;
    return std::string();
}

std::string reply(int code, const char* type, const std::string& body) {
    const char* text = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 403 ? "Forbidden" : code == 404 ? "Not Found"
                     : code == 429 ? "Too Many Requests" : code == 503 ? "Service Unavailable" : "Error";
    std::string r = "HTTP/1.1 " + std::to_string(code) + " " + text + "\r\n";
    r += std::string("Content-Type: ") + type + "\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Cache-Control: no-store\r\n";
    // (no Access-Control-Allow-Origin: the page and the API come from the same address, and no other web page may read the answers)
    r += "Connection: close\r\n\r\n";
    return r + body;
}

std::string json_reply(int code, const std::string& body) { return reply(code, "application/json; charset=utf-8", body); }

std::atomic<int> g_active{0};   // connections being answered right now

} // namespace

bool lan_peer(const std::string& ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255) return false;
    if (a == 127) return true;                              // this computer
    if (a == 10) return true;                               // private networks (RFC 1918)
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 192 && b == 168) return true;
    if (a == 169 && b == 254) return true;                  // link-local (a direct cable, no router)
    if (a == 100 && b >= 64 && b <= 127) return true;       // shared address space: phone hotspots, Tailscale
    return false;
}

namespace {

// One connection: read the request line, answer, close. Runs on its own short-lived thread, so a browser's idle
// "preconnect" sockets (opened ahead and never used) cannot hold up the real requests.
void serve_one(sock_t c, std::string ip) {
#if defined(_WIN32)
    DWORD to = 2000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
#else
    timeval to{2, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof to);
#endif
    std::string req;
    char buf[2048];
    // one deadline for the whole request (the 2 s above are per read): a client that sends a byte now and then cannot hold
    // one of the few connection slots for long
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (req.size() < 8192 && req.find("\r\n\r\n") == std::string::npos && req.find("\n\n") == std::string::npos &&
           std::chrono::steady_clock::now() < deadline) {
        const int got = static_cast<int>(recv(c, buf, sizeof buf, 0));
        if (got <= 0) break;
        req.append(buf, static_cast<size_t>(got));
    }
    if (!req.empty()) {
        const size_t eol = req.find_first_of("\r\n");
        std::string out;
        try { out = handle_request(eol == std::string::npos ? req : req.substr(0, eol), ip); }
        catch (...) { out = reply(500, "text/plain; charset=utf-8", "internal error"); }   // (an exception out of this thread would end the player)
        size_t sent = 0;
        while (sent < out.size()) {
            const int w = static_cast<int>(send(c, out.data() + sent, static_cast<int>(std::min<size_t>(out.size() - sent, 65536)), 0));
            if (w <= 0) break;
            sent += static_cast<size_t>(w);
        }
    }
    mousiki_close_sock(c);
    --g_active;
}

void serve_loop(sock_t listener) {
    while (!g_stop.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(listener, &rd);
        timeval tv{0, 200000};
        const int n = select(static_cast<int>(listener) + 1, &rd, nullptr, nullptr, &tv);
        if (n <= 0) continue;
        sockaddr_in peer{};
#if defined(_WIN32)
        int plen = sizeof peer;
#else
        socklen_t plen = sizeof peer;
#endif
        sock_t c = accept(listener, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (c == MOUSIKI_BAD_SOCK) continue;
        if (g_active.load() >= 24) { mousiki_close_sock(c); continue; }   // a flood: drop it
        char ip[64] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        if (!lan_peer(ip)) { mousiki_close_sock(c); continue; }   // only the home network (and this computer) may talk to it
        ++g_active;
        try { std::thread(serve_one, c, std::string(ip)).detach(); }
        catch (...) { --g_active; mousiki_close_sock(c); }
    }
    mousiki_close_sock(listener);
    // the answers still on their way (each ends within its 2 s timeouts)
    for (int i = 0; i < 60 && g_active.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_running = false;
}

void stop_locked() {
    if (g_thread.joinable()) {
        g_stop = true;
        g_thread.join();
    }
    g_stop = false;
    g_running = false;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------

bool pin_ok(const std::string& pin) {
    if (pin.size() < 4 || pin.size() > 8) return false;
    return std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string make_pin() {
    std::random_device rd;
    std::uniform_int_distribution<int> d(0, 999999);
    char b[8];
    std::snprintf(b, sizeof b, "%06d", d(rd));
    return b;
}

bool parse_request_line(const std::string& line, std::string& path, std::vector<std::pair<std::string, std::string>>& query) {
    path.clear();
    query.clear();
    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return false;
    const std::string method = line.substr(0, sp1);
    if (method != "GET" && method != "HEAD") return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    const std::string target = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
    if (target.empty() || target[0] != '/') return false;
    const size_t qm = target.find('?');
    path = url_decode(target.substr(0, qm));
    if (qm != std::string::npos) {
        const std::string qs = target.substr(qm + 1);
        size_t start = 0;
        while (start <= qs.size()) {
            size_t amp = qs.find('&', start);
            if (amp == std::string::npos) amp = qs.size();
            const std::string part = qs.substr(start, amp - start);
            if (!part.empty()) {
                const size_t eq = part.find('=');
                query.emplace_back(url_decode(part.substr(0, eq)), eq == std::string::npos ? std::string() : url_decode(part.substr(eq + 1)));
            }
            start = amp + 1;
        }
    }
    return true;
}

std::string handle_request(const std::string& request_line, const std::string& peer) {
    std::string path;
    std::vector<std::pair<std::string, std::string>> q;
    if (!parse_request_line(request_line, path, q)) return json_reply(400, "{\"error\":\"bad request\"}");
    if (path == "/" || path == "/index.html") return reply(200, "text/html; charset=utf-8", page());
    if (path == "/favicon.ico") return reply(404, "text/plain", "");
    if (path.compare(0, 5, "/api/") != 0) return json_reply(404, "{\"error\":\"not found\"}");

    std::lock_guard<std::mutex> lk(g_mu);
    const auto now = Clock::now();
    // "is this Mousiki?" -- the phone app's network scan asks with no PIN (pin=0). The answer (version, mode) is no secret and
    // does not count as a wrong PIN: the scan must not lock the phone itself out.
    if (path == "/api/hello" && get(q, "pin") != g_cfg.pin)
        return json_reply(200, std::string("{\"ok\":true,\"pin\":false,\"version\":\"") + tinyjson::escape(MOUSIKI_VERSION) + "\",\"mode\":\"" + tinyjson::escape(g_mode) + "\"}");
    PinGuard& guard = guard_for(peer, now);
    if (now < guard.locked_until) return json_reply(429, "{\"error\":\"too many wrong PINs, wait a minute\"}");
    if (get(q, "pin") != g_cfg.pin || g_cfg.pin.empty()) {
        if (++guard.bad >= 10) { guard.bad = 0; guard.locked_until = now + std::chrono::seconds(60); }
        return json_reply(403, "{\"error\":\"wrong PIN\"}");
    }
    guard.bad = 0;

    if (path == "/api/hello")
        return json_reply(200, std::string("{\"ok\":true,\"version\":\"") + tinyjson::escape(MOUSIKI_VERSION) + "\",\"mode\":\"" + tinyjson::escape(g_mode) + "\"}");
    if (path == "/api/status") return json_reply(200, g_status_json);
    if (path == "/api/screen") {
        auto small = [](const std::string& v) { const long n = std::strtol(v.c_str(), nullptr, 10); return static_cast<int>(std::clamp(n, 0L, 1000L)); };   // (atoi of a huge number: undefined)
        const int cols = small(get(q, "cols")), rows = small(get(q, "rows"));
        if (cols > 0) g_want_cols = std::clamp(cols, 36, 80);
        if (rows > 0) g_want_rows = std::clamp(rows, 20, 80);
        g_screen_asked = now;
        if (g_screen_json.empty()) return json_reply(503, "{\"wait\":true}");
        return json_reply(200, g_screen_json);
    }
    if (path == "/api/image") {
        const std::string want = get(q, "key");
        for (const auto& p : g_pics)
            if (p.first == want && !p.second.empty()) return reply(200, "image/png", p.second);
        return json_reply(404, "{\"error\":\"no such picture\"}");
    }
    if (path == "/api/cmd") {
        Command c;
        c.name = get(q, "name");
        c.arg = get(q, "arg");
        bool ok = false;
        for (const std::string& k : known_commands()) if (k == c.name) ok = true;
        if (!ok) return json_reply(400, "{\"error\":\"unknown command\"}");
        if (c.arg.size() > 12 || !std::all_of(c.arg.begin(), c.arg.end(), [](char ch) { return (ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || ch == '.'; }))
            return json_reply(400, "{\"error\":\"bad value\"}");
        if (g_commands.size() < 32) g_commands.push_back(c);
        g_screen_asked = now;   // the answer to a tap is the next screen: keep it coming
        return json_reply(200, "{\"ok\":true}");
    }
    return json_reply(404, "{\"error\":\"not found\"}");
}

void configure(const Config& c) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (c.pin != g_cfg.pin) g_guards.clear();   // a new PIN (set at the computer) ends every lock-out
    const bool same = c.enabled == g_cfg.enabled && c.port == g_cfg.port && c.pin == g_cfg.pin;
    if (same && (g_running.load() || !c.enabled || !g_error.empty())) return;
    // the PIN can change while it runs; a new port or on/off needs a new listener
    if (c.enabled && g_running.load() && c.port == g_cfg.port) { g_cfg = c; return; }
    stop_locked();
    g_cfg = c;
    g_error.clear();
    if (!c.enabled) return;
    if (!pin_ok(c.pin)) { g_error = "the PIN must be 4 to 8 digits"; return; }
#if defined(_WIN32)
    static bool wsa = false;
    if (!wsa) { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { g_error = "WSAStartup failed"; return; } wsa = true; }
#endif
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == MOUSIKI_BAD_SOCK) { g_error = "cannot open a network socket"; return; }
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof yes);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(static_cast<unsigned short>(c.port));
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 8) != 0) {
        mousiki_close_sock(s);
        g_error = "port " + std::to_string(c.port) + " is in use (another Mousiki?)";
        return;
    }
    g_running = true;
    g_thread = std::thread(serve_loop, s);
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    stop_locked();
    g_cfg.enabled = false;
}

bool running() { return g_running.load(); }

std::string last_error() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_error;
}

namespace {

// The address the system would use towards the internet ("connecting" a UDP socket sends nothing). With a VPN on this is
// usually the VPN's address, which a phone in the home network cannot reach.
std::string default_route_address() {
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == MOUSIKI_BAD_SOCK) return "";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);   // TEST-NET-1: never reached
    std::string out;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        sockaddr_in me{};
#if defined(_WIN32)
        int len = sizeof me;
#else
        socklen_t len = sizeof me;
#endif
        if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
            char ip[64];
            if (inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip) && std::strcmp(ip, "0.0.0.0") != 0) out = ip;
        }
    }
    mousiki_close_sock(s);
    return out;
}

bool contains_ci(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return h.find(n) != std::string::npos;
}

bool vpn_like(const std::string& name) {
    for (const char* k : {"vpn", "tap", "tun", "wireguard", "wintun", "wg", "utun", "ppp", "ipsec", "openvpn", "nord", "proton",
                          "mullvad", "tailscale", "zerotier", "cisco", "fortinet", "anyconnect", "pangp", "hamachi"})
        if (contains_ci(name, k)) return true;
    return false;
}
bool virtual_like(const std::string& name) {
    for (const char* k : {"virtual", "vmware", "virtualbox", "vbox", "hyper-v", "vethernet", "wsl", "docker", "br-", "veth", "virbr",
                          "loopback", "bluetooth"})
        if (contains_ci(name, k)) return true;
    return false;
}

int private_rank(const std::string& ip) {   // 0 best: 192.168.x.x, 10.x.x.x, 172.16-31.x.x; 9: not a home network address
    unsigned a = 0, b = 0;
    if (std::sscanf(ip.c_str(), "%u.%u", &a, &b) != 2) return 9;
    if (a == 192 && b == 168) return 0;
    if (a == 10) return 1;
    if (a == 172 && b >= 16 && b <= 31) return 2;
    return 9;
}

} // namespace

std::vector<Interface> lan_interfaces() {
#if defined(_WIN32)
    static bool wsa = false;
    if (!wsa) { WSADATA d; if (WSAStartup(MAKEWORD(2, 2), &d) == 0) wsa = true; }
#endif
    std::vector<Interface> out;
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && rc == ERROR_BUFFER_OVERFLOW; ++tries) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc == NO_ERROR) {
        for (auto* ad = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); ad; ad = ad->Next) {
            if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            char name[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, ad->FriendlyName, -1, name, sizeof name - 1, nullptr, nullptr);
            char desc[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, ad->Description, -1, desc, sizeof desc - 1, nullptr, nullptr);
            const std::string label = std::string(name) + " (" + desc + ")";
            const bool tunnel = ad->IfType == IF_TYPE_PPP || ad->IfType == IF_TYPE_TUNNEL || ad->IfType == 53 /* prop. virtual */;
            for (auto* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                char ip[64];
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr)->sin_addr, ip, sizeof ip);
                Interface itf{ip, name, tunnel || vpn_like(label), virtual_like(label)};
                out.push_back(itf);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
            char ip[64];
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, ip, sizeof ip);
            const std::string n = i->ifa_name ? i->ifa_name : "";
            out.push_back({ip, n, (i->ifa_flags & IFF_POINTOPOINT) != 0 || vpn_like(n), virtual_like(n)});
        }
        freeifaddrs(list);
    }
#endif
    out.erase(std::remove_if(out.begin(), out.end(), [](const Interface& f) { return f.ip.compare(0, 8, "169.254.") == 0 || f.ip.compare(0, 4, "127.") == 0; }),
              out.end());
    // the best first: a home-network address of a real adapter
    std::stable_sort(out.begin(), out.end(), [](const Interface& a, const Interface& b) {
        auto key = [](const Interface& f) { return (f.vpn ? 100 : 0) + (f.virt ? 50 : 0) + private_rank(f.ip) * 2; };
        return key(a) < key(b);
    });
    return out;
}

std::string lan_address() {
    const std::vector<Interface> all = lan_interfaces();
    if (!all.empty() && !all[0].vpn && !all[0].virt && private_rank(all[0].ip) < 9) return all[0].ip;
    const std::string def = default_route_address();
    if (!all.empty() && private_rank(all[0].ip) < 9) return all[0].ip;
    return def.empty() && !all.empty() ? all[0].ip : def;
}

bool vpn_active() {
    for (const Interface& f : lan_interfaces()) if (f.vpn) return true;
    return false;
}

std::string open_windows_firewall(int port) {
#if defined(_WIN32)
    // one inbound rule for the port, all network profiles (a network marked "public" blocks the phone otherwise); the old
    // rule of the same name is removed first. Windows asks for administrator rights.
    // cmd.exe and netsh.exe by their full paths in the system folder, started there: this runs with administrator rights, so a
    // cmd.exe / netsh.exe lying in the folder Mousiki was started in must never be the one that runs
    wchar_t dir[MAX_PATH];
    const UINT dn = GetSystemDirectoryW(dir, MAX_PATH);
    if (dn == 0 || dn >= MAX_PATH) return "the firewall rule was not added (the Windows system folder could not be found)";
    const std::wstring sys_dir(dir, dn);
    const std::wstring netsh = L"\"" + sys_dir + L"\\netsh.exe\"";
    const std::wstring cmd = sys_dir + L"\\cmd.exe";
    // (the whole command in one more pair of quotes: cmd /c drops the first and the last quote of a line that starts with one)
    const std::wstring args = L"/c \"" + netsh + L" advfirewall firewall delete rule name=\"Mousiki Remote\" >nul 2>&1 & " +
                              netsh + L" advfirewall firewall add rule name=\"Mousiki Remote\" dir=in action=allow protocol=TCP localport=" +
                              std::to_wstring(port) + L" profile=any\"";
    const auto r = reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"runas", cmd.c_str(), args.c_str(), sys_dir.c_str(), SW_HIDE));
    if (r <= 32) return "the firewall rule was not added (administrator rights refused?)";
    return "firewall: port " + std::to_string(port) + " allowed for incoming connections (rule \"Mousiki Remote\")";
#else
    (void)port;
    return "only needed on Windows (Linux: e.g. sudo ufw allow " + std::to_string(port) + "/tcp, if a firewall is on)";
#endif
}

std::string url() {
    int port;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_cfg.enabled) return "";
        port = g_cfg.port;
    }
    const std::string ip = lan_address();
    return "http://" + (ip.empty() ? std::string("<this computer>") : ip) + ":" + std::to_string(port);
}

std::vector<Command> take_commands() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<Command> out;
    out.swap(g_commands);
    return out;
}

bool screen_wanted(int& cols, int& rows) {
    if (!g_running.load()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    if (Clock::now() - g_screen_asked > std::chrono::seconds(3)) return false;
    cols = g_want_cols;
    rows = g_want_rows;
    return true;
}

void set_screen(const char* mode, int cols, int rows, const std::vector<std::string>& lines, const std::vector<Zone>& zones,
                const std::string& text_color) {
    std::string j = "{\"rev\":";
    std::lock_guard<std::mutex> lk(g_mu);
    g_mode = mode;
    j += std::to_string(++g_rev) + ",\"text\":\"" + tinyjson::escape(text_color) + "\",\"mode\":\"" + tinyjson::escape(mode) + "\",\"cols\":" + std::to_string(cols) + ",\"rows\":" + std::to_string(rows) + ",\"lines\":[";
    for (size_t i = 0; i < lines.size(); ++i) j += (i ? ",\"" : "\"") + tinyjson::escape(lines[i]) + "\"";
    j += "],\"zones\":[";
    for (size_t i = 0; i < zones.size(); ++i) {
        const Zone& z = zones[i];
        j += (i ? "," : "") + std::string("{\"x\":") + std::to_string(z.x) + ",\"y\":" + std::to_string(z.y) + ",\"w\":" + std::to_string(z.w) +
             ",\"h\":" + std::to_string(z.h) + ",\"cmd\":\"" + tinyjson::escape(z.cmd) + "\",\"arg\":\"" + tinyjson::escape(z.arg) + "\"}";
    }
    j += "]" + g_pic_json + "}";
    g_pic_json.clear();   // (one screen only: the next one names its picture again)
    g_screen_json.swap(j);
}

bool picture_has(const std::string& key) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& p : g_pics) if (p.first == key && !p.second.empty()) return true;
    return false;
}

void picture_store(const std::string& key, const std::string& png) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const auto& p : g_pics) if (p.first == key) return;
    g_pics.emplace_back(key, png);
    while (g_pics.size() > 10) g_pics.pop_front();
}

void set_picture(bool active, int x, int y, int w, int h, const std::string& key) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_pic_json = active ? ",\"image\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + ",\"w\":" + std::to_string(w) +
                              ",\"h\":" + std::to_string(h) + ",\"key\":\"" + tinyjson::escape(key) + "\"}"
                        : std::string();
}

void set_status(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string j = "{";
    for (size_t i = 0; i < kv.size(); ++i) j += (i ? ",\"" : "\"") + tinyjson::escape(kv[i].first) + "\":\"" + tinyjson::escape(kv[i].second) + "\"";
    j += "}";
    std::lock_guard<std::mutex> lk(g_mu);
    g_status_json.swap(j);
}

} // namespace muisc::remote::net
