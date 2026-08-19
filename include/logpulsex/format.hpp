#pragma once

#include <cctype>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include "logpulsex/log_record.hpp"

// Container operator<< overloads MUST be visible before the format_into
// templates below are defined (two-phase name lookup) — see the
// include-order note at the top of container_format.hpp.
#include "logpulsex/container_format.hpp"

namespace logpulsex::detail {

// Security note: we deliberately do NOT expose a printf/sprintf-style
// "const char* fmt, ..." API. Passing user- or attacker-influenced data as
// a format string is a classic vulnerability class (CWE-134). Instead we
// use a small "{}"/"{:spec}"-placeholder formatter over variadic
// templates, so every argument is type-checked at compile time and
// streamed via operator<<, never interpreted as a format directive.
// Literal braces can be escaped as "{{" and "}}".

// ---------------------------------------------------------------------
// Format specs: {:[[fill]align][sign][#][0][width][.precision][type]}
// Subset of the {fmt}/Python-style mini-language. Examples:
//   "{:.2f}"   -> fixed, 2 decimal places
//   "{:5}"     -> right-padded to width 5 (left-padded for numbers)
//   "{:05}"    -> zero-padded to width 5
//   "{:x}"     -> hex
//   "{:>10}"   -> right-align in a field of width 10
//   "{:^8}"    -> center-align in a field of width 8
// An empty spec ("{}") is the plain substitution path used previously.
// ---------------------------------------------------------------------
struct FormatSpec {
    bool has_spec = false;
    char fill = ' ';
    char align = '\0';   // '<', '>', '^', or '\0' for type-default
    bool sign_plus = false;
    bool alt_form = false;
    bool zero_pad = false;
    int width = -1;
    int precision = -1;
    char type = '\0';    // 'f','e','g','x','X','o','d', or '\0' for default
};

inline FormatSpec parse_spec(std::string_view spec) {
    FormatSpec s;
    if (spec.empty()) return s;
    s.has_spec = true;

    std::size_t i = 0;
    if (spec.size() >= 2 &&
        (spec[1] == '<' || spec[1] == '>' || spec[1] == '^')) {
        s.fill = spec[0];
        s.align = spec[1];
        i = 2;
    } else if (spec[0] == '<' || spec[0] == '>' || spec[0] == '^') {
        s.align = spec[0];
        i = 1;
    }
    if (i < spec.size() && spec[i] == '+') { s.sign_plus = true; ++i; }
    if (i < spec.size() && spec[i] == '#') { s.alt_form = true; ++i; }
    if (i < spec.size() && spec[i] == '0') { s.zero_pad = true; ++i; }

    std::size_t width_start = i;
    while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) ++i;
    if (i > width_start) {
        std::from_chars(spec.data() + width_start, spec.data() + i, s.width);
    }

    if (i < spec.size() && spec[i] == '.') {
        ++i;
        std::size_t prec_start = i;
        while (i < spec.size() && std::isdigit(static_cast<unsigned char>(spec[i]))) ++i;
        if (i > prec_start) {
            std::from_chars(spec.data() + prec_start, spec.data() + i, s.precision);
        }
    }

    if (i < spec.size()) {
        s.type = spec[i];
        ++i;
    }
    return s;
}

// Reused across calls on a given thread, same rationale as
// scratch_stream() below -- a distinct instance because apply_value()'s
// `tmp` is used to build one argument's padded text while the caller's
// `out` stream (the overall message being assembled) is still open, so
// the two must never alias the same object. Unlike scratch_stream(),
// this also resets format flags/precision/fill via copyfmt() from a
// never-mutated, default-constructed prototype: a reused stream carries
// over std::hex/std::fixed/setprecision(...) etc. from whatever the
// previous call set, which a freshly-constructed ostringstream (the
// previous, non-reused version of this code) never had to worry about.
inline std::ostringstream& spec_scratch_stream() {
    static const std::ostringstream pristine;
    thread_local std::ostringstream out;
    out.str(std::string());
    out.clear();
    out.copyfmt(pristine);
    return out;
}

template <typename T>
void apply_value(std::ostringstream& out, const T& value, const FormatSpec& spec) {
    if (!spec.has_spec) {
        out << value;
        return;
    }

    std::ostringstream& tmp = spec_scratch_stream();
    if (spec.sign_plus) tmp << std::showpos;
    if (spec.alt_form) tmp << std::showbase;

    switch (spec.type) {
        case 'x': tmp << std::hex << value; break;
        case 'X': tmp << std::uppercase << std::hex << value; break;
        case 'o': tmp << std::oct << value; break;
        case 'f':
        case 'F':
            tmp << std::fixed;
            if (spec.precision >= 0) tmp << std::setprecision(spec.precision);
            tmp << value;
            break;
        case 'e':
        case 'E':
            tmp << std::scientific;
            if (spec.precision >= 0) tmp << std::setprecision(spec.precision);
            tmp << value;
            break;
        case 'd':
        case 'g':
        case 'G':
        case '\0':
        default:
            if (spec.precision >= 0) tmp << std::setprecision(spec.precision);
            tmp << value;
            break;
    }

    std::string text = tmp.str();
    if (spec.width > 0 && static_cast<std::size_t>(spec.width) > text.size()) {
        int pad = spec.width - static_cast<int>(text.size());
        char fill_char = spec.zero_pad ? '0' : spec.fill;
        char align = spec.align;
        if (align == '\0') {
            align = (spec.zero_pad || std::is_arithmetic_v<T>) ? '>' : '<';
        }

        if (align == '<') {
            text.append(static_cast<std::size_t>(pad), fill_char);
        } else if (align == '^') {
            int left = pad / 2;
            int right = pad - left;
            text = std::string(static_cast<std::size_t>(left), fill_char) + text +
                   std::string(static_cast<std::size_t>(right), fill_char);
        } else { // '>'
            if (spec.zero_pad && !text.empty() && (text[0] == '-' || text[0] == '+')) {
                text = text.substr(0, 1) +
                       std::string(static_cast<std::size_t>(pad), fill_char) +
                       text.substr(1);
            } else {
                text = std::string(static_cast<std::size_t>(pad), fill_char) + text;
            }
        }
    }
    out << text;
}

