#pragma once

#include <cstdint>

#include "bench_harness.hpp"

namespace bench {

void run_micro_benchmarks(Reporter& reporter, std::uint64_t iterations);

} // namespace bench
