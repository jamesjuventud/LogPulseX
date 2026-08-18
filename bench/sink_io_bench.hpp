#pragma once

#include <cstdint>

#include "bench_harness.hpp"

namespace bench {

void run_sink_io_benchmarks(Reporter& reporter, std::uint64_t msg_count);

} // namespace bench
