#pragma once
#include <string>

namespace muisc {

struct ProcResult {
    std::string out;   // combined stdout (stderr redirected away unless merge_stderr=true)
    int exit_code = -1;
    bool ok() const { return exit_code == 0; }
};

// Runs `cmd` through the shell and captures stdout. Caller is responsible
// for shell-quoting any interpolated arguments (see shell_quote()).
ProcResult run_capture(const std::string& cmd, bool merge_stderr = false);

// Wraps a string in single quotes, safely escaping any embedded quotes,
// so it can be dropped into a shell command line.
std::string shell_quote(const std::string& s);

} // namespace muisc
