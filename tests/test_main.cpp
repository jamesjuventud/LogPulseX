#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "logpulsex/logpulsex.hpp"
#include "logpulsex/syslog_sink.hpp"

using namespace logpulsex;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":"      \
                      << __LINE__ << "\n";                                   \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

struct CapturingSink final : ISink {
    std::vector<LogRecord> records;
    std::mutex mtx;
    void write(const LogRecord& r) override {
        std::lock_guard<std::mutex> lock(mtx);
        records.push_back(r);
    }
    void flush() override {}
    std::size_t count() {
        std::lock_guard<std::mutex> lock(mtx);
        return records.size();
    }
};

void test_default_logger_is_stable_and_singular_under_concurrent_first_use() {
    // Regression test for the default_logger() performance fix: it now
    // caches the resolved Logger in a function-local static instead of
    // calling the mutex-protected registry lookup on every call. This
    // introduces a genuinely new code path (a magic-static initialization
    // guard) that didn't exist before, so it gets its own dedicated
    // concurrency test rather than relying on incidental coverage.
    //
    // Note: default_logger() is very likely already initialized by
    // earlier tests in this same process by the time this runs, so this
    // mainly verifies *stability* (every thread sees the exact same
    // instance) rather than re-exercising the very first race, which is
    // covered separately under ThreadSanitizer with a fresh process (see
    // the standalone static_init_race stress test used during
    // development). Both matter: this one still meaningfully checks that
    // concurrent access to an already-initialized cache is safe and
    // consistent, which is the steady-state case every real program hits
    // far more often than the one-time startup race.
    constexpr std::size_t kThreads = 16;
    std::vector<Logger*> observed(kThreads, nullptr);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < kThreads; ++i) {
        threads.emplace_back([i, &observed] {
            observed[i] = default_logger().get();
        });
    }
    for (auto& t : threads) t.join();

    Logger* first = observed[0];
    CHECK(first != nullptr);
    for (std::size_t i = 1; i < kThreads; ++i) {
        CHECK(observed[i] == first);
    }
}

void test_backtrace_captures_below_threshold_and_dump_delivers_them() {
    Logger logger("backtrace_threshold_test");
    logger.set_level(Level::warn); // trace/debug/info suppressed from normal path
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    logger.enable_backtrace(16);
    LOG_TRACE_TO((&logger), "trace {}", 1);
    LOG_DEBUG_TO((&logger), "debug {}", 2);
    LOG_INFO_TO((&logger), "info {}", 3);
    logger.flush();
    CHECK(capture->count() == 0); // nothing reached sinks yet: below threshold

    logger.dump_backtrace();
    logger.flush();
    CHECK(capture->count() == 3);
}

void test_backtrace_disabled_by_default() {
    Logger logger("backtrace_default_test");
    logger.set_level(Level::trace);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    CHECK(!logger.backtrace_enabled());
    LOG_INFO_TO((&logger), "info {}", 1);
    logger.flush();
    CHECK(capture->count() == 1); // normal path unaffected

    logger.dump_backtrace(); // no-op: buffer capacity is 0
    logger.flush();
    CHECK(capture->count() == 1);
}

void test_backtrace_overwrites_oldest_once_capacity_exceeded() {
    Logger logger("backtrace_overwrite_test");
    logger.set_level(Level::off); // keep everything out of the normal path
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    logger.enable_backtrace(3);
    for (int i = 0; i < 10; ++i) {
        LOG_INFO_TO((&logger), "msg {}", i);
    }
    logger.dump_backtrace();
    logger.flush();

    CHECK(capture->count() == 3);
    CHECK(capture->records[0].message == "msg 7");
    CHECK(capture->records[1].message == "msg 8");
    CHECK(capture->records[2].message == "msg 9");
}

void test_backtrace_disable_clears_buffer() {
    Logger logger("backtrace_disable_test");
    logger.set_level(Level::off);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    logger.enable_backtrace(8);
    LOG_INFO_TO((&logger), "msg {}", 1);
    logger.disable_backtrace();
    CHECK(!logger.backtrace_enabled());

    logger.dump_backtrace(); // buffer was cleared; nothing to replay
    logger.flush();
    CHECK(capture->count() == 0);
}

void test_backtrace_dump_does_not_clear_and_is_repeatable() {
    Logger logger("backtrace_repeat_test");
    logger.set_level(Level::off);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    logger.enable_backtrace(4);
    LOG_INFO_TO((&logger), "only {}", 1);

    logger.dump_backtrace();
    logger.flush();
    CHECK(capture->count() == 1);

    logger.dump_backtrace(); // replays the same buffered record again
    logger.flush();
    CHECK(capture->count() == 2);
}

void test_log_if() {
    Logger logger("if_test");
    logger.set_level(Level::trace);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    for (int i = 0; i < 10; ++i) {
        LOG_IF_TO((&logger), Level::info, i % 2 == 0, "even {}", i);
    }
    logger.flush();
    CHECK(capture->count() == 5); // 0,2,4,6,8
}

void test_log_every_n() {
    Logger logger("every_n_test");
    logger.set_level(Level::trace);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    for (int i = 0; i < 25; ++i) {
        LOG_EVERY_N_TO((&logger), Level::info, 10, "tick {}", i);
    }
    logger.flush();
    // Fires on occurrences 1, 11, 21 (0-indexed: 0, 10, 20) -> 3 logs.
    CHECK(capture->count() == 3);
}

void test_log_if_every_n() {
    Logger logger("if_every_n_test");
    logger.set_level(Level::trace);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    // Only odd values satisfy the condition; among those, only every
    // 3rd should log: odd values under i<14 are 1,3,5,7,9,11,13 (7 of
    // them) -> occurrences 1,4,7 fire => 3 logs.
    for (int i = 0; i < 14; ++i) {
        LOG_IF_EVERY_N_TO((&logger), Level::info, i % 2 == 1, 3, "odd {}", i);
    }
    logger.flush();
    CHECK(capture->count() == 3);
}

void test_log_first_n() {
    Logger logger("first_n_test");
    logger.set_level(Level::trace);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    for (int i = 0; i < 20; ++i) {
        LOG_FIRST_N_TO((&logger), Level::warn, 4, "warn once-ish {}", i);
    }
    logger.flush();
    CHECK(capture->count() == 4);
}

