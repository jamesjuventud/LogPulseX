// Copyright 2026-Present James Bryan B. Juventud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Description: A header-only C++20 logging library:
// async, lock-free on the hot path, with console,
// size-based rotating file, daily rotating file,
// syslog, and TCP network sinks, structured (JSON)
// logging, crash-safe flushing, hex dumping,
// and file compression.

#pragma once

#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

#include "logpulsex/gzip_compress.hpp"
#include "logpulsex/sink.hpp"

namespace logpulsex {

// Time-based rotating file sink: rotates once per day at a configured
// local hour:minute, writing to a date-stamped file, e.g.
// "app_2026-07-20.log" -> "app_2026-07-21.log" at midnight (or whatever
// rotation time you configure).
//
// Reliability / security notes mirror RotatingFileSink:
//  - Directory existence validated at construction (fail fast).
//  - Only ever called from the single logging worker thread — no
//    internal locking needed.
//  - A write failure self-heals by attempting one reopen; if that also
//    fails, the record is dropped rather than throwing out of the
//    worker thread (which would silence every other sink too).
//
// Optional post-rotation compression: pass compress_after_rotation=true
// to gzip each closed day's file (e.g. "app_2026-07-20.log" ->
// "app_2026-07-20.log.gz") on a background thread once it's no longer
// being written to, removing the uncompressed original once compression
// succeeds. Requires this library to be built with zlib support
// (LOGPULSEX_HAVE_ZLIB) -- requesting it without that throws
// std::invalid_argument immediately at construction, rather than
// silently not compressing. Compression never blocks the logging worker
// thread: it runs via BackgroundCompressor (see gzip_compress.hpp),
// which the destructor waits on so the process can't exit -- or this
// sink be destroyed -- while a compression job is still writing.
//
// Testability: the time source is injectable via the `Clock` parameter
// (defaults to std::chrono::system_clock::now). Tests use this to
// deterministically simulate crossing a rotation boundary instead of
// waiting on a wall-clock day change.
class DailyFileSink final : public ISink {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    // `max_files == 0` means keep every day's file forever (no pruning).
    DailyFileSink(std::filesystem::path base_path, int rotation_hour, int rotation_minute,
                  std::size_t max_files = 0, bool compress_after_rotation = false,
                  Clock clock = default_clock())
        : base_path_(std::move(base_path)),
          rotation_hour_(rotation_hour),
          rotation_minute_(rotation_minute),
          max_files_(max_files),
          clock_(std::move(clock)),
          compress_after_rotation_(compress_after_rotation) {
        if (rotation_hour_ < 0 || rotation_hour_ > 23) {
            throw std::invalid_argument("DailyFileSink: rotation_hour must be 0-23");
        }
        if (rotation_minute_ < 0 || rotation_minute_ > 59) {
            throw std::invalid_argument("DailyFileSink: rotation_minute must be 0-59");
        }
        if (compress_after_rotation_ && !gzip_compression_available()) {
            throw std::invalid_argument(
                "DailyFileSink: compress_after_rotation requested but this library was "
                "built without zlib support -- rebuild with LOGPULSEX_HAVE_ZLIB defined "
                "and link zlib (CMake: set LOGPULSEX_ENABLE_GZIP ON)");
        }
        std::error_code ec;
        auto parent = base_path_.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
            throw std::invalid_argument(
                "DailyFileSink: directory does not exist: " + parent.string());
        }

        auto now = clock_();
        next_rotation_ = compute_next_rotation(now);
        open_file_for(now);
    }

    void write(const LogRecord& record) override {
        auto now = clock_();
        if (now >= next_rotation_) {
            rotate(now);
        }
        if (!stream_.is_open()) {
            open_file_for(now); // self-heal attempt
            if (!stream_.is_open()) return;
        }
        stream_ << format(record) << '\n';
        if (stream_.fail()) {
            stream_.close();
            stream_.clear();
        }
    }

    void flush() override {
        if (stream_.is_open()) stream_.flush();
    }

