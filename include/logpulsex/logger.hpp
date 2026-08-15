#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logpulsex/format.hpp"
#include "logpulsex/level.hpp"
#include "logpulsex/log_record.hpp"
#include "logpulsex/sink.hpp"
#include "logpulsex/spsc_mpsc_queue.hpp"

namespace logpulsex::detail {

// Tracks whether the *current* thread already holds one of this
// library's own internal mutexes (Logger::sinks_mutex_, Registry::mutex_).
// A synchronous fault (SIGSEGV, ...) is delivered by the OS to the exact
// thread that caused it, so if that thread crashed while it happened to
// be inside one of these critical sections (e.g. the worker thread
// faulting inside a sink's write() call, which runs under
// sinks_mutex_), a crash handler that blindly re-locks the same mutex
// would self-deadlock -- undefined behavior for a non-recursive mutex,
// and just as undefined for try_lock() on an already-owned mutex, so
// this has to be tracked explicitly rather than probed for. The depth
// counter is incremented before lock() and decremented after unlock()
// (not the reverse) so the window where it reports "held" is always a
// superset of the real locked window -- conservative in both directions,
// never a false "safe to lock".
inline int& internal_lock_depth() {
    thread_local int depth = 0;
    return depth;
}

inline bool thread_holds_internal_lock() {
    return internal_lock_depth() > 0;
}

template <typename Mutex>
class InternalMutexGuard {
public:
    explicit InternalMutexGuard(Mutex& m) : mutex_(m) {
        ++internal_lock_depth();
        mutex_.lock();
    }
    ~InternalMutexGuard() {
        mutex_.unlock();
        --internal_lock_depth();
    }
    InternalMutexGuard(const InternalMutexGuard&) = delete;
    InternalMutexGuard& operator=(const InternalMutexGuard&) = delete;

private:
    Mutex& mutex_;
};

} // namespace logpulsex::detail

namespace logpulsex {

// What happens when the queue is full and a producer thread tries to log.
enum class OverflowPolicy {
    block,        // producer waits until space is available (never loses a record)
    drop_newest,  // silently discard the incoming record, keep older ones flowing
    drop_oldest,  // pop+discard the oldest queued record to make room (best-effort)
};

struct LoggerConfig {
    std::size_t queue_capacity = 8192;     // rounded up to a power of two
    OverflowPolicy overflow_policy = OverflowPolicy::block;
    std::chrono::milliseconds worker_idle_sleep{1};
    std::size_t max_batch_per_wake = 256;  // drain limit per wake to keep other sinks responsive
};

// Central logger: owns the queue, the background worker thread, and the
// set of sinks records are dispatched to. One Logger is typically shared
// across an entire application (see `default_logger()` in logpulsex.hpp),
// but nothing prevents constructing several for isolated subsystems.
class Logger {
public:
    explicit Logger(std::string name, LoggerConfig config = {})
        : name_(std::move(name)),
          config_(config),
          queue_(config.queue_capacity) {
        worker_ = std::thread([this] { run(); });
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    ~Logger() {
        shutdown();
    }

    void add_sink(std::shared_ptr<ISink> sink) {
        detail::InternalMutexGuard lock(sinks_mutex_);
        sinks_.push_back(std::move(sink));
    }

    void set_level(Level lvl) noexcept { level_.store(lvl, std::memory_order_relaxed); }
    Level level() const noexcept { return level_.load(std::memory_order_relaxed); }

    bool should_log(Level lvl) const noexcept {
        return lvl >= level() && lvl != Level::off;
    }

    // Called by the LOG_* macros. Takes the format string + args so message
    // construction happens on the producer thread (cheap: a handful of
    // stream insertions), while the slow part — actual I/O — happens later
    // on the worker thread.
    template <typename... Args>
    void log(Level lvl, const char* file, int line, const char* function,
              std::string_view fmt, const Args&... args) {
        if (!should_log(lvl)) return;

        LogRecord record;
        record.level = lvl;
        record.timestamp = std::chrono::system_clock::now();
        record.thread_id = std::this_thread::get_id();
        record.native_thread_id = detail::get_native_thread_id();
        record.process_id = detail::get_process_id();
        record.logger_name = name_;
        record.message = detail::format(fmt, args...);
        record.file = file;
        record.line = line;
        record.function = function;

        enqueue(std::move(record));

        // FATAL implies the process is about to terminate; make sure the
        // record actually reaches its sinks before we return, since there
        // may be no further opportunity to flush.
        if (lvl == Level::fatal) {
            flush();
        }
    }

    // Structured logging: attaches key/value fields to the record instead
    // of (or in addition to) interpolating them into the message text.
    // `fields` entries are typically built with logpulsex::field(key, value).
    // Called by the LOG_*_KV macros.
    void log_kv(Level lvl, const char* file, int line, const char* function,
                std::string_view message, std::initializer_list<Field> fields) {
        if (!should_log(lvl)) return;

        LogRecord record;
        record.level = lvl;
        record.timestamp = std::chrono::system_clock::now();
        record.thread_id = std::this_thread::get_id();
        record.native_thread_id = detail::get_native_thread_id();
        record.process_id = detail::get_process_id();
        record.logger_name = name_;
        record.message = std::string(message);
        record.fields.assign(fields.begin(), fields.end());
        record.file = file;
        record.line = line;
        record.function = function;

        enqueue(std::move(record));

        if (lvl == Level::fatal) {
            flush();
        }
    }

    // Blocks until every currently-queued record has been dispatched and
    // every sink has flushed. Safe to call from any thread.
    void flush() {
        // Drain marker: push a sentinel and wait for the worker to signal
        // past it. Simpler alternative used here: poll until queue empty,
        // then flush sinks. Adequate because flush() is not a hot-path call.
        while (approx_size_.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        detail::InternalMutexGuard lock(sinks_mutex_);
        for (auto& sink : sinks_) sink->flush();
    }

    // Crash-handler-only counterpart to flush(): must never block
    // indefinitely and must never attempt to lock a mutex this exact
    // thread already holds (see InternalMutexGuard's doc comment). Does
    // not wait for the queue to drain -- if the worker thread is the one
    // that crashed, it never will -- so this only flushes whatever sinks
    // have already buffered/written, on a short bounded timeout. Strictly
    // a best-effort improvement over doing nothing: still not a strict
    // POSIX async-signal-safe guarantee (sink->flush() may itself
    // allocate or call libc I/O not on the async-signal-safe list), but
    // it eliminates the deterministic self-deadlock case entirely.
    void flush_best_effort() noexcept {
        if (detail::thread_holds_internal_lock()) return;
        if (!sinks_mutex_.try_lock_for(std::chrono::milliseconds(20))) return;
        std::lock_guard<std::timed_mutex> lock(sinks_mutex_, std::adopt_lock);
        for (auto& sink : sinks_) {
            try { sink->flush(); } catch (...) { /* best effort; nothing more we can do here */ }
        }
    }

    void shutdown() {
        bool expected = false;
        if (!shutting_down_.compare_exchange_strong(expected, true)) {
            return; // already shut down
        }
        if (worker_.joinable()) {
            stop_requested_.store(true, std::memory_order_release);
            worker_.join();
        }
        detail::InternalMutexGuard lock(sinks_mutex_);
        for (auto& sink : sinks_) sink->flush();
    }

    const std::string& name() const noexcept { return name_; }

private:
    void enqueue(LogRecord record) {
        for (;;) {
            if (queue_.try_push(std::move(record))) {
                approx_size_.fetch_add(1, std::memory_order_release);
                return;
            }
            switch (config_.overflow_policy) {
                case OverflowPolicy::drop_newest:
                    ++dropped_count_;
                    return;
                case OverflowPolicy::drop_oldest:
                    queue_.try_pop(); // best-effort: make room, then retry push
                    ++dropped_count_;
                    continue;
                case OverflowPolicy::block:
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    continue;
            }
        }
    }

    void run() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::size_t drained = drain_batch();
            if (drained == 0) {
                std::this_thread::sleep_for(config_.worker_idle_sleep);
            }
        }
        drain_batch(/*unlimited=*/true); // final flush of anything left at shutdown
    }

