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

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <utility>

#include "logpulsex/level.hpp"

#if defined(__cpp_lib_source_location)
#include <source_location>
#define LOGPULSEX_HAS_SOURCE_LOCATION 1
#endif

// Process ID capture. A process's PID never changes during its lifetime,
// so this is computed once (cached in a function-local static -- the
// same thread-safe "magic statics" technique used elsewhere in this
// library, e.g. Registry::instance()/default_logger()) rather than
// making a fresh getpid()-family call on every single log record.
#if defined(_WIN32)
#include <process.h> // _getpid() -- a lightweight MSVC/MinGW CRT function;
                     // deliberately avoids pulling in <windows.h> just for this.
#else
#include <unistd.h> // getpid()
#endif

// Native OS thread ID capture. std::thread::id (see LogRecord::thread_id
// below) is guaranteed unique among live threads but its printed form is
// implementation-defined and unrelated to the identifier the OS itself
// hands out -- the one shown by debuggers (gdb/lldb `info threads`),
// crash dumps/tombstones, and system tools (top -H, Task Manager,
// Activity Monitor). For an application driving hardware, where a fault
// (SIGSEGV, a wedged driver call, ...) is a real possibility, being able
// to match a log line to the exact TID named in a crash report is often
// the fastest way to find the responsible thread. A thread's native TID
// never changes for its lifetime, so -- like get_process_id() above --
// it's computed once and cached, here per-thread via thread_local.
#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetCurrentThreadId()
#elif defined(__APPLE__)
#include <pthread.h> // pthread_threadid_np() -- covers both macOS and iOS
#elif defined(__linux__) || defined(__ANDROID__)
#include <sys/syscall.h> // SYS_gettid
#endif

namespace logpulsex::detail {

inline std::uint64_t get_process_id() noexcept {
    static const std::uint64_t pid = [] {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(::_getpid());
#else
        return static_cast<std::uint64_t>(::getpid());
#endif
    }();
    return pid;
}

inline std::uint64_t get_native_thread_id() noexcept {
    thread_local const std::uint64_t tid = [] {
#if defined(_WIN32)
        return static_cast<std::uint64_t>(::GetCurrentThreadId());
#elif defined(__APPLE__)
        std::uint64_t t = 0;
        ::pthread_threadid_np(nullptr, &t);
        return t;
#elif defined(__ANDROID__)
        // Bionic exposes gettid() directly, unlike glibc historically.
        return static_cast<std::uint64_t>(::gettid());
#elif defined(__linux__)
        // glibc only grew a gettid() wrapper in 2.30; syscall() works on
        // every glibc/musl version.
        return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#else
        // No native TID API on this platform: fall back to a stable
        // per-thread hash. Not a real OS thread id, but still unique and
        // constant for the thread's lifetime, so callers get a usable
        // value everywhere rather than a platform-specific hole.
        return static_cast<std::uint64_t>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
    }();
    return tid;
}

} // namespace logpulsex::detail

namespace logpulsex {

// A single structured key/value field attached to a record, e.g. for
// JSON output: {"user_id": "1234", "request_id": "abc"}.
// Values are pre-stringified at the call site to keep LogRecord copyable
// and avoid storing type-erased data on the hot path.
struct Field {
    std::string key;
    std::string value;
};

// Everything the async pipeline needs to format and dispatch one log line.
// Deliberately a flat, trivially-movable struct — cheap to push through the
// queue. String fields use std::string (SSO covers most short messages
// without heap allocation).
struct LogRecord {
    Level level = Level::info;
    std::chrono::system_clock::time_point timestamp{};
    std::thread::id thread_id{};
    std::uint64_t native_thread_id = 0; // OS-native TID; see get_native_thread_id().
    std::uint64_t process_id = 0;
    std::string logger_name;
    std::string message;
    std::vector<Field> fields;

    // Source location — populated at the call site via macros.
    const char* file = "";
    int line = 0;
    const char* function = "";
};

} // namespace logpulsex
