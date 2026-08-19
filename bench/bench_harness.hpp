#pragma once

// Minimal, zero-dependency benchmark harness shared by all bench/*
// suites. Deliberately hand-rolled rather than pulling in Google
// Benchmark / nanobench -- consistent with this library's "zero
// mandatory deps" design goal (see README's Architecture goals table).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace bench {

using Clock = std::chrono::steady_clock;

// Prevents the optimizer from eliminating a computed value (and the
// benchmarked code that produced it) as dead code.
template <typename T>
inline void do_not_optimize(T const& value) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(value) : "memory");
#else
    // MSVC has no inline-asm-based equivalent on all targets. A volatile
    // storage slot is not viable for types like std::optional<T> because
    // volatile optional is not assignable. Taking the address of the value
    // preserves it from optimizer folding while remaining compatible with
    // non-assignable and non-trivially-copyable types.
    auto const* volatile ptr = &value;
    (void)ptr;
#endif
}

// Percentile/summary statistics computed from a set of per-call
// latency samples (nanoseconds).
struct Stats {
    double mean_ns = 0;
    double stddev_ns = 0;
    double min_ns = 0;
    double max_ns = 0;
    double p50_ns = 0;
    double p90_ns = 0;
    double p99_ns = 0;
    double p999_ns = 0;

    static Stats from_samples(std::vector<double> samples) {
        Stats s;
        if (samples.empty()) return s;
        std::sort(samples.begin(), samples.end());
        double sum = 0;
        for (double v : samples) sum += v;
        s.mean_ns = sum / static_cast<double>(samples.size());
        double var = 0;
        for (double v : samples) var += (v - s.mean_ns) * (v - s.mean_ns);
        s.stddev_ns = std::sqrt(var / static_cast<double>(samples.size()));
        s.min_ns = samples.front();
        s.max_ns = samples.back();
        s.p50_ns = percentile(samples, 0.50);
        s.p90_ns = percentile(samples, 0.90);
        s.p99_ns = percentile(samples, 0.99);
        s.p999_ns = percentile(samples, 0.999);
        return s;
    }

private:
    // samples must already be sorted ascending.
    static double percentile(const std::vector<double>& sorted, double p) {
        if (sorted.size() == 1) return sorted[0];
        double idx = p * static_cast<double>(sorted.size() - 1);
        std::size_t lo = static_cast<std::size_t>(idx);
        std::size_t hi = std::min(lo + 1, sorted.size() - 1);
        double frac = idx - static_cast<double>(lo);
        return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
    }
};

// One reported row. Either a plain throughput measurement (total time
// over N iterations, no per-call samples) or a full latency Stats.
// `suite` groups rows under a console section header; `note` carries
// short auxiliary context (e.g. "58% scaling") that doesn't belong in
// the benchmark name itself.
struct Result {
    std::string suite;
    std::string name;
    std::string note;
    std::uint64_t iterations = 0;
    double total_ns = 0;
    bool has_stats = false;
    Stats stats;

    double ns_per_op() const {
        return iterations == 0 ? 0.0 : total_ns / static_cast<double>(iterations);
    }
    double ops_per_sec() const {
        return total_ns <= 0 ? 0.0 : static_cast<double>(iterations) * 1e9 / total_ns;
    }
};

// Times `fn` called `iterations` times as one tight loop (after
// `warmup` untimed iterations). Use this when per-call timestamping
// overhead would itself dominate the thing being measured (e.g. a
// single queue push/pop, or building a short string).
template <typename Fn>
Result run_throughput(std::string name, std::uint64_t iterations,
                       std::uint64_t warmup, Fn&& fn) {
    for (std::uint64_t i = 0; i < warmup; ++i) fn();

    auto start = Clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) fn();
    auto elapsed = Clock::now() - start;

    Result r;
    r.name = std::move(name);
    r.iterations = iterations;
    r.total_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    return r;
}