    std::size_t drain_batch(bool unlimited = false) {
        std::size_t count = 0;
        // Extra parens around max() prevent accidental macro expansion
        // if a consumer's project includes <windows.h> without NOMINMAX
        // before this header -- see the identical note in throttle.hpp.
        std::size_t limit = unlimited ? (std::numeric_limits<std::size_t>::max)()
                                      : config_.max_batch_per_wake;
        while (count < limit) {
            auto item = queue_.try_pop();
            if (!item.has_value()) break;
            approx_size_.fetch_sub(1, std::memory_order_release);
            dispatch(*item);
            ++count;
        }
        return count;
    }

    void dispatch(const LogRecord& record) {
        detail::InternalMutexGuard lock(sinks_mutex_);
        for (auto& sink : sinks_) {
            if (record.level < sink->level()) continue;
            // A misbehaving sink (e.g. disk full, network down) must not
            // be able to take down the worker thread and silence every
            // other sink. Swallow and continue; sinks are responsible for
            // their own error recovery/backoff internally.
            try {
                sink->write(record);
            } catch (...) {
                ++sink_error_count_;
            }
        }
    }

    std::string name_;
    LoggerConfig config_;
    BoundedMpscQueue<LogRecord> queue_;

    std::atomic<Level> level_{Level::trace};
    std::atomic<std::size_t> approx_size_{0};
    std::atomic<std::size_t> dropped_count_{0};
    std::atomic<std::size_t> sink_error_count_{0};

    std::timed_mutex sinks_mutex_;
    std::vector<std::shared_ptr<ISink>> sinks_;

    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> shutting_down_{false};

public:
    std::size_t dropped_count_snapshot() const noexcept {
        return dropped_count_.load(std::memory_order_relaxed);
    }
    std::size_t sink_error_count_snapshot() const noexcept {
        return sink_error_count_.load(std::memory_order_relaxed);
    }
};

} // namespace logpulsex
