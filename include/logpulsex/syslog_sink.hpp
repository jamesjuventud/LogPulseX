#pragma once

#include <string>

#include "logpulsex/sink.hpp"

// syslog(3) is POSIX-only. On platforms without it (Windows), this sink
// compiles to a documented no-op — write()/flush() do nothing, but the
// class's public API (including priority_for()) is IDENTICAL on every
// platform, not platform-conditional. An earlier version of this header
// only defined priority_for() inside the POSIX branch, which meant any
// code calling SyslogSink::priority_for() (including this library's own
// test suite) failed to compile at all on Windows/MSVC with "class has
// no member priority_for" — caught when a user tried building on
// Windows. Keeping one uniform class shape regardless of platform, and
// only conditionally compiling method *bodies*, avoids that whole bug
// class going forward.
#if !defined(_WIN32)
#include <syslog.h>
#define LOGPULSEX_HAVE_SYSLOG 1
#endif

namespace logpulsex {

// Sends records to the local syslog daemon (or systemd-journald, which
// intercepts the same interface on most modern Linux distros).
//
// Portability note: syslog priority/facility numbers (LOG_DEBUG=7,
// LOG_INFO=6, LOG_WARNING=4, LOG_ERR=3, LOG_CRIT=2, LOG_USER=1<<3) are
// standardized BSD syslog values shared by every POSIX libc. We define
// our own class-scoped copies (kPriorityDebug, etc.) instead of
// depending on <syslog.h>'s macros directly, for two reasons:
//   1. <syslog.h> defines LOG_INFO/LOG_DEBUG as plain integers, which
//      collide with this library's own LOG_INFO(...)/LOG_DEBUG(...)
//      logging macros (handled below via a capture-then-#undef).
//   2. It lets callers — including this library's own tests — reference
//      SyslogSink::kPriorityWarning etc. on every platform, including
//      Windows, instead of needing raw <syslog.h> macros that don't
//      exist there. On POSIX we static_assert our copies match the real
//      <syslog.h> values, so any exotic libc where they differ fails
//      loudly at compile time instead of silently misrouting priorities.
//
// Security note: log messages are passed to ::syslog() as the *argument*
// to a literal "%s" format string — never as the format string itself.
// Passing attacker- or application-controlled text directly as a printf
// family format string is a real vulnerability (CWE-134); this sink is
// structured so that can't happen regardless of what the log message
// contains (including literal '%' characters).
//
// Lifetime note: openlog() retains the `ident` pointer it's given rather
// than copying it (implementation-defined, but true on glibc), so this
// sink keeps its own copy alive for its whole lifetime and only calls
// openlog() once that copy exists.
class SyslogSink final : public ISink {
public:
    static constexpr int kPriorityDebug = 7;
    static constexpr int kPriorityInfo = 6;
    static constexpr int kPriorityWarning = 4;
    static constexpr int kPriorityErr = 3;
    static constexpr int kPriorityCrit = 2;
    static constexpr int kFacilityUser = 1 << 3;

    // True on platforms where this sink actually delivers to syslog(3);
    // false where it's a documented no-op (e.g. Windows). Lets callers
    // detect at runtime (or via `if constexpr`) whether attaching this
    // sink will do anything, instead of silently losing log output.
    static constexpr bool is_supported() noexcept {
#if defined(LOGPULSEX_HAVE_SYSLOG)
        return true;
#else
        return false;
#endif
    }

    explicit SyslogSink(std::string ident, int facility = kFacilityUser)
        : ident_(std::move(ident)) {
#if defined(LOGPULSEX_HAVE_SYSLOG)
        ::openlog(ident_.c_str(), LOG_PID, facility);
#else
        (void)facility; // no syslog(3) on this platform; nothing to configure
#endif
    }

    ~SyslogSink() override {
#if defined(LOGPULSEX_HAVE_SYSLOG)
        ::closelog();
#endif
    }

    void write(const LogRecord& record) override {
#if defined(LOGPULSEX_HAVE_SYSLOG)
        ::syslog(priority_for(record.level), "%s", format(record).c_str());
#else
        (void)record; // documented no-op: no syslog(3) on this platform
#endif
    }

    void flush() override {
        // syslog(3) has no user-facing flush; the daemon owns durability
        // from here. Present for interface symmetry with other sinks,
        // and identical (no-op) on every platform either way.
    }

    static constexpr int priority_for(Level lvl) noexcept {
        switch (lvl) {
            case Level::raw:   return kPriorityDebug;
            case Level::trace: return kPriorityDebug;
            case Level::debug: return kPriorityDebug;
            case Level::info:  return kPriorityInfo;
            case Level::warn:  return kPriorityWarning;
            case Level::error: return kPriorityErr;
            case Level::fatal: return kPriorityCrit;
            default:           return kPriorityInfo;
        }
    }

private:
    std::string ident_;
};

#if defined(LOGPULSEX_HAVE_SYSLOG)
static_assert(SyslogSink::kPriorityDebug == LOG_DEBUG, "syslog LOG_DEBUG value mismatch on this platform");
static_assert(SyslogSink::kPriorityInfo == LOG_INFO, "syslog LOG_INFO value mismatch on this platform");
static_assert(SyslogSink::kPriorityWarning == LOG_WARNING, "syslog LOG_WARNING value mismatch on this platform");
static_assert(SyslogSink::kPriorityErr == LOG_ERR, "syslog LOG_ERR value mismatch on this platform");
static_assert(SyslogSink::kPriorityCrit == LOG_CRIT, "syslog LOG_CRIT value mismatch on this platform");
static_assert(SyslogSink::kFacilityUser == LOG_USER, "syslog LOG_USER value mismatch on this platform");

// See the class-level portability note: <syslog.h> defines LOG_INFO and
// LOG_DEBUG as plain integer macros, which collide with this library's
// own LOG_INFO(...)/LOG_DEBUG(...) logging macros. The static_asserts
// above are the last thing in this header that need the real <syslog.h>
// macro values, so undef the two colliding names now — logpulsex.hpp's
// own definitions (included after this header in the master include)
// always win from here on, regardless of include order.
#undef LOG_DEBUG
#undef LOG_INFO
#endif

} // namespace logpulsex