// Appends literal text to `out`, unescaping "{{" -> "{" and "}}" -> "}".
// Used for the tail of the format string once all arguments are consumed.
inline void append_literal(std::ostringstream& out, std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '{' && i + 1 < text.size() && text[i + 1] == '{') {
            out << '{'; i += 2; continue;
        }
        if (c == '}' && i + 1 < text.size() && text[i + 1] == '}') {
            out << '}'; i += 2; continue;
        }
        out << c;
        ++i;
    }
}

inline void format_into(std::ostringstream& out, std::string_view fmt) {
    // No more arguments — emit the remainder verbatim (with brace
    // unescaping). We intentionally never throw here: a logging call must
    // never be able to crash the application because of a malformed
    // message or a mismatched argument count.
    append_literal(out, fmt);
}

template <typename T, typename... Rest>
void format_into(std::ostringstream& out, std::string_view fmt, const T& value, const Rest&... rest) {
    std::size_t i = 0;
    while (i < fmt.size()) {
        char c = fmt[i];
        if (c == '{' && i + 1 < fmt.size() && fmt[i + 1] == '{') {
            out << '{'; i += 2; continue;
        }
        if (c == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            out << '}'; i += 2; continue;
        }
        if (c == '{') {
            auto close = fmt.find('}', i + 1);
            if (close == std::string_view::npos) {
                // Malformed (unterminated) placeholder: dump the rest
                // verbatim rather than throwing.
                out << fmt.substr(i);
                return;
            }
            std::string_view inner = fmt.substr(i + 1, close - i - 1);
            std::string_view spec_str =
                (!inner.empty() && inner[0] == ':') ? inner.substr(1) : inner;
            apply_value(out, value, parse_spec(spec_str));
            format_into(out, fmt.substr(close + 1), rest...);
            return;
        }
        out << c;
        ++i;
    }
    // Ran out of format string with arguments still unused: ignore the
    // extras rather than throwing — a mismatched call must never crash.
}

// One reused ostringstream per producer thread, instead of a fresh one
// per log call. A fresh ostringstream is cheap to construct, but its
// internal string buffer is not: once a formatted message grows past
// SSO (common for real log lines), every call would otherwise pay a
// heap allocation + deallocation just for scratch space that's about to
// be copied out and discarded. Reusing the same buffer across calls on
// a given thread means it grows once and is then reused indefinitely.
// thread_local (rather than a shared static) keeps this contention-free
// across concurrent producer threads, matching the pattern already used
// for get_native_thread_id() in log_record.hpp. Safe to reuse across
// calls because any code that mutates stream flags/fill (e.g. HexBytes
// in hex.hpp) already restores them via OstreamFormatGuard before
// returning -- see that header's doc comment.
inline std::ostringstream& scratch_stream() {
    thread_local std::ostringstream out;
    out.str(std::string());
    out.clear();
    return out;
}

inline std::string format(std::string_view fmt) {
    // Fast path: the vast majority of zero-argument log calls are a
    // plain literal message with no "{"/"}" characters at all (e.g.
    // LOG_INFO("starting up")), which append_literal() would just copy
    // through unchanged anyway. Skipping scratch_stream()/ostringstream
    // entirely for that case avoids a stream-state reset and an extra
    // buffer copy (ostringstream::str() copies its internal buffer into
    // a new std::string) purely to reproduce the input verbatim.
    // append_literal()'s unescaping ("{{" -> "{", "}}" -> "}") only ever
    // triggers when at least one brace is present, so this check is a
    // precise (not approximate) guard, not a heuristic.
    if (fmt.find_first_of("{}") == std::string_view::npos) {
        return std::string(fmt);
    }
    std::ostringstream& out = scratch_stream();
    append_literal(out, fmt);
    return out.str();
}

template <typename... Args>
std::string format(std::string_view fmt, const Args&... args) {
    std::ostringstream& out = scratch_stream();
    format_into(out, fmt, args...);
    return out.str();
}

// Builds a structured key/value Field for use with LOG_*_KV macros, e.g.
//   LOG_INFO_KV("Order placed", logpulsex::field("order_id", id),
//                                logpulsex::field("amount", 59.99));
// The value is stringified immediately via operator<<, using the same
// container/optional support as ordinary log messages, so vectors, maps,
// etc. work here too.
template <typename T>
Field field(std::string key, const T& value) {
    std::ostringstream ss;
    ss << value;
    return Field{std::move(key), ss.str()};
}

inline Field field(std::string key, std::string value) {
    return Field{std::move(key), std::move(value)};
}

} // namespace logpulsex::detail

namespace logpulsex {
using detail::field;
}
