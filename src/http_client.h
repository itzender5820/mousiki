#pragma once
#include <string>
#include <utility>
#include <vector>

namespace muisc {

// Minimal libcurl-backed HTTP GET -- deliberately narrow (GET + query
// params only, no POST/custom headers/auth) since that's all mousiki's
// two lyrics APIs (Better Lyrics, LRCLIB) actually need. Not a general
// HTTP client; this is what replaced Python's `requests` for those two
// calls specifically.
//
// `params` are URL-encoded (via curl_easy_escape) and appended as a
// query string onto `base_url`. Returns true with `out_body` populated
// on HTTP 200; false (with `err` set to a short human-readable reason)
// on anything else -- network failure, timeout, non-200 status.
bool http_get(const std::string& base_url,
              const std::vector<std::pair<std::string, std::string>>& params,
              std::string& out_body, std::string& err, long timeout_sec = 10);

// Called once at startup (see main.cpp) / shutdown -- libcurl technically
// self-initializes lazily on first use, but that lazy init is documented
// as not thread-safe, and mousiki does make its first HTTP call from a
// background thread (lyrics fetching runs off the main thread). Doing
// the global init explicitly, early, on the main thread before any
// worker thread could possibly race it, sidesteps that entirely.
void http_client_global_init();
void http_client_global_cleanup();

} // namespace muisc
