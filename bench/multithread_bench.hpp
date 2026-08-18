#pragma once

#include <cstdint>
#include <vector>

#include "bench_harness.hpp"

namespace bench {

void run_multithread_benchmarks(Reporter& reporter, const std::vector<unsigned>& thread_counts,
                                 std::uint64_t msgs_per_thread);

} // namespace bench
