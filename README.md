# LogPulseX
A small, header-only C++20 logging library: async, lock-free on the hot path, with console, size-based rotating file, daily rotating file, syslog, and TCP network sinks, structured (JSON) logging, and crash-safe flushing.

## Quick start

```cpp
#include "logpulsex/logpulsex.hpp"

int main() {
    auto logger = logpulsex::default_logger();
    logger->add_sink(std::make_shared<logpulsex::ConsoleSink>());

    auto file_sink = std::make_shared<logpulsex::RotatingFileSink>(
        "app.log", /*max_bytes=*/10 * 1024 * 1024, /*max_files=*/5);
    file_sink->set_formatter(std::make_shared<logpulsex::JsonFormatter>());
    logger->add_sink(file_sink);

    logpulsex::install_crash_handlers();

    LOG_INFO("Server starting on port {}", 8080);
    LOG_ERROR("Failed to connect to {}: {}", "db-primary", "timeout");
}
```

Build:
```
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
./logpulsex_example
ctest
```

Or without CMake:
```
g++ -std=c++20 -O2 -Iinclude -pthread examples/main.cpp -o example
```

## Architecture

```
Application code (LOG_INFO(...) macros)
        |
Logger front-end (compile-time + runtime level filtering, builds LogRecord)
        |
Lock-free bounded MPSC queue (fixed capacity, backpressure policy)
        |
Background worker thread (single consumer, drains + dispatches)
        |
Sink manager -> ConsoleSink / RotatingFileSink / custom sinks
        |
Formatter -> PatternFormatter / JsonFormatter
```

See `include/logpulsex/` — each header is single-purpose and documents its
own design tradeoffs inline.

## Design decisions

- **Header-only.** Easiest to drop into an existing build; no separate
  compiled artifact to version-match. Trade-off: slightly longer compile
  times per translation unit, mitigated by keeping headers focused so you
  only pay for what you `#include`.
- **No printf-style format strings.** `LOG_INFO("{} {}", a, b)` uses
  variadic templates and `operator<<`, so arguments are type-checked at
  compile time and a message can never be misinterpreted as a format
  directive (CWE-134).
- **Bounded queue + explicit overflow policy.** Logging can never cause
  unbounded memory growth. You choose `block` (never lose a record, may
  apply backpressure to producers), `drop_newest`, or `drop_oldest` up
  front, matching your reliability requirements.
- **Single consumer thread.** Sinks and formatters don't need their own
  locking; the queue is the only concurrency-sensitive component, and it
  uses a well-known correct algorithm (Vyukov's bounded MPMC queue,
  restricted here to MPSC use) instead of a mutex.
- **Log-injection-safe formatting.** Message content is escaped before
  being written (control characters, quotes in JSON) so untrusted data
  logged by the application cannot forge additional log lines or break
  structured output (CWE-117).
- **Sink failures are isolated.** One sink throwing (disk full, socket
  closed) is caught and counted, never taking down the worker thread or
  silencing other sinks.
- **`LOGPULSEX_MIN_LEVEL` compile-time stripping.** Set via build flag to
  remove low-severity log statements — including their argument
  evaluation — entirely from release binaries.
- **Crash-safety is best-effort by design.** `install_crash_handlers()`
  flushes on SIGSEGV/SIGABRT/SIGTERM/exit, but the flush path is not
  strictly async-signal-safe (it takes a mutex). This is documented
  explicitly in `registry.hpp` rather than overclaiming a guarantee the
  implementation can't fully back.

## Complex logging features

**Structured key/value logging** — attach fields instead of (or alongside)
free-text messages:
```cpp
LOG_INFO_KV("Order placed",
            logpulsex::field("order_id", std::string("A1234")),
            logpulsex::field("amount", 59.99));
```
Fields render as extra top-level keys in `JsonFormatter` output, and via a
`{fields}` token in `PatternFormatter` patterns.

**Containers and optionals** log directly, including nested containers:
```cpp
std::vector<int> scores{95, 88, 76};
std::map<std::string, int> inventory{{"widgets", 12}};
LOG_INFO("scores={} inventory={}", scores, inventory);
// scores=[95, 88, 76] inventory={widgets: 12}
```
Supported out of the box: `vector`, `array`, `deque`, `list`, `set`,
`unordered_set`, `map`, `unordered_map`, `pair`, `optional` — including
nested combinations (`vector<vector<int>>`, etc). See
`include/logpulsex/container_format.hpp`.

**Format specifiers**, a subset of the `{fmt}`/Python mini-language:
```cpp
LOG_INFO("pi ~= {:.2f}", 3.14159);   // pi ~= 3.14
LOG_INFO("{:#x}", 255);              // 0xff
LOG_INFO("{:05}", 7);                // 00007
LOG_INFO("{:>10}|{:<10}|{:^10}", a, b, c); // right/left/center align
LOG_INFO("literal {{}} braces", 1);  // literal {} braces 1
```
Supported: width, zero-padding, precision, `f`/`e`/`x`/`X`/`o` presentation
types, `<`/`>`/`^` alignment, `+` sign, `#` alt-form, and `{{`/`}}` literal
brace escaping. See `parse_spec`/`apply_value` in `format.hpp`.

