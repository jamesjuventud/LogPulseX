#pragma once

#include <cstddef>
#include <cstdint>
#include <ios>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

// Binary and hex dump logging support. This header is intentionally
// self-contained -- pure formatting utilities with zero dependency on
// Logger/Level -- so it works as a standalone hex-formatting library
// even outside a logging context. It's designed to pair with LOG_RAW
// (see logpulsex.hpp), which is the intended way to actually get
// format_hex_dump()'s multi-line output into your logs -- see that
// function's doc comment for why.

namespace logpulsex {

namespace detail {

// RAII guard that saves an ostream's format flags and fill character on
// construction and restores them on destruction. HexBytes::operator<<
// changes std::hex/std::uppercase/setfill to render bytes, and MUST NOT
// leak that state into whatever else writes to the same stream
// afterward -- this matters concretely here, since format.hpp's
// apply_value() reuses one std::ostringstream across every argument in
// a single LOG_INFO("...", a, b, c) call. Without this guard,
// LOG_INFO("{} {}", hex_bytes(buf, 4), 255) would render the plain
// integer 255 as "ff" too, because the stream would still be in hex
// mode from formatting the first argument.
class OstreamFormatGuard {
public:
    explicit OstreamFormatGuard(std::ostream& os)
        : os_(os), flags_(os.flags()), fill_(os.fill()) {}
    ~OstreamFormatGuard() {
        os_.flags(flags_);
        os_.fill(fill_);
    }
    OstreamFormatGuard(const OstreamFormatGuard&) = delete;
    OstreamFormatGuard& operator=(const OstreamFormatGuard&) = delete;

private:
    std::ostream& os_;
    std::ios::fmtflags flags_;
    char fill_;
};

} // namespace detail

// Non-owning view over a raw byte buffer that streams as space-separated,
// "0x"-prefixed hex pairs -- e.g. HexBytes over {0xA1, 0xB2} renders as
// "0xA1 0xB2", matching the TX/RX command-trace convention used by most
// device/protocol logging (SPI, I2C, UART, USB, etc). Integrates with
// the existing "{}" message formatting exactly like the container
// support in container_format.hpp: just pass it as a LOG_INFO argument.
//
//   uint8_t tx[] = {0xA1, 0xB2};
//   LOG_DEBUG("TX: {}", logpulsex::hex_bytes(tx, 2));   // "TX: 0xA1 0xB2"
//
// Security/reliability note: a caller logging a large or attacker-
// influenced buffer (e.g. an untrusted network payload) should not be
// able to produce an unbounded log line. HexBytes truncates to
// `max_bytes` (default 256) and appends "... (N more bytes)" -- pass a
// larger value explicitly if you specifically need the whole buffer.
class HexBytes {
public:
    static constexpr std::size_t kDefaultMaxBytes = 256;

    HexBytes(const void* data, std::size_t size, std::size_t max_bytes = kDefaultMaxBytes)
        : data_(static_cast<const unsigned char*>(data)), size_(size), max_bytes_(max_bytes) {}

