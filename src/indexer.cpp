#include "generation.hpp"
#include "git.hpp"
#include "merge.hpp"
#include "snapshot.hpp"
#include "srcinfo.hpp"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <iterator>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

namespace {

struct Options {
    std::string repo;
    std::string output;
    std::string diagnostics;
    std::size_t limit = 0;
    std::uint32_t search_block_packages = aurhub::kDefaultSearchBlockPackages;
    std::uint32_t search_super_block_packages =
        aurhub::kDefaultSearchSuperBlockPackages;
    std::size_t jobs = 1;
    std::size_t max_overlay_records = 4096;
    bool full = false;
};

struct BranchDiagnostic {
    std::uint32_t branch_index = 0;
    aurhub::SrcinfoDiagnostic diagnostic;
};

struct ParseStats {
    std::size_t missing = 0;
    std::size_t warning_branches = 0;
    std::size_t quarantined_warning_branches = 0;
    std::size_t quarantined_branches = 0;
    std::size_t warnings = 0;
    std::size_t fatals = 0;
    std::array<std::size_t, aurhub::srcinfo_diagnostic_code_count()> by_code{};
    std::vector<BranchDiagnostic> diagnostics;

    void record(std::uint32_t branch_index,
                const aurhub::SrcinfoResult& result) {
        const std::size_t result_warnings = result.warning_count();
        const std::size_t result_fatals = result.fatal_count();
        warning_branches +=
            result_warnings != 0 && result_fatals == 0 ? 1U : 0U;
        quarantined_warning_branches +=
            result_warnings != 0 && result_fatals != 0 ? 1U : 0U;
        quarantined_branches += result_fatals != 0 ? 1U : 0U;
        warnings += result_warnings;
        fatals += result_fatals;
        diagnostics.reserve(diagnostics.size() + result.diagnostics.size());
        for (const aurhub::SrcinfoDiagnostic& diagnostic : result.diagnostics) {
            const std::size_t code =
                static_cast<std::size_t>(diagnostic.code);
            if (code < by_code.size()) {
                ++by_code[code];
            }
            diagnostics.push_back(BranchDiagnostic{branch_index, diagnostic});
        }
    }

    void merge(ParseStats&& other) {
        missing += other.missing;
        warning_branches += other.warning_branches;
        quarantined_warning_branches += other.quarantined_warning_branches;
        quarantined_branches += other.quarantined_branches;
        warnings += other.warnings;
        fatals += other.fatals;
        for (std::size_t i = 0; i < by_code.size(); ++i) {
            by_code[i] += other.by_code[i];
        }
        diagnostics.insert(diagnostics.end(),
                           std::make_move_iterator(other.diagnostics.begin()),
                           std::make_move_iterator(other.diagnostics.end()));
    }
};

void sort_diagnostics(std::vector<BranchDiagnostic>& diagnostics) {
    std::sort(diagnostics.begin(), diagnostics.end(),
              [](const BranchDiagnostic& left,
                 const BranchDiagnostic& right) {
                  if (left.branch_index != right.branch_index) {
                      return left.branch_index < right.branch_index;
                  }
                  if (left.diagnostic.line != right.diagnostic.line) {
                      return left.diagnostic.line < right.diagnostic.line;
                  }
                  return static_cast<std::uint8_t>(left.diagnostic.code) <
                         static_cast<std::uint8_t>(right.diagnostic.code);
              });
}

void print_parse_stats(const ParseStats& stats) {
    std::cerr << "aurhub-indexer: srcinfo missing=" << stats.missing
              << ", warning_branches=" << stats.warning_branches
              << ", quarantined_with_warnings="
              << stats.quarantined_warning_branches
              << ", quarantined=" << stats.quarantined_branches
              << ", warnings=" << stats.warnings
              << ", fatals=" << stats.fatals << '\n';
    for (std::size_t i = 0; i < stats.by_code.size(); ++i) {
        if (stats.by_code[i] == 0) {
            continue;
        }
        const auto code = static_cast<aurhub::SrcinfoDiagnosticCode>(i);
        std::cerr << "aurhub-indexer: srcinfo "
                  << aurhub::srcinfo_diagnostic_code_name(code) << '='
                  << stats.by_code[i] << '\n';
    }
}

void write_diagnostics(const std::string& path,
                       const std::vector<aurhub::BranchInfo>& branches,
                       std::vector<BranchDiagnostic> diagnostics) {
    sort_diagnostics(diagnostics);
    const std::string temporary =
        path + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open diagnostics output " + temporary);
    }
    output << "severity\tcode\tbranch\tline\tkey\n";
    for (const BranchDiagnostic& record : diagnostics) {
        if (record.branch_index >= branches.size()) {
            throw std::runtime_error("diagnostic references invalid branch");
        }
        output << aurhub::srcinfo_severity_name(record.diagnostic.severity)
               << '\t'
               << aurhub::srcinfo_diagnostic_code_name(record.diagnostic.code)
               << '\t' << branches[record.branch_index].name << '\t'
               << record.diagnostic.line << '\t' << record.diagnostic.key
               << '\n';
    }
    output.close();
    if (!output) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot write diagnostics output " + temporary);
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot publish diagnostics output " + path +
                                 ": " + error.message());
    }
}

