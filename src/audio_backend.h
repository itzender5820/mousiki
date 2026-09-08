#pragma once
#include "miniaudio.h"

// Compile-time platform audio backend selection.
//
// ffplay/SDL handles the actual speaker output for song playback (see
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

} // namespace muisc
