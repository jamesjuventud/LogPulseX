#pragma once

// Bench-only no-op sink: isolates producer/queue overhead from real
// sink I/O cost. Deliberately not part of the public include/ API.

#include "logpulsex/sink.hpp"

namespace bench {

class NullSink final : public logpulsex::ISink {
public:
    void write(const logpulsex::LogRecord&) override {}
    void flush() override {}
};

} // namespace bench
