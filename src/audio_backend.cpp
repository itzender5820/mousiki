#include "audio_backend.h"
#include <cstddef>

namespace muisc {

bool init_platform_audio_context(ma_context& out_context) {
#if defined(MUISC_PLATFORM_ANDROID)
    ma_backend backends[] = { ma_backend_opensl };
#elif defined(MUISC_PLATFORM_WINDOWS)
    ma_backend backends[] = { ma_backend_wasapi };
#elif defined(MUISC_PLATFORM_LINUX)
    ma_backend backends[] = { ma_backend_pulseaudio, ma_backend_alsa };
#elif defined(MUISC_PLATFORM_MACOS)
    ma_backend backends[] = { ma_backend_coreaudio };
#else
    ma_backend backends[] = { ma_backend_null };
#endif

    ma_context_config cfg = ma_context_config_init();
    ma_result result = ma_context_init(backends, sizeof(backends) / sizeof(backends[0]), &cfg, &out_context);
    return result == MA_SUCCESS;
}

const char* platform_backend_name() {
#if defined(MUISC_PLATFORM_ANDROID)
    return "OpenSL ES (Android)";
#elif defined(MUISC_PLATFORM_WINDOWS)
    return "WASAPI (Windows)";
#elif defined(MUISC_PLATFORM_LINUX)
    return "PulseAudio/ALSA -> PipeWire (Linux)";
#elif defined(MUISC_PLATFORM_MACOS)
    return "CoreAudio (macOS)";
#else
    return "null";
#endif
}

} // namespace muisc