aurhub::CompiledPackage compile_package(aurhub::Package package,
                                        std::uint32_t branch_index,
                                        std::int64_t updated_at) {
    package.updated_at = updated_at;
    std::string search = aurhub::normalized_search_text(package);
    std::string json = aurhub::package_json(package);
    return aurhub::CompiledPackage{
        std::move(package.name),
        std::move(search),
        std::move(json),
        branch_index,
        updated_at,
    };
}

class StringArena {
public:
    std::string_view copy(std::string_view value) {
        if (value.empty()) {
            return {};
        }
        if (value.size() > remaining_) {
            const std::size_t allocation =
                std::max(kBlockBytes, value.size());
            blocks_.push_back(std::make_unique<char[]>(allocation));
            cursor_ = blocks_.back().get();
            remaining_ = allocation;
        }
        char* destination = cursor_;
        std::memcpy(destination, value.data(), value.size());
        cursor_ += value.size();
        remaining_ -= value.size();
        return {destination, value.size()};
    }

private:
    static constexpr std::size_t kBlockBytes = static_cast<std::size_t>(1024) * 1024;
    std::vector<std::unique_ptr<char[]>> blocks_;
    char* cursor_ = nullptr;
    std::size_t remaining_ = 0;
};

aurhub::CompiledPackageView compile_package_view(
    aurhub::Package package,
    std::uint32_t branch_index,
    std::int64_t updated_at,
    StringArena& arena) {
    package.updated_at = updated_at;
    const std::string search = aurhub::normalized_search_text(package);
    const std::string json = aurhub::package_json(package);
    return aurhub::CompiledPackageView{
        arena.copy(package.name),
        arena.copy(search),
        arena.copy(json),
        branch_index,
        updated_at,
    };
}

void append_branch_packages(aurhub::GitBatch& batch,
                            aurhub::BranchInfo& branch,
                            std::uint32_t branch_index,
                            bool load_timestamp,
                            std::vector<aurhub::CompiledPackage>& packages,
                            ParseStats& parse_stats) {
    const std::optional<std::string> srcinfo =
        aurhub::read_branch_srcinfo(batch, branch, load_timestamp);
    if (!srcinfo.has_value()) {
        ++parse_stats.missing;
        return;
    }
    aurhub::SrcinfoResult parsed = aurhub::parse_srcinfo(*srcinfo);
    parse_stats.record(branch_index, parsed);
    if (parsed.has_fatal()) {
        return;
    }
    for (aurhub::Package& package : parsed.packages) {
        packages.push_back(
            compile_package(std::move(package), branch_index, branch.updated_at));
    }
}

