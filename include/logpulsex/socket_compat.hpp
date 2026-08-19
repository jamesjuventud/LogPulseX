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

// Cross-platform TCP socket primitives shared by NetworkSink and its
// tests. This is a REAL abstraction over POSIX sockets and Winsock2 —
// not a Windows no-op. An earlier version of this library left Windows
// support as a documented no-op stub in NetworkSink and, worse, left
// test_network_sink.cpp calling raw POSIX socket functions
// (::socket, ::connect, htons, inet_pton, AF_INET, SOCK_STREAM)
// completely unguarded, which failed to compile at all on MSVC. This
// header exists so there is exactly one place that knows the
// POSIX-vs-Winsock differences, used consistently by both the library
// and its tests.
//
// Windows linking note: Winsock functions require linking ws2_32.lib.
// CMakeLists.txt links this automatically on Windows targets. If you're
// building outside CMake (e.g. a raw cl.exe invocation), the #pragma
// comment(lib, "ws2_32.lib") below handles it for MSVC specifically;
// other Windows toolchains (MinGW) need `-lws2_32` passed explicitly.

#include <cstdint>
#include <string>

#if defined(_WIN32)
// NOMINMAX prevents <windows.h> (should anything pull it in transitively,
// or should this be the first Windows-family header included in a given
// translation unit) from defining min/max as function-like macros, which
// would otherwise break any std::numeric_limits<T>::min()/max() call
// elsewhere in that translation unit. This only helps when our headers
// are the first to reach a Windows header in a given .cpp file -- it
// can't retroactively fix a consumer who already included <windows.h>
// without NOMINMAX earlier in the same file, which is why call sites
// that need min()/max() also use the extra-parens workaround
// ((std::numeric_limits<T>::min)()) as the fully portable defense.
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
// winsock2.h must be included before any accidental <windows.h> include
// pulls in winsock.h (the old, incompatible Winsock 1.1 header) — since
// we never include <windows.h> ourselves, order here is what matters for
// consumers of this header.
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace logpulsex::net {

#if defined(_WIN32)
using SocketHandle = SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// RAII Winsock lifecycle management. WSAStartup must be called before
// any other Winsock function, and WSACleanup once the process is done
// with sockets. A function-local static (Meyer's singleton) means this
// runs exactly once, lazily, the first time any socket code path is
// reached — regardless of how many NetworkSink instances get created —
// and WSACleanup runs automatically at static-destruction time (process
// exit), matching the RAII/no-manual-lifecycle-management style used
// throughout the rest of this library. No-op on non-Windows platforms.
inline void ensure_socket_layer_initialized() {
#if defined(_WIN32)
    struct WinsockLifetime {
        WinsockLifetime() {
            WSADATA data;
            ::WSAStartup(MAKEWORD(2, 2), &data);
        }
        ~WinsockLifetime() { ::WSACleanup(); }
    };
    static WinsockLifetime lifetime;
    (void)lifetime;
#endif
}

inline void close_socket_handle(SocketHandle handle) {
    if (handle == kInvalidSocket) return;
#if defined(_WIN32)
    ::closesocket(handle);
#else
    ::close(handle);
#endif
}

inline void set_nonblocking(SocketHandle handle) {
#if defined(_WIN32)
    u_long mode = 1;
    ::ioctlsocket(handle, FIONBIO, &mode);
#else
    int flags = ::fcntl(handle, F_GETFL, 0);
    ::fcntl(handle, F_SETFL, flags | O_NONBLOCK);
#endif
}

inline int last_socket_error() {
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline bool error_is_in_progress(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK; // Windows reports non-blocking connect this way
#else
    return err == EINPROGRESS;
#endif
}

inline bool error_is_would_block(int err) {
#if defined(_WIN32)
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// send() with the "don't raise SIGPIPE on a broken connection" flag,
// where available. MSG_NOSIGNAL is Linux-specific; Windows has no
// equivalent need (TCP send on a broken socket just returns an error,
// no signal involved), so the flag is simply 0 there. macOS/BSD use a
// per-socket SO_NOSIGPIPE option instead, which NetworkSink doesn't
// currently set — acceptable since this sink already treats every send
// failure as "connection lost, buffer and retry," so an unhandled
// SIGPIPE terminating the process on those platforms is a real known
// gap, not silently swallowed. Tracked as a TODO rather than papered
// over.
inline int no_sigpipe_flag() {
#if defined(_WIN32) || defined(__APPLE__)
    return 0;
#elif defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

// Convenience helper: connect a blocking socket to host:port and send a
// single line. Used by tests to signal the throwaway TCP test server to
// shut down, without duplicating raw POSIX/Winsock socket code at each
// call site. Best-effort: failures are silently ignored (this is a test
// teardown helper, not production code — the server also self-times-out
// after 30s with no connection, so a failed shutdown signal just makes
// that particular test run slightly slower, never incorrect).
inline void send_line_fire_and_forget(const std::string& host, int port, const std::string& line) {
    ensure_socket_layer_initialized();

    SocketHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kInvalidSocket) return;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), static_cast<int>(sizeof(addr))) == 0) {
        // send()'s length parameter type differs by platform (int on
    // Windows, size_t on POSIX) -- same reasoning as the identical fix
    // in network_sink.hpp's try_connect().
#if defined(_WIN32)
    ::send(fd, line.data(), static_cast<int>(line.size()), no_sigpipe_flag());
#else
    ::send(fd, line.data(), line.size(), no_sigpipe_flag());
#endif
    }
    close_socket_handle(fd);
}

} // namespace logpulsex::net
