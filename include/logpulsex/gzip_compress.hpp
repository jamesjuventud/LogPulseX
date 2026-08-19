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

// Optional gzip compression support via zlib. Gated behind
// LOGPULSEX_HAVE_ZLIB, which must be defined explicitly (e.g. via a
// build flag) AND requires linking zlib (-lz) -- this library has no
// zlib dependency unless you opt in, matching its existing pattern for
// other optional platform-specific features (syslog, sockets).
//
// CMake: set(LOGPULSEX_ENABLE_GZIP ON) enables this automatically (finds
// zlib, defines the macro, links it). Building without CMake: compile
// with -DLOGPULSEX_HAVE_ZLIB and link -lz yourself.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <system_error>
#include <vector>

#if defined(LOGPULSEX_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace logpulsex {

// True if this build was compiled with zlib support. Check this before
// requesting compression at runtime if you want to detect availability
// yourself; sinks that support compression also enforce this by
// throwing std::invalid_argument at construction time if compression is
// requested without it, rather than silently not compressing.
constexpr bool gzip_compression_available() noexcept {
#if defined(LOGPULSEX_HAVE_ZLIB)
    return true;
#else
    return false;
#endif
}

namespace detail {

#if defined(LOGPULSEX_HAVE_ZLIB)

// Compresses `input` to a new gzip file at `output`. Never throws --
// returns false on any failure (missing input, write error, disk full,
// etc), the same "never crash the calling thread" contract every sink's
// write() path already follows elsewhere in this library -- this
// matters doubly here since it typically runs on a detached background
// thread with no caller directly watching for an exception. On failure,
// best-effort removes any partial/corrupt `output` it may have started
// writing; never touches `input` either way (the caller decides whether
// and when removing the original is safe).
inline bool gzip_compress_file(const std::filesystem::path& input,
                                const std::filesystem::path& output) noexcept {
    try {
        std::ifstream in(input, std::ios::binary);
        if (!in.is_open()) return false;

#if defined(_WIN32)
        gzFile out = gzopen_w(output.wstring().c_str(), "wb");
#else
        gzFile out = gzopen(output.string().c_str(), "wb");
#endif
        if (out == nullptr) return false;

        bool ok = true;
        char buffer[65536];
        while (ok && in.read(buffer, sizeof(buffer)).gcount() >= 0) {
            std::streamsize n = in.gcount();
            if (n > 0) {
                if (gzwrite(out, buffer, static_cast<unsigned int>(n)) != static_cast<int>(n)) {
                    ok = false;
                }
            }
            if (in.eof()) break;
            if (in.bad()) { ok = false; break; }
        }

        if (gzclose(out) != Z_OK) ok = false;

        if (!ok) {
            std::error_code ec;
            std::filesystem::remove(output, ec);
        }
        return ok;
    } catch (...) {
        return false;
    }
}

// Decompresses a gzip file back to plain content. Provided primarily so
// this library's own tests can round-trip-verify compressed output
// without depending on an external `gunzip`/`zcat` binary being present
// in the environment, but useful standalone for reading compressed logs
// back too. Same never-throws contract as gzip_compress_file().
inline bool gzip_decompress_file(const std::filesystem::path& input,
                                  const std::filesystem::path& output) noexcept {
    try {
#if defined(_WIN32)
        gzFile in = gzopen_w(input.wstring().c_str(), "rb");
#else
        gzFile in = gzopen(input.string().c_str(), "rb");
#endif
        if (in == nullptr) return false;

        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            gzclose(in);
            return false;
        }

        bool ok = true;
        char buffer[65536];
        int n;
        while ((n = gzread(in, buffer, sizeof(buffer))) > 0) {
            out.write(buffer, n);
            if (!out) { ok = false; break; }
        }
        if (n < 0) ok = false; // gzread error

        gzclose(in);
        return ok;
    } catch (...) {
        return false;
    }
}

#endif // LOGPULSEX_HAVE_ZLIB

} // namespace detail

// Manages background gzip-compression jobs for a file sink: spawns each
// job on a real OS thread via std::async (never the sink's own worker
// thread, so a large file being compressed never blocks subsequent log
// writes), and lets the owning sink wait for all outstanding jobs at
// shutdown. std::future (not raw std::thread) is used specifically so
// "has this job finished yet" is a simple non-blocking
// wait_for(0s) == future_status::ready check for opportunistic reaping,
// and waiting at shutdown is just future::wait() rather than needing a
// separate hand-rolled done-flag protocol.
//
// This class's public interface always compiles, regardless of whether
// LOGPULSEX_HAVE_ZLIB is defined, so sinks that hold one as a member
// never need their own conditional compilation for the common case --
// only the method bodies are gated. Without zlib, compress_and_remove_
// async() is an intentional, safe no-op: sinks enforce that this is
// never actually reachable by throwing at construction time if
// compression is requested without zlib support compiled in.
class BackgroundCompressor {
public:
    ~BackgroundCompressor() { wait_all(); }

    // Compresses `input` to `input` + ".gz" on a background thread, then
    // removes `input` on success. Never removes `input` on failure --
    // data safety first: a leftover uncompressed file is far better than
    // silently losing log data because compression failed partway.
    void compress_and_remove_async(std::filesystem::path input) {
#if defined(LOGPULSEX_HAVE_ZLIB)
        reap_finished();
        std::filesystem::path output = input;
        output += ".gz";
        jobs_.push_back(std::async(std::launch::async, [input, output] {
            if (detail::gzip_compress_file(input, output)) {
                std::error_code ec;
                std::filesystem::remove(input, ec); // best-effort
            }
        }));
#else
        (void)input; // unreachable in practice; see class doc comment
#endif
    }

    // Blocks until every outstanding compression job has finished.
    // Called automatically from the destructor; also safe to call
    // explicitly (e.g. before a test asserts a .gz file now exists).
    void wait_all() {
#if defined(LOGPULSEX_HAVE_ZLIB)
        for (auto& job : jobs_) {
            if (job.valid()) job.wait();
        }
        jobs_.clear();
#endif
    }

    std::size_t outstanding_count() const noexcept {
#if defined(LOGPULSEX_HAVE_ZLIB)
        return jobs_.size();
#else
        return 0;
#endif
    }

private:
#if defined(LOGPULSEX_HAVE_ZLIB)
    void reap_finished() {
        jobs_.erase(
            std::remove_if(jobs_.begin(), jobs_.end(), [](std::future<void>& f) {
                return !f.valid() ||
                       f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
            }),
            jobs_.end());
    }

    std::vector<std::future<void>> jobs_;
#endif
};

} // namespace logpulsex
