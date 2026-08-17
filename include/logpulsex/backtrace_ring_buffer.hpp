#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <vector>

#include "logpulsex/internal_lock.hpp"

namespace logpulsex::detail {

// Fixed-capacity circular buffer that always accepts a new element,
// silently overwriting the oldest one once full. Backs Logger's
// backtrace feature (see enable_backtrace()/dump_backtrace() in
// logger.hpp): keep the last N records around in memory, at whatever
// verbosity you choose -- independent of the runtime severity filter --
// so they can be replayed to sinks after something goes wrong, without
// paying sink I/O cost for every one of them up front.
//
// std::timed_mutex (not a lock-free structure) is a deliberate,
// conservative choice: this buffer is only ever touched when the
// backtrace feature is explicitly enabled, contention is expected to be
// low relative to the main logging queue, and the simplicity directly
// reduces the surface for a subtle concurrency bug in a facility whose
// entire purpose is to be trustworthy after something has already gone
// wrong. It participates in the same internal-lock reentrancy tracking
// used by Logger/Registry (see internal_lock.hpp) so a crash-handler-
// driven dump can detect and skip a self-owned lock instead of
// deadlocking on it.
template <typename T>
class RingBuffer {
public:
    RingBuffer() = default;

    // Not safe to call concurrently with itself or clear() on the same
    // instance -- only ever called from enable_backtrace(), which (like
    // Logger::set_level()/add_sink()) is not expected to race with
    // itself under normal use.
    void reset(std::size_t new_capacity) {
        InternalMutexGuard lock(mutex_);
        capacity_ = new_capacity;
        items_.clear();
        items_.reserve(capacity_);
        next_ = 0;
    }

    void clear() {
        InternalMutexGuard lock(mutex_);
        items_.clear();
        next_ = 0;
    }

    // Thread-safe: any number of producer threads may call this
    // concurrently.
    void push(T item) {
        InternalMutexGuard lock(mutex_);
        if (capacity_ == 0) return; // disabled (or racing a concurrent reset(0))
        if (items_.size() < capacity_) {
            items_.push_back(std::move(item));
        } else {
            items_[next_] = std::move(item);
        }
        next_ = (next_ + 1) % capacity_;
    }

    // Copies out the current contents in insertion (oldest-to-newest)
    // order. A snapshot copy -- rather than invoking a callback while
    // holding the lock -- keeps the mutex held for a short, bounded time
    // and avoids calling back into sink I/O (which can block or throw)
    // from inside the lock. Blocks if another thread holds the lock; see
    // try_snapshot() for the bounded, crash-handler-safe equivalent.
    std::vector<T> snapshot() const {
        InternalMutexGuard lock(mutex_);
        return snapshot_locked();
    }

    // Bounded counterpart to snapshot(), for use from a crash handler:
    // never blocks longer than `timeout`, and never attempts to lock a
    // mutex this exact thread already holds (returns false immediately in
    // that case -- see internal_lock.hpp for why that check has to come
    // first, ahead of even trying try_lock_for()).
    bool try_snapshot(std::chrono::milliseconds timeout, std::vector<T>& out) const {
        if (thread_holds_internal_lock()) return false;
        if (!mutex_.try_lock_for(timeout)) return false;
        std::lock_guard<std::timed_mutex> lock(mutex_, std::adopt_lock);
        LockDepthScope depth_scope;
        out = snapshot_locked();
        return true;
    }

    std::size_t capacity() const {
        InternalMutexGuard lock(mutex_);
        return capacity_;
    }

private:
    std::vector<T> snapshot_locked() const {
        std::vector<T> out;
        out.reserve(items_.size());
        if (items_.size() < capacity_) {
            out = items_; // never wrapped yet: already oldest-to-newest
        } else {
            for (std::size_t i = 0; i < items_.size(); ++i) {
                out.push_back(items_[(next_ + i) % capacity_]);
            }
        }
        return out;
    }

    mutable std::timed_mutex mutex_;
    std::size_t capacity_ = 0;
    std::vector<T> items_;
    std::size_t next_ = 0;
};

} // namespace logpulsex::detail
