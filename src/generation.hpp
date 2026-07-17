#pragma once

#include "model.hpp"
#include "search.hpp"
#include "snapshot.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aurhub {

inline constexpr std::array<char, 8> kGenerationMagic{
    'A', 'U', 'R', 'H', 'O', 'V', 'R', '\0'};
inline constexpr std::uint32_t kGenerationVersion = 1;

enum class GenerationSource : std::uint8_t {
    base_active = 0,
    base_shadow = 1,
    base_branch = 2,
    inline_record = 3,
};

struct GenerationLocation {
    GenerationSource source = GenerationSource::inline_record;
    std::uint32_t index = 0;
};

struct GenerationBranchRef {
    std::uint32_t source;
    std::uint32_t index;
};

struct GenerationPackageRef {
    std::uint32_t source;
    std::uint32_t index;
    std::uint32_t branch_index;
    std::uint32_t reserved;
};

struct InlineBranchRecord {
    Slice32 name;
    std::array<std::byte, 20> oid;
    std::uint32_t reserved;
    std::int64_t updated_at;
};

struct InlinePackageRecord {
    Slice32 name;
    Slice32 search;
    Slice32 json;
    std::int64_t updated_at;
};

struct GenerationHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint64_t base_created_at;
    std::uint64_t base_file_size;
    std::uint64_t branch_count;
    std::uint64_t package_count;
    std::uint64_t shadow_package_count;
    std::uint64_t branch_refs_offset;
    std::uint64_t package_refs_offset;
    std::uint64_t shadow_package_refs_offset;
    std::uint64_t inline_branches_offset;
    std::uint64_t inline_branch_count;
    std::uint64_t inline_packages_offset;
    std::uint64_t inline_package_count;
    std::uint64_t strings_offset;
    std::uint64_t strings_size;
    std::uint64_t search_texts_offset;
    std::uint64_t search_texts_size;
    std::uint64_t packages_gz_offset;
    std::uint64_t packages_gz_size;
    std::int64_t created_at;
    std::uint32_t flags;
    std::uint32_t reserved32;
    std::uint64_t reserved[2];
};

static_assert(sizeof(GenerationBranchRef) == 8);
static_assert(sizeof(GenerationPackageRef) == 16);
static_assert(sizeof(InlineBranchRecord) == 40);
static_assert(sizeof(InlinePackageRecord) == 32);
static_assert(sizeof(GenerationHeader) == 192);

struct GenerationPackageInput {
    std::string_view name;
    GenerationLocation location;
    std::uint32_t branch_index;
    std::uint32_t details_index;
};

struct GenerationPackageDetails {
    std::string_view search;
    std::string_view json;
    std::int64_t updated_at;
};

static_assert(sizeof(GenerationPackageInput) == 32);
static_assert(sizeof(GenerationPackageDetails) == 40);

class GenerationView {
public:
    explicit GenerationView(
        const std::string& path,
         SnapshotValidation validation = SnapshotValidation::records_only);
    ~GenerationView();

    GenerationView(const GenerationView&) = delete;
    GenerationView& operator=(const GenerationView&) = delete;
    GenerationView(GenerationView&&) = delete;
    GenerationView& operator=(GenerationView&&) = delete;

    bool is_overlay() const;
    const SnapshotView& base_snapshot() const;
    std::size_t branch_count() const;
    std::size_t package_count() const;
    std::size_t shadow_package_count() const;
    std::int64_t created_at() const;

    std::string_view branch_name(std::size_t index) const;
    const std::array<std::byte, 20>& branch_oid(std::size_t index) const;
    std::int64_t branch_updated_at(std::size_t index) const;
    BranchInfo branch_info(std::size_t index) const;
    GenerationLocation branch_location(std::size_t index) const;

    std::string_view name(std::size_t index) const;
    std::optional<std::size_t> find_package(std::string_view name) const;
    std::string_view search_text(std::size_t index) const;
    std::string_view json(std::size_t index) const;
    std::int64_t updated_at(std::size_t index) const;
    GenerationLocation location(std::size_t index) const;
    std::uint32_t package_branch_index(std::size_t index) const;

    std::string_view shadow_name(std::size_t index) const;
    std::string_view shadow_search_text(std::size_t index) const;
    std::string_view shadow_json(std::size_t index) const;
    std::int64_t shadow_updated_at(std::size_t index) const;
    GenerationLocation shadow_location(std::size_t index) const;
    std::uint32_t shadow_branch_index(std::size_t index) const;

    void search(std::string_view normalized_query,
                std::size_t max_results,
                std::vector<std::size_t>& results,
                SearchStats* stats = nullptr) const;

    std::span<const std::byte> packages_gz() const;
    int packages_gz_file_descriptor() const;
    std::uint64_t packages_gz_file_offset() const;

private:
    const GenerationBranchRef& branch_ref(std::size_t index) const;
    const GenerationPackageRef& package_ref(std::size_t index) const;
    const GenerationPackageRef& shadow_package_ref(std::size_t index) const;
    const InlineBranchRecord& inline_branch(std::size_t index) const;
    const InlinePackageRecord& inline_package(std::size_t index) const;
    std::string_view string_slice(Slice32 slice) const;
    std::string_view search_slice(Slice32 slice) const;
    std::string_view base_string_slice(Slice32 slice) const;
    std::string_view base_search_slice(Slice32 slice) const;
    std::string_view package_name(const GenerationPackageRef& ref) const;
    std::string_view package_search(const GenerationPackageRef& ref) const;
    std::string_view package_json(const GenerationPackageRef& ref) const;
    std::int64_t package_updated_at(const GenerationPackageRef& ref) const;
    void close_overlay();

    std::shared_ptr<SnapshotView> base_;
    const PackageRecord* base_records_ = nullptr;
    const PackageRecord* base_shadow_records_ = nullptr;
    const char* base_strings_ = nullptr;
    const char* base_search_texts_ = nullptr;
    int overlay_fd_ = -1;
    void* overlay_mapping_ = nullptr;
    std::size_t overlay_mapping_size_ = 0;
    const GenerationHeader* header_ = nullptr;
    const GenerationBranchRef* branch_refs_ = nullptr;
    const GenerationPackageRef* package_refs_ = nullptr;
    const GenerationPackageRef* shadow_package_refs_ = nullptr;
    const InlineBranchRecord* inline_branches_ = nullptr;
    const InlinePackageRecord* inline_packages_ = nullptr;
    const char* strings_ = nullptr;
    const char* search_texts_ = nullptr;
    const std::byte* packages_gz_ = nullptr;
    std::vector<std::uint8_t> base_active_kept_;
    std::vector<std::uint8_t> base_dirty_search_blocks_;
    std::vector<std::uint32_t> base_to_current_;
    std::vector<std::uint32_t> extra_active_;
};

bool generation_is_overlay(const std::string& path);
void install_generation_base(const std::string& output_path);
void restore_generation_base(const std::string& output_path);

void write_generation_overlay(
    const std::string& path,
    const SnapshotView& base,
    std::span<const BranchInfo> branches,
    std::span<const GenerationLocation> branch_locations,
    std::span<const GenerationPackageInput> packages,
    std::span<const GenerationPackageInput> shadow_packages,
    std::span<const GenerationPackageDetails> package_details);

}  // namespace aurhub