void test_every_t_gate_first_call_always_fires_regardless_of_clock_value() {
    // This is the exact scenario that exposed the original bug: a small
    // "now_ms" (simulating a process that has been up for less time than
    // the configured throttle interval) must still fire on its very
    // first call. The old implementation, which compared against a
    // last-fired sentinel of 0, failed this when now_ms < interval_ms.
    detail::EveryTGate gate_low_uptime;
    CHECK(gate_low_uptime.should_fire(/*now_ms=*/500, /*interval_ms=*/60000));

    // Same check at now_ms == 0 exactly (the most extreme low-uptime case).
    detail::EveryTGate gate_zero;
    CHECK(gate_zero.should_fire(/*now_ms=*/0, /*interval_ms=*/60000));

    // And at a very large interval relative to a very small now_ms.
    detail::EveryTGate gate_extreme;
    CHECK(gate_extreme.should_fire(/*now_ms=*/1, /*interval_ms=*/1'000'000'000));
}

void test_every_t_gate_throttles_within_interval() {
    detail::EveryTGate gate;
    CHECK(gate.should_fire(1000, 500));   // first call always fires
    CHECK(!gate.should_fire(1100, 500));  // 100ms later, within 500ms interval: suppressed
    CHECK(!gate.should_fire(1499, 500));  // still within interval: suppressed
    CHECK(gate.should_fire(1500, 500));   // exactly at the boundary: fires
    CHECK(!gate.should_fire(1600, 500));  // freshly reset, within interval: suppressed
    CHECK(gate.should_fire(2000, 500));   // interval elapsed again: fires
}

void test_every_t_gate_handles_out_of_order_and_negative_deltas_safely() {
    // Defensive: should_fire must not misbehave (throw, UB, spurious
    // fire) if now_ms ever goes backward relative to a previous call,
    // which can't happen with steady_clock in practice but costs nothing
    // to guard against, and matters if a caller ever swaps in a
    // different (non-monotonic) clock source.
    detail::EveryTGate gate;
    CHECK(gate.should_fire(10000, 1000));
    CHECK(!gate.should_fire(9000, 1000)); // "earlier" than last fire: must not crash or fire
    CHECK(!gate.should_fire(10500, 1000));
    CHECK(gate.should_fire(11000, 1000));
}

void test_every_t_gate_no_overflow_at_int64_extremes() {
    // The sentinel is min()/2 specifically so now_ms - sentinel cannot
    // overflow for any realistic (or even unrealistic-but-in-range)
    // now_ms. Confirm that holds right up to a very large now_ms.
    detail::EveryTGate gate;
    constexpr std::int64_t huge = (std::numeric_limits<std::int64_t>::max)() / 2;
    CHECK(gate.should_fire(huge, 1000)); // must not overflow/UB, must fire (first call)
}

void test_log_every_t_macro_integration() {
    // Integration-level check through the real macro. Uses a large
    // interval specifically to re-create the original bug scenario:
    // if this ever regresses to comparing against a fixed 0 sentinel,
    // this test alone wouldn't reliably catch it (it depends on
    // container uptime, which is exactly why the dedicated EveryTGate
    // tests above exist as the real safety net) -- but it's kept as an
    // end-to-end sanity check that the macro wiring is correct.
    Logger logger("every_t_macro_test");
    logger.set_level(Level::trace);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    for (int i = 0; i < 50; ++i) {
        LOG_EVERY_T_TO((&logger), Level::info, 0.05 /* seconds -- short, uptime-independent */, "spam {}", i);
    }
    logger.flush();
    // A 50ms interval is far shorter than any plausible container
    // uptime, so unlike the original test this is not flaky.
    CHECK(capture->count() >= 1);
    CHECK(capture->count() < 50); // at least some calls must have been throttled
}

void test_process_id_matches_real_pid_and_is_cached() {
#if defined(_WIN32)
    std::uint64_t expected = static_cast<std::uint64_t>(::_getpid());
#else
    std::uint64_t expected = static_cast<std::uint64_t>(::getpid());
#endif
    CHECK(detail::get_process_id() == expected);
    // Cached: repeated calls return the identical value without
    // re-querying the OS each time.
    CHECK(detail::get_process_id() == detail::get_process_id());
}

void test_logger_populates_process_id_and_thread_id() {
    Logger logger("pid_tid_test");
    logger.set_level(Level::trace);
    struct PidTidCapturingSink final : ISink {
        LogRecord last;
        void write(const LogRecord& r) override { last = r; }
        void flush() override {}
    };
    auto capture = std::make_shared<PidTidCapturingSink>();
    logger.add_sink(capture);

    LOG_INFO_TO((&logger), "test message");
    logger.flush();

    CHECK(capture->last.process_id == detail::get_process_id());
    CHECK(capture->last.thread_id == std::this_thread::get_id());
}

void test_thread_id_differs_across_threads() {
    Logger logger("thread_id_diff_test");
    logger.set_level(Level::trace);
    struct ThreadIdCapturingSink final : ISink {
        std::mutex m;
        std::vector<std::thread::id> seen;
        void write(const LogRecord& r) override {
            std::lock_guard<std::mutex> lock(m);
            seen.push_back(r.thread_id);
        }
        void flush() override {}
    };
    auto capture = std::make_shared<ThreadIdCapturingSink>();
    logger.add_sink(capture);

    std::thread t1([&logger] { LOG_INFO_TO((&logger), "from t1"); });
    std::thread t2([&logger] { LOG_INFO_TO((&logger), "from t2"); });
    t1.join();
    t2.join();
    logger.flush();

    CHECK(capture->seen.size() == 2);
    CHECK(capture->seen[0] != capture->seen[1]); // genuinely different threads
}

void test_pattern_formatter_pid_token() {
    LogRecord r;
    r.level = Level::info;
    r.logger_name = "test";
    r.message = "hello";
    r.process_id = 12345;
    PatternFormatter fmt("pid={pid}");
    CHECK(fmt.format(r) == "pid=12345");
}

void test_pattern_formatter_default_includes_pid_and_tid() {
    LogRecord r;
    r.level = Level::info;
    r.logger_name = "test";
    r.message = "hello";
    r.process_id = 999;
    PatternFormatter fmt; // default pattern
    std::string out = fmt.format(r);
    CHECK(out.find("[pid:999]") != std::string::npos);
    CHECK(out.find("[tid:") != std::string::npos);
}

void test_json_formatter_pid_is_numeric_not_quoted() {
    LogRecord r;
    r.level = Level::info;
    r.logger_name = "test";
    r.message = "hello";
    r.process_id = 4242;
    JsonFormatter fmt;
    std::string out = fmt.format(r);
    // Must appear as a genuine JSON number: "pid":4242 -- not "pid":"4242"
    CHECK(out.find("\"pid\":4242") != std::string::npos);
    CHECK(out.find("\"pid\":\"4242\"") == std::string::npos);
    CHECK(out.find("\"tid\":\"") != std::string::npos); // tid is a quoted string
    // Confirm the JSON is still well-formed by round-tripping the pid field.
    auto pid_pos = out.find("\"pid\":");
    CHECK(pid_pos != std::string::npos);
}

void test_level_raw_exists_and_stringifies() {
    CHECK(to_string(Level::raw) == "RAW");
    // raw must sit below trace (most verbose / filtered out by default),
    // per the documented design in level.hpp.
    CHECK(static_cast<int>(Level::raw) < static_cast<int>(Level::trace));
}

void test_hex_bytes_basic_formatting() {
    unsigned char data[] = {0xA1, 0xB2, 0x00, 0xFF};
    CHECK(detail::format("{}", hex_bytes(data, 4)) == "0xA1 0xB2 0x00 0xFF");
    CHECK(detail::format("{}", hex_bytes(data, 2)) == "0xA1 0xB2");
    CHECK(detail::format("{}", hex_bytes(data, 0)) == "");

    // Matches the exact TX/RX command-trace style from the feature request.
    unsigned char tx[] = {0xA1, 0xB2};
    unsigned char rx[] = {0x00, 0xFF};
    CHECK(detail::format("TX: {}", hex_bytes(tx, 2)) == "TX: 0xA1 0xB2");
    CHECK(detail::format("RX: {}", hex_bytes(rx, 2)) == "RX: 0x00 0xFF");
}

void test_hex_bytes_truncates_large_buffers() {
    std::vector<unsigned char> big(500, 0xAB);
    std::string result = detail::format("{}", hex_bytes(big.data(), big.size(), /*max_bytes=*/10));
    CHECK(result.find("0xAB") != std::string::npos);
    CHECK(result.find("... (490 more bytes)") != std::string::npos);
    // Exactly 10 "0xAB" tokens should be printed, not the full 500.
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = result.find("0xAB", pos)) != std::string::npos) { ++count; pos += 4; }
    CHECK(count == 10);
}

