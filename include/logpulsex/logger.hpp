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

#include "logpulsex/backtrace_ring_buffer.hpp"
#include "logpulsex/format.hpp"
#include "logpulsex/internal_lock.hpp"
#include "logpulsex/level.hpp"
#include "logpulsex/log_record.hpp"
#include "logpulsex/sink.hpp"
#include "logpulsex/spsc_mpsc_queue.hpp"

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
        bool normal = should_log(lvl);
        bool capture = backtrace_enabled_.load(std::memory_order_relaxed);
        if (!normal && !capture) return;

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

        if (capture) backtrace_buffer_.push(record);
        if (!normal) return;

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
        bool normal = should_log(lvl);
        bool capture = backtrace_enabled_.load(std::memory_order_relaxed);
        if (!normal && !capture) return;

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

        if (capture) backtrace_buffer_.push(record);
        if (!normal) return;

        enqueue(std::move(record));

        if (lvl == Level::fatal) {
            flush();
        }
    }

    // Blocks until every currently-queued record has been dispatched and
    // every sink has flushed. Safe to call from any thread.
    void flush() {
        if (approx_size_.load(std::memory_order_acquire) > 0) {
            std::unique_lock<std::mutex> lk(cv_mutex_);
            flush_waiters_.fetch_add(1, std::memory_order_relaxed);
            // Bounded wait per iteration: a missed/racing notify from the
            // worker just costs one extra short wait, never an indefinite
            // hang -- the real drain progress is still driven by
            // approx_size_, this condvar only avoids busy-polling for it.
            while (approx_size_.load(std::memory_order_acquire) > 0) {
                drained_cv_.wait_for(lk, std::chrono::milliseconds(1));
            }
            flush_waiters_.fetch_sub(1, std::memory_order_relaxed);
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
        detail::LockDepthScope depth_scope;

        // Best-effort: surface any buffered backtrace context before
        // flushing, so a crash report captures the trace/debug detail
        // leading up to it, not just the fact that it happened. Bounded
        // and reentrancy-aware for the same reason as sinks_mutex_ above
        // -- see RingBuffer::try_snapshot()'s doc comment.
        if (backtrace_enabled_.load(std::memory_order_relaxed)) {
            std::vector<LogRecord> backtrace;
            if (backtrace_buffer_.try_snapshot(std::chrono::milliseconds(20), backtrace)) {
                for (auto& record : backtrace) {
                    for (auto& sink : sinks_) {
                        if (record.level < sink->level()) continue;
                        try { sink->write(record); } catch (...) { /* best effort */ }
                    }
                }
            }
        }

        for (auto& sink : sinks_) {
            try { sink->flush(); } catch (...) { /* best effort; nothing more we can do here */ }
        }
    }

    // Keeps the last `n` records in memory -- at any level not compiled
    // out via LOGPULSEX_MIN_LEVEL -- bypassing the runtime severity
    // filter set by set_level(), so verbose diagnostic detail survives
    // even when the configured level is quieter (e.g. info/warn).
    // Nothing reaches sinks until dump_backtrace() is called, so this
    // buys the diagnostic value of trace-level logging without paying
    // sink I/O for every message up front. Safe to call from any thread.
    void enable_backtrace(std::size_t n) {
        backtrace_buffer_.reset(n);
        backtrace_enabled_.store(true, std::memory_order_relaxed);
    }

    void disable_backtrace() {
        backtrace_enabled_.store(false, std::memory_order_relaxed);
        backtrace_buffer_.clear();
    }

    bool backtrace_enabled() const noexcept {
        return backtrace_enabled_.load(std::memory_order_relaxed);
    }

    // Replays every currently-buffered record through the normal async
    // queue, in original insertion (oldest-to-newest) order, preserving
    // each record's original timestamp/level so a reader can tell they're
    // historical context rather than newly-occurring events. Goes through
    // the same queue + worker thread as ordinary log() calls -- never
    // writes to a sink directly from the calling thread -- so it
    // respects the existing single-writer-per-sink contract (see ISink's
    // doc comment). Does not clear the buffer: it keeps collecting, and
    // calling this again later simply replays whatever it holds then.
    void dump_backtrace() {
        for (auto& record : backtrace_buffer_.snapshot()) {
            enqueue(std::move(record));
        }
    }

    void shutdown() {
        bool expected = false;
        if (!shutting_down_.compare_exchange_strong(expected, true)) {
            return; // already shut down
        }
        if (worker_.joinable()) {
            stop_requested_.store(true, std::memory_order_release);
            {
                // Wake an idle worker (and any still-blocked producers)
                // immediately instead of leaving them to their bounded
                // wait timeouts, so shutdown isn't held up by them.
                std::lock_guard<std::mutex> lk(cv_mutex_);
                not_empty_cv_.notify_all();
                not_full_cv_.notify_all();
            }
            worker_.join();
        }
        detail::InternalMutexGuard lock(sinks_mutex_);
        for (auto& sink : sinks_) sink->flush();
    }

    const std::string& name() const noexcept { return name_; }

private:
    void enqueue(LogRecord record) {
        for (;;) {
            // try_push_preserve() never touches `record` on a failed
            // attempt (unlike try_push(T), whose by-value parameter is
            // move-constructed at the call site regardless of outcome,
            // which would silently empty message/logger_name/fields on
            // the first failed attempt and corrupt every subsequent
            // retry under drop_oldest/block).
            if (queue_.try_push_preserve(record)) {
                approx_size_.fetch_add(1, std::memory_order_release);
                // Wake an idle worker. Relaxed pre-check keeps the common
                // (already-running worker) case lock-free, matching the
                // rest of the producer hot path.
                if (worker_idle_.load(std::memory_order_relaxed)) {
                    std::lock_guard<std::mutex> lk(cv_mutex_);
                    not_empty_cv_.notify_one();
                }
                return;
            }
            switch (config_.overflow_policy) {
                case OverflowPolicy::drop_newest:
                    ++dropped_count_;
                    return;
                case OverflowPolicy::drop_oldest: {
                    // BoundedMpscQueue::try_pop() is single-consumer-only
                    // (see spsc_mpsc_queue.hpp). The worker thread already
                    // calls it from drain_batch(); without pop_mutex_ this
                    // producer-side eviction would run concurrently with
                    // that call from a different thread, corrupting the
                    // queue's internal dequeue cursor and livelocking both
                    // sides. Only paid when drop_oldest is configured --
                    // block/drop_newest never pop from a producer thread.
                    std::optional<LogRecord> evicted;
                    {
                        std::lock_guard<std::mutex> pop_lock(pop_mutex_);
                        evicted = queue_.try_pop();
                    }
                    // Keep approx_size_ in sync with the eviction, or it
                    // permanently drifts above the real occupancy and
                    // flush() would wait forever for it to reach zero.
                    if (evicted.has_value()) {
                        approx_size_.fetch_sub(1, std::memory_order_release);
                    }
                    ++dropped_count_;
                    continue;
                }
                case OverflowPolicy::block: {
                    std::unique_lock<std::mutex> lk(cv_mutex_);
                    if (shutting_down_.load(std::memory_order_acquire)) {
                        // No one will ever drain again; drop rather than
                        // block this thread forever.
                        ++dropped_count_;
                        return;
                    }
                    blocked_producers_.fetch_add(1, std::memory_order_relaxed);
                    // Bounded wait: a safety net against a missed wakeup
                    // (e.g. a race against the consumer's notify) instead
                    // of ever blocking indefinitely on a signal that might
                    // not arrive. The queue itself remains the source of
                    // truth -- we always re-check via try_push above.
                    not_full_cv_.wait_for(lk, std::chrono::milliseconds(1));
                    blocked_producers_.fetch_sub(1, std::memory_order_relaxed);
                    continue;
                }
            }
        }
    }

    void run() {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            std::size_t drained = drain_batch();
            if (drained == 0 && !spin_for_work()) {
                wait_for_work();
            }
        }
        drain_batch(/*unlimited=*/true); // final flush of anything left at shutdown
    }

    // Brief lock-free spin before the worker commits to sleeping. Producers
    // only pay the cv_mutex_ lock+notify cost when worker_idle_ is true, so
    // for bursty/tight-loop producers (the common case) this lets the
    // worker notice the next record on its own -- via a plain relaxed load,
    // no lock ever taken -- without ever setting worker_idle_ and forcing
    // every producer through the mutex. Only a producer that arrives after
    // this spin gives up (genuinely idle worker) pays that cost, in
    // exchange for a fast wake instead of sleeping the full idle interval.
    bool spin_for_work() {
        for (int i = 0; i < kIdleSpinIterations; ++i) {
            if (approx_size_.load(std::memory_order_acquire) > 0) return true;
            std::this_thread::yield();
        }
        return false;
    }

    void wait_for_work() {
        std::unique_lock<std::mutex> lk(cv_mutex_);
        worker_idle_.store(true, std::memory_order_relaxed);
        // No predicate: any notify_one() (new work or shutdown) must wake
        // this immediately. A predicate-checking overload that only
        // re-checks stop_requested_ would ignore a notify meant to signal
        // new work, silently sleeping the full timeout regardless -- this
        // plain wait_for() returns on any notify, spurious wake, or
        // timeout, and run()'s caller always re-checks drain_batch()/
        // stop_requested_ right after this returns either way.
        not_empty_cv_.wait_for(lk, config_.worker_idle_sleep);
        worker_idle_.store(false, std::memory_order_relaxed);
    }

    std::size_t drain_batch(bool unlimited = false) {
        std::size_t count = 0;
        // Extra parens around max() prevent accidental macro expansion
        // if a consumer's project includes <windows.h> without NOMINMAX
        // before this header -- see the identical note in throttle.hpp.
        std::size_t limit = unlimited ? (std::numeric_limits<std::size_t>::max)()
                                      : config_.max_batch_per_wake;
        bool freed_space = false;
        {
            // Locked once for the whole batch instead of once per record:
            // sinks_ essentially never changes at runtime, so paying a
            // (timed_mutex) lock/unlock cycle per dispatched message was
            // pure overhead on the hottest part of the consumer path.
            detail::InternalMutexGuard lock(sinks_mutex_);
            const bool needs_pop_lock =
                config_.overflow_policy == OverflowPolicy::drop_oldest;
            while (count < limit) {
                std::optional<LogRecord> item;
                if (needs_pop_lock) {
                    // Must match the eviction side's locking above, or the
                    // two concurrent try_pop() callers can still race.
                    std::lock_guard<std::mutex> pop_lock(pop_mutex_);
                    item = queue_.try_pop();
                } else {
                    item = queue_.try_pop();
                }
                if (!item.has_value()) break;
                std::size_t remaining =
                    approx_size_.fetch_sub(1, std::memory_order_acq_rel) - 1;
                dispatch_locked(*item);
                ++count;
                freed_space = true;
                if (remaining == 0 && flush_waiters_.load(std::memory_order_relaxed) > 0) {
                    std::lock_guard<std::mutex> cv_lk(cv_mutex_);
                    drained_cv_.notify_all();
                }
            }
        }
        if (freed_space && blocked_producers_.load(std::memory_order_relaxed) > 0) {
            std::lock_guard<std::mutex> cv_lk(cv_mutex_);
            not_full_cv_.notify_all();
        }
        return count;
    }

    // Precondition: caller already holds sinks_mutex_ (see drain_batch()).
    void dispatch_locked(const LogRecord& record) {
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

    // Guards queue_.try_pop() calls only when overflow_policy ==
    // drop_oldest, where both the worker (drain_batch) and producer
    // threads (eviction on a full queue) can call try_pop() concurrently.
    // try_pop() is single-consumer-only, so unsynchronized concurrent
    // callers corrupt the queue's internal cursor -- see enqueue().
    std::mutex pop_mutex_;

    std::atomic<bool> backtrace_enabled_{false};
    detail::RingBuffer<LogRecord> backtrace_buffer_;

    std::thread worker_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> shutting_down_{false};

    // Wakeup signaling for the otherwise lock-free queue's full/idle
    // transitions -- never guards queue contents, only used to avoid
    // sleep-based busy-polling on the producer (block policy) and
    // consumer (flush()) sides. Kept off the uncontended hot path via
    // the relaxed atomic pre-checks above.
    // Wakeup signaling for the otherwise lock-free queue's full/idle
    // transitions -- never guards queue contents, only used to avoid
    // sleep-based busy-polling on the producer (block policy) and
    // consumer (idle worker, flush()) sides. Kept off the uncontended
    // hot path via the relaxed atomic pre-checks above.
    std::mutex cv_mutex_;
    std::condition_variable not_full_cv_;   // producers wait here when the queue is full
    std::condition_variable not_empty_cv_;  // worker waits here when the queue is idle
    std::condition_variable drained_cv_;    // flush() waits here for approx_size_ == 0
    std::atomic<bool> worker_idle_{false};
    std::atomic<std::size_t> blocked_producers_{0};
    std::atomic<std::size_t> flush_waiters_{0};

    // Bounded spin budget in spin_for_work() before the worker commits to
    // a real sleep. Small enough to keep the worst-case genuinely-idle
    // wake latency negligible, large enough to absorb the gap between
    // back-to-back producer calls in bursty/tight-loop workloads.
    static constexpr int kIdleSpinIterations = 200;

public:
    std::size_t dropped_count_snapshot() const noexcept {
        return dropped_count_.load(std::memory_order_relaxed);
    }
    std::size_t sink_error_count_snapshot() const noexcept {
        return sink_error_count_.load(std::memory_order_relaxed);
    }
};

} // namespace logpulsex
