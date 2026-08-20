// Copyright 2026-Present James Bryan B. Juventud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Description: A header-only C++20 logging library:
// async, lock-free on the hot path, with console,
// size-based rotating file, daily rotating file,
// syslog, and TCP network sinks, structured (JSON)
// logging, crash-safe flushing, hex dumping,
// and file compression.

// Regression test for the LOGPULSEX_MIN_LEVEL vs. Level::raw interaction
// bug: this repo's own CMakeLists.txt sets LOGPULSEX_MIN_LEVEL=1 for
// Release builds to strip trace/debug chatter, but since Level::raw is
// numbered below trace (0 vs 1) for ordering purposes, that threshold
// used to silently compile out LOG_RAW as well -- breaking hex-dump
// logging in every Release build. Must be compiled with a nonzero
// LOGPULSEX_MIN_LEVEL (e.g. -DLOGPULSEX_MIN_LEVEL=2) to actually exercise
// the fix; at the default of 0 every level passes trivially either way.

#ifndef LOGPULSEX_MIN_LEVEL
#error "This test must be compiled with a nonzero -DLOGPULSEX_MIN_LEVEL to exercise the fix (see CMakeLists.txt's logpulsex_min_level_test target)."
#endif

#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

#include "logpulsex/logpulsex.hpp"

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

// Compile-time checks: fail the build itself (not just at runtime) if
// the exemption regresses, since this is fundamentally a compile-time
// property.
static_assert(level_enabled_at_compile_time(Level::raw),
              "Level::raw must never be stripped by LOGPULSEX_MIN_LEVEL");
static_assert(!level_enabled_at_compile_time(Level::trace) || LOGPULSEX_MIN_LEVEL <= static_cast<int>(Level::trace),
              "sanity: trace should still be gated normally by LOGPULSEX_MIN_LEVEL");

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

void test_log_raw_survives_nonzero_min_level() {
    Logger logger("min_level_raw_test");
    logger.set_level(Level::raw);
    auto capture = std::make_shared<CapturingSink>();
    capture->set_level(Level::raw);
    logger.add_sink(capture);

    LOG_RAW_TO((&logger), "{}", format_hex_dump("Hi", 2));
    logger.flush();

    // Under the pre-fix behavior, LOG_RAW_TO compiled to nothing at
    // LOGPULSEX_MIN_LEVEL >= 1, so count() would be 0 here.
    CHECK(capture->count() == 1);
    CHECK(capture->records[0].level == Level::raw);
}

void test_trace_is_still_stripped_when_min_level_requires_it() {
    // Confirms the raw exemption didn't accidentally widen to other
    // levels: trace must still be compiled out whenever
    // LOGPULSEX_MIN_LEVEL exceeds its numeric value.
    Logger logger("min_level_trace_test");
    logger.set_level(Level::trace);
    auto capture = std::make_shared<CapturingSink>();
    logger.add_sink(capture);

    LOG_TRACE_TO((&logger), "should be compiled out");
    logger.flush();

    if constexpr (LOGPULSEX_MIN_LEVEL > static_cast<int>(Level::trace)) {
        CHECK(capture->count() == 0);
    } else {
        CHECK(capture->count() == 1);
    }
}

int main() {
    test_log_raw_survives_nonzero_min_level();
    test_trace_is_still_stripped_when_min_level_requires_it();

    if (g_failures == 0) {
        std::cout << "All tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test(s) failed.\n";
    return 1;
}