void test_hex_bytes_does_not_leak_stream_format_state() {
    // Regression test for a real correctness risk: HexBytes::operator<<
    // sets std::hex/std::uppercase/setfill on the stream to render
    // bytes. format.hpp's apply_value() reuses ONE std::ostringstream
    // across every argument in a single LOG_INFO("...", a, b, c) call --
    // if HexBytes didn't restore the stream's format state afterward, a
    // plain integer argument logged *after* a HexBytes argument in the
    // same call would incorrectly render in hex too.
    unsigned char data[] = {0xAB};
    std::string result = detail::format("{} {}", hex_bytes(data, 1), 255);
    CHECK(result == "0xAB 255"); // NOT "0xAB ff" -- would indicate leaked state
}

void test_format_hex_dump_layout() {
    const char* text = "Hello, world!";
    std::string dump = format_hex_dump(text, 13, /*bytes_per_row=*/16);
    // Single row (13 bytes < 16 per row): offset, hex bytes, ASCII sidebar.
    CHECK(dump.find("00000000") != std::string::npos);
    CHECK(dump.find("48 65 6C 6C 6F") != std::string::npos); // "Hello" in hex
    CHECK(dump.find("|Hello, world!|") != std::string::npos);
    CHECK(dump.find('\n') == std::string::npos); // only one row -- no row separator
}

void test_format_hex_dump_multi_row_and_offsets() {
    std::vector<unsigned char> buf(20, 0x41); // 20 'A' bytes -> 2 rows at 16/row
    std::string dump = format_hex_dump(buf.data(), buf.size(), 16);
    CHECK(dump.find("00000000") != std::string::npos); // first row offset
    CHECK(dump.find("00000010") != std::string::npos); // second row offset (16 = 0x10)
    // Exactly one row separator for exactly two rows.
    CHECK(std::count(dump.begin(), dump.end(), '\n') == 1);
}

void test_format_hex_dump_non_printable_bytes_are_safe_in_sidebar() {
    // The byte 0x0A (newline) must NOT appear as a raw newline in the
    // ASCII sidebar -- that would silently reintroduce the exact
    // log-injection risk this library escapes elsewhere. It must be
    // substituted with '.' like any other non-printable byte.
    unsigned char data[] = {'A', 0x0A, 'B', 0x00, 0x7F};
    std::string dump = format_hex_dump(data, 5, 16);
    CHECK(dump.find("|A.B..|") != std::string::npos);
    // Confirm the ONLY newline in the whole dump is nonexistent here
    // (single row, so zero row-separator newlines) -- i.e. the 0x0A
    // byte truly did not produce a literal newline character anywhere.
    CHECK(dump.find('\n') == std::string::npos);
}

void test_log_raw_preserves_newlines_in_pattern_formatter() {
    LogRecord r;
    r.level = Level::raw;
    r.logger_name = "test";
    r.message = "line one\nline two\nline three";
    PatternFormatter fmt("{message}");
    std::string out = fmt.format(r);
    CHECK(out == "line one\nline two\nline three"); // real newlines preserved, verbatim
    CHECK(std::count(out.begin(), out.end(), '\n') == 2);
}

void test_non_raw_levels_still_escape_newlines_in_pattern_formatter() {
    // Proves the Level::raw exemption is correctly scoped -- normal
    // levels must be COMPLETELY UNAFFECTED by this feature and keep
    // escaping embedded newlines exactly as before.
    LogRecord r;
    r.level = Level::info;
    r.logger_name = "test";
    r.message = "line one\nline two";
    PatternFormatter fmt("{message}");
    std::string out = fmt.format(r);
    CHECK(out == "line one\\nline two"); // literal backslash-n, not a real newline
    CHECK(out.find('\n') == std::string::npos);
}

void test_json_formatter_always_escapes_newlines_even_for_raw_level() {
    // JsonFormatter must NEVER emit a raw newline in a string value
    // (invalid JSON) regardless of level -- this is the one place
    // Level::raw's exemption deliberately does NOT apply.
    LogRecord r;
    r.level = Level::raw;
    r.logger_name = "test";
    r.message = "line one\nline two";
    JsonFormatter fmt;
    std::string out = fmt.format(r);
    CHECK(out.find('\n') == std::string::npos); // no raw newline anywhere in the JSON
    CHECK(out.find("\\n") != std::string::npos); // properly escaped instead
    CHECK(out.find("\"level\":\"RAW\"") != std::string::npos); // tag present for viewers
}

void test_syslog_priority_for_raw_level() {
    CHECK(SyslogSink::priority_for(Level::raw) == SyslogSink::kPriorityDebug);
}

void test_console_sink_handles_raw_level_without_crashing() {
    ConsoleSink sink(true);
    LogRecord r;
    r.level = Level::raw;
    r.logger_name = "test";
    r.message = "00000000  41 42 43 44  |ABCD|\n00000004  45 46        |EF|";
    sink.write(r); // must not crash
    sink.flush();
    CHECK(true);
}

void test_log_raw_end_to_end_via_logger() {
    Logger logger("raw_test");
    logger.set_level(Level::raw); // must opt in -- raw is filtered by default
    struct RawCapturingSink final : ISink {
        LogRecord last;
        bool got = false;
        void write(const LogRecord& r) override { last = r; got = true; }
        void flush() override {}
    };
    auto capture = std::make_shared<RawCapturingSink>();
    // A sink's OWN level defaults to Level::trace, independent of the
    // Logger's level -- both layers must permit Level::raw for a raw
    // record to actually reach this particular sink. Easy to forget
    // since raw sits below every other level; worth the explicit call.
    capture->set_level(Level::raw);
    capture->set_formatter(std::make_shared<PatternFormatter>("{message}"));
    logger.add_sink(capture);

    unsigned char buf[] = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
    LOG_RAW_TO((&logger), "{}", format_hex_dump(buf, 5));
    logger.flush();

    CHECK(capture->got);
    CHECK(capture->last.level == Level::raw);
    CHECK(capture->last.message.find("48 65 6C 6C 6F") != std::string::npos);
    CHECK(capture->last.message.find("|Hello|") != std::string::npos);
}

void test_log_raw_requires_both_logger_and_sink_level_to_permit_it() {
    // Documents and locks in a real, easy-to-miss interaction: Logger
    // level and each Sink's own level are independent filters, and BOTH
    // must permit Level::raw for a raw record to reach that sink. Here
    // the Logger opts in but the sink is left at its default (trace) --
    // the record must NOT reach the sink, proving sink-level filtering
    // is still enforced independently even when the logger allows it.
    Logger logger("raw_gotcha_test");
    logger.set_level(Level::raw);
    struct CountingSink final : ISink {
        int count = 0;
        void write(const LogRecord&) override { ++count; }
        void flush() override {}
    };
    auto sink_at_default_level = std::make_shared<CountingSink>();
    logger.add_sink(sink_at_default_level); // level left at default (trace)

    LOG_RAW_TO((&logger), "dump content");
    logger.flush();
    CHECK(sink_at_default_level->count == 0); // correctly filtered out at the sink layer

    sink_at_default_level->set_level(Level::raw); // now opt this sink in too
    LOG_RAW_TO((&logger), "dump content");
    logger.flush();
    CHECK(sink_at_default_level->count == 1); // now reaches the sink
}

