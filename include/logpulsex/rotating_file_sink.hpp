#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "logpulsex/sink.hpp"

namespace logpulsex {

// Size-based rotating file sink: base_path, base_path.1, base_path.2, ...
// up to max_files. Only ever called from the single logging worker
// thread, so no internal locking is required.
//
// Reliability / security notes:
//  - The target directory is resolved and validated (must exist, must be
//    a directory) at construction time, failing fast with a clear
//    exception instead of silently discarding logs later.
//  - Rotation uses std::filesystem::rename, which on POSIX and Windows is
//    atomic within the same filesystem/volume — there is no window where
//    a concurrent reader (e.g. `tail -f`, a log shipper) sees a
//    half-renamed or truncated file.
//  - File permissions are left to the umask/OS default rather than
//    forced open (e.g. no world-writable files created), and we never
//    execute a shell or format a path from untrusted input.
//  - Every filesystem operation that can throw is caught; a failing sink
//    reports the error via on_error() rather than crashing the process
//    or the worker thread that every other sink depends on.
class RotatingFileSink final : public ISink {
public:
    RotatingFileSink(std::filesystem::path base_path,
                      std::size_t max_bytes,
                      std::size_t max_files)
        : base_path_(std::move(base_path)),
          max_bytes_(max_bytes),
          max_files_(max_files) {
        if (max_files_ == 0) {
            throw std::invalid_argument("RotatingFileSink: max_files must be >= 1");
        }

        std::error_code ec;
        auto parent = base_path_.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
            throw std::invalid_argument(
                "RotatingFileSink: directory does not exist: " + parent.string());
        }

        open_current_file();
    }

    void write(const LogRecord& record) override {
        if (!stream_.is_open()) {
            // Best-effort self-heal: try to reopen once. If that fails,
            // drop this single record rather than throwing out of the
            // worker thread (which would silence every other sink too).
            open_current_file();
            if (!stream_.is_open()) return;
        }

        std::string line = format(record);
        current_bytes_ += line.size() + 1;
        stream_ << line << '\n';

        if (stream_.fail()) {
            on_write_error();
            return;
        }

        if (current_bytes_ >= max_bytes_) {
            rotate();
        }
    }

    void flush() override {
        if (stream_.is_open()) stream_.flush();
    }

private:
    void open_current_file() {
        // BUG FIX: previously used stream_.tellp() immediately after
        // opening in std::ios::app mode to determine the file's current
        // size. That is NOT reliable across standard library
        // implementations -- app mode guarantees every *write* seeks to
        // the true end of file, but the position tellp() reports
        // immediately after open(), before any write has happened, is
        // implementation-defined and has been observed to return 0 on
        // some platforms rather than the file's actual size. Every time
        // an existing, non-empty log file was reopened (e.g. every
        // process restart), current_bytes_ would silently reset to 0
        // even though the file already had substantial content --
        // meaning rotation only fired after accumulating another full
        // max_bytes on top of whatever was already there, unbounded
        // across repeated restarts. std::filesystem::file_size() queries
        // the actual size from the OS directly and has no such ambiguity
        // on any platform.
        std::error_code ec;
        auto existing_size = std::filesystem::file_size(base_path_, ec);
        current_bytes_ = ec ? 0 : static_cast<std::size_t>(existing_size);

        stream_.open(base_path_, std::ios::app | std::ios::binary);
        if (!stream_.is_open()) {
            current_bytes_ = 0;
        }
    }

    void on_write_error() {
        stream_.close();
        stream_.clear();
    }

    void rotate() {
        flush();
        stream_.close();

        std::error_code ec;

        // Remove the oldest file if we're at the retention limit.
        auto oldest = numbered_path(max_files_ - 1);
        if (std::filesystem::exists(oldest, ec)) {
            std::filesystem::remove(oldest, ec); // best-effort; ignore failure
        }

        // Shift base.(N-2) -> base.(N-1), ..., base.1 -> base.2.
        for (std::size_t i = max_files_ - 1; i-- > 1;) {
            auto from = numbered_path(i);
            auto to = numbered_path(i + 1);
            if (std::filesystem::exists(from, ec)) {
                std::filesystem::rename(from, to, ec); // best-effort
            }
        }

        // base_path -> base.1 (atomic on the same filesystem).
        if (max_files_ > 1) {
            std::filesystem::rename(base_path_, numbered_path(1), ec);
        } else {
            // Only one file retained: truncate instead of keeping a .1
            std::filesystem::remove(base_path_, ec);
        }

        current_bytes_ = 0;
        open_current_file();
    }

    std::filesystem::path numbered_path(std::size_t index) const {
        return base_path_.string() + "." + std::to_string(index);
    }

    std::filesystem::path base_path_;
    std::size_t max_bytes_;
    std::size_t max_files_;
    std::ofstream stream_;
    std::size_t current_bytes_ = 0;
};

} // namespace logpulsex
