#pragma once
#include "miniaudio.h"
#include <string>
#include <vector>

// Compile-time platform audio backend selection.
//
// miniaudio handles the actual speaker output for song playback (see
// player.h). This context exists for anything *we* touch the audio
// subsystem for directly — right now that's just decoding cached files
// for the waveform (via ma_decoder, which doesn't need a device context),
// but it's wired up so a future native playback/mixer path can reuse it
// without re-deriving the per-platform backend list.
//
//   Linux   -> PulseAudio/ALSA (PipeWire intercepts both via its
//              pipewire-pulse / pipewire-alsa compatibility shims — there
//              is no separate native "pipewire" backend in miniaudio)
//   Windows -> WASAPI
//   Android -> OpenSL ES
namespace muisc {

// Initializes a ma_context using the platform-appropriate backend list.
// Returns true on success. Caller owns `out_context` and must call
// ma_context_uninit() on it when done.
bool init_platform_audio_context(ma_context& out_context);

const char* platform_backend_name();

// ---- the sound output device (Settings -> GENERAL -> AUDIO "Choose Audio Device", the % key) ------------------------------------
// One choice for all modes: config.txt AudioDevice=<name>; empty = the system's default device. Devices are found again by their
// name, the only thing that stays the same between runs.
struct AudioOutputDevice {
    std::string name;        // as the system names it ("Speakers (Realtek(R) Audio)", "Built-in Output" ...)
    bool is_default = false; // the system's default output right now
};
// The playback devices of this platform's backend, in the system's order. Opens a context of its own for it (a few ms, on WASAPI
// up to ~100 ms): call it when the list is wanted, not every frame. With MOUSIKI_NULL_AUDIO=1 / MOUSIKI_RADIO_NULL=1 (tests) the
// silent null device. Empty with `err` set when the backend could not be opened.
std::vector<AudioOutputDevice> list_output_devices(std::string* err = nullptr);
// Looks `name` up among the playback devices of `ctx`: true and `out` set when it is there. An empty name is never found
// (= use the default device).
bool find_output_device(ma_context& ctx, const std::string& name, ma_device_id& out);

} // namespace muisc
