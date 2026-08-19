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
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <vector>

namespace logpulsex {

// Bounded multi-producer, single-consumer lock-free queue.
//
// This is Dmitry Vyukov's well-known bounded MPMC queue algorithm,
// restricted to a single consumer (which is all the logging pipeline
// needs: many application threads produce, one worker thread consumes).
// It is correct as a general MPMC queue too, but we only document/use the
// MPSC contract here to keep reasoning about it simple.
//
// Design properties relevant to a logging library:
//  - Bounded capacity: memory usage is fixed and known up front. Logging
//    can never cause unbounded memory growth, which matters under load or
//    log-flooding conditions (denial-of-service via memory exhaustion).
//  - Producers never block the OS scheduler on a mutex: push() is a small
//    number of atomic operations, keeping the hot path fast and making
//    behavior under contention predictable.
//  - Capacity must be a power of two (enforced in the constructor) so the
//    index mask is a cheap bitwise AND instead of a modulo.
template <typename T>
class BoundedMpscQueue {
public:
    explicit BoundedMpscQueue(std::size_t capacity)
        : capacity_(next_power_of_two(capacity)),
          mask_(capacity_ - 1),
          buffer_(capacity_) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            buffer_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    BoundedMpscQueue(const BoundedMpscQueue&) = delete;
    BoundedMpscQueue& operator=(const BoundedMpscQueue&) = delete;

    // Attempts to enqueue by copy/move. Returns false if the queue is full
    // (caller decides the backpressure policy — block, drop, retry, etc.).
    // Note: on failure this by-value overload's local `item` is simply
    // discarded (matches historical behavior) -- callers that need to
    // retry the *same* value after a failed attempt without risking data
    // loss on move-only/non-trivial T should use try_push_preserve()
    // instead, which never touches the caller's object unless the push
    // actually succeeds.
    bool try_push(T item) noexcept {
        return try_push_preserve(item);
    }

    // Like try_push(), but takes item by lvalue reference and only ever
    // moves from it once success is guaranteed. On failure (queue full),
    // `item` is left completely untouched, so a retry loop can safely
    // call this again with the same object -- unlike passing by value,
    // where the move into the (discarded) parameter happens unconditionally
    // at the call site regardless of the eventual outcome, silently
    // emptying non-trivial members (e.g. std::string/std::vector) even on
    // a failed attempt.
    bool try_push_preserve(T& item) noexcept {
        Cell* cell;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[pos & mask_];
            std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            std::intptr_t diff =
                static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
                // CAS failed: another producer won the slot, pos was
                // refreshed by compare_exchange_weak, retry.
            } else if (diff < 0) {
                return false; // queue full; item untouched
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->data = std::move(item);
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Single-consumer only. Returns std::nullopt if empty.
    std::optional<T> try_pop() noexcept {
        Cell* cell = &buffer_[dequeue_pos_ & mask_];
        std::size_t seq = cell->sequence.load(std::memory_order_acquire);
        std::intptr_t diff = static_cast<std::intptr_t>(seq) -
                              static_cast<std::intptr_t>(dequeue_pos_ + 1);
        if (diff != 0) {
            return std::nullopt; // empty
        }
        T result = std::move(cell->data);
        cell->sequence.store(dequeue_pos_ + mask_ + 1, std::memory_order_release);
        ++dequeue_pos_;
        return result;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    struct Cell {
        std::atomic<std::size_t> sequence;
        T data;
    };

    static std::size_t next_power_of_two(std::size_t n) {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p == 0 ? 1 : p;
    }

    static constexpr std::size_t hardware_destructive_interference_size = 64;

    const std::size_t capacity_;
    const std::size_t mask_;
    std::vector<Cell> buffer_;

    // Padding avoids false sharing between the producer-side and
    // consumer-side cursors, which live on different cache lines and are
    // written by different threads at high frequency.
    alignas(hardware_destructive_interference_size) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(hardware_destructive_interference_size) std::size_t dequeue_pos_{0};
};

} // namespace logpulsex
