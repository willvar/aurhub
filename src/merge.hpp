#pragma once

#include "generation.hpp"
#include "model.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aurhub {

struct PackageNameTable {
    std::string storage;
    std::vector<std::uint32_t> starts;

    std::string_view name(std::size_t index) const {
        const std::size_t first = starts[index];
        const std::size_t after_newline = starts[index + 1U];
        return {storage.data() + first, after_newline - first - 1U};
    }
};

void decode_package_names(std::span<const std::byte> gzip,
                          std::size_t expected_count,
                          PackageNameTable& table);

inline constexpr std::uint32_t kNoPackageDetails =
    std::numeric_limits<std::uint32_t>::max();

void merge_generation_inputs(
    const GenerationView& previous,
    const PackageNameTable& previous_names,
    std::span<const std::uint32_t> old_to_new,
    std::vector<CompiledPackage>& changed_packages,
    std::vector<GenerationPackageInput>& packages,
    std::vector<GenerationPackageInput>& shadow_packages,
    std::vector<GenerationPackageDetails>& package_details);

}  // namespace aurhub
