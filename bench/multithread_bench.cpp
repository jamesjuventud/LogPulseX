#include "multithread_bench.hpp"

#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>

#include "bench_null_sink.hpp"
#include "logpulsex/logger.hpp"

namespace bench {

namespace {

double run_one(unsigned thread_count, std::uint64_t msgs_per_thread) {
    logpulsex::LoggerConfig config;
    config.queue_capacity = 65536;
    config.overflow_policy = logpulsex::OverflowPolicy::block;
    logpulsex::Logger logger("bench-multithread", config);
    logger.add_sink(std::make_shared<NullSink>());

    std::vector<std::thread> producers;
    producers.reserve(thread_count);

    auto start = Clock::now();
    for (unsigned t = 0; t < thread_count; ++t) {
        producers.emplace_back([&logger, msgs_per_thread] {
            for (std::uint64_t i = 0; i < msgs_per_thread; ++i) {
                logger.log(logpulsex::Level::info, __FILE__, __LINE__, __func__,
                            "Order {} for {} amount {:.2f}", i, "bob", 59.99);
            }
        });
    }
    for (auto& th : producers) th.join();
    logger.flush(); // wait for the worker to fully drain before stopping the clock
    auto elapsed = Clock::now() - start;

    logger.shutdown();
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

} // namespace

void run_multithread_benchmarks(Reporter& reporter, const std::vector<unsigned>& thread_counts,
                                 std::uint64_t msgs_per_thread) {
    // 1-thread ops/sec is the baseline every other row's scaling
    // efficiency is measured against, regardless of whether 1 also
    // appears in thread_counts.
    double baseline_ops_per_sec =
        static_cast<double>(msgs_per_thread) * 1e9 / run_one(1, msgs_per_thread);

    for (unsigned threads : thread_counts) {
        if (threads == 0) continue;
        double total_ns = run_one(threads, msgs_per_thread);
        std::uint64_t total_msgs = static_cast<std::uint64_t>(threads) * msgs_per_thread;
        double ops_per_sec = static_cast<double>(total_msgs) * 1e9 / total_ns;
        double ideal_ops_per_sec = baseline_ops_per_sec * static_cast<double>(threads);
        double efficiency_pct = (ops_per_sec / ideal_ops_per_sec) * 100.0;

        std::ostringstream note;
        note << std::fixed << std::setprecision(0) << efficiency_pct << "% scaling vs. 1 thread";

        Result r;
        r.name = std::to_string(threads) + " producer thread(s)";
        r.iterations = total_msgs;
        r.total_ns = total_ns;
        reporter.add("multithread", r, note.str());
    }
}

} // namespace bench
