#include "merge.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <zlib.h>

namespace aurhub {

namespace {

enum class MergeSource : std::uint8_t {
    previous_shadow,
    previous_active,
    changed,
};

struct MergeCandidate {
    std::string_view name;
    std::uint32_t branch_index;
    std::size_t index;
    MergeSource source;
};

bool merge_candidate_less(const MergeCandidate& left,
                          const MergeCandidate& right) {
    if (left.name != right.name) {
        return left.name < right.name;
    }
    if (left.branch_index != right.branch_index) {
        return left.branch_index < right.branch_index;
    }
    return static_cast<std::uint8_t>(left.source) <
           static_cast<std::uint8_t>(right.source);
}

}  // namespace

void decode_package_names(std::span<const std::byte> gzip,
                          std::size_t expected_count,
                          PackageNameTable& table) {
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        throw std::runtime_error("inflateInit2 failed for packages.gz");
    }

    if (expected_count <=
        std::numeric_limits<std::size_t>::max() / 24U) {
        table.storage.reserve(expected_count * 24U);
    }
    const auto* input = reinterpret_cast<const Bytef*>(gzip.data());
    std::size_t remaining = gzip.size();
    std::array<char, static_cast<std::size_t>(64) * 1024> output{};
    int status = Z_OK;
    try {
        while (status != Z_STREAM_END) {
            if (stream.avail_in == 0 && remaining != 0) {
                const std::size_t chunk = std::min<std::size_t>(
                    remaining, std::numeric_limits<uInt>::max());
                stream.next_in = const_cast<Bytef*>(input);
                stream.avail_in = static_cast<uInt>(chunk);
                input += chunk;
                remaining -= chunk;
            }
            stream.next_out = reinterpret_cast<Bytef*>(output.data());
            stream.avail_out = static_cast<uInt>(output.size());
            status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) {
                throw std::runtime_error("cannot inflate packages.gz");
            }
            const std::size_t produced = output.size() - stream.avail_out;
            table.storage.append(output.data(), produced);
            if (status != Z_STREAM_END && produced == 0 &&
                stream.avail_in == 0 && remaining == 0) {
                throw std::runtime_error("truncated packages.gz");
            }
        }
        if (stream.avail_in != 0 || remaining != 0) {
            throw std::runtime_error("packages.gz has trailing data");
        }
    } catch (...) {
        inflateEnd(&stream);
        throw;
    }
    inflateEnd(&stream);

    if (table.storage.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("packages.gz name table exceeds 4 GiB");
    }
    table.starts.reserve(expected_count + 1U);
    table.starts.push_back(0);
    std::size_t first = 0;
    while (first < table.storage.size()) {
        const std::size_t newline = table.storage.find('\n', first);
        if (newline == std::string::npos) {
            throw std::runtime_error("packages.gz lacks a final newline");
        }
        first = newline + 1;
        table.starts.push_back(static_cast<std::uint32_t>(first));
    }
    if (table.starts.size() != expected_count + 1U) {
        throw std::runtime_error("packages.gz package count does not match");
    }
}

