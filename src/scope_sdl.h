#pragma once
// SDL2, loaded at run time for the scope window (scope_window_app.cpp). Only the handful of functions, constants and
// struct layouts the window uses are declared here, taken from the SDL2 headers (SDL 2.0.18 or newer: SDL_RenderGeometry).
// SDL2's ABI is stable across all 2.x releases, and sdl2-compat (SDL2 on top of SDL3) provides the same.
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace muisc::sdl {

struct Window; struct Renderer; struct Texture;
struct Rect { int x, y, w, h; };
struct Point { int x, y; };
struct Vertex { float x, y; uint8_t r, g, b, a; float u, v; };   // SDL_Vertex: SDL_FPoint, SDL_Color, SDL_FPoint
struct Version { uint8_t major, minor, patch; };

constexpr uint32_t kInitVideo = 0x20;
constexpr uint32_t kWinFullscreenDesktop = 0x1001, kWinResizable = 0x20, kWinHighDpi = 0x2000, kWinShown = 0x4;
constexpr uint32_t kWinMinimized = 0x40, kWinHidden = 0x8;
constexpr int kPosCentered = 0x2FFF0000;
constexpr uint32_t kRenAccelerated = 0x2, kRenVsync = 0x4, kRenTarget = 0x8;
constexpr uint32_t kFmtARGB8888 = 0x16362004;
constexpr int kTexStatic = 0, kTexTarget = 2;
constexpr int kBlendNone = 0, kBlendBlend = 1, kBlendAdd = 2;
constexpr int kFactorOne = 2, kOpRevSubtract = 3;
constexpr int kScaleNearest = 0;
constexpr int kScaleLinear = 1;
// events
constexpr uint32_t kEvQuit = 0x100, kEvWindow = 0x200, kEvKeyDown = 0x300, kEvMouseDown = 0x401;
constexpr uint32_t kEvTargetsReset = 0x2000, kEvDeviceReset = 0x2001;
constexpr uint8_t kWinEvResized = 5, kWinEvSizeChanged = 6, kWinEvClose = 14;
constexpr int32_t kKeyEsc = 27, kKeyF11 = 0x40000044;

struct Api {
    void* lib = nullptr;
    int (*Init)(uint32_t) = nullptr;
    void (*Quit)() = nullptr;
    const char* (*GetError)() = nullptr;
    int (*SetHint)(const char*, const char*) = nullptr;
    void (*GetVersion)(Version*) = nullptr;
    Window* (*CreateWin)(const char*, int, int, int, int, uint32_t) = nullptr;
    void (*DestroyWindow)(Window*) = nullptr;
    int (*SetWindowFullscreen)(Window*, uint32_t) = nullptr;
    uint32_t (*GetWindowFlags)(Window*) = nullptr;
    void (*GetWindowPosition)(Window*, int*, int*) = nullptr;
    void (*GetWindowSize)(Window*, int*, int*) = nullptr;
    void (*SetWindowTitle)(Window*, const char*) = nullptr;
    void (*SetWindowAlwaysOnTop)(Window*, int) = nullptr;            // 2.0.16, optional
    Renderer* (*CreateRenderer)(Window*, int, uint32_t) = nullptr;
    void (*DestroyRenderer)(Renderer*) = nullptr;
    int (*GetRendererOutputSize)(Renderer*, int*, int*) = nullptr;
    int (*GetRendererInfo)(Renderer*, void*) = nullptr;
    Texture* (*CreateTexture)(Renderer*, uint32_t, int, int, int) = nullptr;
    void (*DestroyTexture)(Texture*) = nullptr;
    int (*UpdateTexture)(Texture*, const Rect*, const void*, int) = nullptr;
    int (*SetTextureBlendMode)(Texture*, int) = nullptr;
    int (*SetTextureAlphaMod)(Texture*, uint8_t) = nullptr;
    int (*SetTextureScaleMode)(Texture*, int) = nullptr;            // 2.0.12, optional
    int (*SetRenderTarget)(Renderer*, Texture*) = nullptr;
    int (*SetRenderDrawColor)(Renderer*, uint8_t, uint8_t, uint8_t, uint8_t) = nullptr;
    int (*SetRenderDrawBlendMode)(Renderer*, int) = nullptr;
    int (*RenderClear)(Renderer*) = nullptr;
    int (*RenderFillRect)(Renderer*, const Rect*) = nullptr;
    int (*RenderDrawLines)(Renderer*, const Point*, int) = nullptr;
    int (*RenderCopy)(Renderer*, Texture*, const Rect*, const Rect*) = nullptr;
    int (*RenderGeometry)(Renderer*, Texture*, const Vertex*, int, const int*, int) = nullptr;
    void (*RenderPresent)(Renderer*) = nullptr;
    int (*RenderReadPixels)(Renderer*, const Rect*, uint32_t, void*, int) = nullptr;
    int (*ComposeCustomBlendMode)(int, int, int, int, int, int) = nullptr;
    int (*PollEvent)(void*) = nullptr;
    uint64_t (*GetPerformanceCounter)() = nullptr;
    uint64_t (*GetPerformanceFrequency)() = nullptr;
    void (*Delay)(uint32_t) = nullptr;
    int (*GetNumVideoDisplays)() = nullptr;
    int (*GetDisplayBounds)(int, Rect*) = nullptr;
};

inline void* lib_open() {
#if defined(_WIN32)
    if (HMODULE h = LoadLibraryA("SDL2.dll")) return reinterpret_cast<void*>(h);
    return nullptr;
#elif defined(__APPLE__)
    for (const char* n : {"libSDL2-2.0.0.dylib", "libSDL2.dylib", "/opt/homebrew/lib/libSDL2-2.0.0.dylib",
                          "/usr/local/lib/libSDL2-2.0.0.dylib", "/opt/local/lib/libSDL2-2.0.0.dylib"})
        if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
#else
    for (const char* n : {"libSDL2-2.0.so.0", "libSDL2-2.0.so", "libSDL2.so"})
        if (void* h = dlopen(n, RTLD_NOW | RTLD_LOCAL)) return h;
    return nullptr;
#endif
}
inline void* lib_sym(void* lib, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}
inline void lib_close(void* lib) {
    if (!lib) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

inline const char* missing_hint() {
#if defined(_WIN32)
    return "the scope window needs SDL2.dll (2.0.18+) next to mousiki.exe: get SDL2-<version>-win32-x64.zip from github.com/libsdl-org/SDL/releases";
#elif defined(__APPLE__)
    return "the scope window needs SDL2 (2.0.18+): brew install sdl2";
#else
    return "the scope window needs SDL2 (2.0.18+): install the package libsdl2-2.0-0 (Debian/Ubuntu), SDL2 (Fedora/Arch)";
#endif
}

// Loads SDL2 and every function. False (with `err`) when the library or a required function is missing.
inline bool load(Api& a, std::string* err) {
    a.lib = lib_open();
    if (!a.lib) { if (err) *err = missing_hint(); return false; }
    bool ok = true;
    std::string missing;
    auto get = [&](auto& fn, const char* name, bool required) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lib_sym(a.lib, name));
        if (!fn && required) { ok = false; if (missing.empty()) missing = name; }
    };
    get(a.Init, "SDL_Init", true); get(a.Quit, "SDL_Quit", true); get(a.GetError, "SDL_GetError", true);
    get(a.SetHint, "SDL_SetHint", true); get(a.GetVersion, "SDL_GetVersion", true);
    get(a.CreateWin, "SDL_CreateWindow", true); get(a.DestroyWindow, "SDL_DestroyWindow", true);
    get(a.SetWindowFullscreen, "SDL_SetWindowFullscreen", true); get(a.GetWindowFlags, "SDL_GetWindowFlags", true);
    get(a.GetWindowPosition, "SDL_GetWindowPosition", true); get(a.GetWindowSize, "SDL_GetWindowSize", true);
    get(a.SetWindowTitle, "SDL_SetWindowTitle", true); get(a.SetWindowAlwaysOnTop, "SDL_SetWindowAlwaysOnTop", false);
    get(a.CreateRenderer, "SDL_CreateRenderer", true); get(a.DestroyRenderer, "SDL_DestroyRenderer", true);
    get(a.GetRendererOutputSize, "SDL_GetRendererOutputSize", true); get(a.GetRendererInfo, "SDL_GetRendererInfo", true);
    get(a.CreateTexture, "SDL_CreateTexture", true); get(a.DestroyTexture, "SDL_DestroyTexture", true);
    get(a.UpdateTexture, "SDL_UpdateTexture", true); get(a.SetTextureBlendMode, "SDL_SetTextureBlendMode", true);
    get(a.SetTextureAlphaMod, "SDL_SetTextureAlphaMod", true); get(a.SetTextureScaleMode, "SDL_SetTextureScaleMode", false);
    get(a.SetRenderTarget, "SDL_SetRenderTarget", true); get(a.SetRenderDrawColor, "SDL_SetRenderDrawColor", true);
    get(a.SetRenderDrawBlendMode, "SDL_SetRenderDrawBlendMode", true); get(a.RenderClear, "SDL_RenderClear", true);
    get(a.RenderFillRect, "SDL_RenderFillRect", true); get(a.RenderDrawLines, "SDL_RenderDrawLines", true); get(a.RenderCopy, "SDL_RenderCopy", true);
    get(a.RenderGeometry, "SDL_RenderGeometry", true); get(a.RenderPresent, "SDL_RenderPresent", true);
    get(a.RenderReadPixels, "SDL_RenderReadPixels", true); get(a.ComposeCustomBlendMode, "SDL_ComposeCustomBlendMode", true);
    get(a.PollEvent, "SDL_PollEvent", true);
    get(a.GetPerformanceCounter, "SDL_GetPerformanceCounter", true); get(a.GetPerformanceFrequency, "SDL_GetPerformanceFrequency", true);
    get(a.Delay, "SDL_Delay", true);
    get(a.GetNumVideoDisplays, "SDL_GetNumVideoDisplays", true); get(a.GetDisplayBounds, "SDL_GetDisplayBounds", true);
    if (!ok) {
        if (err) *err = "SDL2 is too old (" + missing + " is missing); " + missing_hint();
        lib_close(a.lib);
        a.lib = nullptr;
    }
    return ok;
}

} // namespace muisc::sdl
