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

// Streaming support for standard containers, so LOG_INFO("{}", my_vector)
// works without the caller writing their own operator<<.
//
// IMPORTANT — include-order requirement: because of how C++ two-phase
// name lookup works for templates, these overloads must be visible
// (parsed) BEFORE the format_into() templates in format.hpp are defined
// in this translation unit, or they will not be found for container
// arguments. format.hpp enforces this by including this header first;
// if you use logpulsex::detail::format_into directly from your own header
// without going through format.hpp, preserve that include order.
//
// Trade-off: declaring operator<< for std::pair/std::vector/etc. inside
// namespace `logpulsex` (rather than a deeply private detail namespace) is
// what makes ordinary unqualified lookup find them from logpulsex::detail.
// If your project also defines its own operator<< for these same
// standard types in a way that could be visible in the same translation
// unit, you may get an ambiguous-overload error — in that case, prefer
// your own and skip including this header directly (format.hpp still
// needs *a* definition, so keep exactly one visible).

#include <array>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace logpulsex {

// Forward declarations. Required so that generic helpers below
// (stream_sequence / stream_map), when instantiated for a nested
// container like std::vector<std::vector<int>>, can find operator<< for
// the *inner* container via ordinary unqualified lookup at their point
// of definition — ADL alone won't find them here, since these overloads
// live in namespace `logpulsex`, not in `std` or the element type's
// namespace. Declaring (not yet defining) them up front is sufficient.
template <typename A, typename B>
std::ostream& operator<<(std::ostream& os, const std::pair<A, B>& p);
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::optional<T>& o);
template <typename T, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::vector<T, Alloc>& v);
template <typename T, std::size_t N>
std::ostream& operator<<(std::ostream& os, const std::array<T, N>& a);
template <typename T, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::deque<T, Alloc>& d);
template <typename T, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::list<T, Alloc>& l);
template <typename T, typename Compare, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::set<T, Compare, Alloc>& s);
template <typename T, typename Hash, typename Eq, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::unordered_set<T, Hash, Eq, Alloc>& s);
template <typename K, typename V, typename Compare, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::map<K, V, Compare, Alloc>& m);
template <typename K, typename V, typename Hash, typename Eq, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::unordered_map<K, V, Hash, Eq, Alloc>& m);

template <typename A, typename B>
std::ostream& operator<<(std::ostream& os, const std::pair<A, B>& p) {
    return os << '(' << p.first << ", " << p.second << ')';
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const std::optional<T>& o) {
    if (o.has_value()) return os << *o;
    return os << "nullopt";
}

namespace detail {

template <typename Iterable>
std::ostream& stream_sequence(std::ostream& os, const Iterable& items,
                               char open, char close) {
    os << open;
    bool first = true;
    for (const auto& item : items) {
        if (!first) os << ", ";
        first = false;
        os << item;
    }
    return os << close;
}

template <typename Map>
std::ostream& stream_map(std::ostream& os, const Map& m) {
    os << '{';
    bool first = true;
    for (const auto& [key, value] : m) {
        if (!first) os << ", ";
        first = false;
        os << key << ": " << value;
    }
    return os << '}';
}

} // namespace detail

template <typename T, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::vector<T, Alloc>& v) {
    return detail::stream_sequence(os, v, '[', ']');
}

template <typename T, std::size_t N>
std::ostream& operator<<(std::ostream& os, const std::array<T, N>& a) {
    return detail::stream_sequence(os, a, '[', ']');
}

template <typename T, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::deque<T, Alloc>& d) {
    return detail::stream_sequence(os, d, '[', ']');
}

template <typename T, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::list<T, Alloc>& l) {
    return detail::stream_sequence(os, l, '[', ']');
}

template <typename T, typename Compare, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::set<T, Compare, Alloc>& s) {
    return detail::stream_sequence(os, s, '{', '}');
}

template <typename T, typename Hash, typename Eq, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::unordered_set<T, Hash, Eq, Alloc>& s) {
    return detail::stream_sequence(os, s, '{', '}');
}

template <typename K, typename V, typename Compare, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::map<K, V, Compare, Alloc>& m) {
    return detail::stream_map(os, m);
}

template <typename K, typename V, typename Hash, typename Eq, typename Alloc>
std::ostream& operator<<(std::ostream& os, const std::unordered_map<K, V, Hash, Eq, Alloc>& m) {
    return detail::stream_map(os, m);
}

} // namespace logpulsex
