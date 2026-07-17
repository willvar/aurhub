#include "generation.hpp"
#include "search.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Options {
    std::string snapshot;
    std::string type;
    std::string query;
    std::size_t iterations = 1000;
    std::size_t warmup = 50;
    bool records_only = false;
};

std::optional<std::size_t> exact_package(
    const aurhub::GenerationView& snapshot,
    std::string_view name) {
    return snapshot.find_package(name);
}

std::size_t run_query(const aurhub::GenerationView& snapshot,
                      const Options& options,
                      std::vector<std::size_t>& matches,
                      std::string& body) {
    matches.clear();
    if (options.type == "info") {
        if (const auto index = exact_package(snapshot, options.query); index) {
            matches.push_back(*index);
        }
    } else {
        snapshot.search(aurhub::normalize_search_query(options.query), 500,
                        matches);
    }

    body.clear();
    for (const std::size_t index : matches) {
        body.append(snapshot.json(index));
        body.push_back('\n');
    }
    return body.size() + matches.size();
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    try {
        Options options;
        CLI::App app{"AUR snapshot query benchmark"};
        app.add_option("--snapshot", options.snapshot, "Snapshot file")->required();
        app.add_option("--type", options.type, "info or search")
            ->required()
            ->check(CLI::IsMember({"info", "search"}));
        app.add_option("--query", options.query, "Query text")->required();
        app.add_option("--iterations", options.iterations, "Number of iterations");
        app.add_option("--warmup", options.warmup, "Warmup iterations");
        app.add_flag("--records-only", options.records_only,
                     "Skip full snapshot validation");
        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& e) {
            return app.exit(e);
        }
        if (options.iterations == 0) {
            std::cerr << "aurhub-query-bench: --iterations must be positive\n";
            return 1;
        }

        const aurhub::GenerationView snapshot(
            options.snapshot,
            options.records_only ? aurhub::SnapshotValidation::records_only
                                 : aurhub::SnapshotValidation::full);
        std::vector<std::size_t> matches;
        matches.reserve(500);
        std::string body;
        body.reserve(static_cast<std::size_t>(512) * 1024);
        std::size_t checksum = 0;
        for (std::size_t i = 0; i < options.warmup; ++i) {
            checksum += run_query(snapshot, options, matches, body);
        }

        std::vector<std::uint64_t> timings;
        timings.reserve(options.iterations);
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < options.iterations; ++i) {
            const auto started = std::chrono::steady_clock::now();
            checksum += run_query(snapshot, options, matches, body);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started)
                    .count();
            const auto elapsed_ns = static_cast<std::uint64_t>(elapsed);
            timings.push_back(elapsed_ns);
            total += elapsed_ns;
        }
        if (checksum == 0) {
            std::cerr << "aurhub-query-bench: empty checksum\n";
        }
        std::sort(timings.begin(), timings.end());
        const std::size_t p50 = (timings.size() - 1U) * 50U / 100U;
        const std::size_t p99 = (timings.size() - 1U) * 99U / 100U;
        const std::uint64_t average = total / options.iterations;
        const double qps =
            average == 0
                ? 0.0
                : 1'000'000'000.0 / static_cast<double>(average);
        std::cout << "type=" << options.type << " query="
                  << std::quoted(options.query) << " matches=" << matches.size()
                  << " bytes=" << body.size() << " avg_ns=" << average
                  << " p50_ns=" << timings[p50]
                  << " p99_ns=" << timings[p99] << " qps=" << std::fixed
                  << std::setprecision(2) << qps << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "aurhub-query-bench: " << error.what() << '\n';
        return 1;
    }
}