void append_branch_package_views(
    aurhub::GitBatch& batch,
    aurhub::BranchInfo& branch,
    std::uint32_t branch_index,
    bool load_timestamp,
    StringArena& arena,
    std::vector<aurhub::CompiledPackageView>& packages,
    ParseStats& parse_stats) {
    const std::optional<std::string> srcinfo =
        aurhub::read_branch_srcinfo(batch, branch, load_timestamp);
    if (!srcinfo.has_value()) {
        ++parse_stats.missing;
        return;
    }
    aurhub::SrcinfoResult parsed = aurhub::parse_srcinfo(*srcinfo);
    parse_stats.record(branch_index, parsed);
    if (parsed.has_fatal()) {
        return;
    }
    for (aurhub::Package& package : parsed.packages) {
        packages.push_back(compile_package_view(
            std::move(package), branch_index, branch.updated_at, arena));
    }
}

void partition_package_views(
    std::vector<aurhub::CompiledPackageView>& raw_packages,
    std::vector<aurhub::CompiledPackageView>& packages,
    std::vector<aurhub::CompiledPackageView>& shadow_packages) {
    std::stable_sort(
        raw_packages.begin(), raw_packages.end(),
        [](const aurhub::CompiledPackageView& left,
           const aurhub::CompiledPackageView& right) {
            if (left.name != right.name) {
                return left.name < right.name;
            }
            return left.branch_index < right.branch_index;
        });
    packages.clear();
    shadow_packages.clear();
    packages.reserve(raw_packages.size());
    for (std::size_t begin = 0; begin < raw_packages.size();) {
        std::size_t end = begin + 1;
        while (end < raw_packages.size() &&
               raw_packages[end].name == raw_packages[begin].name) {
            ++end;
        }
        shadow_packages.insert(shadow_packages.end(),
                               raw_packages.begin() +
                                   static_cast<std::ptrdiff_t>(begin),
                               raw_packages.begin() +
                                   static_cast<std::ptrdiff_t>(end - 1));
        packages.push_back(raw_packages[end - 1]);
        begin = end;
    }
}

