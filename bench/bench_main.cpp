// LogPulseX benchmark suite entry point.
//
//   ./logpulsex_bench [--suite=micro,throughput,multithread,sink_io]
//                      [--iterations=N] [--duration=SECONDS]
//                      [--threads=1,2,4,8] [--csv=path] [--markdown=path]
//
// Build in Release mode -- the Debug CMake config enables
// -fsanitize=address,undefined, which badly skews timings.

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bench_harness.hpp"
#include "micro_bench.hpp"
#include "multithread_bench.hpp"
#include "sink_io_bench.hpp"
#include "throughput_bench.hpp"

namespace {

struct Options {
    std::vector<std::string> suites{"micro", "throughput", "multithread", "sink_io"};
    std::uint64_t iterations = 1'000'000;
    std::chrono::seconds duration{2};
    std::vector<unsigned> threads{1, 2, 4, 8};
    std::string csv_path;
    std::string markdown_path;
};

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (starts_with(arg, "--suite=")) {
            opts.suites = split_csv(arg.substr(8));
        } else if (starts_with(arg, "--iterations=")) {
            opts.iterations = std::stoull(arg.substr(13));
        } else if (starts_with(arg, "--duration=")) {
            opts.duration = std::chrono::seconds(std::stoll(arg.substr(11)));
        } else if (starts_with(arg, "--threads=")) {
            opts.threads.clear();
            for (const auto& t : split_csv(arg.substr(10))) {
                opts.threads.push_back(static_cast<unsigned>(std::stoul(t)));
            }
        } else if (starts_with(arg, "--csv=")) {
            opts.csv_path = arg.substr(6);
        } else if (starts_with(arg, "--markdown=")) {
            opts.markdown_path = arg.substr(11);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: logpulsex_bench [--suite=micro,throughput,multithread,sink_io]\n"
                          "                       [--iterations=N] [--duration=SECONDS]\n"
                          "                       [--threads=1,2,4,8] [--csv=path] [--markdown=path]\n";
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << " (use --help)\n";
            std::exit(1);
        }
    }
    return opts;
}

bool has_suite(const Options& opts, const std::string& name) {
    for (const auto& s : opts.suites) {
        if (s == name) return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    Options opts = parse_args(argc, argv);
    bench::Reporter reporter;

    if (has_suite(opts, "micro")) {
        std::cout << "Running micro benchmarks (" << opts.iterations << " iterations each)...\n";
        bench::run_micro_benchmarks(reporter, opts.iterations);
    }
    if (has_suite(opts, "throughput")) {
        std::cout << "Running single-thread throughput/latency benchmarks ("
                      << opts.duration.count() << "s sustained, " << opts.iterations
                      << " latency samples)...\n";
        bench::run_throughput_benchmarks(reporter, opts.duration, opts.iterations);
    }
    if (has_suite(opts, "multithread")) {
        std::cout << "Running multi-thread scaling benchmarks (" << opts.iterations
                      << " msgs/thread)...\n";
        bench::run_multithread_benchmarks(reporter, opts.threads, opts.iterations);
    }
    if (has_suite(opts, "sink_io")) {
        std::cout << "Running real sink I/O benchmarks (" << opts.iterations << " msgs)...\n";
        bench::run_sink_io_benchmarks(reporter, opts.iterations);
    }

    std::cout << '\n';
    reporter.print_console();

    if (!opts.csv_path.empty()) {
        if (reporter.write_csv(opts.csv_path)) {
            std::cout << "\nWrote CSV results to " << opts.csv_path << '\n';
        } else {
            std::cerr << "\nFailed to write CSV to " << opts.csv_path << '\n';
            return 1;
        }
    }

    if (!opts.markdown_path.empty()) {
        if (reporter.write_markdown(opts.markdown_path)) {
            std::cout << "Wrote Markdown table to " << opts.markdown_path << '\n';
        } else {
            std::cerr << "Failed to write Markdown to " << opts.markdown_path << '\n';
            return 1;
        }
    }

    return 0;
}