// Times `fn` individually on each of `iterations` calls (after `warmup`
// untimed iterations), recording per-call latency so percentiles can be
// reported. More overhead per call than run_throughput() (one
// steady_clock::now() pair per iteration), so prefer run_throughput()
// unless the percentile distribution itself is the point.
template <typename Fn>
Result run_latency(std::string name, std::uint64_t iterations,
                    std::uint64_t warmup, Fn&& fn) {
    for (std::uint64_t i = 0; i < warmup; ++i) fn();

    std::vector<double> samples;
    samples.reserve(iterations);
    double total_ns = 0;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        fn();
        auto elapsed = Clock::now() - start;
        double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        samples.push_back(ns);
        total_ns += ns;
    }

    Result r;
    r.name = std::move(name);
    r.iterations = iterations;
    r.total_ns = total_ns;
    r.has_stats = true;
    r.stats = Stats::from_samples(std::move(samples));
    return r;
}

// Collects Results across every suite that ran, then prints a console
// report and/or writes CSV/Markdown files.
class Reporter {
public:
    void add(Result r) { results_.push_back(std::move(r)); }

    // Convenience overload: tags a Result with its suite (and optional
    // note) at the call site, so suite/note assignment doesn't have to
    // be spelled out as separate statements in every bench_*.cpp file.
    void add(std::string suite, Result r, std::string note = {}) {
        r.suite = std::move(suite);
        if (!note.empty()) r.note = std::move(note);
        results_.push_back(std::move(r));
    }

    // Prints one narrow, per-suite table (Benchmark | iters | ns/op |
    // ops/sec | note) grouped under a header, in the order suites first
    // appeared -- keeps each table's line width sane regardless of how
    // many suites ran. Rows with latency percentiles are called out in a
    // separate, equally narrow table afterwards rather than padding
    // every row with unused "-" columns.
    void print_console() const {
        for (const auto& suite : suite_order()) {
            std::cout << "\n== " << suite << " ==\n";
            std::cout << std::left << std::setw(kNameWidth) << "Benchmark"
                       << std::right << std::setw(kIterWidth) << "iters"
                       << std::setw(kNsWidth) << "ns/op"
                       << std::setw(kOpsWidth) << "ops/sec"
                       << "  " << std::left << "note" << '\n';
            std::cout << std::string(kNameWidth + kIterWidth + kNsWidth + kOpsWidth + 24, '-')
                       << '\n';
            for (const auto& r : results_) {
                if (r.suite != suite) continue;
                std::cout << std::left << std::setw(kNameWidth) << truncate(r.name, kNameWidth)
                           << std::right << std::setw(kIterWidth) << r.iterations
                           << std::setw(kNsWidth) << std::fixed << std::setprecision(1) << r.ns_per_op()
                           << std::setw(kOpsWidth) << format_grouped(r.ops_per_sec())
                           << "  " << std::left << r.note << '\n';
            }
        }

        bool any_stats = false;
        for (const auto& r : results_) any_stats = any_stats || r.has_stats;
        if (!any_stats) return;

        std::cout << "\n== latency percentiles (ns) ==\n";
        std::cout << std::left << std::setw(kNameWidth) << "Benchmark"
                   << std::right << std::setw(10) << "p50" << std::setw(10) << "p90"
                   << std::setw(10) << "p99" << std::setw(10) << "p99.9" << std::setw(10) << "max"
                   << '\n';
        std::cout << std::string(kNameWidth + 50, '-') << '\n';
        for (const auto& r : results_) {
            if (!r.has_stats) continue;
            std::cout << std::left << std::setw(kNameWidth) << truncate(r.name, kNameWidth)
                       << std::right
                       << std::setw(10) << std::fixed << std::setprecision(0) << r.stats.p50_ns
                       << std::setw(10) << std::fixed << std::setprecision(0) << r.stats.p90_ns
                       << std::setw(10) << std::fixed << std::setprecision(0) << r.stats.p99_ns
                       << std::setw(10) << std::fixed << std::setprecision(0) << r.stats.p999_ns
                       << std::setw(10) << std::fixed << std::setprecision(0) << r.stats.max_ns
                       << '\n';
        }
    }

