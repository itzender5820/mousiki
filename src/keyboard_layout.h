#pragma once
#include <string>

// Which keyboard layout is in use, so the cheat sheets can name the key that switches between the music player and the
// radio the way it is printed on THIS keyboard. The switch key types the character '*' (the key is checked as that
// character), which sits on different keys on different layouts:
//   German / Austrian / Spanish / Italian   SHIFT and the + key        -> "SHIFT and +"
//   US / UK / Irish / Canadian / Australian SHIFT and the 8 key         -> "SHIFT+8"
//   anything else, or unknown               just the character          -> "*"
// Detection (once, cached): Linux XKB_DEFAULT_LAYOUT, `setxkbmap -query`, `localectl status`, /etc/default/keyboard, then the
// locale; macOS the selected input source; Windows GetKeyboardLayout().
namespace muisc {

std::string keyboard_layout_code();     // "de", "us", "gb", "fr", ... or "" when it cannot be told
std::string mode_switch_key_label();    // the text shown in the cheat sheets (see above)
std::string mode_switch_back_label();   // the same for the key that goes back to the previous mode ('_': SHIFT and - / SHIFT+-)
// "/" (the search key) as printed on THIS keyboard: SHIFT+7 on German, Austrian, Swiss, Spanish and Italian layouts, "/" elsewhere.
// "(" and ")" (the spectrogram / scope window keys) as printed on THIS keyboard: SHIFT+8 / SHIFT+9 on German, Austrian,
// Spanish and Italian layouts, SHIFT+9 / SHIFT+0 on US / UK ones; any other key (or layout) as it is.
std::string symbol_key_label(const std::string& key);
// A mode-switch key as set in config.txt (HKeyModeNext / HKeyModePrev; the typed character, 0 = none) as the cheat sheets show it.
std::string mode_key_label(int ch);
// The DEFAULT keys of "next mode" / "karaoke bigger" (next=true) and "previous mode" / "karaoke smaller": '*' and '_' (side by side on
// German-style keyboards), but "'" and ";" on US / UK style ones (] [ { } are the radio's timeshift keys), where '*' is SHIFT+8. Only fresh configs get them; a saved key stays.
std::string default_size_mode_key(bool next);
// The DEFAULT key of the CHOOSE AUDIO DEVICE overlay (HKeyAudioDevice): "°" (SHIFT+^) where the keyboard has it on that key (German,
// Austrian, Swiss), "&" elsewhere (US / UK keyboards have no °; "%" is the music player's "queue item to the bottom").
std::string default_audio_device_key();

} // namespace muisc
