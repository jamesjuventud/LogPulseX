// Copyright 2026-Present James Bryan B. Juventud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0

// Description: A header-only C++20 logging library:
// async, lock-free on the hot path, with console,
// size-based rotating file, daily rotating file,
// syslog, and TCP network sinks, structured (JSON)
// logging, crash-safe flushing, hex dumping,
// and file compression.

#pragma once

// Single include for consumers of the library.
//
//   #include "logpulsex/logpulsex.hpp"
//
//   int main() {
//       auto logger = logpulsex::default_logger();
//       logger->add_sink(std::make_shared<logpulsex::ConsoleSink>());
//       logpulsex::install_crash_handlers();
//
//       LOG_INFO("Server starting on port {}", 8080);
//       LOG_ERROR("Failed to connect to {}: {}", host, err);
//   }

#include "logpulsex/container_format.hpp"
#include "logpulsex/daily_file_sink.hpp"
#include "logpulsex/formatter.hpp"
#include "logpulsex/gzip_compress.hpp"
#include "logpulsex/hex.hpp"
#include "logpulsex/level.hpp"
#include "logpulsex/log_record.hpp"
#include "logpulsex/logger.hpp"
#include "logpulsex/network_sink.hpp"
#include "logpulsex/registry.hpp"
#include "logpulsex/rotating_file_sink.hpp"
#include "logpulsex/sink.hpp"
#include "logpulsex/syslog_sink.hpp"
#include "logpulsex/throttle.hpp"

// ---------------------------------------------------------------------
// Macros
//
// Each macro expands to a guarded call: the LOGPULSEX_MIN_LEVEL check is a
// compile-time constant expression, so when a level is compiled out
// (e.g. -DLOGPULSEX_MIN_LEVEL=2 strips trace/debug from a release build)
// the compiler eliminates the call entirely, including evaluation of the
// arguments — a disabled LOG_DEBUG(expensive_to_compute()) costs nothing.
//
// The runtime should_log() check inside Logger::log() additionally lets
// you change verbosity at runtime (e.g. via a config reload or signal)
// without recompiling, for levels that were NOT compiled out.
// ---------------------------------------------------------------------

