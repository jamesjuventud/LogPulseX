#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <utility>

#include "logpulsex/level.hpp"

#if defined(__cpp_lib_source_location)
#include <source_location>
#define LOGPULSEX_HAS_SOURCE_LOCATION 1
#endif

// Process ID capture. A process's PID never changes during its lifetime,
// so this is computed once (cached in a function-local static -- the
// same thread-safe "magic statics" technique used elsewhere in this
// library, e.g. Registry::instance()/default_logger()) rather than
// making a fresh getpid()-family call on every single log record.
#if defined(_WIN32)
#include <process.h> // _getpid() -- a lightweight MSVC/MinGW CRT function;
                     // deliberately avoids pulling in <windows.h> just for this.
#else
#include <unistd.h> // getpid()
#endif

namespace logpulsex::detail {

inline std::uint64_t get_process_id() noexcept {
    static const std::uint64_t pid = [] {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(::_getpid());
#else
        return static_cast<std::uint64_t>(::getpid());
#endif
    }();
    return pid;
}

} // namespace logpulsex::detail

namespace logpulsex {

// A single structured key/value field attached to a record, e.g. for
// JSON output: {"user_id": "1234", "request_id": "abc"}.
// Values are pre-stringified at the call site to keep LogRecord copyable
// and avoid storing type-erased data on the hot path.
struct Field {
    std::string key;
    std::string value;
};

// Everything the async pipeline needs to format and dispatch one log line.
// Deliberately a flat, trivially-movable struct — cheap to push through the
// queue. String fields use std::string (SSO covers most short messages
// without heap allocation).
struct LogRecord {
    Level level = Level::info;
    std::chrono::system_clock::time_point timestamp{};
    std::thread::id thread_id{};
    std::uint64_t process_id = 0;
    std::string logger_name;
    std::string message;
    std::vector<Field> fields;

    // Source location — populated at the call site via macros.
    const char* file = "";
    int line = 0;
    const char* function = "";
};

} // namespace logpulsex
