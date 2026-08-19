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
#include <iomanip>
#include <sstream>
#include <string>

#include "logpulsex/log_record.hpp"

namespace logpulsex {

class IFormatter {
public:
    virtual ~IFormatter() = default;
    virtual std::string format(const LogRecord& record) const = 0;
};

namespace detail {

// Formats an ISO-8601-ish local timestamp with millisecond precision.
// Uses the thread-safe *_r/_s variants of localtime, never the bare
// localtime() (which returns a pointer to shared static storage and is a
// well-known source of data races in multi-threaded logging code).
inline std::string format_timestamp(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(tp);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream out;
    out << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    out << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return out.str();
}

// Neutralizes characters that would let a caller who controls the log
// message (e.g. echoing untrusted user input) forge extra log lines or
// break structured output — a class of vulnerability generally referred
// to as log injection / log forging (CWE-117). We escape control
// characters rather than silently dropping them so no information is
// lost and the escaping is unambiguous to reverse.
inline void append_escaped_plain(std::string& out, std::string_view text) {
    for (char raw_c : text) {
        unsigned char c = static_cast<unsigned char>(raw_c);
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

inline void append_escaped_json(std::string& out, std::string_view text) {
    for (char raw_c : text) {
        unsigned char c = static_cast<unsigned char>(raw_c);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

// Reused across records on the worker thread (the only thread that ever
// calls a formatter -- see ISink's doc comment), avoiding a fresh
// ostringstream construction/destruction per record purely to render a
// std::thread::id, whose printed form has no direct string conversion.
inline std::string stringify_thread_id(std::thread::id id) {
    thread_local std::ostringstream ss;
    ss.str(std::string());
    ss.clear();
    ss << id;
    return ss.str();
}

} // namespace detail

// Human-readable "%time% [%level%] logger: message" style output.
// The pattern itself is fixed at construction time (not attacker-
// controlled at the call site), so there is no injection risk from the
// pattern; message content is still escaped since it may originate from
// untrusted input the application chose to log.
class PatternFormatter final : public IFormatter {
public:
    explicit PatternFormatter(std::string pattern = "{time} [{level}] [pid:{pid}] [tid:{thread}] [ntid:{native_tid}] {logger}: {message}")
        : pattern_(std::move(pattern)) {}

    std::string format(const LogRecord& record) const override {
        std::string out;
        out.reserve(record.message.size() + 64);

        std::string_view p = pattern_;
        while (!p.empty()) {
            auto pos = p.find('{');
            if (pos == std::string_view::npos) {
                out.append(p);
                break;
            }
            out.append(p.substr(0, pos));
            auto end = p.find('}', pos);
            if (end == std::string_view::npos) {
                out.append(p.substr(pos));
                break;
            }
            std::string_view token = p.substr(pos + 1, end - pos - 1);
            append_token(out, token, record);
            p.remove_prefix(end + 1);
        }
        return out;
    }

private:
    static void append_token(std::string& out, std::string_view token, const LogRecord& r) {
        if (token == "time") {
            out += detail::format_timestamp(r.timestamp);
        } else if (token == "level") {
            out += to_string(r.level);
        } else if (token == "logger") {
            out += r.logger_name;
        } else if (token == "thread") {
            out += detail::stringify_thread_id(r.thread_id);
        } else if (token == "native_tid") {
            out += std::to_string(r.native_thread_id);
        } else if (token == "pid") {
            out += std::to_string(r.process_id);
        } else if (token == "file") {
            out += r.file;
        } else if (token == "line") {
            out += std::to_string(r.line);
        } else if (token == "function") {
            out += r.function;
        } else if (token == "message") {
            if (r.level == Level::raw) {
                // Level::raw is a deliberate, narrow trust boundary: see
                // the doc comment on Level::raw in level.hpp and on
                // format_hex_dump() in hex.hpp. Pre-formatted multi-line
                // content (hex dumps) is emitted verbatim here so its
                // intended row layout survives, rather than being
                // escaped into one line of literal "\n" text the way
                // every other level's messages safely are.
                out += r.message;
            } else {
                detail::append_escaped_plain(out, r.message);
            }
        } else if (token == "fields") {
            bool first = true;
            for (const auto& f : r.fields) {
                if (!first) out += ' ';
                first = false;
                out += f.key;
                out += '=';
                detail::append_escaped_plain(out, f.value);
            }
        } else {
            // Unknown token: emit verbatim rather than throwing, so a typo
            // in a pattern string never takes down a running application.
            out += '{';
            out += token;
            out += '}';
        }
    }

    std::string pattern_;
};

// Structured JSON-lines output, suitable for ingestion by log pipelines
// (ELK, Loki, CloudWatch, etc). One self-contained JSON object per line.
//
// Unlike PatternFormatter, this formatter does NOT exempt Level::raw
// from escaping: a raw, unescaped newline embedded in a JSON string
// value would produce invalid JSON regardless of trust level, so
// message content is always escaped here. The `"level":"RAW"` field
// this produces for hex-dump records is deliberately still emitted
// (via the same to_string(record.level) used for every other level)
// so a downstream viewer can detect raw-level records and re-expand
// the escaped "\n" sequences back into real line breaks when rendering,
// without this formatter ever needing to compromise JSON validity to
// achieve that.
class JsonFormatter final : public IFormatter {
public:
    std::string format(const LogRecord& record) const override {
        std::string out;
        out.reserve(record.message.size() + 128);
        out += "{\"time\":\"";
        out += detail::format_timestamp(record.timestamp);
        out += "\",\"level\":\"";
        out += to_string(record.level);
        out += "\",\"pid\":";
        out += std::to_string(record.process_id);
        out += ",\"tid\":\"";
        detail::append_escaped_json(out, detail::stringify_thread_id(record.thread_id));
        out += "\",\"native_tid\":";
        out += std::to_string(record.native_thread_id);
        out += ",\"logger\":\"";
        detail::append_escaped_json(out, record.logger_name);
        out += "\",\"message\":\"";
        detail::append_escaped_json(out, record.message);
        out += "\",\"file\":\"";
        detail::append_escaped_json(out, record.file);
        out += "\",\"line\":";
        out += std::to_string(record.line);
        for (const auto& field : record.fields) {
            out += ",\"";
            detail::append_escaped_json(out, field.key);
            out += "\":\"";
            detail::append_escaped_json(out, field.value);
            out += "\"";
        }
        out += "}";
        return out;
    }
};

} // namespace logpulsex
