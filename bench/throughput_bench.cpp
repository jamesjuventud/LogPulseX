#include "throughput_bench.hpp"

#include <memory>

#include "bench_null_sink.hpp"
#include "logpulsex/logger.hpp"

namespace bench {

void run_throughput_benchmarks(Reporter& reporter, std::chrono::seconds duration,
                                std::uint64_t latency_iterations) {
    // Dedicated logger (not default_logger()) so this bench doesn't
    // fight over the process-wide singleton, and log() is called
    // directly since the LOG_* macros always target default_logger().
    logpulsex::LoggerConfig config;
    config.queue_capacity = 65536;
    config.overflow_policy = logpulsex::OverflowPolicy::block;
    logpulsex::Logger logger("bench-throughput", config);
    logger.add_sink(std::make_shared<NullSink>());

    // Per-call latency of the producer-side log() call (enqueue only --
    // the worker thread does the (nonexistent, here) I/O asynchronously).
    constexpr std::uint64_t latency_warmup = 5000;
    reporter.add("throughput", run_latency("log() call latency (NullSink)", latency_iterations, latency_warmup, [&] {
        logger.log(logpulsex::Level::info, __FILE__, __LINE__, __func__,
                    "Order {} for {} amount {:.2f}", 123, "bob", 59.99);
    }));

    // Matches spdlog's "C-string (400 bytes)" benchmark payload, for a
    // like-for-like comparison against its null_st result.
    std::string large_message(400, 'x');
    reporter.add("throughput", run_latency("log() call latency (NullSink, 400B msg)", latency_iterations,
                                            latency_warmup, [&] {
        logger.log(logpulsex::Level::info, __FILE__, __LINE__, __func__, "{}", large_message);
    }));

    // Sustained max producer rate: push as fast as possible for
    // `duration`, then measure how long the worker takes to fully drain
    // the queue -- this separates producer throughput from consumer
    // (worker + sink) throughput.
    std::uint64_t pushed = 0;
    auto push_start = Clock::now();
    auto push_deadline = push_start + duration;
    while (Clock::now() < push_deadline) {
        logger.log(logpulsex::Level::info, __FILE__, __LINE__, __func__,
                    "Order {} for {} amount {:.2f}", 123, "bob", 59.99);
        ++pushed;
    }
    auto push_elapsed = Clock::now() - push_start;

    auto drain_start = Clock::now();
    logger.flush();
    auto drain_elapsed = Clock::now() - drain_start;

    bench::Result producer;
    producer.name = "sustained producer rate (NullSink)";
    producer.iterations = pushed;
    producer.total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(push_elapsed).count());
    reporter.add("throughput", producer);

    bench::Result drain;
    drain.name = "queue drain after producer stops";
    drain.iterations = 1;
    drain.total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(drain_elapsed).count());
    reporter.add("throughput", drain);

    logger.shutdown();
}

} // namespace bench
