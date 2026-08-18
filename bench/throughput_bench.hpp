#pragma once

#include <chrono>
#include <cstdint>

#include "bench_harness.hpp"

namespace bench {

void run_throughput_benchmarks(Reporter& reporter, std::chrono::seconds duration,
                                std::uint64_t latency_iterations);

} // namespace bench