    // Returns false if the file could not be opened for writing.
    bool write_csv(const std::string& path) const {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out) return false;
        out << "suite,name,note,iterations,ns_per_op,ops_per_sec,mean_ns,stddev_ns,min_ns,"
               "p50_ns,p90_ns,p99_ns,p999_ns,max_ns\n";
        for (const auto& r : results_) {
            out << csv_escape(r.suite) << ',' << csv_escape(r.name) << ',' << csv_escape(r.note)
                << ',' << r.iterations << ',' << r.ns_per_op() << ',' << r.ops_per_sec() << ',';
            if (r.has_stats) {
                out << r.stats.mean_ns << ',' << r.stats.stddev_ns << ',' << r.stats.min_ns << ','
                    << r.stats.p50_ns << ',' << r.stats.p90_ns << ',' << r.stats.p99_ns << ','
                    << r.stats.p999_ns << ',' << r.stats.max_ns;
            } else {
                out << ",,,,,,,";
            }
            out << '\n';
        }
        return true;
    }

    // Writes a GitHub-flavored Markdown table, suitable for pasting
    // directly into a README or PR description. Returns false if the
    // file could not be opened for writing.
    bool write_markdown(const std::string& path) const {
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out) return false;
        out << "| Suite | Benchmark | iters | ns/op | ops/sec | Note | p50 | p90 | p99 | p99.9 | max |\n";
        out << "|---|---|---:|---:|---:|---|---:|---:|---:|---:|---:|\n";
        for (const auto& r : results_) {
            out << "| " << markdown_escape(r.suite) << " | " << markdown_escape(r.name) << " | "
                << r.iterations << " | " << std::fixed << std::setprecision(1) << r.ns_per_op()
                << " | " << std::fixed << std::setprecision(0) << r.ops_per_sec() << " | "
                << markdown_escape(r.note) << " | ";
            if (r.has_stats) {
                out << std::fixed << std::setprecision(0) << r.stats.p50_ns << " | "
                    << r.stats.p90_ns << " | " << r.stats.p99_ns << " | " << r.stats.p999_ns
                    << " | " << r.stats.max_ns << " |\n";
            } else {
                out << "- | - | - | - | - |\n";
            }
        }
        return true;
    }

    const std::vector<Result>& results() const { return results_; }

private:
    static constexpr int kNameWidth = 32;
    static constexpr int kIterWidth = 10;
    static constexpr int kNsWidth = 10;
    static constexpr int kOpsWidth = 14;

    // Suite names in first-seen order (avoids depending on std::map's
    // alphabetical order, which would separate e.g. "micro" from
    // "multithread" less usefully than run order does).
    std::vector<std::string> suite_order() const {
        std::vector<std::string> order;
        for (const auto& r : results_) {
            if (std::find(order.begin(), order.end(), r.suite) == order.end()) {
                order.push_back(r.suite);
            }
        }
        return order;
    }

    static std::string truncate(const std::string& s, int width) {
        auto max_len = static_cast<std::size_t>(width);
        if (s.size() <= max_len) return s;
        return s.substr(0, max_len - 1) + "~";
    }

    // Formats with thousands separators (e.g. 2438515 -> "2,438,515") so
    // large ops/sec figures are readable at a glance without a locale
    // dependency.
    static std::string format_grouped(double value) {
        auto rounded = static_cast<std::int64_t>(value + (value >= 0 ? 0.5 : -0.5));
        std::string digits = std::to_string(rounded < 0 ? -rounded : rounded);
        std::string grouped;
        int count = 0;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            if (count != 0 && count % 3 == 0) grouped += ',';
            grouped += *it;
            ++count;
        }
        std::reverse(grouped.begin(), grouped.end());
        return rounded < 0 ? "-" + grouped : grouped;
    }

    static std::string csv_escape(const std::string& s) {
        if (s.find(',') == std::string::npos) return s;
        std::string out = "\"";
        out += s;
        out += '"';
        return out;
    }

    static std::string markdown_escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '|') out += '\\';
            out += c;
        }
        return out;
    }

    std::vector<Result> results_;
};

} // namespace bench

