#pragma once

#include "snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aurhub {

struct SearchStats {
    std::size_t super_blocks_checked = 0;
    std::size_t super_blocks_scanned = 0;
    std::size_t blocks_checked = 0;
    std::size_t blocks_scanned = 0;
    std::size_t packages_scanned = 0;
};

std::string normalize_search_query(std::string_view value);

void search_packages(const SnapshotView& snapshot,
                     std::string_view normalized_query,
                     std::size_t max_results,
                     std::vector<std::size_t>& results,
                     SearchStats* stats = nullptr);

void search_packages_kept(const SnapshotView& snapshot,
                          std::string_view normalized_query,
                          std::size_t max_results,
                          std::span<const std::uint8_t> kept,
                          std::span<const std::uint8_t> dirty_blocks,
                          std::vector<std::size_t>& results,
                          SearchStats* stats = nullptr);

}  // namespace aurhub