    friend std::ostream& operator<<(std::ostream& os, const HexBytes& hb) {
        detail::OstreamFormatGuard guard(os);
        std::size_t shown = hb.size_ < hb.max_bytes_ ? hb.size_ : hb.max_bytes_;
        os << std::hex << std::uppercase;
        for (std::size_t i = 0; i < shown; ++i) {
            if (i) os << ' ';
            unsigned int byte_value = hb.data_[i];
            os << "0x" << std::setfill('0') << std::setw(2) << byte_value;
        }
        if (shown < hb.size_) {
            os << std::dec << " ... (" << (hb.size_ - shown) << " more bytes)";
        }
        return os;
    }

private:
    const unsigned char* data_;
    std::size_t size_;
    std::size_t max_bytes_;
};

// Factory functions, mirroring the field()/format() helper style used
// elsewhere in this library.
inline HexBytes hex_bytes(const void* data, std::size_t size,
                           std::size_t max_bytes = HexBytes::kDefaultMaxBytes) {
    return HexBytes(data, size, max_bytes);
}

// Constrained to types with .data()/.size() (std::vector, std::array,
// std::string, etc). Deliberately does NOT match raw C arrays: without
// this constraint, calling hex_bytes(raw_array, count) would ambiguously
// prefer this template (binding a reference to an array is an exact
// match, cheaper than the array-to-pointer decay the pointer overload
// above needs) and then fail to compile, since arrays have no .data().
// Excluding array types here lets that common call pattern correctly
// resolve to the pointer overload instead.
template <typename Container>
    requires requires(const Container& c) { c.data(); c.size(); }
HexBytes hex_bytes(const Container& c, std::size_t max_bytes = HexBytes::kDefaultMaxBytes) {
    return HexBytes(c.data(), c.size() * sizeof(typename Container::value_type), max_bytes);
}

namespace detail {

// True if a byte renders as a printable, single-column ASCII character.
// Used for the ASCII sidebar in format_hex_dump(). Deliberately
// excludes control characters -- including the newline byte 0x0A --
// since letting a raw control byte straight through into the sidebar
// would defeat the purpose of the level-based safety boundary described
// on format_hex_dump() and LOG_RAW below: every byte from the input
// buffer either becomes a "XX " hex pair (hex digits and spaces only)
// or, in the sidebar, passes through this filter, so the *only* '\n'
// characters that can ever appear in this function's output are the row
// separators the function inserts itself -- never one derived from
// attacker- or device-controlled buffer content.
inline bool is_printable_ascii(unsigned char c) {
    return c >= 0x20 && c < 0x7F;
}

} // namespace detail

// Formats a classic multi-row hex dump -- offset, hex bytes (16 per row
// by default), and an ASCII sidebar -- as ONE string containing real,
// embedded '\n' row separators, e.g.:
//
//   00000000  48 65 6c 6c 6f 2c 20 77 6f 72 6c 64 21 0a 00 00  |Hello, world!...|
//
// To log this while preserving the multi-row layout, use LOG_RAW (see
// logpulsex.hpp), which logs at Level::raw -- the one level whose plain-
// text rendering (PatternFormatter, console) does NOT escape embedded
// '\n' bytes, specifically so dumps like this one render as intended
// instead of collapsing into one line of literal "\n" text (which is
// what every other level's injection-safe escaping would otherwise do
// to it, and rightly so for ordinary messages). JsonFormatter is
// unaffected by level -- it always escapes newlines to keep the JSON
// valid, and instead carries `"level":"RAW"` as the signal a downstream
// viewer can use to re-expand `\n` when rendering.
//
//   LOG_RAW("{}", logpulsex::format_hex_dump(buf, len));
//
// This function itself has no dependency on Logger/Level and can be used
// standalone (writing to a file, printing outside the logger, etc) --
// just be aware the returned string contains real newlines and should
// not be passed to a normal (non-raw) LOG_* macro, where it would be
// escaped into an unreadable single line by design.
inline std::string format_hex_dump(const void* data, std::size_t size,
                                    std::size_t bytes_per_row = 16) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(size * 4 + (size / (bytes_per_row == 0 ? 1 : bytes_per_row) + 1) * 12);

    if (bytes_per_row == 0) bytes_per_row = 16;

    for (std::size_t row_start = 0; row_start < size; row_start += bytes_per_row) {
        std::size_t row_len = (size - row_start < bytes_per_row) ? (size - row_start) : bytes_per_row;

        std::ostringstream row;
        row << std::hex << std::uppercase << std::setfill('0');
        row << std::setw(8) << row_start << "  ";

        for (std::size_t i = 0; i < bytes_per_row; ++i) {
            if (i < row_len) {
                row << std::setw(2) << static_cast<unsigned int>(bytes[row_start + i]) << ' ';
            } else {
                row << "   "; // pad short final row so the ASCII column still aligns
            }
        }

        row << " |";
        for (std::size_t i = 0; i < row_len; ++i) {
            unsigned char c = bytes[row_start + i];
            row << (detail::is_printable_ascii(c) ? static_cast<char>(c) : '.');
        }
        row << '|';

        out += row.str();
        if (row_start + bytes_per_row < size) out += '\n';
    }
    return out;
}

} // namespace logpulsex
