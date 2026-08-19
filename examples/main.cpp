#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "logpulsex/logpulsex.hpp"

int main() {
    using namespace logpulsex;

    // --- Set up the default logger with two sinks -------------------
    auto logger = default_logger();
    logger->set_level(Level::raw); // raw is more verbose than trace; permits everything through the logger-level gate

    auto console = std::make_shared<ConsoleSink>();
    console->set_level(Level::info); // keep console less noisy than the file

    auto daily = std::make_shared<DailyFileSink>("app_daily.log",0,0,0,true);
    daily->set_level(Level::raw); // daily file gets more detail than console

    auto file_sink = std::make_shared<RotatingFileSink>(
        "app.log", /*max_bytes=*/1 * 1024 * 5024, /*max_files=*/5);
    file_sink->set_formatter(std::make_shared<JsonFormatter>());
    // Route verbose raw/hex-dump content to the file only -- both the
    // Logger's and this sink's own level need to permit Level::raw
    // independently (see the two-layer filtering note in level.hpp);
    // console is deliberately left untouched so dumps don't clutter it.
    file_sink->set_level(Level::raw);

    logger->add_sink(console);
    logger->add_sink(file_sink);
    logger->add_sink(daily);

    install_crash_handlers();

    // --- Basic usage --------------------------------------------------
    LOG_INFO("Server starting on port {}", 8080);
    LOG_DEBUG("Config loaded: {} entries", 42);

    // --- Log injection safety: this newline-laden "username" cannot
    // forge extra log lines — it gets escaped by the formatter.
    std::string untrusted_username = "admin\nFAKE LINE: root logged in";
    LOG_WARN("Login attempt for user={}", untrusted_username);

    // --- Structured fields via a per-subsystem logger -----------------
    auto db_logger = get_logger("db");
    db_logger->add_sink(console);
    LOG_INFO_TO(db_logger, "Query executed in {} ms", 12.4);

    // --- Structured key/value logging ---------------------------------
    LOG_INFO_KV("Order placed",
                logpulsex::field("order_id", std::string("A1234")),
                logpulsex::field("amount", 59.99),
                logpulsex::field("items", std::vector<int>{101, 205, 310}));

    // --- Containers and format specs -----------------------------------
    std::vector<int> scores{95, 88, 76};
    std::map<std::string, int> inventory{{"widgets", 12}, {"gadgets", 4}};
    LOG_INFO("scores={} inventory={}", scores, inventory);
    LOG_INFO("pi ~= {:.2f}, hex = {:#x}, padded = {:05}", 3.14159, 255, 7);

    // --- Binary & hex dump logging --------------------------------------
    // Short byte sequences (e.g. device protocol command/response pairs)
    // format inline with any normal log level:
    std::uint8_t tx[] = {0xA1, 0xB2};
    std::uint8_t rx[] = {0x00, 0xFF};
    LOG_DEBUG("TX: {}", hex_bytes(tx, sizeof(tx))); // "TX: 0xA1 0xB2"
    LOG_DEBUG("RX: {}", hex_bytes(rx, sizeof(rx))); // "RX: 0x00 0xFF"

    // Larger raw buffers use LOG_RAW + format_hex_dump() for a classic
    // multi-row offset/hex/ASCII layout. LOG_RAW is the one level whose
    // plain-text rendering preserves embedded newlines instead of
    // escaping them -- see the note on Level::raw in level.hpp.
    std::uint8_t device_payload[20];
    for (std::size_t i = 0; i < sizeof(device_payload); ++i) {
        device_payload[i] = static_cast<std::uint8_t>('A' + (i % 26));
    }
    LOG_RAW("Device payload ({} bytes):\n{}", sizeof(device_payload),
            format_hex_dump(device_payload, sizeof(device_payload)));

    // --- Backtrace ring buffer: keep the last N records (any level)
    // in memory even though the logger/console are only tuned down to
    // info/warn, then replay them on demand (e.g. right before/after a
    // recoverable error) without paying sink I/O cost for every one of
    // them up front. A crash also triggers a best-effort replay via
    // install_crash_handlers() above.
    logger->enable_backtrace(50);
    LOG_DEBUG("Cache miss for key={}", "session:42");     // console: suppressed (below info); file/daily: written live
    LOG_DEBUG("Retrying upstream call, attempt={}", 2);   // console: suppressed (below info); file/daily: written live
    LOG_WARN("Upstream call slow: {} ms", 850);            // console + file/daily: written live (warn passes every sink's level)
    // Replays all 3 buffered records through the normal pipeline again --
    // each sink re-applies its own level filter, so console still only
    // shows the warn line (now twice: once live, once replayed), while
    // file/daily (level raw) show all 3 lines twice.
    dump_backtrace();

    // --- Concurrency: many producer threads, one consumer thread ------
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([i, logger] {
            for (int j = 0; j < 1000; ++j) {
                LOG_INFO_TO(logger, "worker {} tick {}", i, j);
            }
        });
    }
    for (auto& t : workers) t.join();

    LOG_ERROR("Simulated recoverable error: {}", "connection reset");

    logger->flush();
    LOG_INFO("Shutdown complete. Dropped records: {}", logger->dropped_count_snapshot());
    return 0;
}
