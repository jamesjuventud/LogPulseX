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

#include <atomic>
#include <cstdint>
#include <limits>

namespace logpulsex::detail {

// Sentinel meaning "never fired yet." Deliberately a free constant, not a
// static member function of EveryTGate called from within EveryTGate's
// own default member initializer -- that pattern (`std::atomic<int64_t>
// last_fired_ms_{sentinel()};` where `sentinel()` is `EveryTGate::sentinel`)
// compiles fine on GCC and Clang, but MSVC rejects it with C3615
// ("constexpr function cannot result in a constant expression"): the
// class is still incomplete at the point its own NSDMI is parsed, and
// MSVC's constant-expression evaluator is stricter about calling even a
// self-contained static member function of that same, not-yet-complete
// class from inside it. Hoisting the constant out to true namespace
// scope, with zero dependency on EveryTGate's definition, sidesteps the
// ambiguity entirely and is portable across all three compilers.
// Windows.h note: if a consumer's project includes <windows.h> without
// NOMINMAX defined (common in real-world Windows codebases, and outside
// this library's control since it can happen in an unrelated header
// included earlier in the same translation unit), min and max become
// function-like macros that would otherwise silently break this call to
// std::numeric_limits<...>::min(). Wrapping the call in extra parens
// prevents the preprocessor from matching the macro-looking token
// sequence, which is the standard, well-known workaround for this exact
// situation -- more portable than requiring correct include order from
// every consumer.
inline constexpr std::int64_t kEveryTGateSentinel =
    (std::numeric_limits<std::int64_t>::min)() / 2;

// Backing logic for LOG_EVERY_T. Pulled out of the macro into its own
// class specifically so it can be unit tested with explicit, controlled
// "now" values instead of depending on the real wall/steady clock —
// see the bug this fixes below.
//
// Bug this fixes: an earlier version stored last-fired time in an
// std::atomic<int64_t> initialized to 0, and fired when
// `now_ms - last_fired_ms >= interval_ms`. That implicitly assumes the
// process has already been running longer than `interval_ms` by the time
// LOG_EVERY_T is first reached (since now_ms is steady_clock's count
// since an unspecified epoch, typically system/process uptime). On a
// freshly started process — or any process where a long-interval
// LOG_EVERY_T is reached within the first `interval_ms` of uptime — the
// very first call was silently suppressed instead of firing immediately,
// which is the expected semantics (mirroring glog's LOG_EVERY_T: the
// first occurrence always logs). This was caught by a regression run in
// a container with ~69s of uptime against a 60s test interval.
//
// Fix: use an explicit "has fired at least once" sentinel
// (std::numeric_limits<int64_t>::min() / 2, not the raw min — dividing
// by 2 leaves headroom so `now_ms - sentinel` can never overflow a
// signed 64-bit integer for any realistic now_ms) so the first call
// always satisfies the elapsed-time check regardless of process uptime.
class EveryTGate {
public:
    // now_ms/interval_ms: caller-supplied clock reading and threshold,
    // both in the same unit (milliseconds here, but the class doesn't
    // care as long as they're consistent) so tests can drive it with
    // arbitrary synthetic timelines instead of the real clock.
    bool should_fire(std::int64_t now_ms, std::int64_t interval_ms) {
        std::int64_t prev = last_fired_ms_.load(std::memory_order_relaxed);
        if (now_ms - prev < interval_ms) {
            return false;
        }
        // CAS races against other threads hitting the same call site
        // concurrently; only the winner fires, others correctly back off
        // as if they'd lost a normal timing race.
        return last_fired_ms_.compare_exchange_strong(
            prev, now_ms, std::memory_order_relaxed);
    }

private:
    std::atomic<std::int64_t> last_fired_ms_{kEveryTGateSentinel};
};

} // namespace logpulsex::detail