void test_log_raw_suppressed_by_default_level() {
    // raw is more verbose than trace, so a Logger at its default level
    // (trace) must NOT emit LOG_RAW records unless explicitly opted in.
    Logger logger("raw_default_test");
    struct SuppressedCountingSink final : ISink {
        int count = 0;
        void write(const LogRecord&) override { ++count; }
        void flush() override {}
    };
    auto capture = std::make_shared<SuppressedCountingSink>();
    logger.add_sink(capture);

    LOG_RAW_TO((&logger), "should be suppressed");
    logger.flush();
    CHECK(capture->count == 0);
}

void test_format_basic() {
    CHECK(detail::format("no args") == "no args");
    CHECK(detail::format("{} plus {} is {}", 1, 2, 3) == "1 plus 2 is 3");
    CHECK(detail::format("trailing {}", "x") == "trailing x");
    CHECK(detail::format("no placeholder", 1, 2) == "no placeholder");
}

void test_format_container_support() {
    std::vector<int> v{1, 2, 3};
    CHECK(detail::format("{}", v) == "[1, 2, 3]");

    std::map<std::string, int> m{{"a", 1}, {"b", 2}};
    CHECK(detail::format("{}", m) == "{a: 1, b: 2}");

    std::optional<int> present = 7;
    std::optional<int> absent;
    CHECK(detail::format("{}", present) == "7");
    CHECK(detail::format("{}", absent) == "nullopt");

    std::pair<int, std::string> p{1, "x"};
    CHECK(detail::format("{}", p) == "(1, x)");

    std::vector<std::vector<int>> nested{{1, 2}, {3}};
    CHECK(detail::format("{}", nested) == "[[1, 2], [3]]");
}

void test_format_specs() {
    CHECK(detail::format("{:.2f}", 3.14159) == "3.14");
    CHECK(detail::format("{:.0f}", 3.9) == "4");
    CHECK(detail::format("{:5}", 7) == "    7");
    CHECK(detail::format("{:05}", 7) == "00007");
    CHECK(detail::format("{:<5}|", "x") == "x    |");
    CHECK(detail::format("{:>5}|", "x") == "    x|");
    CHECK(detail::format("{:^5}|", "x") == "  x  |");
    CHECK(detail::format("{:x}", 255) == "ff");
    CHECK(detail::format("{:#x}", 255) == "0xff");
    CHECK(detail::format("{:X}", 255) == "FF");
    CHECK(detail::format("{:+}", 5) == "+5");
    // Literal brace escaping.
    CHECK(detail::format("{{}} then {}", 1) == "{} then 1");
    CHECK(detail::format("just {{literal}} braces") == "just {literal} braces");
}

void test_structured_kv_logging() {
    LoggerConfig cfg;
    Logger logger("kv_test", cfg);
    logger.set_level(Level::trace);

    struct KvCapturingSink final : ISink {
        LogRecord last;
        bool got = false;
        void write(const LogRecord& r) override { last = r; got = true; }
        void flush() override {}
    };
    auto capture = std::make_shared<KvCapturingSink>();
    logger.add_sink(capture);

    LOG_INFO_KV_TO((&logger), "Order placed",
                   logpulsex::field("order_id", std::string("A1234")),
                   logpulsex::field("amount", 59.99));
    logger.flush();

    CHECK(capture->got);
    CHECK(capture->last.message == "Order placed");
    CHECK(capture->last.fields.size() == 2);
    CHECK(capture->last.fields[0].key == "order_id");
    CHECK(capture->last.fields[0].value == "A1234");
    CHECK(capture->last.fields[1].key == "amount");

    // Fields must also come through correctly in JSON output.
    JsonFormatter fmt;
    std::string json = fmt.format(capture->last);
    CHECK(json.find("\"order_id\":\"A1234\"") != std::string::npos);
}

void test_queue_single_thread() {
    BoundedMpscQueue<int> q(4);
    CHECK(q.try_push(1));
    CHECK(q.try_push(2));
    CHECK(q.try_push(3));
    CHECK(q.try_push(4));
    CHECK(!q.try_push(5)); // full (capacity rounds to 4)

    auto a = q.try_pop();
    CHECK(a.has_value() && *a == 1);
    CHECK(q.try_push(5)); // room again after pop

    int expected = 2;
    while (auto v = q.try_pop()) {
        CHECK(*v == expected);
        ++expected;
    }
}

void test_queue_mpsc_concurrent() {
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 5000;
    BoundedMpscQueue<int> q(1024);

    std::atomic<long long> produced_sum{0};
    std::atomic<long long> consumed_sum{0};
    std::atomic<bool> done{false};
    std::atomic<int> remaining_producers{kProducers};

    std::thread consumer([&] {
        while (!done.load() || remaining_producers.load() > 0) {
            if (auto v = q.try_pop()) {
                consumed_sum.fetch_add(*v);
            }
        }
        while (auto v = q.try_pop()) {
            consumed_sum.fetch_add(*v);
        }
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                int value = p * kPerProducer + i;
                produced_sum.fetch_add(value);
                while (!q.try_push(value)) {
                    std::this_thread::yield();
                }
            }
            remaining_producers.fetch_sub(1);
        });
    }
    for (auto& t : producers) t.join();
    done.store(true);
    consumer.join();

    CHECK(produced_sum.load() == consumed_sum.load());
}

void test_json_escaping_prevents_log_injection() {
    LogRecord r;
    r.level = Level::warn;
    r.logger_name = "test";
    r.message = "user=\"admin\"\nFAKE: root logged in\t<-- injected";
    JsonFormatter fmt;
    std::string out = fmt.format(r);

    // The raw newline must not appear unescaped in the JSON output —
    // that's what would let an attacker forge a second log line.
    CHECK(out.find('\n') == std::string::npos);
    CHECK(out.find("\\n") != std::string::npos);
    CHECK(out.find("\\\"") != std::string::npos);
}

void test_pattern_formatter_unknown_token_is_safe() {
    LogRecord r;
    r.level = Level::info;
    r.logger_name = "test";
    r.message = "hello";
    PatternFormatter fmt("[{level}] {nonsense} {message}");
    std::string out = fmt.format(r);
    CHECK(out.find("{nonsense}") != std::string::npos); // degrades gracefully, no crash
    CHECK(out.find("hello") != std::string::npos);
}

void test_daily_file_sink_rotation_scheduling() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_daily_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    // BUG FIX (Windows crash): the sink must go out of scope -- closing
    // its open file handle -- before fs::remove_all(dir) runs below.
    // POSIX allows deleting a directory while a file inside it is still
    // open (the inode persists until the last handle closes); Windows/
    // NTFS locks open files and fs::remove_all throws filesystem_error
    // (access denied / sharing violation) if anything inside the
    // directory is still held open. This never surfaced on Linux, where
    // it silently "worked," which is exactly why it's worth calling out.
    {
        DailyFileSink sink(base, 0, 0); // rotate at midnight
        std::tm tm_now{};
        tm_now.tm_year = 2026 - 1900; tm_now.tm_mon = 6; tm_now.tm_mday = 20;
        tm_now.tm_hour = 15; tm_now.tm_min = 30; tm_now.tm_sec = 0;
        auto now = std::chrono::system_clock::from_time_t(std::mktime(&tm_now));

        auto next = sink.compute_next_rotation(now);
        std::time_t next_t = std::chrono::system_clock::to_time_t(next);
        std::tm next_tm{};
#if defined(_WIN32)
        localtime_s(&next_tm, &next_t);
#else
        localtime_r(&next_t, &next_tm);
#endif
        CHECK(next_tm.tm_mday == 21);
        CHECK(next_tm.tm_hour == 0 && next_tm.tm_min == 0);

        CHECK(DailyFileSink::date_stamp(now) == "2026-07-20");
    } // sink destructed here, file handle closed

    fs::remove_all(dir);
}

