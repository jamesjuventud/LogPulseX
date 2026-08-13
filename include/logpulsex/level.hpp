#pragma once

#include <cstdint>
#include <string_view>

namespace logpulsex {

// Ordered so that comparisons (level >= threshold) work naturally.
//
// `raw` sits below `trace` (most verbose, filtered out by any normal
// threshold by default) and exists specifically for LOG_RAW: logging
// pre-formatted, deliberately multi-line content -- primarily hex/binary
// dumps produced by format_hex_dump() in hex.hpp. Unlike every other
// level, records logged at Level::raw are NOT escaped for embedded '\n'
// bytes when rendered by PatternFormatter/console sinks, so the intended
// multi-row layout survives instead of being flattened into literal
// "\n" text. This is a deliberate, narrow trust boundary: it is safe for
// content produced by this library's own hex-dump formatting (which by
// construction can only contain hex digits, fixed layout characters, and
// filtered printable-ASCII sidebar text -- never a raw, uncontrolled
// control byte), but a caller who logs arbitrary untrusted text at
// Level::raw takes on responsibility for that text's safety themselves,
// same as any other explicit escape hatch. JsonFormatter is NOT affected
// by this -- it always escapes embedded newlines regardless of level,
// since a raw newline in a JSON string value would make the JSON
// invalid; the `"level":"RAW"` tag it emits instead is the signal a
// downstream viewer can use to re-expand `\n` when rendering.
//
// COMPATIBILITY NOTE: adding `raw` below `trace` shifts every existing
// level's numeric value up by one (trace was 0, is now 1; ... off was 6,
// is now 7). This only matters if you set LOGPULSEX_MIN_LEVEL to a
// specific *integer* in your build rather than referencing level names
// in code (which is unaffected) -- e.g. an old `-DLOGPULSEX_MIN_LEVEL=2`
// meant "strip trace and debug, keep info+" and now means "strip raw,
// trace, and debug, keep info+" (info's own numeric value also shifted
// from 2 to 3, so an *unchanged* flag value of 2 now maps to what used
// to be debug's threshold, not info's) -- re-check any hardcoded
// numeric threshold in your build configuration after upgrading.
enum class Level : std::uint8_t {
    raw   = 0,  // pre-formatted, deliberately multi-line content (hex/binary dumps)
    trace = 1,
    debug = 2,
    info  = 3,
    warn  = 4,
    error = 5,
    fatal = 6,
    off   = 7   // never emitted; used as a sink/logger threshold to silence output
};

constexpr std::string_view to_string(Level lvl) noexcept {
    switch (lvl) {
        case Level::raw:   return "RAW";
        case Level::trace: return "TRACE";
        case Level::debug: return "DEBUG";
        case Level::info:  return "INFO";
        case Level::warn:  return "WARN";
        case Level::error: return "ERROR";
        case Level::fatal: return "FATAL";
        case Level::off:   return "OFF";
    }
    return "UNKNOWN"; // unreachable, but keeps -Wreturn-type / MSVC happy
}

// Compile-time minimum level. Define LOGPULSEX_MIN_LEVEL before including this
// header (e.g. via build flags: -DLOGPULSEX_MIN_LEVEL=2) to strip lower-level
// log statements entirely from release builds — they cost nothing, not even
// argument evaluation.
#ifndef LOGPULSEX_MIN_LEVEL
#define LOGPULSEX_MIN_LEVEL 0
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif

// At the default LOGPULSEX_MIN_LEVEL (0) this is trivially true for every
// Level, which is correct behavior (nothing stripped) — GCC's
// -Wtype-limits flags that as suspicious. Silenced locally rather than
// weakened, since LOGPULSEX_MIN_LEVEL is a legitimate build knob that can
// validly take non-zero values too.
constexpr bool level_enabled_at_compile_time(Level lvl) noexcept {
    return static_cast<int>(lvl) >= LOGPULSEX_MIN_LEVEL;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace logpulsex
