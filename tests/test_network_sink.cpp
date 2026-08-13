#include "logpulsex/logpulsex.hpp"
#include "logpulsex/network_sink.hpp"
#include "logpulsex/socket_compat.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace logpulsex;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAIL: " << #cond << " (" << __LINE__ << ")\n"; ++failures; } } while(0)

static std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream f(path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
    return lines;
}

// Locate tcp_test_server.py regardless of the working directory tests
// happen to be run from. This matters more than it might seem: MSVC /
// Visual Studio's default CMake debug working directory is typically the
// build output directory (e.g. out/build/x64-Debug/cpplog/), NOT the
// project root or tests/ -- none of the relative-path candidates below
// would ever resolve there, which is exactly what caused this test to
// crash with an unhandled exception when run from Visual Studio.
// CMakeLists.txt defines LOGPULSEX_TESTS_SOURCE_DIR as the absolute path to
// tests/ at configure time specifically so this works regardless of
// whatever working directory the IDE or build system chooses -- that's
// the primary, reliable lookup. The relative-path candidates remain as a
// fallback for ad-hoc, non-CMake compilation (e.g. a bare g++ invocation
// from the project root during development).
// Launches tcp_test_server.py in the background, cross-platform.
// Fixes three related portability bugs found together:
//   1. Trailing "&" is POSIX shell syntax for backgrounding a process.
//      On Windows, std::system() invokes cmd.exe, where "&" is a command
//      *separator* (run sequentially), not a backgrounding operator --
//      it does not detach the process the way POSIX "&" does. Windows
//      needs "start /B" instead.
//   2. "python3" is the reliable interpreter name on Linux/macOS, but is
//      NOT reliably registered on Windows -- the official python.org
//      installer's most consistently available command there is the
//      "py" launcher (py -3), which is the Python core team's own
//      recommended way to handle this exact ambiguity on Windows.
//   3. Paths are quoted, since Windows user directories commonly contain
//      spaces (e.g. "C:\Users\Jane Smith\AppData\Local\Temp\...").
static void launch_server_background(const std::string& script, int port, const std::string& outfile) {
    std::string cmd;
#if defined(_WIN32)
    cmd = "start /B py -3 \"" + script + "\" " + std::to_string(port) + " \"" + outfile + "\"";
#else
    cmd = "python3 \"" + script + "\" " + std::to_string(port) + " \"" + outfile + "\" &";
#endif
    std::system(cmd.c_str());
}

static std::string find_server_script() {
#if defined(LOGPULSEX_TESTS_SOURCE_DIR)
    {
        fs::path candidate = fs::path(LOGPULSEX_TESTS_SOURCE_DIR) / "tcp_test_server.py";
        if (fs::exists(candidate)) return fs::absolute(candidate).string();
    }
#endif

    for (const char* candidate : {"tests/tcp_test_server.py", "tcp_test_server.py",
                                   "../tests/tcp_test_server.py"}) {
        if (fs::exists(candidate)) return fs::absolute(candidate).string();
    }
    throw std::runtime_error(
        "tcp_test_server.py not found -- if building without CMake, run this "
        "test from the project root or tests/ dir instead");
}