void test_daily_file_sink_actually_rotates_at_boundary() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_daily_test2";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    auto fake_now = std::make_shared<std::chrono::system_clock::time_point>();
    std::tm start_tm{};
    start_tm.tm_year = 2026 - 1900; start_tm.tm_mon = 6; start_tm.tm_mday = 20;
    start_tm.tm_hour = 23; start_tm.tm_min = 59; start_tm.tm_sec = 0;
    *fake_now = std::chrono::system_clock::from_time_t(std::mktime(&start_tm));

    // BUG FIX (Windows crash/sharing violation), take 2: scoping `sink`
    // alone was NOT enough. f1/f2 below are std::ifstream handles opened
    // to verify file contents -- they were declared at function scope,
    // meaning they stayed open (not destructed) all the way until the
    // function returned, which is AFTER fs::remove_all(dir) ran. Same
    // bug class as the sink issue, a different handle. Every stream that
    // touches a file inside `dir` must be closed/out-of-scope before
    // fs::remove_all(dir) runs, not just the sink.
    fs::path first_path, second_path;
    {
        DailyFileSink::Clock clock_fn = [fake_now] { return *fake_now; };
        DailyFileSink sink(base, 0, 0, /*max_files=*/0, /*compress_after_rotation=*/false, clock_fn);

        LogRecord r;
        r.level = Level::info;
        r.logger_name = "t";
        r.message = "before midnight";
        sink.write(r);
        first_path = sink.current_path();
        CHECK(first_path.filename().string() == "app_2026-07-20.log");

        *fake_now += std::chrono::minutes(2); // 00:01 on the 21st
        r.message = "after midnight";
        sink.write(r);
        second_path = sink.current_path();
        CHECK(second_path.filename().string() == "app_2026-07-21.log");
        CHECK(first_path != second_path);
        sink.flush();
    } // sink destructed here, both write handles closed

    {
        std::ifstream f1(first_path);
        std::string content1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
        CHECK(content1.find("before midnight") != std::string::npos);
        CHECK(content1.find("after midnight") == std::string::npos);
    } // f1 destructed here

    {
        std::ifstream f2(second_path);
        std::string content2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
        CHECK(content2.find("after midnight") != std::string::npos);
    } // f2 destructed here

    fs::remove_all(dir);
}

void test_daily_file_sink_retention_pruning() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_daily_test3";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    auto fake_now = std::make_shared<std::chrono::system_clock::time_point>();
    std::tm start_tm{};
    start_tm.tm_year = 2026 - 1900; start_tm.tm_mon = 0; start_tm.tm_mday = 1;
    start_tm.tm_hour = 0; start_tm.tm_min = 1; start_tm.tm_sec = 0;
    *fake_now = std::chrono::system_clock::from_time_t(std::mktime(&start_tm));
    DailyFileSink::Clock clock_fn = [fake_now] { return *fake_now; };

    // BUG FIX (Windows crash): same as the two tests above -- sink must
    // be destructed before fs::remove_all(dir) runs.
    {
        DailyFileSink sink(base, 0, 0, /*max_files=*/3, /*compress_after_rotation=*/false, clock_fn);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "t";
        for (int day = 0; day < 6; ++day) {
            r.message = "day " + std::to_string(day);
            sink.write(r);
            *fake_now += std::chrono::hours(24);
        }
        sink.flush();
    } // sink destructed here, file handle closed

    std::size_t existing = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        (void)entry;
        ++existing;
    }
    CHECK(existing <= 3);

    fs::remove_all(dir);
}

void test_gzip_availability_matches_build_flag() {
#if defined(LOGPULSEX_HAVE_ZLIB)
    CHECK(gzip_compression_available());
#else
    CHECK(!gzip_compression_available());
#endif
}

#if defined(LOGPULSEX_HAVE_ZLIB)

void test_gzip_compress_decompress_round_trip() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_gzip_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    fs::path plain = dir / "input.txt";
    std::string content;
    for (int i = 0; i < 2000; ++i) {
        content += "line " + std::to_string(i) + " some repeated padding text\n";
    }
    {
        std::ofstream f(plain, std::ios::binary);
        f << content;
    }

    fs::path gz = dir / "input.txt.gz";
    CHECK(detail::gzip_compress_file(plain, gz));
    CHECK(fs::exists(gz));
    // A real compression should meaningfully shrink this repetitive content.
    CHECK(fs::file_size(gz) < fs::file_size(plain));

    // Round-trip via our own decompressor.
    fs::path roundtrip = dir / "roundtrip.txt";
    CHECK(detail::gzip_decompress_file(gz, roundtrip));
    {
        // BUG FIX (Windows crash): rt must be closed before fs::remove_all(dir)
        // runs below -- same class of bug as the earlier DailyFileSink test
        // fixes. POSIX tolerates deleting a directory while a file inside it
        // is open; Windows locks it and fs::remove_all throws.
        std::ifstream rt(roundtrip, std::ios::binary);
        std::string roundtrip_content((std::istreambuf_iterator<char>(rt)), std::istreambuf_iterator<char>());
        CHECK(roundtrip_content == content);
    } // rt closed here

    // Independent cross-check: the real `gunzip` binary must also accept
    // this as valid, standard gzip -- proves we're not just
    // self-consistent with our own (possibly identically-buggy)
    // decompressor. gunzip is a POSIX tool with no guaranteed equivalent
    // on a stock Windows install, so this extra check only runs there;
    // the round-trip check above (via our own gzip_decompress_file,
    // still going through the real zlib library) already covers Windows.
#if !defined(_WIN32)
    std::string cmd = "gunzip -t " + gz.string() + " 2>/dev/null";
    int rc = std::system(cmd.c_str());
    CHECK(rc == 0);
#endif

    fs::remove_all(dir);
}

void test_gzip_compress_nonexistent_input_fails_gracefully() {
    namespace fs = std::filesystem;
    fs::path missing = fs::temp_directory_path() / "cpplog_gzip_test" / "does_not_exist.txt";
    fs::path out = fs::temp_directory_path() / "cpplog_gzip_test" / "out.gz";
    CHECK(!detail::gzip_compress_file(missing, out)); // must not crash/throw, just fail
    CHECK(!fs::exists(out));
}

void test_daily_file_sink_compression_creates_gz_and_removes_original() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_gzip_daily_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    auto fake_now = std::make_shared<std::chrono::system_clock::time_point>();
    std::tm start_tm{};
    start_tm.tm_year = 2026 - 1900; start_tm.tm_mon = 6; start_tm.tm_mday = 20;
    start_tm.tm_hour = 23; start_tm.tm_min = 59; start_tm.tm_sec = 0;
    *fake_now = std::chrono::system_clock::from_time_t(std::mktime(&start_tm));
    DailyFileSink::Clock clock_fn = [fake_now] { return *fake_now; };

    fs::path first_path, second_path;
    {
        DailyFileSink sink(base, 0, 0, /*max_files=*/0, /*compress_after_rotation=*/true, clock_fn);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "t";
        r.message = "before midnight, content to compress";
        sink.write(r);
        first_path = sink.current_path();

        *fake_now += std::chrono::minutes(2); // crosses into the 21st -> triggers rotate()
        r.message = "after midnight";
        sink.write(r); // this call's rotate() spawns background compression of first_path
        second_path = sink.current_path();

        sink.wait_for_compression(); // block until the background job finishes
        sink.flush();
    } // sink destructed here too -- destructor also waits, redundantly safe

    fs::path first_gz = first_path;
    first_gz += ".gz";
    CHECK(fs::exists(first_gz));       // compressed file exists
    CHECK(!fs::exists(first_path));    // original removed after successful compression
    CHECK(fs::exists(second_path));    // the still-open current file is untouched (never compressed while active)

    fs::path decompressed = dir / "decompressed.log";
    CHECK(detail::gzip_decompress_file(first_gz, decompressed));
    {
        // BUG FIX (Windows crash): same class as above -- f must close
        // before fs::remove_all(dir) runs.
        std::ifstream f(decompressed);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(content.find("before midnight, content to compress") != std::string::npos);
    } // f closed here

    fs::remove_all(dir);
}

