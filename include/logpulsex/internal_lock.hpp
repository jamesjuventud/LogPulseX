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

namespace logpulsex::detail {

// Tracks whether the *current* thread already holds one of this
// library's own internal mutexes (Logger::sinks_mutex_, Registry::mutex_,
// RingBuffer's own mutex, ...). A synchronous fault (SIGSEGV, ...) is
// delivered by the OS to the exact thread that caused it, so if that
// thread crashed while it happened to be inside one of these critical
// sections (e.g. the worker thread faulting inside a sink's write()
// call, which runs under sinks_mutex_), a crash handler that blindly
// re-locks the same mutex would self-deadlock -- undefined behavior for
// a non-recursive mutex, and just as undefined for
// try_lock()/try_lock_for() on an already-owned mutex, so this has to be
// tracked explicitly rather than probed for. The depth counter is
// incremented before lock() and decremented after unlock() (not the
// reverse) so the window where it reports "held" is always a superset
// of the real locked window -- conservative in both directions, never a
// false "safe to lock".
inline int& internal_lock_depth() {
    thread_local int depth = 0;
    return depth;
}

inline bool thread_holds_internal_lock() {
    return internal_lock_depth() > 0;
}

// Bumps the depth counter for a scope that already acquired one of these
// mutexes through some other means (e.g. a bounded try_lock_for(), which
// InternalMutexGuard below deliberately does not support -- its
// constructor always calls the blocking lock()). Keeps the "does this
// thread hold an internal lock" invariant accurate for those call sites
// too, without requiring every acquisition path to go through
// InternalMutexGuard itself.
class LockDepthScope {
public:
    LockDepthScope() { ++internal_lock_depth(); }
    ~LockDepthScope() { --internal_lock_depth(); }
    LockDepthScope(const LockDepthScope&) = delete;
    LockDepthScope& operator=(const LockDepthScope&) = delete;
};

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
