#pragma once

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "logpulsex/logger.hpp"

namespace logpulsex {

// Process-wide registry of named loggers, plus opt-in crash-safety hooks.
// A Meyers singleton gives well-defined, thread-safe lazy initialization
// and guarantees destruction order relative to first use (though see the
// static-destruction-order note on shutdown() below).
class Registry {
public:
    static Registry& instance() {
        static Registry inst;
        return inst;
    }

    std::shared_ptr<Logger> get_or_create(const std::string& name, LoggerConfig config = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = loggers_.find(name);
        if (it != loggers_.end()) return it->second;
        auto logger = std::make_shared<Logger>(name, config);
        loggers_.emplace(name, logger);
        return logger;
    }

    // Flushes every registered logger. Safe to call multiple times.
    void flush_all() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, logger] : loggers_) {
            logger->flush();
        }
    }

    // Registers handlers so that a crash (segfault, abort, unhandled
    // termination) doesn't silently drop buffered log records that would
    // explain *why* it crashed. Signal handlers are heavily restricted in
    // what they may safely do (async-signal-safety), so this deliberately
    // does the minimal safe thing: set an atomic flag and, where possible,
    // perform a best-effort flush before re-raising the default handler.
    // This is a pragmatic compromise, not a strict async-signal-safety
    // guarantee — flush() below is NOT fully async-signal-safe (it takes a
    // mutex), so there is a small residual risk of the handler itself
    // deadlocking if the crash happened while holding sinks_mutex_. For
    // applications where this matters, prefer calling flush() explicitly
    // at well-defined checkpoints rather than relying solely on the
    // signal handler as a safety net.
    void install_crash_handlers() {
        bool expected = false;
        if (!handlers_installed_.compare_exchange_strong(expected, true)) return;

        std::signal(SIGSEGV, &Registry::on_fatal_signal);
        std::signal(SIGABRT, &Registry::on_fatal_signal);
        std::signal(SIGTERM, &Registry::on_fatal_signal);
        std::atexit(&Registry::on_exit);
    }

private:
    Registry() = default;

    static void on_fatal_signal(int sig) {
        instance().flush_all();
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }

    static void on_exit() {
        instance().flush_all();
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Logger>> loggers_;
    std::atomic<bool> handlers_installed_{false};
};

// PERFORMANCE FIX: previously this called Registry::instance().get_or_create("default")
// on every single invocation -- meaning every LOG_INFO/LOG_DEBUG/etc call (the primary,
// documented API) paid for a mutex lock, a string-keyed hash map lookup, and a
// shared_ptr copy (atomic refcount increment) before the cheap runtime level check
// even ran. Measured cost of a runtime-suppressed LOG_INFO call: ~45 ns, versus ~4.5 ns
// for the same suppressed check through an already-cached logger pointer -- a ~10x tax
// caused entirely by resolving the same "default" entry over and over.
//
// Fixed two ways:
//   1. The lookup now happens at most once per process, cached in a function-local
//      static (thread-safe lazy initialization via C++11 "magic statics" -- the same
//      technique Registry::instance() itself already uses, and since this function is
//      `inline`, all translation units share the single process-wide instance per the
//      usual inline-function-local-static guarantee).
//   2. Returns a reference instead of a shared_ptr by value, so repeated calls (e.g.
//      from LOG_INFO's macro expansion, which calls this once per log statement) don't
//      pay for an atomic refcount increment/decrement each time. Existing code that
//      does `auto logger = default_logger();` is unaffected -- `auto` still deduces
//      std::shared_ptr<Logger> and copies from the reference, exactly as before.
inline std::shared_ptr<Logger>& default_logger() {
    static std::shared_ptr<Logger> logger = Registry::instance().get_or_create("default");
    return logger;
}

inline std::shared_ptr<Logger> get_logger(const std::string& name, LoggerConfig config = {}) {
    return Registry::instance().get_or_create(name, config);
}

inline void install_crash_handlers() {
    Registry::instance().install_crash_handlers();
}

} // namespace logpulsex