#define LOGPULSEX_LOG_IMPL(logger_ptr, lvl, ...)                                   \
    do {                                                                         \
        if constexpr (::logpulsex::level_enabled_at_compile_time(lvl)) {           \
            (logger_ptr)->log((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__); \
        }                                                                        \
    } while (0)

#define LOG_RAW_TO(logger_ptr, ...)   LOGPULSEX_LOG_IMPL(logger_ptr, ::logpulsex::Level::raw,   __VA_ARGS__)
#define LOG_TRACE_TO(logger_ptr, ...) LOGPULSEX_LOG_IMPL(logger_ptr, ::logpulsex::Level::trace, __VA_ARGS__)
#define LOG_DEBUG_TO(logger_ptr, ...) LOGPULSEX_LOG_IMPL(logger_ptr, ::logpulsex::Level::debug, __VA_ARGS__)
#define LOG_INFO_TO(logger_ptr, ...)  LOGPULSEX_LOG_IMPL(logger_ptr, ::logpulsex::Level::info,  __VA_ARGS__)
#define LOG_WARN_TO(logger_ptr, ...)  LOGPULSEX_LOG_IMPL(logger_ptr, ::logpulsex::Level::warn,  __VA_ARGS__)
#define LOG_ERROR_TO(logger_ptr, ...) LOGPULSEX_LOG_IMPL(logger_ptr, ::logpulsex::Level::error, __VA_ARGS__)
#define LOG_FATAL_TO(logger_ptr, ...) LOGPULSEX_LOG_IMPL(logger_ptr, ::logpulsex::Level::fatal, __VA_ARGS__)

// Convenience macros that log to the process-wide default logger.
// LOG_RAW is intended primarily for pre-formatted, deliberately
// multi-line content -- see format_hex_dump() in hex.hpp -- and is the
// only level whose plain-text rendering does not escape embedded '\n'
// bytes. See the compatibility/trust-boundary note on Level::raw in
// level.hpp before using it with anything other than this library's own
// hex-dump output.
#define LOG_RAW(...)   LOG_RAW_TO(::logpulsex::default_logger(),   __VA_ARGS__)
#define LOG_TRACE(...) LOG_TRACE_TO(::logpulsex::default_logger(), __VA_ARGS__)
#define LOG_DEBUG(...) LOG_DEBUG_TO(::logpulsex::default_logger(), __VA_ARGS__)
#define LOG_INFO(...)  LOG_INFO_TO(::logpulsex::default_logger(),  __VA_ARGS__)
#define LOG_WARN(...)  LOG_WARN_TO(::logpulsex::default_logger(),  __VA_ARGS__)
#define LOG_ERROR(...) LOG_ERROR_TO(::logpulsex::default_logger(), __VA_ARGS__)
#define LOG_FATAL(...) LOG_FATAL_TO(::logpulsex::default_logger(), __VA_ARGS__)

// ---------------------------------------------------------------------
// Structured (key/value) logging macros.
//
//   LOG_INFO_KV("Order placed", logpulsex::field("order_id", id),
//                                logpulsex::field("amount", 59.99));
//
// The message is plain text (no {} interpolation here — mixing free-form
// interpolation and structured fields in one call invites confusion
// about which mechanism owns which piece of data). Fields are carried on
// LogRecord::fields and rendered by JsonFormatter as extra top-level keys,
// or by PatternFormatter wherever the pattern includes a {fields} token.
// ---------------------------------------------------------------------

#define LOGPULSEX_LOG_KV_IMPL(logger_ptr, lvl, message, ...)                         \
    do {                                                                           \
        if constexpr (::logpulsex::level_enabled_at_compile_time(lvl)) {             \
            (logger_ptr)->log_kv((lvl), __FILE__, __LINE__, __func__, (message),   \
                                  {__VA_ARGS__});                                  \
        }                                                                          \
    } while (0)

#define LOG_TRACE_KV_TO(logger_ptr, message, ...) LOGPULSEX_LOG_KV_IMPL(logger_ptr, ::logpulsex::Level::trace, message, __VA_ARGS__)
#define LOG_DEBUG_KV_TO(logger_ptr, message, ...) LOGPULSEX_LOG_KV_IMPL(logger_ptr, ::logpulsex::Level::debug, message, __VA_ARGS__)
#define LOG_INFO_KV_TO(logger_ptr, message, ...)  LOGPULSEX_LOG_KV_IMPL(logger_ptr, ::logpulsex::Level::info,  message, __VA_ARGS__)
#define LOG_WARN_KV_TO(logger_ptr, message, ...)  LOGPULSEX_LOG_KV_IMPL(logger_ptr, ::logpulsex::Level::warn,  message, __VA_ARGS__)
#define LOG_ERROR_KV_TO(logger_ptr, message, ...) LOGPULSEX_LOG_KV_IMPL(logger_ptr, ::logpulsex::Level::error, message, __VA_ARGS__)
#define LOG_FATAL_KV_TO(logger_ptr, message, ...) LOGPULSEX_LOG_KV_IMPL(logger_ptr, ::logpulsex::Level::fatal, message, __VA_ARGS__)

#define LOG_TRACE_KV(message, ...) LOG_TRACE_KV_TO(::logpulsex::default_logger(), message, __VA_ARGS__)
#define LOG_DEBUG_KV(message, ...) LOG_DEBUG_KV_TO(::logpulsex::default_logger(), message, __VA_ARGS__)
#define LOG_INFO_KV(message, ...)  LOG_INFO_KV_TO(::logpulsex::default_logger(),  message, __VA_ARGS__)
#define LOG_WARN_KV(message, ...)  LOG_WARN_KV_TO(::logpulsex::default_logger(),  message, __VA_ARGS__)
#define LOG_ERROR_KV(message, ...) LOG_ERROR_KV_TO(::logpulsex::default_logger(), message, __VA_ARGS__)
#define LOG_FATAL_KV(message, ...) LOG_FATAL_KV_TO(::logpulsex::default_logger(), message, __VA_ARGS__)

// ---------------------------------------------------------------------
// Conditional and rate-limited logging macros (glog-style).
//
// The level is a runtime argument here rather than baked into the macro
// name (e.g. `LOG_IF(logpulsex::Level::warn, cond, ...)`), which keeps this
// family to a handful of macros instead of one per level x per variant.
// It still compiles away entirely when the level is stripped via
// LOGPULSEX_MIN_LEVEL, same as the plain LOG_* macros, as long as you pass
// a literal/constexpr level (the normal case).
//
// Each of LOG_EVERY_N / LOG_IF_EVERY_N / LOG_FIRST_N / LOG_EVERY_T keeps
// a function-local static counter tied to that specific call site in
// your source code — so these macros must be used inside a function
// body (not at namespace scope), same restriction as glog's equivalents.
// The counters are atomic, so the macros are safe to call from multiple
// threads concurrently; under heavy contention the exact occurrence that
// fires may shift by one, which is an accepted tradeoff for avoiding a
// lock on every call.
//
//   LOG_IF(Level::warn, queue_depth > 1000, "Queue backed up: {}", queue_depth);
//   LOG_EVERY_N(Level::info, 100, "Processed {} so far", count);      // 1st, 101st, 201st...
//   LOG_IF_EVERY_N(Level::warn, retry, 10, "Retry #{}", n);           // every 10th retry
//   LOG_FIRST_N(Level::warn, 5, "Deprecated API called: {}", name);   // only the first 5 times
//   LOG_EVERY_T(Level::info, 1.0, "Heartbeat, tick={}", tick);        // at most once per second
// ---------------------------------------------------------------------

#define LOGPULSEX_LOG_IF_IMPL(logger_ptr, lvl, cond, ...)                     \
    do {                                                                    \
        if constexpr (::logpulsex::level_enabled_at_compile_time(lvl)) {      \
            if (cond) {                                                     \
                (logger_ptr)->log((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__); \
            }                                                               \
        }                                                                   \
    } while (0)

#define LOG_IF_TO(logger_ptr, lvl, cond, ...) LOGPULSEX_LOG_IF_IMPL(logger_ptr, lvl, cond, __VA_ARGS__)
#define LOG_IF(lvl, cond, ...) LOG_IF_TO(::logpulsex::default_logger(), lvl, cond, __VA_ARGS__)

#define LOGPULSEX_LOG_EVERY_N_IMPL(logger_ptr, lvl, n, ...)                              \
    do {                                                                               \
        if constexpr (::logpulsex::level_enabled_at_compile_time(lvl)) {                 \
            static std::atomic<std::uint64_t> logpulsex_every_n_counter_{0};             \
            std::uint64_t logpulsex_occurrence_ =                                        \
                logpulsex_every_n_counter_.fetch_add(1, std::memory_order_relaxed);      \
            if (logpulsex_occurrence_ % static_cast<std::uint64_t>(n) == 0) {            \
                (logger_ptr)->log((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__);   \
            }                                                                          \
        }                                                                              \
    } while (0)

#define LOG_EVERY_N_TO(logger_ptr, lvl, n, ...) LOGPULSEX_LOG_EVERY_N_IMPL(logger_ptr, lvl, n, __VA_ARGS__)
#define LOG_EVERY_N(lvl, n, ...) LOG_EVERY_N_TO(::logpulsex::default_logger(), lvl, n, __VA_ARGS__)

#define LOGPULSEX_LOG_IF_EVERY_N_IMPL(logger_ptr, lvl, cond, n, ...)                         \
    do {                                                                                   \
        if constexpr (::logpulsex::level_enabled_at_compile_time(lvl)) {                     \
            if (cond) {                                                                    \
                static std::atomic<std::uint64_t> logpulsex_if_every_n_counter_{0};          \
                std::uint64_t logpulsex_occurrence_ =                                        \
                    logpulsex_if_every_n_counter_.fetch_add(1, std::memory_order_relaxed);   \
                if (logpulsex_occurrence_ % static_cast<std::uint64_t>(n) == 0) {            \
                    (logger_ptr)->log((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__);   \
                }                                                                           \
            }                                                                              \
        }                                                                                  \
    } while (0)

#define LOG_IF_EVERY_N_TO(logger_ptr, lvl, cond, n, ...) LOGPULSEX_LOG_IF_EVERY_N_IMPL(logger_ptr, lvl, cond, n, __VA_ARGS__)
#define LOG_IF_EVERY_N(lvl, cond, n, ...) LOG_IF_EVERY_N_TO(::logpulsex::default_logger(), lvl, cond, n, __VA_ARGS__)

#define LOGPULSEX_LOG_FIRST_N_IMPL(logger_ptr, lvl, n, ...)                              \
    do {                                                                               \
        if constexpr (::logpulsex::level_enabled_at_compile_time(lvl)) {                 \
            static std::atomic<std::uint64_t> logpulsex_first_n_counter_{0};             \
            std::uint64_t logpulsex_occurrence_ =                                        \
                logpulsex_first_n_counter_.fetch_add(1, std::memory_order_relaxed);      \
            if (logpulsex_occurrence_ < static_cast<std::uint64_t>(n)) {                 \
                (logger_ptr)->log((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__);   \
            }                                                                          \
        }                                                                              \
    } while (0)

#define LOG_FIRST_N_TO(logger_ptr, lvl, n, ...) LOGPULSEX_LOG_FIRST_N_IMPL(logger_ptr, lvl, n, __VA_ARGS__)
#define LOG_FIRST_N(lvl, n, ...) LOG_FIRST_N_TO(::logpulsex::default_logger(), lvl, n, __VA_ARGS__)

// Time-based throttle: logs at most once per `seconds` (a double) from
// this call site. Uses a compare-exchange so that under concurrent
// access from multiple threads, exactly one thread wins the right to log
// for a given interval — others simply skip, rather than double-logging
// or blocking.
#define LOGPULSEX_LOG_EVERY_T_IMPL(logger_ptr, lvl, seconds, ...)                                    \
    do {                                                                                           \
        if constexpr (::logpulsex::level_enabled_at_compile_time(lvl)) {                             \
            static ::logpulsex::detail::EveryTGate logpulsex_every_t_gate_;                            \
            auto logpulsex_now_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(          \
                std::chrono::steady_clock::now().time_since_epoch()).count();                      \
            std::int64_t logpulsex_interval_ms_ = static_cast<std::int64_t>((seconds) * 1000.0);     \
            if (logpulsex_every_t_gate_.should_fire(logpulsex_now_ms_, logpulsex_interval_ms_)) {        \
                (logger_ptr)->log((lvl), __FILE__, __LINE__, __func__, __VA_ARGS__);                \
            }                                                                                       \
        }                                                                                           \
    } while (0)

#define LOG_EVERY_T_TO(logger_ptr, lvl, seconds, ...) LOGPULSEX_LOG_EVERY_T_IMPL(logger_ptr, lvl, seconds, __VA_ARGS__)
#define LOG_EVERY_T(lvl, seconds, ...) LOG_EVERY_T_TO(::logpulsex::default_logger(), lvl, seconds, __VA_ARGS__)
