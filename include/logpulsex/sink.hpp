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

#include <iostream>
#include <memory>
#include <mutex>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "logpulsex/formatter.hpp"
#include "logpulsex/level.hpp"
#include "logpulsex/log_record.hpp"

namespace logpulsex {

// A Sink receives already-dequeued records from the single worker thread,
// so implementations do NOT need to be thread-safe against concurrent
// writes from multiple application threads. They DO need to be safe to
// call flush()/write() interleaved on that one thread, which is trivial.
class ISink {
public:
    virtual ~ISink() = default;

    virtual void write(const LogRecord& record) = 0;
    virtual void flush() = 0;

    Level level() const noexcept { return level_.load(std::memory_order_relaxed); }
    void set_level(Level lvl) noexcept { level_.store(lvl, std::memory_order_relaxed); }

    void set_formatter(std::shared_ptr<IFormatter> formatter) {
        formatter_ = std::move(formatter);
    }

protected:
    std::string format(const LogRecord& record) const {
        return formatter_ ? formatter_->format(record) : record.message;
    }

private:
    std::atomic<Level> level_{Level::trace};
    std::shared_ptr<IFormatter> formatter_ = std::make_shared<PatternFormatter>();
};

// Writes to stdout (info and below) / stderr (warn and above), with
// optional ANSI colors. Colors are disabled automatically when the stream
// is not a terminal, so redirected output (e.g. `app > log.txt`) stays
// clean plain text.
//
// Thread safety: a Sink is normally only ever touched by the single
// worker thread of the Logger it's attached to, so ISink implementations
// don't need internal locking as a rule. ConsoleSink is the deliberate
// exception: routing several subsystem loggers (each with its own
// worker thread) to one shared console is common enough that this sink
// guards its writes with a mutex so that pattern is safe. If you write a
// custom sink and share it across multiple Logger instances, apply the
// same guard — it is not automatic for sinks in general.
class ConsoleSink final : public ISink {
public:
    explicit ConsoleSink(bool force_color = false) {
        color_enabled_ = force_color || stream_is_tty();
    }

    void write(const LogRecord& record) override {
        std::string line = format(record);
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostream& stream = (record.level >= Level::warn) ? std::cerr : std::cout;
        if (color_enabled_) {
            stream << color_for(record.level) << line << "\033[0m" << '\n';
        } else {
            stream << line << '\n';
        }
    }

    void flush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout.flush();
        std::cerr.flush();
    }

private:
    static bool stream_is_tty() {
#if defined(_WIN32)
        return false; // conservative default; extend with GetConsoleMode if needed
#else
        return ::isatty(fileno(stdout)) != 0;
#endif
    }

    static const char* color_for(Level lvl) {
        switch (lvl) {
            case Level::raw:   return "\033[35m"; // magenta -- visually distinct for hex/binary dumps
            case Level::trace: return "\033[90m";
            case Level::debug: return "\033[36m";
            case Level::info:  return "\033[32m";
            case Level::warn:  return "\033[33m";
            case Level::error: return "\033[31m";
            case Level::fatal: return "\033[1;31m";
            default: return "";
        }
    }

    bool color_enabled_ = false;
    std::mutex mutex_;
};

} // namespace logpulsex