void print_progress(std::size_t current,
                    std::size_t total,
                    std::size_t packages,
                    std::chrono::steady_clock::time_point started) {
    if (current % 1000 != 0 && current != total) {
        return;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
            .count();
    std::cerr << "\raurhub-indexer: " << current << '/' << total << " branches, "
              << packages << " packages, "
              << static_cast<std::size_t>(static_cast<double>(current) /
                                          std::max(seconds, 0.001))
              << " branches/s";
}

struct FullWorkerState {
    StringArena arena;
    std::vector<aurhub::CompiledPackageView> packages;
    ParseStats parse_stats;
};

void full_build(const Options& options,
                std::vector<aurhub::BranchInfo> branches,
                std::chrono::steady_clock::time_point started) {
    if (options.limit != 0 && branches.size() > options.limit) {
        branches.resize(options.limit);
    }
    if (branches.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("too many Git branches for snapshot format");
    }
    const std::size_t jobs =
        std::min(options.jobs, std::max<std::size_t>(branches.size(), 1));
    std::cerr << "aurhub-indexer: full build, " << branches.size()
              << " branches selected, jobs=" << jobs << '\n';

    std::vector<std::unique_ptr<FullWorkerState>> states;
    states.reserve(jobs);
    const std::size_t expected_per_worker =
        (branches.size() + jobs - 1U) / jobs;
    for (std::size_t i = 0; i < jobs; ++i) {
        auto state = std::make_unique<FullWorkerState>();
        state->packages.reserve(expected_per_worker +
                                expected_per_worker / 8U);
        states.push_back(std::move(state));
    }

    std::atomic<std::size_t> next_branch{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> package_count{0};
    std::atomic<bool> failed{false};
    std::mutex progress_mutex;
    std::mutex error_mutex;
    std::exception_ptr worker_error;
    auto run_worker = [&](FullWorkerState& state) {
        try {
            aurhub::GitBatch batch(options.repo);
            while (!failed.load(std::memory_order_relaxed)) {
                const std::size_t index =
                    next_branch.fetch_add(1, std::memory_order_relaxed);
                if (index >= branches.size()) {
                    break;
                }
                const std::size_t before = state.packages.size();
                append_branch_package_views(
                    batch, branches[index],
                    static_cast<std::uint32_t>(index), true, state.arena,
                    state.packages, state.parse_stats);
                const std::size_t total_packages =
                    package_count.fetch_add(state.packages.size() - before,
                                            std::memory_order_relaxed) +
                    state.packages.size() - before;
                const std::size_t current =
                    completed.fetch_add(1, std::memory_order_relaxed) + 1U;
                if (current % 1000U == 0 || current == branches.size()) {
                    const std::lock_guard<std::mutex> lock(progress_mutex);
                    print_progress(current, branches.size(), total_packages,
                                   started);
                }
            }
        } catch (...) {
            failed.store(true, std::memory_order_relaxed);
            const std::lock_guard<std::mutex> lock(error_mutex);
            if (!worker_error) {
                worker_error = std::current_exception();
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(jobs);
    try {
        for (std::size_t i = 0; i < jobs; ++i) {
            workers.emplace_back(run_worker, std::ref(*states[i]));
        }
    } catch (...) {
        failed.store(true, std::memory_order_relaxed);
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    if (worker_error) {
        std::rethrow_exception(worker_error);
    }
    std::cerr << '\n';

    ParseStats parse_stats;
    std::size_t raw_count = 0;
    for (std::unique_ptr<FullWorkerState>& state : states) {
        parse_stats.merge(std::move(state->parse_stats));
        raw_count += state->packages.size();
    }
    std::vector<aurhub::CompiledPackageView> raw_packages;
    raw_packages.reserve(raw_count);
    for (const std::unique_ptr<FullWorkerState>& state : states) {
        raw_packages.insert(raw_packages.end(), state->packages.begin(),
                            state->packages.end());
        std::vector<aurhub::CompiledPackageView>().swap(state->packages);
    }
    std::vector<aurhub::CompiledPackageView> packages;
    std::vector<aurhub::CompiledPackageView> shadow_packages;
    partition_package_views(raw_packages, packages, shadow_packages);
    std::vector<aurhub::CompiledPackageView>().swap(raw_packages);
    aurhub::write_snapshot(options.output, packages, shadow_packages, branches,
                           options.search_block_packages,
                           options.search_super_block_packages);
    print_parse_stats(parse_stats);
    if (!options.diagnostics.empty()) {
        write_diagnostics(options.diagnostics, branches,
                          std::move(parse_stats.diagnostics));
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
            .count();
    std::cerr << "aurhub-indexer: wrote " << packages.size() << " packages to "
              << options.output << " in " << seconds << "s (missing="
              << parse_stats.missing
              << ", quarantined=" << parse_stats.quarantined_branches
              << ", warning_branches=" << parse_stats.warning_branches
              << ", shadow=" << shadow_packages.size() << ")\n";
}

struct ChangeCounts {
    std::size_t added = 0;
    std::size_t updated = 0;
    std::size_t deleted = 0;

    std::size_t total() const { return added + updated + deleted; }
};

ChangeCounts count_changes(const aurhub::GenerationView& previous,
                           const std::vector<aurhub::BranchInfo>& remote) {
    ChangeCounts counts;
    std::size_t old_index = 0;
    std::size_t remote_index = 0;
    while (old_index < previous.branch_count() || remote_index < remote.size()) {
        if (old_index == previous.branch_count()) {
            counts.added += remote.size() - remote_index;
            break;
        }
        if (remote_index == remote.size()) {
            counts.deleted += previous.branch_count() - old_index;
            break;
        }
        const std::string_view old_name = previous.branch_name(old_index);
        const std::string& remote_name = remote[remote_index].name;
        if (old_name < remote_name) {
            ++counts.deleted;
            ++old_index;
        } else if (remote_name < old_name) {
            ++counts.added;
            ++remote_index;
        } else {
            if (previous.branch_oid(old_index) != remote[remote_index].oid) {
                ++counts.updated;
            }
            ++old_index;
            ++remote_index;
        }
    }
    return counts;
}

struct PendingBranch {
    std::size_t branch_index;
};

void incremental_build(const Options& options,
                       const aurhub::GenerationView& previous,
                       std::vector<aurhub::BranchInfo> branches,
                       const ChangeCounts& counts,
                       std::chrono::steady_clock::time_point started) {
    constexpr std::uint32_t kMissingBranch =
        std::numeric_limits<std::uint32_t>::max();
    std::vector<std::uint32_t> old_to_new(previous.branch_count(),
                                          kMissingBranch);
    std::vector<aurhub::GenerationLocation> branch_locations(
        branches.size(),
        {aurhub::GenerationSource::inline_record, 0});
    std::vector<PendingBranch> pending;
    pending.reserve(counts.added + counts.updated);

    std::size_t old_index = 0;
    for (std::size_t new_index = 0; new_index < branches.size(); ++new_index) {
        const std::string& remote_name = branches[new_index].name;
        while (old_index < previous.branch_count() &&
               previous.branch_name(old_index) < remote_name) {
            ++old_index;
        }
        if (old_index == previous.branch_count() ||
            remote_name < previous.branch_name(old_index)) {
            pending.push_back(PendingBranch{new_index});
            continue;
        }
        if (previous.branch_oid(old_index) == branches[new_index].oid) {
            branches[new_index].updated_at =
                previous.branch_updated_at(old_index);
            branch_locations[new_index] = previous.branch_location(old_index);
            old_to_new[old_index] = static_cast<std::uint32_t>(new_index);
        } else {
            pending.push_back(PendingBranch{new_index});
        }
        ++old_index;
    }

    aurhub::PackageNameTable previous_names;
    aurhub::decode_package_names(previous.packages_gz(), previous.package_count(),
                         previous_names);

    aurhub::GitBatch batch(options.repo);
    std::vector<aurhub::CompiledPackage> changed_packages;
    changed_packages.reserve(pending.size() * 2U);
    ParseStats parse_stats;
    for (std::size_t i = 0; i < pending.size(); ++i) {
        const std::size_t branch_index = pending[i].branch_index;
        append_branch_packages(batch, branches[branch_index],
                               static_cast<std::uint32_t>(branch_index), true,
                               changed_packages, parse_stats);
        print_progress(i + 1, pending.size(),
                       previous.package_count() +
                           previous.shadow_package_count() +
                           changed_packages.size(),
                       started);
    }
    if (!pending.empty()) {
        std::cerr << '\n';
    }
    print_parse_stats(parse_stats);

    std::vector<aurhub::GenerationPackageInput> packages;
    std::vector<aurhub::GenerationPackageInput> shadow_packages;
    std::vector<aurhub::GenerationPackageDetails> package_details;
    aurhub::merge_generation_inputs(previous, previous_names, old_to_new,
                            changed_packages, packages, shadow_packages,
                            package_details);
    std::size_t overlay_records = 0;
    for (const aurhub::GenerationLocation location : branch_locations) {
        if (location.source != aurhub::GenerationSource::base_branch) {
            ++overlay_records;
        }
    }
    const auto count_inline_package =
        [&](const aurhub::GenerationPackageInput& package) {
            if (package.location.source !=
                    aurhub::GenerationSource::base_active &&
                package.location.source !=
                    aurhub::GenerationSource::base_shadow) {
                ++overlay_records;
            }
        };
    for (const aurhub::GenerationPackageInput& package : packages) {
        count_inline_package(package);
    }
    for (const aurhub::GenerationPackageInput& package : shadow_packages) {
        count_inline_package(package);
    }

    const auto matches_base_exactly = [&] {
        const aurhub::SnapshotView& base = previous.base_snapshot();
        if (branches.size() != base.branch_count() ||
            packages.size() != base.package_count() ||
            shadow_packages.size() != base.shadow_package_count()) {
            return false;
        }
        for (std::size_t i = 0; i < branch_locations.size(); ++i) {
            if (branch_locations[i].source !=
                    aurhub::GenerationSource::base_branch ||
                branch_locations[i].index != i) {
                return false;
            }
        }
        for (std::size_t i = 0; i < packages.size(); ++i) {
            if (packages[i].location.source !=
                    aurhub::GenerationSource::base_active ||
                packages[i].location.index != i) {
                return false;
            }
        }
        for (std::size_t i = 0; i < shadow_packages.size(); ++i) {
            if (shadow_packages[i].location.source !=
                    aurhub::GenerationSource::base_shadow ||
                shadow_packages[i].location.index != i) {
                return false;
            }
        }
        return true;
    };
    const bool collapsed = previous.is_overlay() && overlay_records == 0 &&
                           matches_base_exactly();
    const bool compacted = !collapsed &&
                      (options.max_overlay_records == 0 ||
                       overlay_records > options.max_overlay_records);
    if (collapsed) {
        aurhub::restore_generation_base(options.output);
    } else if (compacted) {
        std::vector<aurhub::CompiledPackageView> package_views;
        std::vector<aurhub::CompiledPackageView> shadow_views;
        package_views.reserve(packages.size());
        shadow_views.reserve(shadow_packages.size());
        const auto expand_package =
            [&](const aurhub::GenerationPackageInput& package) {
                if (package.details_index != aurhub::kNoPackageDetails) {
                    const aurhub::GenerationPackageDetails& details =
                        package_details[package.details_index];
                    return aurhub::CompiledPackageView{
                        package.name, details.search, details.json,
                        package.branch_index, details.updated_at};
                }
                const aurhub::SnapshotView& base = previous.base_snapshot();
                if (package.location.source ==
                    aurhub::GenerationSource::base_active) {
                    return aurhub::CompiledPackageView{
                        package.name, base.search_text(package.location.index),
                        base.json(package.location.index), package.branch_index,
                        base.record(package.location.index).updated_at};
                }
                if (package.location.source ==
                    aurhub::GenerationSource::base_shadow) {
                    return aurhub::CompiledPackageView{
                        package.name,
                        base.shadow_search_text(package.location.index),
                        base.shadow_json(package.location.index),
                        package.branch_index,
                        base.shadow_record(package.location.index).updated_at};
                }
                throw std::runtime_error(
                    "compact package lacks reusable data");
            };
        for (const aurhub::GenerationPackageInput& package : packages) {
            package_views.push_back(expand_package(package));
        }
        for (const aurhub::GenerationPackageInput& package : shadow_packages) {
            shadow_views.push_back(expand_package(package));
        }
        aurhub::write_snapshot(
            options.output, package_views, shadow_views, branches,
            options.search_block_packages,
            options.search_super_block_packages);
    } else {
        if (!previous.is_overlay()) {
            aurhub::install_generation_base(options.output);
        }
        aurhub::write_generation_overlay(
            options.output, previous.base_snapshot(), branches,
            branch_locations, packages, shadow_packages, package_details);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
            .count();
    std::cerr << "aurhub-indexer: incremental update complete in " << seconds
              << "s (added=" << counts.added << ", updated=" << counts.updated
              << ", deleted=" << counts.deleted << ", packages="
              << packages.size() << ", shadow=" << shadow_packages.size()
              << ", missing=" << parse_stats.missing
              << ", quarantined=" << parse_stats.quarantined_branches
              << ", warning_branches=" << parse_stats.warning_branches
              << ", overlay_records=" << overlay_records
              << ", compacted=" << (compacted ? 1 : 0)
              << ", collapsed=" << (collapsed ? 1 : 0) << ")\n";
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    Options options;
    CLI::App app{"AUR snapshot indexer"};
    app.add_option("--repo", options.repo, "Git repository path")->required();
    app.add_option("--output", options.output, "Snapshot output path")->required();
    app.add_option("--diagnostics", options.diagnostics,
                   "Write diagnostics to file");
    app.add_option("--limit", options.limit, "Branch limit (0 = all)");
    app.add_option("--search-block-packages", options.search_block_packages,
                   "Packages per search block")
        ->check(CLI::Range(1U, 65535U));
    app.add_option("--search-super-block-packages",
                   options.search_super_block_packages,
                   "Packages per super block (0 = disabled)")
        ->check(CLI::Range(0U, 65535U));
    app.add_option("--jobs", options.jobs, "Worker threads")
        ->check(CLI::Range(1U, 256U));
    app.add_option("--max-overlay-records", options.max_overlay_records,
                   "Max overlay records before compaction");
    app.add_flag("--full", options.full, "Force full rebuild");
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }
    if (options.repo.find("://") != std::string::npos) {
        const std::filesystem::path url_path(options.repo);
        std::string local = url_path.filename().string();
        if (local.empty()) {
            local = "aur-mirror.git";
        }
        if (!local.ends_with(".git")) {
            local += ".git";
        }
        if (!std::filesystem::exists(local)) {
            if (std::system(("git -c protocol.version=2 clone --bare"
                             " --depth=1 --no-single-branch " +
                             options.repo + " " + local)
                                .c_str()) != 0) {
                std::cerr << "aurhub-indexer: git clone failed\n";
                return 1;
            }
        } else {
            if (std::system(("git --git-dir=" + local + " fetch " +
                             options.repo +
                             " +refs/heads/*:refs/heads/*")
                                .c_str()) != 0) {
                std::cerr << "aurhub-indexer: git fetch failed\n";
                return 1;
            }
        }
        options.repo = std::filesystem::absolute(local)
                           .lexically_normal()
                           .string();
    }
    if (options.search_super_block_packages != 0 &&
        (options.search_super_block_packages < options.search_block_packages ||
         options.search_super_block_packages %
                 options.search_block_packages !=
             0)) {
        std::cerr
            << "aurhub-indexer: search-super-block-packages must be a multiple"
               " of search-block-packages\n";
        return 1;
    }
    try {
        const auto started = std::chrono::steady_clock::now();
        if (options.diagnostics.empty() && !options.full && options.limit == 0 &&
            std::filesystem::exists(options.output)) {
            try {
                const aurhub::GenerationView previous(
                    options.output, aurhub::SnapshotValidation::records_only);
                if (previous.base_snapshot().search_block_packages() ==
                        options.search_block_packages &&
                    previous.base_snapshot().search_super_block_packages() ==
                        options.search_super_block_packages) {
                    std::vector<aurhub::BranchInfo> remote =
                        aurhub::list_branches_fast(options.repo);
                    const ChangeCounts counts = count_changes(previous, remote);
                    if (counts.total() == 0) {
                        const double seconds = std::chrono::duration<double>(
                                                   std::chrono::steady_clock::now() -
                                                   started)
                                                   .count();
                        std::cerr << "aurhub-indexer: already up to date in "
                                  << seconds << "s (" << remote.size()
                                  << " refs checked, no snapshot write)\n";
                        return 0;
                    }
                    incremental_build(options, previous, std::move(remote),
                                      counts, started);
                    return 0;
                }
                std::cerr << "aurhub-indexer: search layout changed; full rebuild\n";
            } catch (const std::exception& error) {
                std::cerr << "aurhub-indexer: cannot reuse snapshot: "
                          << error.what() << "; full rebuild\n";
            }
        }

        std::vector<aurhub::BranchInfo> branches =
            aurhub::list_branches_fast(options.repo);
        full_build(options, std::move(branches), started);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "aurhub-indexer: " << error.what() << '\n';
        return 1;
    }
}
