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

#include "sink_io_bench.hpp"

#include <filesystem>
#include <memory>
#include <string>

#include "logpulsex/formatter.hpp"
#include "logpulsex/logger.hpp"
#include "logpulsex/rotating_file_sink.hpp"

namespace bench {

namespace {

// Scratch files are written to (and removed from) the current working
// directory the bench binary is run from; run it from a writable dir.
std::filesystem::path scratch_path(const char* name) {
    return std::filesystem::path("bench_scratch_" + std::string(name) + ".log");
}

// Matches spdlog's "C-string (400 bytes)" benchmark payload.
constexpr std::size_t kLargeMessageBytes = 400;

double run_rotating_file(std::shared_ptr<logpulsex::IFormatter> formatter,
                          std::uint64_t msg_count, const char* scratch_name,
                          const std::string* fixed_message = nullptr) {
    auto path = scratch_path(scratch_name);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    logpulsex::LoggerConfig config;
    config.queue_capacity = 65536;
    config.overflow_policy = logpulsex::OverflowPolicy::block;
    logpulsex::Logger logger("bench-sink-io", config);

    auto sink = std::make_shared<logpulsex::RotatingFileSink>(
        path, /*max_bytes=*/std::size_t{1} << 30, /*max_files=*/1);
    sink->set_formatter(std::move(formatter));
    logger.add_sink(sink);

    auto start = Clock::now();
    if (fixed_message) {
        for (std::uint64_t i = 0; i < msg_count; ++i) {
            logger.log(logpulsex::Level::info, __FILE__, __LINE__, __func__, "{}", *fixed_message);
        }
    } else {
        for (std::uint64_t i = 0; i < msg_count; ++i) {
            logger.log(logpulsex::Level::info, __FILE__, __LINE__, __func__,
                        "Order {} for {} amount {:.2f}", i, "bob", 59.99);
        }
    }
    logger.flush();
    auto elapsed = Clock::now() - start;

    logger.shutdown();
    std::filesystem::remove(path, ec);

    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

} // namespace

void run_sink_io_benchmarks(Reporter& reporter, std::uint64_t msg_count) {
    {
        double total_ns = run_rotating_file(
            std::make_shared<logpulsex::PatternFormatter>(), msg_count, "pattern");
        Result r;
        r.name = "RotatingFileSink + PatternFormatter";
        r.iterations = msg_count;
        r.total_ns = total_ns;
        reporter.add("sink_io", r);
    }
    {
        double total_ns = run_rotating_file(
            std::make_shared<logpulsex::JsonFormatter>(), msg_count, "json");
        Result r;
        r.name = "RotatingFileSink + JsonFormatter";
        r.iterations = msg_count;
        r.total_ns = total_ns;
        reporter.add("sink_io", r);
    }
    {
        std::string large_message(kLargeMessageBytes, 'x');
        double total_ns = run_rotating_file(
            std::make_shared<logpulsex::PatternFormatter>(), msg_count, "pattern_400b",
            &large_message);
        Result r;
        r.name = "RotatingFileSink + PatternFormatter (400B msg)";
        r.iterations = msg_count;
        r.total_ns = total_ns;
        reporter.add("sink_io", r);
    }
}

} // namespace bench
