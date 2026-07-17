#include "search.hpp"

#include <algorithm>
#include <cstdint>

namespace aurhub {
namespace {

std::uint32_t trigram_hash(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

bool trigram_maybe_present(const SearchBlock& block, std::uint32_t trigram) {
    const std::uint32_t mixed = trigram_hash(trigram);
    const std::uint16_t first = static_cast<std::uint16_t>(mixed);
    const std::uint16_t second = static_cast<std::uint16_t>(mixed >> 16U);
    return (block.trigrams[first >> 6U] &
            (std::uint64_t{1} << (first & 63U))) != 0 &&
           (block.trigrams[second >> 6U] &
            (std::uint64_t{1} << (second & 63U))) != 0;
}

bool bitmap_contains(const SearchBlock& block, std::string_view query) {
    if (query.empty()) {
        return false;
    }
    if (query.size() == 1) {
        const auto byte = static_cast<unsigned char>(query.front());
        return (block.bytes[byte >> 6U] &
                (std::uint64_t{1} << (byte & 63U))) != 0;
    }

    for (std::size_t i = 2; i < query.size(); ++i) {
        const auto first = static_cast<std::uint32_t>(
            static_cast<unsigned char>(query[i - 2]));
        const auto second = static_cast<std::uint32_t>(
            static_cast<unsigned char>(query[i - 1]));
        const auto third =
            static_cast<std::uint32_t>(static_cast<unsigned char>(query[i]));
        if (!trigram_maybe_present(
                block, (first << 16U) | (second << 8U) | third)) {
            return false;
        }
    }

    for (std::size_t i = 1; i < query.size(); ++i) {
        const auto high = static_cast<std::uint16_t>(
            static_cast<unsigned char>(query[i - 1]));
        const auto low =
            static_cast<std::uint16_t>(static_cast<unsigned char>(query[i]));
        const std::uint16_t bigram = static_cast<std::uint16_t>((high << 8U) | low);
        if ((block.bigrams[bigram >> 6U] &
             (std::uint64_t{1} << (bigram & 63U))) == 0) {
            return false;
        }
    }
    return true;
}

template <bool FilterKept>
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void scan_range(const SnapshotView& snapshot,
                std::string_view query,
                std::size_t first,
                std::size_t count,
                std::size_t max_results,
                std::span<const std::uint8_t> kept,
                std::vector<std::size_t>& results,
                SearchStats* stats) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    const std::size_t end = first + count;
    for (std::size_t index = first;
         index < end && results.size() < max_results; ++index) {
        if (stats != nullptr) {
            ++stats->packages_scanned;
        }
        if constexpr (FilterKept) {
            if (kept[index] == 0) {
                continue;
            }
        }
        if (snapshot.search_text(index).contains(query)) {
            results.push_back(index);
        }
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void scan_blocks(const SnapshotView& snapshot,
                 std::string_view normalized_query,
                 std::size_t first_block,
                 std::size_t end_block,
                 std::size_t max_results,
                 std::span<const std::uint8_t> kept,
                 std::span<const std::uint8_t> dirty_blocks,
                 std::vector<std::size_t>& results,
                 SearchStats* stats) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    for (std::size_t block_index = first_block;
         block_index < end_block && results.size() < max_results;
         ++block_index) {
        const SearchBlock& block = snapshot.search_block(block_index);
        if (stats != nullptr) {
            ++stats->blocks_checked;
        }
        if (!bitmap_contains(block, normalized_query)) {
            continue;
        }
        if (stats != nullptr) {
            ++stats->blocks_scanned;
        }
        const std::size_t first_package =
            block_index * snapshot.search_block_packages();
        const std::size_t package_count = std::min(
            snapshot.search_block_packages(),
            snapshot.package_count() - first_package);
        if (!dirty_blocks.empty() && dirty_blocks[block_index] != 0) {
            scan_range<true>(snapshot, normalized_query, first_package,
                             package_count, max_results, kept, results,
                             stats);
        } else {
            scan_range<false>(snapshot, normalized_query, first_package,
                              package_count, max_results, {}, results,
                              stats);
        }
    }
}

}  // namespace

std::string normalize_search_query(std::string_view value) {
    std::string out(value);
    for (char& ch : out) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z')) {
            ch = static_cast<char>(byte + ('a' - 'A'));
        }
    }
    return out;
}

void search_packages(const SnapshotView& snapshot,
                     std::string_view normalized_query,
                     std::size_t max_results,
                     std::vector<std::size_t>& results,
                     SearchStats* stats) {
    search_packages_kept(snapshot, normalized_query, max_results, {},
                         {}, results, stats);
}

void search_packages_kept(const SnapshotView& snapshot,
                          std::string_view normalized_query,
                          std::size_t max_results,
                          std::span<const std::uint8_t> kept,
                          std::span<const std::uint8_t> dirty_blocks,
                          std::vector<std::size_t>& results,
                          SearchStats* stats) {
    results.clear();
    if (stats != nullptr) {
        *stats = {};
    }
    if (results.capacity() < max_results) {
        results.reserve(max_results);
    }
    if (normalized_query.empty() || max_results == 0) {
        return;
    }
    if (!kept.empty() && kept.size() != snapshot.package_count()) {
        return;
    }
    if (!dirty_blocks.empty() &&
        dirty_blocks.size() != snapshot.search_block_count()) {
        return;
    }

    if (snapshot.search_super_block_count() == 0) {
        scan_blocks(snapshot, normalized_query, 0, snapshot.search_block_count(),
                    max_results, kept, dirty_blocks, results, stats);
        return;
    }

    const std::size_t fine_block_packages = snapshot.search_block_packages();
    for (std::size_t super_index = 0;
         super_index < snapshot.search_super_block_count() &&
         results.size() < max_results;
        ++super_index) {
        const SearchBlock& super_block = snapshot.search_super_block(super_index);
        const std::size_t super_first =
            super_index * snapshot.search_super_block_packages();
        const std::size_t super_end =
            std::min(snapshot.package_count(),
                     super_first + snapshot.search_super_block_packages());
        const std::size_t fine_begin = super_first / fine_block_packages;
        const std::size_t fine_end = std::min(
            snapshot.search_block_count(),
            (super_end + fine_block_packages - 1U) / fine_block_packages);
        if (stats != nullptr) {
            ++stats->super_blocks_checked;
        }
        if (!bitmap_contains(super_block, normalized_query)) {
            continue;
        }
        if (stats != nullptr) {
            ++stats->super_blocks_scanned;
        }
        scan_blocks(snapshot, normalized_query, fine_begin, fine_end,
                    max_results, kept, dirty_blocks, results, stats);
    }
}

}  // namespace aurhub
