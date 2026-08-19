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
#include <cstring>
#include <deque>
#include <string>

#include "logpulsex/sink.hpp"

// Platform socket layer. POSIX and Winsock share the vast majority of
// their API surface (socket/connect/send/select/getaddrinfo all have
// identical names and near-identical signatures on both), but differ in
// a handful of specific ways this file has to bridge:
//   - Handle type: POSIX uses a plain `int`, invalid = -1. Winsock uses
//     the unsigned type SOCKET, invalid = INVALID_SOCKET (NOT -1 bit for
//     bit once stored in an unsigned type) -- comparing a Winsock handle
//     against 0 the way POSIX code compares against -1 is a genuine bug,
//     not just a style choice, so this file uses a portable SocketHandle
//     alias + kInvalidSocket constant instead of raw `int`/`-1` anywhere
//     a handle is stored or compared.
//   - Library init: Winsock requires WSAStartup()/WSACleanup() before/
//     after any socket calls; POSIX has no equivalent.
//   - Non-blocking mode: fcntl(F_SETFL, O_NONBLOCK) on POSIX vs.
//     ioctlsocket(FIONBIO) on Windows.
//   - Error reporting: errno/EINPROGRESS/EAGAIN/EWOULDBLOCK on POSIX vs.
//     WSAGetLastError()/WSAEWOULDBLOCK on Windows (Windows has no
//     separate EINPROGRESS for connect(); WSAEWOULDBLOCK covers both).
//   - Close: close() on POSIX vs. closesocket() on Windows.
//   - MSG_NOSIGNAL (suppress SIGPIPE on send to a closed peer) is a
//     Linux-specific flag with no Windows equivalent -- Windows sockets
//     never raise SIGPIPE, so it's simply omitted there.
#if defined(_WIN32)
// NOMINMAX prevents <windows.h> from defining min/max as macros, which
// would otherwise break std::numeric_limits<T>::min()/max() calls
// elsewhere in the translation unit -- see the fuller explanation in
// socket_compat.hpp. Call sites that need min()/max() also use the
// extra-parens workaround as the fully portable defense, since this
// define can't help a consumer who already included <windows.h> earlier
// in the same file without it.
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define LOGPULSEX_HAVE_WINSOCK 1
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define LOGPULSEX_HAVE_POSIX_SOCKETS 1
#endif

namespace logpulsex {

#if defined(LOGPULSEX_HAVE_WINSOCK)
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// Sends each formatted record as a newline-delimited line over TCP to a
// remote log collector (e.g. Logstash, Fluentd, a custom ingester).
// Fully functional on both POSIX and Windows (Winsock) -- not a
// documented no-op on either platform.
//
// Reliability notes — network sinks are the riskiest sink category
// because the network WILL fail intermittently, and a naive
// implementation can either (a) block the single worker thread for the
// OS's TCP connect/send timeout, stalling every other sink behind it, or
// (b) throw and silently stop logging. This implementation:
//  - Never blocks longer than a short, explicit timeout on connect/send
//    (using non-blocking sockets + select(), which both POSIX and
//    Winsock support with the same signature).
//  - On any failure, drops the connection and retries lazily on the next
//    write() rather than retrying synchronously in a loop.
//  - Bounds its local retry backlog (`max_buffered_lines`) so a sustained
//    outage cannot grow memory without limit — oldest buffered lines are
//    dropped first, mirroring the queue's own backpressure philosophy.
//  - All socket operations are wrapped; nothing here can throw out of
//    write()/flush(), matching every other sink's contract with the
//    worker thread.
//
// Security note: this sink sends plaintext over a raw TCP socket with no
// TLS. Treat it as suitable for a trusted internal network only (e.g.
// container-to-sidecar, same-VPC log collector) — do not point it at a
// collector over an untrusted network without putting it behind a TLS
// tunnel (stunnel, a service mesh sidecar, etc.), since log content may
// include sensitive data.
class NetworkSink final : public ISink {
public:
    NetworkSink(std::string host, int port,
                std::size_t max_buffered_lines = 1000,
                int connect_timeout_ms = 200,
                int send_timeout_ms = 100)
        : host_(std::move(host)),
          port_(port),
          max_buffered_lines_(max_buffered_lines),
          connect_timeout_ms_(connect_timeout_ms),
          send_timeout_ms_(send_timeout_ms) {
#if defined(LOGPULSEX_HAVE_WINSOCK)
        // WSAStartup/WSACleanup are refcounted per-process by the OS, so
        // it's correct (and the standard pattern) for each socket-using
        // object to call WSAStartup in its constructor and WSACleanup in
        // its destructor rather than requiring a single global call --
        // the OS balances nested calls internally.
        WSADATA wsa_data;
        wsa_startup_ok_ = (::WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0);
#endif
    }

    ~NetworkSink() override {
        close_socket();
#if defined(LOGPULSEX_HAVE_WINSOCK)
        if (wsa_startup_ok_) ::WSACleanup();
#endif
    }

    void write(const LogRecord& record) override {
        std::string line = format(record);
        line += '\n';

        if (fd_ == kInvalidSocket && !try_connect()) {
            buffer_line(std::move(line));
            return;
        }

        // Drain any backlog from a prior outage before sending the new
        // line, so ordering is preserved as much as practical.
        while (!backlog_.empty()) {
            if (!send_line(backlog_.front())) {
                buffer_line(std::move(line));
                return;
            }
            backlog_.pop_front();
        }

        if (!send_line(line)) {
            buffer_line(std::move(line));
        }
    }