    // Exposed for tests: what the next scheduled rotation time is.
    std::chrono::system_clock::time_point next_rotation_time() const { return next_rotation_; }

    // Exposed for tests: the path currently being written to.
    const std::filesystem::path& current_path() const { return current_path_; }

    // Pure function of (now, configured rotation time) — public so tests
    // can verify the rollover-scheduling logic directly without needing a
    // real filesystem or waiting on a real clock.
    std::chrono::system_clock::time_point compute_next_rotation(
        std::chrono::system_clock::time_point now) const {
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        tm_buf.tm_hour = rotation_hour_;
        tm_buf.tm_min = rotation_minute_;
        tm_buf.tm_sec = 0;
        std::time_t candidate = std::mktime(&tm_buf);
        auto candidate_tp = std::chrono::system_clock::from_time_t(candidate);
        if (candidate_tp <= now) {
            candidate_tp += std::chrono::hours(24);
        }
        return candidate_tp;
    }

    static std::string date_stamp(std::chrono::system_clock::time_point tp) {
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        std::ostringstream ss;
        ss << (tm_buf.tm_year + 1900) << '-'
           << (tm_buf.tm_mon + 1 < 10 ? "0" : "") << (tm_buf.tm_mon + 1) << '-'
           << (tm_buf.tm_mday < 10 ? "0" : "") << tm_buf.tm_mday;
        return ss.str();
    }

    // Exposed for tests: blocks until any in-flight background
    // compression job has finished, and reports how many were
    // outstanding at the time of the call.
    void wait_for_compression() { compressor_.wait_all(); }
    std::size_t outstanding_compression_jobs() const { return compressor_.outstanding_count(); }

private:
    static Clock default_clock() {
        return [] { return std::chrono::system_clock::now(); };
    }

    std::filesystem::path path_for_date(const std::string& date) const {
        auto stem = base_path_.stem().string();
        auto ext = base_path_.extension().string();
        auto dir = base_path_.parent_path();
        std::filesystem::path name = stem + "_" + date + ext;
        return dir.empty() ? name : dir / name;
    }

    void open_file_for(std::chrono::system_clock::time_point now) {
        current_path_ = path_for_date(date_stamp(now));
        stream_.open(current_path_, std::ios::app | std::ios::binary);
        history_.push_back(current_path_);
        prune_if_needed();
    }

    void rotate(std::chrono::system_clock::time_point now) {
        flush();
        stream_.close();
        if (compress_after_rotation_) {
            // Capture the path BEFORE open_file_for() overwrites
            // current_path_ with the new day's file. Compression runs on
            // a background thread (never this worker thread) and
            // removes the original only on success -- a failed
            // compression leaves the plain .log file in place rather
            // than losing data.
            compressor_.compress_and_remove_async(current_path_);
        }
        next_rotation_ = compute_next_rotation(now);
        open_file_for(now);
    }

    void prune_if_needed() {
        if (max_files_ == 0) return;
        std::error_code ec;
        while (history_.size() > max_files_) {
            // Best-effort remove both possible names: compression is
            // asynchronous relative to pruning, so by the time a file
            // ages out of retention it may still be the plain .log (if
            // compression is disabled, still in flight, or failed) or
            // may already be the .gz version. Trying both is simpler and
            // more robust than attempting to synchronize with
            // BackgroundCompressor's in-flight jobs -- filesystem::remove
            // on a path that doesn't exist is just a harmless no-op.
            std::filesystem::remove(history_.front(), ec);
            std::filesystem::path gz_variant = history_.front();
            gz_variant += ".gz";
            std::filesystem::remove(gz_variant, ec);
            history_.pop_front();
        }
    }

    std::filesystem::path base_path_;
    int rotation_hour_;
    int rotation_minute_;
    std::size_t max_files_;
    Clock clock_;
    bool compress_after_rotation_;

    std::chrono::system_clock::time_point next_rotation_;
    std::filesystem::path current_path_;
    std::ofstream stream_;
    std::deque<std::filesystem::path> history_;
    BackgroundCompressor compressor_;
};

} // namespace logpulsex
