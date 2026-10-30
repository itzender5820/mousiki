#include <clocale>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif
#include "app.h"
#include "http_client.h"

int main() {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    std::setlocale(LC_ALL, ".UTF-8");
#else
    if (!std::setlocale(LC_ALL, "")) {
        std::setlocale(LC_ALL, "C.UTF-8");
    } else {
        const char* cur = std::setlocale(LC_CTYPE, nullptr);
        if (cur && std::string(cur) == "C") {
            std::setlocale(LC_ALL, "C.UTF-8");
        }
    }
#endif

    // Must happen before any thread could possibly make an HTTP call
    // (lyrics fetching runs on a background thread) -- libcurl's lazy
    // self-init on first use isn't documented as thread-safe, so this
    // does it explicitly, once, here on the main thread first.
    muisc::http_client_global_init();

    muisc::App app;
    int rc = app.run();

    muisc::http_client_global_cleanup();
    return rc;
}