int main() {
    // An unresolvable script path (or any other setup failure) should
    // produce a clear message and a non-zero exit code, not an unhandled
    // exception crashing the process -- which is what happened here: the
    // exception from find_server_script() had nothing catching it.
    std::string server_script;
    try {
        server_script = find_server_script();
    } catch (const std::exception& e) {
        std::cerr << "Setup failed: " << e.what() << "\n";
        return 1;
    }

    // --- Test 1: connect to a listening server, send lines, verify receipt ---
    {
        std::string outfile = (fs::temp_directory_path() / "nettest_out1.txt").string();
        std::remove(outfile.c_str());
        launch_server_background(server_script, 19001, outfile);
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let it bind

        {
            NetworkSink sink("127.0.0.1", 19001);
            LogRecord r;
            r.level = Level::info;
            r.logger_name = "test";
            for (int i = 0; i < 5; ++i) {
                r.message = "line " + std::to_string(i);
                sink.write(r);
            }
            sink.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            CHECK(sink.is_connected());
            CHECK(sink.backlog_size() == 0);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto lines = read_lines(outfile);
        CHECK(lines.size() == 5);
        if (!lines.empty()) {
            CHECK(lines[0].find("line 0") != std::string::npos);
            CHECK(lines[4].find("line 4") != std::string::npos);
        }

        // Shut server down. Uses the cross-platform socket helper instead
        // of raw ::socket/::connect/htons/inet_pton -- those are POSIX
        // names with no unguarded equivalent on Windows (this exact code,
        // unguarded, is what failed to compile on MSVC: C2039/C3861/C2065
        // for connect/inet_pton/AF_INET/htons/socket/SOCK_STREAM).
        logpulsex::net::send_line_fire_and_forget("127.0.0.1", 19001, "__STOP__\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // --- Test 2: no server listening at all -- must not block/crash,
    // and must buffer lines locally instead of losing them silently ---
    {
        NetworkSink sink("127.0.0.1", 19002, /*max_buffered_lines=*/10,
                          /*connect_timeout_ms=*/150, /*send_timeout_ms=*/100);
        auto start = std::chrono::steady_clock::now();
        LogRecord r;
        r.level = Level::warn;
        r.logger_name = "test";
        r.message = "nobody home";
        sink.write(r);
        auto elapsed = std::chrono::steady_clock::now() - start;
        // Must return promptly (bounded by the connect timeout), never
        // hang indefinitely.
        CHECK(elapsed < std::chrono::seconds(2));
        CHECK(!sink.is_connected());
        CHECK(sink.backlog_size() == 1);
    }

    // --- Test 3: bounded backlog under sustained outage -- memory must
    // not grow without limit; oldest entries drop first ---
    {
        NetworkSink sink("127.0.0.1", 19003, /*max_buffered_lines=*/5,
                          /*connect_timeout_ms=*/100, /*send_timeout_ms=*/100);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "test";
        for (int i = 0; i < 20; ++i) {
            r.message = "msg " + std::to_string(i);
            sink.write(r);
        }
        CHECK(sink.backlog_size() == 5); // capped, not 20
        CHECK(sink.dropped_count() == 15);
    }

    // --- Test 4: reconnect after an outage -- start with no server,
    // buffer some lines, THEN start the server and verify a subsequent
    // write()/flush() delivers the backlog ---
    {
        std::string outfile = (fs::temp_directory_path() / "nettest_out4.txt").string();
        std::remove(outfile.c_str());

        NetworkSink sink("127.0.0.1", 19004, /*max_buffered_lines=*/100,
                          /*connect_timeout_ms=*/100, /*send_timeout_ms=*/100);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "test";
        for (int i = 0; i < 3; ++i) {
            r.message = "buffered " + std::to_string(i);
            sink.write(r);
        }
        CHECK(sink.backlog_size() == 3);
        CHECK(!sink.is_connected());

        // Now bring the server up.
        launch_server_background(server_script, 19004, outfile);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        // A subsequent write (or flush) should reconnect and drain backlog.
        r.message = "trigger reconnect";
        sink.write(r);
        sink.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        CHECK(sink.is_connected());
        CHECK(sink.backlog_size() == 0);

        auto lines = read_lines(outfile);
        CHECK(lines.size() == 4); // 3 buffered + 1 trigger
        if (lines.size() == 4) {
            CHECK(lines[0].find("buffered 0") != std::string::npos);
            CHECK(lines[3].find("trigger reconnect") != std::string::npos);
        }

        logpulsex::net::send_line_fire_and_forget("127.0.0.1", 19004, "__STOP__\n");
    }

    // --- Test 5: integration with Logger -- sink errors must never
    // propagate out and silence other sinks ---
    {
        Logger logger("network_integration_test");
        logger.set_level(Level::trace);
        auto net_sink = std::make_shared<NetworkSink>("127.0.0.1", 19005,
                                                        10, 100, 100); // nothing listening
        auto capture_calls = std::make_shared<std::atomic<int>>(0);
        struct CountingSink final : ISink {
            std::shared_ptr<std::atomic<int>> counter;
            explicit CountingSink(std::shared_ptr<std::atomic<int>> c) : counter(std::move(c)) {}
            void write(const LogRecord&) override { counter->fetch_add(1); }
            void flush() override {}
        };
        logger.add_sink(net_sink);
        logger.add_sink(std::make_shared<CountingSink>(capture_calls));

        for (int i = 0; i < 10; ++i) {
            logger.log(Level::info, __FILE__, __LINE__, __func__, "msg {}", i);
        }
        logger.flush();
        CHECK(capture_calls->load() == 10); // the other sink got everything
        CHECK(logger.sink_error_count_snapshot() == 0); // network sink never threw
    }

    if (failures == 0) { std::cout << "All NetworkSink tests passed.\n"; return 0; }
    std::cerr << failures << " failure(s).\n";
    return 1;
}