    void flush() override {
        // Best-effort: try once to drain the backlog now. If the peer is
        // still unreachable, the lines stay buffered (bounded) for the
        // next write()/flush() to retry — flush() must not block
        // indefinitely waiting for a network peer.
        if (fd_ == kInvalidSocket) {
            try_connect();
        }
        while (fd_ != kInvalidSocket && !backlog_.empty()) {
            if (!send_line(backlog_.front())) break;
            backlog_.pop_front();
        }
    }

    // Test/inspection hooks.
    bool is_connected() const noexcept { return fd_ != kInvalidSocket; }
    std::size_t backlog_size() const noexcept { return backlog_.size(); }
    std::size_t dropped_count() const noexcept { return dropped_.load(std::memory_order_relaxed); }

private:
    void buffer_line(std::string line) {
        if (backlog_.size() >= max_buffered_lines_) {
            backlog_.pop_front(); // drop oldest to bound memory
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        backlog_.push_back(std::move(line));
    }

    SocketHandle fd_ = kInvalidSocket;
#if defined(LOGPULSEX_HAVE_WINSOCK)
    bool wsa_startup_ok_ = false;
#endif

    bool try_connect() {
#if defined(LOGPULSEX_HAVE_WINSOCK)
        if (!wsa_startup_ok_) return false;
#endif
        close_socket();

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* result = nullptr;
        std::string port_str = std::to_string(port_);
        if (::getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result) != 0) {
            return false;
        }

        bool connected = false;
        for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
            SocketHandle fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (fd == kInvalidSocket) continue;

            set_nonblocking(fd);
            // connect()'s third parameter type genuinely differs by
            // platform: Winsock declares it `int`, POSIX declares it
            // `socklen_t` (unsigned). Casting unconditionally to `int`
            // was correct for Windows but silently mismatched the POSIX
            // signature, producing a real -Wsign-conversion warning
            // (int -> socklen_t) — caught in our own build despite this
            // code targeting Windows compatibility, since we compile the
            // POSIX branch on every regular Linux build. Cast to
            // whichever type each platform's connect() actually expects.
#if defined(LOGPULSEX_HAVE_WINSOCK)
            int rc = ::connect(fd, rp->ai_addr, static_cast<int>(rp->ai_addrlen));
#else
            int rc = ::connect(fd, rp->ai_addr, static_cast<socklen_t>(rp->ai_addrlen));
#endif
            if (rc == 0) {
                connected = true;
            } else if (connect_in_progress()) {
                fd_set write_set;
                FD_ZERO(&write_set);
                FD_SET(fd, &write_set);
                struct timeval tv{};
                tv.tv_sec = connect_timeout_ms_ / 1000;
                tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;
#if defined(LOGPULSEX_HAVE_WINSOCK)
                int sel = ::select(0, nullptr, &write_set, nullptr, &tv);
#else
                int sel = ::select(fd + 1, nullptr, &write_set, nullptr, &tv);
#endif
                if (sel > 0) {
                    int so_error = 0;
#if defined(LOGPULSEX_HAVE_WINSOCK)
                    int len = static_cast<int>(sizeof(so_error));
                    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
#else
                    socklen_t len = static_cast<socklen_t>(sizeof(so_error));
                    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
#endif
                    connected = (so_error == 0);
                }
            }

            if (connected) {
                fd_ = fd;
                break;
            }
            close_handle(fd);
        }
        ::freeaddrinfo(result);
        return connected;
    }

    bool send_line(const std::string& line) {
        if (fd_ == kInvalidSocket) return false;

        std::size_t sent = 0;
        while (sent < line.size()) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(fd_, &write_set);
            struct timeval tv{};
            tv.tv_sec = send_timeout_ms_ / 1000;
            tv.tv_usec = (send_timeout_ms_ % 1000) * 1000;
#if defined(LOGPULSEX_HAVE_WINSOCK)
            int sel = ::select(0, nullptr, &write_set, nullptr, &tv);
#else
            int sel = ::select(fd_ + 1, nullptr, &write_set, nullptr, &tv);
#endif
            if (sel <= 0) {
                close_socket();
                return false;
            }
#if defined(LOGPULSEX_HAVE_WINSOCK)
            int n = ::send(fd_, line.data() + sent, static_cast<int>(line.size() - sent), 0);
            if (n == SOCKET_ERROR) {
                int err = ::WSAGetLastError();
                if (err == WSAEWOULDBLOCK) continue;
                close_socket();
                return false;
            }
#else
            ssize_t n = ::send(fd_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                close_socket();
                return false;
            }
#endif
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    static bool connect_in_progress() {
#if defined(LOGPULSEX_HAVE_WINSOCK)
        return ::WSAGetLastError() == WSAEWOULDBLOCK;
#else
        return errno == EINPROGRESS;
#endif
    }

    static void set_nonblocking(SocketHandle fd) {
#if defined(LOGPULSEX_HAVE_WINSOCK)
        u_long mode = 1;
        ::ioctlsocket(fd, FIONBIO, &mode);
#else
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    static void close_handle(SocketHandle fd) {
#if defined(LOGPULSEX_HAVE_WINSOCK)
        ::closesocket(fd);
#else
        ::close(fd);
#endif
    }

    void close_socket() {
        if (fd_ != kInvalidSocket) {
            close_handle(fd_);
            fd_ = kInvalidSocket;
        }
    }

    std::string host_;
    int port_;
    std::size_t max_buffered_lines_;
    int connect_timeout_ms_;
    int send_timeout_ms_;
    std::deque<std::string> backlog_;
    std::atomic<std::size_t> dropped_{0};
};

} // namespace logpulsex
