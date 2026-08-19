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

#include "micro_bench.hpp"

#include <map>
#include <vector>

#include "logpulsex/format.hpp"
#include "logpulsex/formatter.hpp"
#include "logpulsex/spsc_mpsc_queue.hpp"

namespace bench {

namespace {

logpulsex::LogRecord make_sample_record() {
    logpulsex::LogRecord record;
    record.level = logpulsex::Level::info;
    record.timestamp = std::chrono::system_clock::now();
    record.thread_id = std::this_thread::get_id();
    record.native_thread_id = 12345;
    record.process_id = 6789;
    record.logger_name = "bench";
    record.message = "Order placed for user=alice amount=59.99 items=3";
    record.file = "bench_micro.cpp";
    record.line = 42;
    record.function = "run_micro_benchmarks";
    return record;
}

} // namespace

void run_micro_benchmarks(Reporter& reporter, std::uint64_t iterations) {
    constexpr std::uint64_t warmup = 1000;

    reporter.add("micro", run_throughput("format: no args", iterations, warmup, [] {
        auto s = logpulsex::detail::format("Server starting on port 8080");
        do_not_optimize(s);
    }));

    reporter.add("micro", run_throughput("format: 1 int arg", iterations, warmup, [] {
        auto s = logpulsex::detail::format("Value: {}", 42);
        do_not_optimize(s);
    }));

    reporter.add("micro", run_throughput("format: 1 string arg", iterations, warmup, [] {
        auto s = logpulsex::detail::format("User: {}", "alice");
        do_not_optimize(s);
    }));

    reporter.add("micro", run_throughput("format: mixed 3 args", iterations, warmup, [] {
        auto s = logpulsex::detail::format("Order {} for {} amount {:.2f}", 123, "bob", 59.99);
        do_not_optimize(s);
    }));

    reporter.add("micro", run_throughput("format: vector<int> arg", iterations, warmup, [] {
        std::vector<int> scores{95, 88, 76};
        auto s = logpulsex::detail::format("scores={}", scores);
        do_not_optimize(s);
    }));

    // Single-thread MPSC queue round trip (push immediately followed by
    // pop keeps the queue from ever filling, isolating steady-state cost).
    {
        logpulsex::BoundedMpscQueue<logpulsex::LogRecord> queue(1024);
        logpulsex::LogRecord record = make_sample_record();
        reporter.add("micro", run_throughput("queue: push+pop round trip", iterations, warmup, [&] {
            queue.try_push(record);
            auto popped = queue.try_pop();
            do_not_optimize(popped);
        }));
    }

    {
        logpulsex::PatternFormatter formatter;
        logpulsex::LogRecord record = make_sample_record();
        reporter.add("micro", run_throughput("PatternFormatter::format", iterations, warmup, [&] {
            auto s = formatter.format(record);
            do_not_optimize(s);
        }));
    }

    {
        logpulsex::JsonFormatter formatter;
        logpulsex::LogRecord record = make_sample_record();
        reporter.add("micro", run_throughput("JsonFormatter::format", iterations, warmup, [&] {
            auto s = formatter.format(record);
            do_not_optimize(s);
        }));
    }
}

} // namespace bench