void merge_generation_inputs(
    const GenerationView& previous,
    const PackageNameTable& previous_names,
    std::span<const std::uint32_t> old_to_new,
    std::vector<CompiledPackage>& changed_packages,
    std::vector<GenerationPackageInput>& packages,
    std::vector<GenerationPackageInput>& shadow_packages,
    std::vector<GenerationPackageDetails>& package_details) {
    constexpr std::uint32_t kMissingBranch =
        std::numeric_limits<std::uint32_t>::max();
    std::stable_sort(
        changed_packages.begin(), changed_packages.end(),
        [](const CompiledPackage& left, const CompiledPackage& right) {
            if (left.name != right.name) {
                return left.name < right.name;
            }
            return left.branch_index < right.branch_index;
        });

    std::size_t shadow_index = 0;
    std::size_t active_index = 0;
    std::size_t changed_index = 0;
    const auto next_shadow = [&]() -> std::optional<MergeCandidate> {
        while (shadow_index < previous.shadow_package_count()) {
            const std::uint32_t branch_index =
                old_to_new[previous.shadow_branch_index(shadow_index)];
            if (branch_index != kMissingBranch) {
                return MergeCandidate{
                    previous.shadow_name(shadow_index), branch_index,
                    shadow_index, MergeSource::previous_shadow};
            }
            ++shadow_index;
        }
        return std::nullopt;
    };
    const auto next_active = [&]() -> std::optional<MergeCandidate> {
        while (active_index < previous.package_count()) {
            const std::uint32_t branch_index =
                old_to_new[previous.package_branch_index(active_index)];
            if (branch_index != kMissingBranch) {
                return MergeCandidate{
                    previous_names.name(active_index), branch_index,
                    active_index, MergeSource::previous_active};
            }
            ++active_index;
        }
        return std::nullopt;
    };
    const auto next_changed = [&]() -> std::optional<MergeCandidate> {
        if (changed_index == changed_packages.size()) {
            return std::nullopt;
        }
        const CompiledPackage& package =
            changed_packages[changed_index];
        return MergeCandidate{package.name, package.branch_index, changed_index,
                              MergeSource::changed};
    };

    std::optional<MergeCandidate> shadow = next_shadow();
    std::optional<MergeCandidate> active = next_active();
    std::optional<MergeCandidate> changed = next_changed();
    package_details.clear();
    package_details.reserve(changed_packages.size() + 64U);
    const auto add_details = [&](std::string_view search,
                                  std::string_view json,
                                  std::int64_t updated_at) {
        if (package_details.size() >= kNoPackageDetails) {
            throw std::runtime_error("too many inline package details");
        }
        const std::uint32_t index =
            static_cast<std::uint32_t>(package_details.size());
        package_details.push_back(
            GenerationPackageDetails{search, json, updated_at});
        return index;
    };
    const auto make_input = [&](const MergeCandidate& candidate) {
        switch (candidate.source) {
            case MergeSource::previous_shadow: {
                const GenerationLocation location =
                    previous.shadow_location(candidate.index);
                std::uint32_t details_index = kNoPackageDetails;
                if (location.source != GenerationSource::base_active &&
                    location.source != GenerationSource::base_shadow) {
                    details_index = add_details(
                        previous.shadow_search_text(candidate.index),
                        previous.shadow_json(candidate.index),
                        previous.shadow_updated_at(candidate.index));
                }
                return GenerationPackageInput{
                    candidate.name, location, candidate.branch_index,
                    details_index,
                };
            }
            case MergeSource::previous_active: {
                const GenerationLocation location =
                    previous.location(candidate.index);
                std::uint32_t details_index = kNoPackageDetails;
                if (location.source != GenerationSource::base_active &&
                    location.source != GenerationSource::base_shadow) {
                    details_index = add_details(
                        previous.search_text(candidate.index),
                        previous.json(candidate.index),
                        previous.updated_at(candidate.index));
                }
                return GenerationPackageInput{
                    candidate.name, location, candidate.branch_index,
                    details_index,
                };
            }
            case MergeSource::changed: {
                const CompiledPackageView package =
                    package_view(changed_packages[candidate.index]);
                return GenerationPackageInput{
                    package.name,
                    {GenerationSource::inline_record, 0},
                    package.branch_index,
                    add_details(package.search, package.json,
                                package.updated_at),
                };
            }
        }
        throw std::runtime_error("invalid package merge source");
    };
    const auto consume = [&](MergeSource source) {
        switch (source) {
            case MergeSource::previous_shadow:
                ++shadow_index;
                shadow = next_shadow();
                break;
            case MergeSource::previous_active:
                ++active_index;
                active = next_active();
                break;
            case MergeSource::changed:
                ++changed_index;
                changed = next_changed();
                break;
        }
    };

    packages.clear();
    shadow_packages.clear();
    packages.reserve(previous.package_count() + changed_packages.size());
    shadow_packages.reserve(previous.shadow_package_count() +
                            changed_packages.size());
    std::optional<GenerationPackageInput> winner;
    while (shadow || active || changed) {
        const MergeCandidate* candidate = nullptr;
        for (const std::optional<MergeCandidate>* current :
              {&shadow, &active, &changed}) {
            if (*current &&
                (candidate == nullptr ||
                  merge_candidate_less(**current, *candidate))) {
                candidate = &**current;
            }
        }
        if (candidate == nullptr) {
            throw std::runtime_error("package merge lost its candidate");
        }
        const GenerationPackageInput input = make_input(*candidate);
        const MergeSource source = candidate->source;
        consume(source);
        if (winner && winner->name == input.name) {
            shadow_packages.push_back(*winner);
        } else if (winner) {
            packages.push_back(*winner);
        }
        winner = input;
    }
    if (winner) {
        packages.push_back(*winner);
    }
}

}  // namespace aurhub