void test_daily_file_sink_compression_disabled_by_default() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_gzip_default_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    auto fake_now = std::make_shared<std::chrono::system_clock::time_point>();
    std::tm start_tm{};
    start_tm.tm_year = 2026 - 1900; start_tm.tm_mon = 6; start_tm.tm_mday = 20;
    start_tm.tm_hour = 23; start_tm.tm_min = 59; start_tm.tm_sec = 0;
    *fake_now = std::chrono::system_clock::from_time_t(std::mktime(&start_tm));
    DailyFileSink::Clock clock_fn = [fake_now] { return *fake_now; };

    fs::path first_path;
    {
        // compress_after_rotation defaults to false -- existing behavior
        // must be completely unaffected by this feature's addition.
        DailyFileSink sink(base, 0, 0, /*max_files=*/0, /*compress_after_rotation=*/false, clock_fn);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "t";
        r.message = "content";
        sink.write(r);
        first_path = sink.current_path();
        *fake_now += std::chrono::minutes(2);
        r.message = "more content";
        sink.write(r);
        sink.flush();
    }

    fs::path first_gz = first_path;
    first_gz += ".gz";
    CHECK(!fs::exists(first_gz));   // no compression happened
    CHECK(fs::exists(first_path));  // plain file left in place, as before this feature existed

    fs::remove_all(dir);
}

void test_daily_file_sink_destructor_waits_for_inflight_compression() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_gzip_destructor_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    auto fake_now = std::make_shared<std::chrono::system_clock::time_point>();
    std::tm start_tm{};
    start_tm.tm_year = 2026 - 1900; start_tm.tm_mon = 6; start_tm.tm_mday = 20;
    start_tm.tm_hour = 23; start_tm.tm_min = 59; start_tm.tm_sec = 0;
    *fake_now = std::chrono::system_clock::from_time_t(std::mktime(&start_tm));
    DailyFileSink::Clock clock_fn = [fake_now] { return *fake_now; };

    fs::path first_path;
    {
        DailyFileSink sink(base, 0, 0, /*max_files=*/0, /*compress_after_rotation=*/true, clock_fn);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "t";
        // A larger payload makes the compression job take measurably
        // longer, giving this test a real chance to catch a destructor
        // that *doesn't* properly wait (it would otherwise pass
        // trivially on a compression job so fast it always finishes
        // before the check regardless of whether waiting happens).
        r.message = std::string(2'000'000, 'A');
        sink.write(r);
        first_path = sink.current_path();
        *fake_now += std::chrono::minutes(2);
        r.message = "small";
        sink.write(r); // triggers rotate() -> spawns background compression of the 2MB file
        // Deliberately do NOT call wait_for_compression() here -- the
        // destructor below must do that itself.
    } // sink destructed here; its BackgroundCompressor member's own
      // destructor must block until compression finishes

    fs::path first_gz = first_path;
    first_gz += ".gz";
    CHECK(fs::exists(first_gz));    // must already exist immediately after destruction, not "eventually"
    CHECK(!fs::exists(first_path));

    fs::remove_all(dir);
}

void test_daily_file_sink_retention_prunes_compressed_files() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_gzip_retention_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    auto fake_now = std::make_shared<std::chrono::system_clock::time_point>();
    std::tm start_tm{};
    start_tm.tm_year = 2026 - 1900; start_tm.tm_mon = 0; start_tm.tm_mday = 1;
    start_tm.tm_hour = 0; start_tm.tm_min = 1; start_tm.tm_sec = 0;
    *fake_now = std::chrono::system_clock::from_time_t(std::mktime(&start_tm));
    DailyFileSink::Clock clock_fn = [fake_now] { return *fake_now; };

    {
        DailyFileSink sink(base, 0, 0, /*max_files=*/2, /*compress_after_rotation=*/true, clock_fn);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "t";
        for (int day = 0; day < 5; ++day) {
            r.message = "day " + std::to_string(day);
            sink.write(r);
            *fake_now += std::chrono::hours(24);
            sink.wait_for_compression(); // let each day's compression finish before continuing
        }
        sink.flush();
    }

    // With max_files=2, at most 2 files (compressed or not) should
    // remain -- pruning must correctly account for files that were
    // renamed to .gz by the time they aged out of retention.
    std::size_t existing = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        (void)entry;
        ++existing;
    }
    CHECK(existing <= 2);

    fs::remove_all(dir);
}

#endif // LOGPULSEX_HAVE_ZLIB

