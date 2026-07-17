#pragma once

#include "model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aurhub {

inline constexpr std::array<char, 8> kSnapshotMagic{
    'A', 'U', 'R', 'H', 'S', 'N', 'P', '\0'};
inline constexpr std::uint32_t kSnapshotVersion = 1;
inline constexpr std::uint32_t kDefaultSearchBlockPackages = 64;
inline constexpr std::uint32_t kDefaultSearchSuperBlockPackages = 512;
inline constexpr std::size_t kByteBitmapWords = 256 / 64;
inline constexpr std::size_t kBitmapWords = 65536 / 64;

struct Slice32 {
    std::uint32_t offset;
    std::uint32_t size;
};

struct SnapshotHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint64_t package_count;
    std::uint64_t shadow_package_count;
    std::uint64_t branch_count;
    std::uint64_t records_offset;
    std::uint64_t shadow_records_offset;
    std::uint64_t branches_offset;
    std::uint64_t search_blocks_offset;
    std::uint64_t search_block_count;
    std::uint64_t strings_offset;
    std::uint64_t strings_size;
    std::uint64_t search_texts_offset;
    std::uint64_t search_texts_size;
    std::uint64_t packages_gz_offset;
    std::uint64_t packages_gz_size;
    std::int64_t created_at;
    std::uint64_t search_super_blocks_offset;
    std::uint64_t search_super_block_count;
    std::uint32_t search_block_packages;
    std::uint32_t search_super_block_packages;
    std::uint32_t flags;
    std::uint32_t reserved32;
    std::uint64_t reserved[1];
};

struct PackageRecord {
    Slice32 name;
    Slice32 search;
    Slice32 json;
    std::uint32_t branch_index;
    std::uint32_t reserved;
    std::int64_t updated_at;
};

struct BranchRecord {
    Slice32 name;
    std::array<std::byte, 20> oid;
    std::uint32_t reserved;
    std::int64_t updated_at;
};

struct SearchBlock {
    std::uint32_t first_package;
    std::uint16_t package_count;
    std::uint16_t reserved;
    std::array<std::uint64_t, kByteBitmapWords> bytes;
    std::array<std::uint64_t, kBitmapWords> bigrams;
    std::array<std::uint64_t, kBitmapWords> trigrams;
};

static_assert(sizeof(Slice32) == 8);
static_assert(sizeof(SnapshotHeader) == 176);
static_assert(sizeof(PackageRecord) == 40);
static_assert(sizeof(BranchRecord) == 40);
static_assert(sizeof(SearchBlock) == 16424);

void write_snapshot(const std::string& path,
                    std::span<const CompiledPackageView> packages,
                    std::span<const CompiledPackageView> shadow_packages,
                    const std::vector<BranchInfo>& branches,
                    std::uint32_t search_block_packages =
                        kDefaultSearchBlockPackages,
                    std::uint32_t search_super_block_packages =
                        kDefaultSearchSuperBlockPackages);

enum class SnapshotValidation : std::uint8_t {
    full,
    records_only,
};

class SnapshotView {
public:
    SnapshotView() = default;
    explicit SnapshotView(
        const std::string& path,
        SnapshotValidation validation = SnapshotValidation::records_only);
    ~SnapshotView();

    SnapshotView(const SnapshotView&) = delete;
    SnapshotView& operator=(const SnapshotView&) = delete;
    SnapshotView(SnapshotView&& other) noexcept;
    SnapshotView& operator=(SnapshotView&& other) noexcept;

    std::size_t package_count() const;
    std::size_t shadow_package_count() const;
    std::size_t branch_count() const;
    std::size_t search_block_packages() const;
    std::size_t search_super_block_packages() const;
    std::size_t search_block_count() const;
    std::size_t search_super_block_count() const;
    std::int64_t created_at() const;
    const PackageRecord& record(std::size_t index) const;
    const PackageRecord& shadow_record(std::size_t index) const;
    const PackageRecord* records_data() const;
    const PackageRecord* shadow_records_data() const;
    const char* strings_data() const;
    const char* search_texts_data() const;
    const BranchRecord& branch_record(std::size_t index) const;
    const SearchBlock& search_block(std::size_t index) const;
    const SearchBlock& search_super_block(std::size_t index) const;
    std::string_view name(std::size_t index) const;
    std::string_view shadow_name(std::size_t index) const;
    std::string_view branch_name(std::size_t index) const;
    BranchInfo branch_info(std::size_t index) const;
    std::string_view search_text(std::size_t index) const;
    std::string_view shadow_search_text(std::size_t index) const;
    std::string_view json(std::size_t index) const;
    std::string_view shadow_json(std::size_t index) const;
    std::span<const std::byte> packages_gz() const;
    int file_descriptor() const;
    std::uint64_t packages_gz_file_offset() const;

private:
    std::string_view string_slice(Slice32 slice) const;
    void close();

    int fd_ = -1;
    void* mapping_ = nullptr;
    std::size_t mapping_size_ = 0;
    const SnapshotHeader* header_ = nullptr;
    const PackageRecord* records_ = nullptr;
    const PackageRecord* shadow_records_ = nullptr;
    const BranchRecord* branches_ = nullptr;
    const SearchBlock* search_blocks_ = nullptr;
    const SearchBlock* search_super_blocks_ = nullptr;
    const char* strings_ = nullptr;
    const char* search_texts_ = nullptr;
    const std::byte* packages_gz_ = nullptr;
};

}  // namespace aurhub
