// The remote control's overlay: draws the status and the QR code of the remote app in a box on the
// main screen.
#include "remote_overlay.h"

#include <algorithm>
#include <cstdlib>

#include "qr_code.h"
#include "remote_server.h"
#include "terminal_ui.h"
#include "string_util.h"

namespace muisc::remote_ui {

namespace {

constexpr const char* kR = "\x1b[0m";

int vis(const std::string& s) {
    std::string plain;
    for (size_t k = 0; k < s.size(); ++k) {
        if (s[k] == '\x1b' && k + 1 < s.size() && s[k + 1] == '[') {
            k += 2;
            while (k < s.size() && !(s[k] >= 0x40 && s[k] <= 0x7e)) ++k;
            continue;
        }
        plain += s[k];
    }
    return display_width(plain);
}
std::string blanks(int n) { return std::string(static_cast<size_t>(std::max(0, n)), ' '); }
std::string rep(const std::string& g, int n) { std::string o; for (int i = 0; i < n; ++i) o += g; return o; }

} // namespace

std::string scan_url(const Values& val) {
    const std::string ip = remote::net::lan_address();
    return "http://" + (ip.empty() ? std::string("127.0.0.1") : ip) + ":" + std::to_string(val.port) + "/#pin=" + val.pin;
}

std::vector<std::string> build(const State& st, const Values& val, const Style& s, int max_w, int max_h, int& width) {
    const std::string ip = remote::net::lan_address();
    const std::string address = "http://" + (ip.empty() ? std::string("<this computer>") : ip) + ":" + std::to_string(val.port);
    const bool running = remote::net::running();
    const std::string err = remote::net::last_error();

    std::vector<std::string> qr;
    if (val.enabled && running && !val.pin.empty()) qr = qr::render_half_blocks(qr::encode(scan_url(val)), 2);
    const int qr_w = qr.empty() ? 0 : vis(qr[0]);
    const int text_w = 34;   // the widest line of the help text below
    int inner = std::max(58, qr_w + 3 + text_w);
    const bool fits = inner + 4 <= max_w && static_cast<int>(std::max(qr.size(), size_t{12})) + 10 <= max_h;   // top, blank, 5 rows, blank, ..., status, bottom
    if (!fits) { qr.clear(); inner = std::min(std::max(40, max_w - 4), 62); }
    width = inner + 4;

    std::vector<std::string> out;
    auto line = [&](const std::string& content) {
        out.push_back("\x01" + s.v + kR + " " + content + kR + blanks(inner - vis(content)) + " " + "\x01" + s.v + kR);
    };
    {
        const std::string title = " REMOTE APP ";
        out.push_back("\x01" + s.ul + s.h + kR + s.header + title + kR + "\x01" + rep(s.h, inner + 1 - vis(title)) + s.ur + kR);
    }
    line("");
    auto row = [&](int r, const std::string& label, const std::string& value, const std::string& tail) {
        std::string v;
        if (st.editing && st.row == r) v = s.change + " " + st.buffer + "_ " + kR;
        else v = (st.row == r ? s.cursor : s.value) + " " + value + " " + kR;
        line(s.header + pad_right(label, 13) + kR + ": " + v + (tail.empty() ? "" : "  " + s.legend + tail + kR));
    };
    row(0, "REMOTE APP", val.enabled ? "< ON >" : "< OFF >", val.enabled ? "a phone can control Mousiki" : "off: nothing listens");
    row(1, "PORT", std::to_string(val.port), "1024 - 65535 (default 4747)");
    row(2, "PIN", val.pin.empty() ? "-" : val.pin, "[n] a new random PIN");
    line(s.header + pad_right("ADDRESS", 13) + kR + ": " + s.text + address + kR);
    {   // the computer's other addresses (a VPN, a second adapter): the phone may need one of them instead
        std::string others;
        for (const auto& f : remote::net::lan_interfaces()) {
            if (f.ip == ip) continue;
            others += (others.empty() ? "" : "  ") + f.ip + (f.vpn ? " (VPN)" : f.virt ? " (virtual)" : "");
        }
        if (!others.empty()) line(s.header + pad_right("ALSO", 13) + kR + ": " + s.legend + truncate_str(others, inner - 15) + kR);
    }
    {
        std::string state;
        if (!val.enabled) state = s.legend + "off" + kR;
        else if (running) state = s.text + "running" + kR;
        else state = "\x1b[38;5;203m" + (err.empty() ? std::string("starting ...") : err) + kR;
        line(s.header + pad_right("SERVER", 13) + kR + ": " + state);
    }
    line("");

    const bool vpn = remote::net::vpn_active();
    std::vector<std::string> help = {
        s.header + "How to connect" + kR,
        s.text + "1. The phone in the same network" + kR,
        s.text + "   (Wi-Fi) as this computer." + kR,
        s.text + "2. Scan the code with the camera:" + kR,
        s.text + "   the remote opens, logged in." + kR,
        s.text + "   Or the Android app Mousiki" + kR,
        s.text + "   Remote: address, port, PIN." + kR,
        "",
#if defined(_WIN32)
        s.legend + "No connection? [f] opens the port" + kR,
        s.legend + "in the Windows firewall (admin)." + kR,
#else
        s.legend + "No connection? Check the firewall" + kR,
        s.legend + "(port above) of this computer." + kR,
#endif
        vpn ? "\x1b[38;5;203m" + std::string("A VPN is on: allow \"local network") + kR : s.legend + "s in the settings keeps it all." + kR,
        vpn ? "\x1b[38;5;203m" + std::string("access\" in the VPN app, or switch") + kR : std::string(),
        vpn ? "\x1b[38;5;203m" + std::string("it off; s saves the settings.") + kR : std::string(),
    };
    if (!qr.empty()) {
        const size_t n = std::max(qr.size(), help.size());
        for (size_t i = 0; i < n; ++i) {
            const std::string left = i < qr.size() ? qr[i] : blanks(qr_w);
            const std::string right = i < help.size() ? help[i] : std::string();
            line(left + "   " + right);
        }
    } else {
        if (!val.enabled) { line(s.legend + "Switch REMOTE APP on: the QR code to scan appears here." + kR); line(""); }
        else if (!running) { line(s.legend + "The QR code appears once the server runs." + kR); line(""); }
        else { line(s.legend + truncate_str("(the window is too small for the QR code: " + address + "/#pin=" + val.pin + ")", inner) + kR); line(""); }
        for (const auto& h : help) line(h);
    }
    line(st.status.empty() ? std::string() : "\x1b[38;5;246m" + truncate_str(st.status, inner) + kR);
    {
        const std::string legend = st.editing ? " [0-9] Type  [ENTER] Apply  [ESC] Cancel "
#if defined(_WIN32)
                                              : " [↑↓] Select  [←→/ENTER] Change  [n] New PIN  [f] Firewall  [ESC] Back ";
#else
                                              : " [↑↓] Select  [←→/ENTER] Change  [n] New PIN  [ESC] Back ";
#endif
        const std::string lg = truncate_str(legend, inner - 1);
        out.push_back("\x02" + s.ll + s.h + kR + s.legend + lg + kR + "\x02" + rep(s.h, inner + 1 - vis(lg)) + s.lr + kR);
    }
    if (static_cast<int>(out.size()) > max_h && max_h > 3) {   // a very small window: keep the top and the bottom line
        const std::string last = out.back();
        out.resize(static_cast<size_t>(max_h - 1));
        out.push_back(last);
    }
    // the frame's colour: the border gradient of the mode, row by row (smooth), or top colour + bottom line (rough)
    const size_t n = out.size();
    for (size_t i = 0; i < n; ++i) {
        const float t = n > 1 ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
        const std::string top = s.border_at ? s.border_at(t) : s.border;
        const std::string bottom = s.border_at ? s.border_at(t) : s.border_bottom;
        std::string& l = out[i];
        for (size_t at = l.find('\x01'); at != std::string::npos; at = l.find('\x01', at + top.size())) l.replace(at, 1, top);
        for (size_t at = l.find('\x02'); at != std::string::npos; at = l.find('\x02', at + bottom.size())) l.replace(at, 1, bottom);
    }
    return out;
}

bool key(State& st, Values& val, int k, bool arrow, bool& close) {
    close = false;
    if (st.editing) {
        if (!arrow && k >= '0' && k <= '9') {
            if (st.buffer.size() < (st.row == 1 ? 5u : 8u)) st.buffer += static_cast<char>(k);
            return false;
        }
        if (k == 8 || k == 127) { if (!st.buffer.empty()) st.buffer.pop_back(); return false; }
        if (k == 27) { st.editing = false; st.status = "not changed"; return false; }
        if (k == '\r' || k == '\n') {
            if (st.row == 1) {
                const int p = muisc::atoi_safe(st.buffer.c_str());
                if (p < 1024 || p > 65535) { st.status = "the port must be 1024 - 65535"; return false; }
                st.editing = false;
                if (p == val.port) return false;
                val.port = p;
                st.status = "port " + std::to_string(p) + " (the phone needs the new address)";
                return true;
            }
            if (!remote::net::pin_ok(st.buffer)) { st.status = "the PIN must be 4 to 8 digits"; return false; }
            st.editing = false;
            if (st.buffer == val.pin) return false;
            val.pin = st.buffer;
            st.status = "new PIN set: the phone asks for it again";
            return true;
        }
        return false;
    }
    if (k == 27 || (!arrow && (k == 'q' || k == 'Q'))) { close = true; return false; }
    if (arrow && k == 'A') { st.row = (st.row + 2) % 3; st.status.clear(); return false; }
    if (arrow && k == 'B') { st.row = (st.row + 1) % 3; st.status.clear(); return false; }
    if (!arrow && (k == 'f' || k == 'F')) {   // Windows: allow the port in the firewall (asks for administrator rights)
        st.status = remote::net::open_windows_firewall(val.port);
        return false;
    }
    if (!arrow && (k == 'n' || k == 'N')) {
        val.pin = remote::net::make_pin();
        st.status = "new PIN " + val.pin + ": the phone asks for it again";
        return true;
    }
    const bool change = (arrow && (k == 'C' || k == 'D')) || k == '\r' || k == '\n' || k == ' ';
    if (!change) return false;
    if (st.row == 0) {
        val.enabled = !val.enabled;
        if (val.enabled && val.pin.empty()) val.pin = remote::net::make_pin();
        st.status = val.enabled ? "remote app on" : "remote app off";
        return true;
    }
    if (arrow) return false;   // port / PIN: ENTER types a new value
    st.editing = true;
    st.buffer = st.row == 1 ? std::to_string(val.port) : val.pin;
    st.status.clear();
    return false;
}

} // namespace muisc::remote_ui