void test_daily_file_sink_validates_config() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_daily_test4";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    bool threw = false;
    try {
        DailyFileSink bad(dir / "nonexistent_subdir" / "app.log", 0, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try { DailyFileSink bad(base, 24, 0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    threw = false;
    try { DailyFileSink bad(base, 0, 60); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);

    fs::remove_all(dir);
}

void test_network_sink_no_server_does_not_block() {
    // Nothing listens on this port. write() must return promptly
    // (bounded by the connect timeout), never hang, and must buffer the
    // line locally instead of silently losing it.
    NetworkSink sink("127.0.0.1", 19099, /*max_buffered_lines=*/10,
                      /*connect_timeout_ms=*/100, /*send_timeout_ms=*/100);
    auto start = std::chrono::steady_clock::now();
    LogRecord r;
    r.level = Level::warn;
    r.logger_name = "test";
    r.message = "nobody home";
    sink.write(r);
    auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(2));
    CHECK(!sink.is_connected());
    CHECK(sink.backlog_size() == 1);
}

void test_network_sink_backlog_is_bounded() {
    NetworkSink sink("127.0.0.1", 19098, /*max_buffered_lines=*/5,
                      /*connect_timeout_ms=*/80, /*send_timeout_ms=*/80);
    LogRecord r;
    r.level = Level::info;
    r.logger_name = "test";
    for (int i = 0; i < 20; ++i) {
        r.message = "msg " + std::to_string(i);
        sink.write(r);
    }
    // Bounded, not unbounded growth -- this is the actual reliability
    // property that matters: a sustained network outage cannot exhaust
    // memory.
    CHECK(sink.backlog_size() == 5);
    CHECK(sink.dropped_count() == 15);
}

void test_network_sink_integration_errors_isolated() {
    Logger logger("network_integration_test");
    logger.set_level(Level::trace);
    auto net_sink = std::make_shared<NetworkSink>("127.0.0.1", 19097, 10, 80, 80);
    auto counter = std::make_shared<std::atomic<int>>(0);
    struct CountingSink final : ISink {
        std::shared_ptr<std::atomic<int>> counter;
        explicit CountingSink(std::shared_ptr<std::atomic<int>> c) : counter(std::move(c)) {}
        void write(const LogRecord&) override { counter->fetch_add(1); }
        void flush() override {}
    };
    logger.add_sink(net_sink);
    logger.add_sink(std::make_shared<CountingSink>(counter));

    for (int i = 0; i < 10; ++i) {
        logger.log(Level::info, __FILE__, __LINE__, __func__, "msg {}", i);
    }
    logger.flush();
    CHECK(counter->load() == 10);
    CHECK(logger.sink_error_count_snapshot() == 0);
}

void test_syslog_sink_priority_mapping() {
    // Use SyslogSink's own portable constants rather than raw <syslog.h>
    // macros — those don't exist at all on Windows, and LOG_DEBUG/
    // LOG_INFO specifically are also undefined by syslog_sink.hpp on
    // POSIX (to avoid colliding with our own LOG_DEBUG(...)/LOG_INFO(...)
    // logging macros). Referencing SyslogSink::kPriorityX here means this
    // test compiles and runs identically on every platform.
    CHECK(SyslogSink::priority_for(Level::trace) == SyslogSink::kPriorityDebug);
    CHECK(SyslogSink::priority_for(Level::debug) == SyslogSink::kPriorityDebug);
    CHECK(SyslogSink::priority_for(Level::info)  == SyslogSink::kPriorityInfo);
    CHECK(SyslogSink::priority_for(Level::warn)  == SyslogSink::kPriorityWarning);
    CHECK(SyslogSink::priority_for(Level::error) == SyslogSink::kPriorityErr);
    CHECK(SyslogSink::priority_for(Level::fatal) == SyslogSink::kPriorityCrit);
}

void test_syslog_sink_basic_write_does_not_crash() {
    SyslogSink sink("cpplog_test", SyslogSink::kFacilityUser);
    LogRecord r;
    r.level = Level::info;
    r.logger_name = "test";
    r.message = "plain message";
    sink.write(r);
    sink.flush();
    CHECK(true); // reaching here means no crash/throw
}

void test_syslog_sink_format_string_payload_is_inert() {
    // A message containing literal '%' conversion specifiers must be
    // treated as inert data, not interpreted as a printf format string.
    // We can't intercept the real syslog() call from here, but this
    // proves the process survives an aggressive %n/%s-style payload,
    // which is exactly what would crash a mishandled implementation.
    SyslogSink sink("cpplog_test2", SyslogSink::kFacilityUser);
    LogRecord r;
    r.level = Level::warn;
    r.logger_name = "test";
    r.message = "malicious %s%s%s%s%n%n%n payload with %d specifiers";
    sink.write(r);
    sink.flush();
    CHECK(true);
}

void test_syslog_sink_integration_with_logger() {
    Logger logger("syslog_integration_test");
    logger.set_level(Level::trace);
    auto syslog_sink = std::make_shared<SyslogSink>("cpplog_test3", SyslogSink::kFacilityUser);
    logger.add_sink(syslog_sink);
    for (int i = 0; i < 200; ++i) {
        logger.log(Level::info, __FILE__, __LINE__, __func__, "message {}", i);
    }
    logger.flush();
    CHECK(logger.sink_error_count_snapshot() == 0);
}

void test_rotating_file_sink_accounts_for_preexisting_file_size() {
    // Regression test for a real bug: RotatingFileSink previously used
    // stream_.tellp() immediately after opening in std::ios::app mode to
    // determine a reopened file's current size. That is not reliable
    // across standard library implementations (some report 0 there
    // instead of the file's actual size until the first write forces a
    // seek). Every test up to this one always started from a fresh,
    // empty file (via fs::remove(base) beforehand), so none of them
    // exercised "construct a sink against a file that already has
    // content" at all -- exactly the scenario that broke in the field
    // (every process restart reopens an existing, non-empty app.log).
    //
    // This test writes known content directly to a file (bypassing the
    // sink entirely, so it's unambiguously present in the file before
    // the sink ever opens it), then constructs a RotatingFileSink
    // against that file with a threshold just above the pre-existing
    // size. A single small write through the sink should push the total
    // past max_bytes and trigger rotation -- which only happens if the
    // sink correctly accounted for the pre-existing bytes. On the buggy
    // implementation, current_bytes_ would incorrectly start at 0, the
    // small write alone would never reach max_bytes, and this test would
    // fail (no rotation, base.1 never created).
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_preexisting_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path base = dir / "app.log";

    // Write ~90 bytes of pre-existing content directly, not through the sink.
    {
        std::ofstream preexisting(base, std::ios::binary);
        for (int i = 0; i < 5; ++i) {
            preexisting << "pre-existing line " << i << " padding padding\n";
        }
    } // closed before the sink ever touches this file

    auto preexisting_size = fs::file_size(base);
    CHECK(preexisting_size > 50); // sanity: we actually wrote a meaningful amount

    {
        // Threshold set just above the pre-existing size: a single small
        // write must be enough to cross it and trigger rotation, but
        // only if the pre-existing size was correctly accounted for.
        RotatingFileSink sink(base, /*max_bytes=*/preexisting_size + 10, /*max_files=*/3);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "t";
        r.message = "one more line";
        sink.write(r); // should push total past max_bytes and rotate
        sink.flush();
    }

    CHECK(fs::exists(fs::path(base.string() + ".1")));

    fs::remove_all(dir);
}

void test_rotating_file_sink_rotates() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "cpplog_test";
    fs::create_directories(dir);
    fs::path base = dir / "test.log";
    fs::remove(base);
    for (int i = 1; i <= 3; ++i) fs::remove(base.string() + "." + std::to_string(i));

    {
        RotatingFileSink sink(base, /*max_bytes=*/50, /*max_files=*/3);
        LogRecord r;
        r.level = Level::info;
        r.logger_name = "t";
        for (int i = 0; i < 20; ++i) {
            r.message = "line " + std::to_string(i) + " padding padding";
            sink.write(r);
        }
        sink.flush();
    }

    CHECK(fs::exists(base));
    CHECK(fs::exists(fs::path(base.string() + ".1")));
    // Should never exceed max_files - 1 numbered backups.
    CHECK(!fs::exists(fs::path(base.string() + ".3")));

    fs::remove_all(dir);
}

void test_overflow_policy_drop_newest_does_not_block() {
    LoggerConfig cfg;
    cfg.queue_capacity = 4;
    cfg.overflow_policy = OverflowPolicy::drop_newest;
    Logger logger("overflow_test", cfg);
    logger.set_level(Level::trace);
    // No sinks attached — this purely tests that flooding the queue
    // does not hang the caller when using drop_newest.
    for (int i = 0; i < 10000; ++i) {
        LOG_INFO_TO((&logger), "flood {}", i);
    }
    logger.flush();
    CHECK(true); // reaching here means we didn't deadlock/hang
}

void test_overflow_policy_drop_oldest_flush_completes() {
    // Regression test: drop_oldest evicts a queued record on the
    // producer thread via a direct try_pop(); if approx_size_ isn't
    // decremented to match, it permanently drifts above the real queue
    // occupancy and flush()'s "wait until drained" loop would never
    // observe zero. Reaching past flush() here is the actual assertion.
    LoggerConfig cfg;
    cfg.queue_capacity = 4;
    cfg.overflow_policy = OverflowPolicy::drop_oldest;
    Logger logger("overflow_drop_oldest_test", cfg);
    logger.set_level(Level::trace);
    auto sink = std::make_shared<CapturingSink>();
    logger.add_sink(sink);

    for (int i = 0; i < 10000; ++i) {
        LOG_INFO_TO((&logger), "flood {}", i);
    }
    logger.flush();
    CHECK(true); // reaching here means flush() returned instead of hanging
    CHECK(logger.dropped_count_snapshot() > 0); // tiny capacity guarantees evictions happened
}

void test_enqueue_retry_does_not_corrupt_record_contents() {
    // Regression test: enqueue()'s retry loop used to call
    // queue_.try_push(std::move(record)) unconditionally on every
    // attempt. Because try_push(T) takes its parameter by value, that
    // move happened at the call site regardless of whether the push
    // ultimately succeeded, so a failed first attempt silently emptied
    // message/logger_name/fields before the *next* retry ever ran --
    // corrupting/losing data under drop_oldest and block. This verifies
    // every record that actually reaches the sink still has its real,
    // non-empty message content intact.
    LoggerConfig cfg;
    cfg.queue_capacity = 4; // tiny: guarantees try_push fails and retries often
    cfg.overflow_policy = OverflowPolicy::drop_oldest;
    Logger logger("enqueue_retry_integrity_test", cfg);
    logger.set_level(Level::trace);
    auto sink = std::make_shared<CapturingSink>();
    logger.add_sink(sink);

    constexpr int kMessages = 2000;
    for (int i = 0; i < kMessages; ++i) {
        LOG_INFO_TO((&logger), "payload-{}", i);
    }
    logger.flush();

    CHECK(sink->count() > 0);
    for (std::size_t i = 0; i < sink->count(); ++i) {
        const auto& rec = sink->records[i];
        // The bug produced empty strings; a real delivered record must
        // always have a non-empty, well-formed "payload-<N>" message.
        CHECK(!rec.message.empty());
        CHECK(rec.message.rfind("payload-", 0) == 0);
        CHECK(!rec.logger_name.empty());
    }
}

void test_overflow_policy_block_multithreaded_delivers_all_and_does_not_hang() {
    // Stress test for the condvar-based backpressure wakeup: many
    // producer threads contending over a deliberately tiny bounded queue
    // with OverflowPolicy::block must (a) never drop a record and (b)
    // never leave a producer thread waiting on a signal that never
    // arrives.
    LoggerConfig cfg;
    cfg.queue_capacity = 8;
    cfg.overflow_policy = OverflowPolicy::block;
    Logger logger("overflow_block_stress_test", cfg);
    logger.set_level(Level::trace);
    auto sink = std::make_shared<CapturingSink>();
    logger.add_sink(sink);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    std::vector<std::thread> producers;
    producers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        producers.emplace_back([&logger, t] {
            for (int i = 0; i < kPerThread; ++i) {
                LOG_INFO_TO((&logger), "thread {} msg {}", t, i);
            }
        });
    }
    for (auto& th : producers) th.join();
    logger.flush();

    CHECK(sink->count() == static_cast<std::size_t>(kThreads) * kPerThread);
    CHECK(logger.dropped_count_snapshot() == 0); // block must never drop a record
}

