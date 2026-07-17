#include "generation.hpp"
#include "search.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::string snapshot;
    std::size_t iterations = 100;
    std::size_t warmup = 5;
    std::vector<std::string> queries;
};

struct Result {
    std::size_t matches = 0;
    std::uint64_t average_ns = 0;
    std::uint64_t p50_ns = 0;
    std::uint64_t p99_ns = 0;
    aurhub::SearchStats stats;
};

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
Result benchmark(const aurhub::GenerationView& snapshot,
                 std::string_view query,
                 std::size_t warmup,
                 std::size_t iterations) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const std::string normalized = aurhub::normalize_search_query(query);
    std::vector<std::size_t> matches;
    matches.reserve(500);
    aurhub::SearchStats stats;
    snapshot.search(normalized, 500, matches, &stats);
    for (std::size_t i = 0; i < warmup; ++i) {
        snapshot.search(normalized, 500, matches);
    }

    std::vector<std::uint64_t> timings;
    timings.reserve(iterations);
    std::uint64_t total = 0;
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto started = std::chrono::steady_clock::now();
        snapshot.search(normalized, 500, matches);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        const auto elapsed_ns = static_cast<std::uint64_t>(elapsed);
        timings.push_back(elapsed_ns);
        total += elapsed_ns;
        checksum += matches.size();
    }
    if (checksum == static_cast<std::size_t>(-1)) {
        std::abort();
    }
    std::sort(timings.begin(), timings.end());
    const std::size_t p50_index = (timings.size() - 1U) * 50U / 100U;
    const std::size_t p99_index = (timings.size() - 1U) * 99U / 100U;
    return Result{
        matches.size(),
        total / iterations,
        timings[p50_index],
        timings[p99_index],
        stats,
    };
}

void print_result(std::string_view query,
                  const Result& result) {
    const double qps = result.average_ns == 0
                           ? 0.0
                           : 1'000'000'000.0 /
                                 static_cast<double>(result.average_ns);
    std::cout << "query=" << std::quoted(query)
              << " matches=" << result.matches
              << " avg_ns=" << result.average_ns
              << " p50_ns=" << result.p50_ns
              << " p99_ns=" << result.p99_ns
              << " super_blocks=" << result.stats.super_blocks_scanned << '/'
              << result.stats.super_blocks_checked
              << " blocks=" << result.stats.blocks_scanned << '/'
              << result.stats.blocks_checked
              << " packages_scanned=" << result.stats.packages_scanned
              << " qps=" << std::fixed
              << std::setprecision(2) << qps << '\n';
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    Options options;
    CLI::App app{"AUR search benchmark"};
    app.add_option("--snapshot", options.snapshot, "Snapshot file")->required();
    app.add_option("--iterations", options.iterations, "Iteration count");
    app.add_option("--warmup", options.warmup, "Warmup iterations");
    app.add_option("--query", options.queries,
                   "Query text (repeatable)");
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }
    if (options.iterations == 0) {
        std::cerr << "aurhub-search-bench: --iterations must be positive\n";
        return 1;
    }
    if (options.queries.empty()) {
        options.queries = {"a", "yay", "systemd", "zzzzzzzznomatch", "qzxqzx"};
    }

    try {
        const aurhub::GenerationView snapshot(options.snapshot);
        for (const std::string& query : options.queries) {
            const Result result = benchmark(snapshot, query,
                                            options.warmup,
                                            options.iterations);
            print_result(query, result);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "aurhub-search-bench: " << error.what() << '\n';
        return 1;
    }
}