**Conditional and rate-limited logging** (glog-style):
```cpp
LOG_IF(logpulsex::Level::warn, queue_depth > 1000, "Queue backed up: {}", queue_depth);
LOG_EVERY_N(logpulsex::Level::info, 100, "Processed {} so far", count);      // 1st, 101st, 201st...
LOG_IF_EVERY_N(logpulsex::Level::warn, retry_failed, 10, "Retry #{}", n);    // every 10th time retry_failed is true
LOG_FIRST_N(logpulsex::Level::warn, 5, "Deprecated API called: {}", name);   // only the first 5 times
LOG_EVERY_T(logpulsex::Level::info, 1.0, "Heartbeat, tick={}", tick);        // at most once per second
```
These use a function-local static counter (or, for `LOG_EVERY_T`, a
dedicated `EveryTGate`) tied to the call site, so they must be used
inside a function body (not at namespace scope). Both are atomic and
safe to hit from multiple threads concurrently — verified under
ThreadSanitizer with 8 threads sharing one call site (exact occurrence
that fires may shift by one under heavy contention, by design, in
exchange for not taking a lock on every call).

> **Bug found and fixed during testing:** an earlier version of
> `LOG_EVERY_T` compared the current time against a last-fired sentinel
> initialized to `0`, using `steady_clock`'s time-since-epoch (typically
> process/system uptime) as "now." That silently assumed the process had
> already been running longer than the configured interval by the time
> the call site was first reached — on a freshly started process (or any
> long-interval `LOG_EVERY_T` reached within the first `interval`
> seconds of uptime), the very first call was wrongly suppressed instead
> of firing immediately, which is the expected behavior (mirroring
> glog's `LOG_EVERY_T`: the first occurrence always logs). This was
> caught by a regression run in a container with ~69s of uptime against
> a 60s test interval. The fix extracts the throttle logic into
> `logpulsex::detail::EveryTGate` (`throttle.hpp`) with an explicit
> "never fired yet" sentinel far enough from zero that the first call
> always fires regardless of clock value, and is now covered by 4
> dedicated deterministic unit tests that drive it with explicit
> synthetic clock readings (including the exact low-uptime scenario)
> instead of depending on real elapsed wall-clock time.

**Daily rotating file sink** — rotates once per day at a configured local
time instead of by size:
```cpp
auto daily = std::make_shared<logpulsex::DailyFileSink>(
    "app.log", /*rotation_hour=*/0, /*rotation_minute=*/0, /*max_files=*/14);
logger->add_sink(daily);
// writes to app_2026-07-20.log, then app_2026-07-21.log after midnight, etc.
```
`max_files` (optional, default 0 = keep forever) prunes the oldest daily
files once the limit is exceeded. The rotation clock is injectable
(`DailyFileSink::Clock`) specifically so tests can simulate crossing a
midnight boundary deterministically instead of waiting on the real clock
— see `test_daily_file_sink_actually_rotates_at_boundary` in the test
suite for exactly how.

**Syslog sink** — sends records to the local syslog daemon / journald
(POSIX only; compiles to a documented no-op on Windows):
```cpp
#include "logpulsex/syslog_sink.hpp"
auto sl = std::make_shared<logpulsex::SyslogSink>("myapp", LOG_USER);
logger->add_sink(sl);
```
Log levels map to syslog priorities (`trace`/`debug`→`LOG_DEBUG`,
`info`→`LOG_INFO`, `warn`→`LOG_WARNING`, `error`→`LOG_ERR`,
`fatal`→`LOG_CRIT`). Message content is always passed as the *argument*
to a literal `"%s"` format string, never as the format string itself, so
log content containing `%` conversion specifiers can't be misinterpreted
by `syslog()` (CWE-134) — verified with an explicit `%n`/`%s`-payload
test. Note: `<syslog.h>` defines plain-integer `LOG_INFO`/`LOG_DEBUG`
macros that collide with this library's own `LOG_INFO(...)`/`LOG_DEBUG(...)`
macros; `syslog_sink.hpp` neutralizes this automatically (captures the
values it needs, then `#undef`s the colliding names) so the library's
logging macros always win regardless of include order — you don't need
to do anything, but it's worth knowing if you ever see `LOG_INFO`/`LOG_DEBUG`
referenced as bare integers elsewhere in your codebase after including
this header.

**Network sink** — ships formatted lines to a remote TCP log collector:
```cpp
#include "logpulsex/network_sink.hpp"
auto net = std::make_shared<logpulsex::NetworkSink>(
    "logs.internal.example.com", 5140,
    /*max_buffered_lines=*/1000, /*connect_timeout_ms=*/200, /*send_timeout_ms=*/100);
logger->add_sink(net);
```
Built for a flaky network by design: connect/send never block longer than
the configured timeouts (non-blocking sockets + `select()`), a failed
send buffers the line locally instead of losing it, and the local backlog
is bounded (`max_buffered_lines`) so a sustained outage can't grow memory
without limit — oldest buffered lines drop first. Reconnection is
lazy: the next `write()` or `flush()` after an outage retries the
connection and drains anything backlogged. Verified against a real local
TCP server across 5 scenarios: normal delivery, no server listening
(must not block), bounded backlog under sustained outage, reconnect +
backlog drain after the server comes back up, and sink-failure isolation
inside a `Logger` with another sink attached. Plaintext, no TLS — treat
as trusted-network-only (see security note in `network_sink.hpp`) unless
tunneled.

## Binary and hex dump logging

Two complementary tools for logging raw bytes, in `hex.hpp`.

**Inline command/response formatting** with `hex_bytes()` — for short
byte sequences (device protocol traces, command/response pairs), works
with any normal log level via the existing `{}` message formatting:
```cpp
std::uint8_t tx[] = {0xA1, 0xB2};
std::uint8_t rx[] = {0x00, 0xFF};
LOG_DEBUG("TX: {}", logpulsex::hex_bytes(tx, sizeof(tx))); // "TX: 0xA1 0xB2"
LOG_DEBUG("RX: {}", logpulsex::hex_bytes(rx, sizeof(rx))); // "RX: 0x00 0xFF"
```
Truncates to 256 bytes by default (pass a larger `max_bytes` explicitly
if needed) so a large or attacker-influenced buffer can't produce an
unbounded log line.

**Multi-row hex dumps** with `format_hex_dump()` + `LOG_RAW` — for
larger raw buffers, produces a classic offset/hex/ASCII layout:
```cpp
LOG_RAW("Device payload ({} bytes):\n{}", size, logpulsex::format_hex_dump(buf, size));
// 00000000  41 42 43 44 45 46 47 48 49 4A 4B 4C 4D 4E 4F 50  |ABCDEFGHIJKLMNOP|
// 00000010  51 52 53 54                                      |QRST|
```

### `Level::raw` and why it's needed

Every other level's plain-text rendering escapes embedded `\n` bytes in
the message (see the log-injection notes elsewhere in this file) — by
design, so untrusted text can't forge extra log lines. That's exactly
wrong for a hex dump, which *wants* real line breaks for its layout. A
new level, `Level::raw`, is the one exception: `PatternFormatter`/console
output renders `Level::raw` messages verbatim, preserving real newlines.

This is safe specifically because `format_hex_dump()`'s output can, by
construction, only ever contain hex digits, fixed layout characters, and
ASCII-sidebar text already filtered through a printable-character check
(which excludes the newline byte and every other control character) —
the *only* `\n` characters it can produce are the row separators the
function inserts itself, never one derived from the input buffer. Using
`LOG_RAW` with arbitrary untrusted text instead of this library's own
hex-dump output forfeits that guarantee — it's a deliberate, narrow
escape hatch, not a blanket exemption, and the caller takes on
responsibility for what they pass.

`JsonFormatter` is **not** affected by any of this — it always escapes
embedded newlines regardless of level, since a raw newline in a JSON
string value would produce invalid JSON. Instead, raw-level JSON records
carry `"level":"RAW"`, which is the signal a downstream viewer tool can
use to re-expand the escaped `\n` sequences back into real line breaks
when rendering — full JSON validity is preserved, and the multi-line
intent survives as metadata rather than as literal structure.

### Two gotchas worth knowing

1. **`raw` sits below `trace`** (most verbose tier, filtered out by any
   default threshold), so `LOG_RAW` calls are silently suppressed unless
   you explicitly opt in with `logger->set_level(Level::raw)`.
2. **Both the `Logger`'s level and each `Sink`'s own level are
   independent filters** — both must permit `Level::raw` for a raw
   record to reach a particular sink. Easy to forget: opening up the
   logger alone is not enough if a sink's own level (which defaults to
   `trace`) hasn't also been lowered. This is pre-existing, correct
   two-layer filtering behavior (lets you route verbose raw dumps to one
   sink, e.g. a file, without cluttering another, e.g. the console) —
   just unusually easy to trip over with `raw` specifically, since it
   sits below every other level. See `examples/main.cpp` for the
   intended pattern: route dumps to a dedicated sink, leave others
   untouched.

**Compatibility note:** adding `Level::raw` below `trace` shifted every
existing level's numeric value up by one. This only matters if your
build sets `LOGPULSEX_MIN_LEVEL` to a specific *integer* rather than
referencing level names in code — re-check any hardcoded numeric
threshold after upgrading; see the full note on `Level` in `level.hpp`.