#include <clocale>
#include "app.h"
#include "http_client.h"

int main() {
    if (!std::setlocale(LC_ALL, "")) {
        std::setlocale(LC_ALL, "C.UTF-8");
    } else {
        const char* cur = std::setlocale(LC_CTYPE, nullptr);
        if (cur && std::string(cur) == "C") {
            std::setlocale(LC_ALL, "C.UTF-8");
        }
    }

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
