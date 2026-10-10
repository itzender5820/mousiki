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

    muisc::App app;
    return app.run();
}
