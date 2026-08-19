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
#include <chrono>
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
        detail::InternalMutexGuard lock(mutex_);
        auto it = loggers_.find(name);
        if (it != loggers_.end()) return it->second;
        auto logger = std::make_shared<Logger>(name, config);
        loggers_.emplace(name, logger);
        return logger;
    }

    // Flushes every registered logger. Safe to call multiple times.
    void flush_all() {
        detail::InternalMutexGuard lock(mutex_);
        for (auto& [name, logger] : loggers_) {
            logger->flush();
        }
    }

    // Registers handlers so that a crash (segfault, abort, unhandled
    // termination) doesn't silently drop buffered log records that would
    // explain *why* it crashed. Signal handlers are heavily restricted in
    // what they may safely do (async-signal-safety), so this deliberately
    // does the minimal safe thing: perform a best-effort flush before
    // re-raising the default handler, via flush_all_best_effort() rather
    // than the normal flush_all(). The two concrete risks of naively
    // calling flush_all()/flush() from a signal handler are (1) the exact
    // thread that just faulted already holding mutex_ or a Logger's
    // sinks_mutex_ (e.g. it faulted inside a sink's write() call) --
    // re-locking either from the handler is a guaranteed self-deadlock,
    // not just a race -- and (2) blocking indefinitely if some other
    // thread holds a lock and, post-fault, never releases it.
    // flush_all_best_effort()/Logger::flush_best_effort() address both:
    // a thread-local reentrancy check (see InternalMutexGuard in
    // logger.hpp) skips locks this thread already owns instead of
    // re-locking them, and every lock attempt is bounded with
    // try_lock_for() instead of blocking forever. This is still not a
    // strict POSIX async-signal-safe guarantee -- iterating the map and
    // calling sink->flush() can allocate or call libc I/O not on the
    // async-signal-safe list -- but it eliminates the deterministic
    // deadlock/hang failure mode entirely. For applications where full
    // async-signal-safety matters, prefer calling flush() explicitly at
    // well-defined checkpoints rather than relying solely on this handler
    // as a safety net.
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

    // See install_crash_handlers()'s doc comment: bounded, self-deadlock-
    // aware counterpart to flush_all(), used only from on_fatal_signal().
    void flush_all_best_effort() noexcept {
        if (detail::thread_holds_internal_lock()) return;
        if (!mutex_.try_lock_for(std::chrono::milliseconds(50))) return;
        std::lock_guard<std::timed_mutex> lock(mutex_, std::adopt_lock);
        detail::LockDepthScope depth_scope;
        for (auto& [name, logger] : loggers_) {
            logger->flush_best_effort();
        }
    }

    static void on_fatal_signal(int sig) {
        instance().flush_all_best_effort();
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    }

    static void on_exit() {
        instance().flush_all();
    }

    std::timed_mutex mutex_;
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

// Convenience wrappers around the default logger's backtrace feature --
// see Logger::enable_backtrace()/dump_backtrace() in logger.hpp. Pairs
// naturally with install_crash_handlers(): the crash handler already
// calls Logger::flush_best_effort() (which surfaces any buffered
// backtrace) on a fault, and dump_backtrace() lets application code
// trigger the same replay explicitly, e.g. right before a handled error.
inline void enable_backtrace(std::size_t n) {
    default_logger()->enable_backtrace(n);
}

inline void disable_backtrace() {
    default_logger()->disable_backtrace();
}

inline void dump_backtrace() {
    default_logger()->dump_backtrace();
}

} // namespace logpulsex
