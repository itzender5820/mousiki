// Starting programs: the quoting of arguments for a .cmd / .bat program (yt-dlp or Python installed as a shim on Windows).
// cmd.exe reads that command line before the program does, so text from a search field must neither end its argument
// ("BatBadBut") nor expand %NAME%. The text checks run on every platform; the round trip through a real cmd.exe on Windows.
#include "test_framework.h"
#include "process_util.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

using namespace muisc;

TEST(cmd_quoting_keeps_text_inside_its_quotes) {
    std::string q;
    CHECK(quote_arg_for_cmd("plain", q));
    CHECK_EQ(q, std::string("\"plain\""));
    CHECK(quote_arg_for_cmd("a \"b\" & echo PWNED", q));   // a quote is doubled: cmd.exe never leaves the quoted text
    CHECK_EQ(q, std::string("\"a \"\"b\"\" & echo PWNED\""));
    CHECK(quote_arg_for_cmd("100% %PATH%", q));            // % becomes an expansion that yields a plain %
    CHECK_EQ(q, std::string("\"100%%cd:~,% %%cd:~,%PATH%%cd:~,%\""));
    CHECK(quote_arg_for_cmd("C:\\dir\\", q));               // backslashes before the closing quote are doubled
    CHECK_EQ(q, std::string("\"C:\\dir\\\\\""));
    CHECK(quote_arg_for_cmd("x\\\"y", q));                  // ... and before a quote
    CHECK_EQ(q, std::string("\"x\\\\\"\"y\""));
    CHECK(quote_arg_for_cmd("", q));
    CHECK_EQ(q, std::string("\"\""));
    CHECK(!quote_arg_for_cmd("two\nlines", q));             // cannot be passed through cmd.exe: refused
    CHECK(!quote_arg_for_cmd("cr\r", q));
}

#if defined(_WIN32)
// The real thing: a .cmd file hands its arguments to Python, which prints them back as JSON. What arrives must be exactly
// what was sent, and nothing in it may have run as a command.
TEST(cmd_round_trip_through_a_real_cmd_exe) {
    const Args py = python_command();
    if (py.empty()) return;   // (no Python on this machine: nothing to check against)
    mt::TempHome home;
    const std::filesystem::path shim = home.path() / "echoargs.cmd";
    {
        std::ofstream f(shim);
        f << "@";
        for (size_t i = 0; i < py.size(); ++i) {   // (only the program is quoted: the py launcher does not read a quoted "-3" as its option)
            if (i == 0) f << "\"" << py[i] << "\" ";
            else f << py[i] << " ";
        }
        f << "-c \"import sys, json; print(json.dumps(sys.argv[1:]))\" %*\r\n";
    }
    const std::vector<std::string> sent = {"plain", "two words", "a \"quoted\" word", "x\" & echo PWNED & \"", "%PATH%",
                                           "100%", "C:\\dir\\", "caret^ bang! amp& pipe| lt< gt>", "back\\\"slash"};
    Args args = {shim.string()};
    args.insert(args.end(), sent.begin(), sent.end());
    const ProcResult r = run_capture(args, true);
    CHECK_EQ(r.exit_code, 0);
    CHECK(r.out.find("PWNED\r\n") == std::string::npos && r.out.find("PWNED\n") == std::string::npos);   // nothing ran
    std::string want = "[";
    for (size_t i = 0; i < sent.size(); ++i) {
        if (i) want += ", ";
        want += "\"";
        for (char c : sent[i]) {
            if (c == '"' || c == '\\') want += '\\';
            want += c;
        }
        want += "\"";
    }
    want += "]";
    std::string got = r.out;
    while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) got.pop_back();
    CHECK_EQ(got, want);
}
#endif

#if !defined(_WIN32)
// The capture pipe must not leak into a child that another thread starts at the same moment: the reader would otherwise wait for
// THAT child's exit (an ffprobe stuck behind a radio ffmpeg). One thread keeps starting "sleep 1" while two others capture "true".
TEST(a_captured_command_never_waits_for_an_unrelated_child) {
    std::atomic<bool> stop{false};
    std::atomic<long long> worst_ms{0};
    std::thread sleeper([&] {
        while (!stop.load()) {
            auto c = spawn_capture({"sleep", "1"}, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (c) c->kill();
            c.reset();
            // (killed at once would hide the leak: keep one alive for a while every so often)
            auto d = spawn_capture({"sleep", "1"}, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            (void)d;
        }
    });
    auto worker = [&] {
        for (int i = 0; i < 150; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            run_capture({"true"});
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            long long w = worst_ms.load();
            while (ms > w && !worst_ms.compare_exchange_weak(w, ms)) {}
        }
    };
    std::thread a(worker), b(worker);
    a.join();
    b.join();
    stop = true;
    sleeper.join();
    CHECK(worst_ms.load() < 500);   // with a leaked write end this was up to 1000 ms (the sleeper's lifetime)
}
#endif