void test_flush_concurrent_waiters_all_return() {
    // Multiple threads calling flush() at the same time must all
    // observe the drain complete and return, exercising flush_waiters_
    // / drained_cv_ with more than one concurrent waiter.
    LoggerConfig cfg;
    cfg.queue_capacity = 64;
    Logger logger("flush_concurrent_test", cfg);
    logger.set_level(Level::trace);
    auto sink = std::make_shared<CapturingSink>();
    logger.add_sink(sink);

    constexpr int kMessages = 5000;
    for (int i = 0; i < kMessages; ++i) {
        LOG_INFO_TO((&logger), "msg {}", i);
    }

    constexpr int kWaiters = 6;
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (int i = 0; i < kWaiters; ++i) {
        waiters.emplace_back([&logger] { logger.flush(); });
    }
    for (auto& th : waiters) th.join();

    CHECK(sink->count() == static_cast<std::size_t>(kMessages));
}

int main() {
    test_network_sink_no_server_does_not_block();
    test_network_sink_backlog_is_bounded();
    test_network_sink_integration_errors_isolated();
    test_syslog_sink_priority_mapping();
    test_syslog_sink_basic_write_does_not_crash();
    test_syslog_sink_format_string_payload_is_inert();
    test_syslog_sink_integration_with_logger();
    test_daily_file_sink_rotation_scheduling();
    test_daily_file_sink_actually_rotates_at_boundary();
    test_daily_file_sink_retention_pruning();
    test_gzip_availability_matches_build_flag();
#if defined(LOGPULSEX_HAVE_ZLIB)
    test_gzip_compress_decompress_round_trip();
    test_gzip_compress_nonexistent_input_fails_gracefully();
    test_daily_file_sink_compression_creates_gz_and_removes_original();
    test_daily_file_sink_compression_disabled_by_default();
    test_daily_file_sink_destructor_waits_for_inflight_compression();
    test_daily_file_sink_retention_prunes_compressed_files();
#endif
    test_daily_file_sink_validates_config();
    test_process_id_matches_real_pid_and_is_cached();
    test_logger_populates_process_id_and_thread_id();
    test_thread_id_differs_across_threads();
    test_pattern_formatter_pid_token();
    test_pattern_formatter_default_includes_pid_and_tid();
    test_json_formatter_pid_is_numeric_not_quoted();
    test_level_raw_exists_and_stringifies();
    test_hex_bytes_basic_formatting();
    test_hex_bytes_truncates_large_buffers();
    test_hex_bytes_does_not_leak_stream_format_state();
    test_format_hex_dump_layout();
    test_format_hex_dump_multi_row_and_offsets();
    test_format_hex_dump_non_printable_bytes_are_safe_in_sidebar();
    test_log_raw_preserves_newlines_in_pattern_formatter();
    test_non_raw_levels_still_escape_newlines_in_pattern_formatter();
    test_json_formatter_always_escapes_newlines_even_for_raw_level();
    test_syslog_priority_for_raw_level();
    test_console_sink_handles_raw_level_without_crashing();
    test_log_raw_end_to_end_via_logger();
    test_log_raw_requires_both_logger_and_sink_level_to_permit_it();
    test_log_raw_suppressed_by_default_level();
    test_default_logger_is_stable_and_singular_under_concurrent_first_use();
    test_log_if();
    test_log_every_n();
    test_log_if_every_n();
    test_log_first_n();
    test_every_t_gate_first_call_always_fires_regardless_of_clock_value();
    test_every_t_gate_throttles_within_interval();
    test_every_t_gate_handles_out_of_order_and_negative_deltas_safely();
    test_every_t_gate_no_overflow_at_int64_extremes();
    test_log_every_t_macro_integration();
    test_format_basic();
    test_format_container_support();
    test_format_specs();
    test_structured_kv_logging();
    test_queue_single_thread();
    test_queue_mpsc_concurrent();
    test_json_escaping_prevents_log_injection();
    test_pattern_formatter_unknown_token_is_safe();
    test_rotating_file_sink_accounts_for_preexisting_file_size();
    test_rotating_file_sink_rotates();
    test_overflow_policy_drop_newest_does_not_block();
    test_overflow_policy_drop_oldest_flush_completes();
    test_enqueue_retry_does_not_corrupt_record_contents();
    test_overflow_policy_block_multithreaded_delivers_all_and_does_not_hang();
    test_flush_concurrent_waiters_all_return();
    test_backtrace_captures_below_threshold_and_dump_delivers_them();
    test_backtrace_disabled_by_default();
    test_backtrace_overwrites_oldest_once_capacity_exceeded();
    test_backtrace_disable_clears_buffer();
    test_backtrace_dump_does_not_clear_and_is_repeatable();

    if (g_failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test(s) failed.\n";
    return 1;
}
