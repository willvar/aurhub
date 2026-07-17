#include "generation.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <zlib.h>

namespace aurhub {
namespace {

std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

bool region_valid(std::uint64_t offset,
                  std::uint64_t size,
                  std::uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

bool slice_valid(Slice32 slice, std::uint64_t section_size) {
    return slice.offset <= section_size &&
           slice.size <= section_size - slice.offset;
}

void append_slice(std::string& blob, Slice32& slice, std::string_view value) {
    if (blob.size() > std::numeric_limits<std::uint32_t>::max() ||
        value.size() > std::numeric_limits<std::uint32_t>::max() ||
        value.size() >
            std::numeric_limits<std::uint32_t>::max() - blob.size()) {
        throw std::runtime_error("generation string section exceeds 4 GiB");
    }
    slice.offset = static_cast<std::uint32_t>(blob.size());
    slice.size = static_cast<std::uint32_t>(value.size());
    blob.append(value);
}

void write_all(int fd, const void* data, std::size_t size) {
    const auto* cursor = static_cast<const std::byte*>(data);
    while (size != 0) {
        const ssize_t written = ::write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("write generation: ") +
                                     std::strerror(errno));
        }
        cursor += written;
        size -= static_cast<std::size_t>(written);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void write_padding(int fd, std::uint64_t bytes) {
    static constexpr std::array<std::byte, 64> zeros{};
    while (bytes != 0) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes, zeros.size()));
        write_all(fd, zeros.data(), count);
        bytes -= count;
    }
}

std::vector<std::byte> gzip_names(
    std::span<const GenerationPackageInput> packages) {
    std::string names;
    for (const GenerationPackageInput& package : packages) {
        names.append(package.name).push_back('\n');
    }
    if (names.size() > std::numeric_limits<uInt>::max()) {
        throw std::runtime_error("generation package list exceeds zlib limit");
    }

    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }
    std::vector<std::byte> output(
        compressBound(static_cast<uLong>(names.size())) + 64U);
    stream.next_in = reinterpret_cast<Bytef*>(names.data());
    stream.avail_in = static_cast<uInt>(names.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    const int status = deflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        deflateEnd(&stream);
        throw std::runtime_error("generation gzip compression failed");
    }
    output.resize(stream.total_out);
    deflateEnd(&stream);
    return output;
}

template <typename T>
std::uint64_t byte_size(std::size_t count) {
    if (count > std::numeric_limits<std::uint64_t>::max() / sizeof(T)) {
        throw std::runtime_error("generation table is too large");
    }
    return static_cast<std::uint64_t>(count) * sizeof(T);
}

std::uint32_t source_value(GenerationSource source) {
    return static_cast<std::uint32_t>(source);
}

GenerationSource checked_source(std::uint32_t source) {
    if (source > source_value(GenerationSource::inline_record)) {
        throw std::runtime_error("generation contains an invalid source");
    }
    return static_cast<GenerationSource>(source);
}

}  // namespace

bool generation_is_overlay(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    std::array<char, 8> magic{};
    ssize_t count = 0;
    do {
        count = ::read(fd, magic.data(), magic.size());
    } while (count < 0 && errno == EINTR);
    ::close(fd);
    return count == static_cast<ssize_t>(magic.size()) &&
           magic == kGenerationMagic;
}

void replace_with_hard_link(const std::string& source,
                            const std::string& destination) {
    const std::string temporary =
        destination + ".tmp." + std::to_string(::getpid());
    ::unlink(temporary.c_str());
    if (::link(source.c_str(), temporary.c_str()) != 0) {
        throw std::runtime_error(std::string("link generation: ") +
                                 std::strerror(errno));
    }
    if (::rename(temporary.c_str(), destination.c_str()) != 0) {
        const std::string error = std::strerror(errno);
        ::unlink(temporary.c_str());
        throw std::runtime_error("rename generation link: " + error);
    }
}

void install_generation_base(const std::string& output_path) {
    replace_with_hard_link(output_path, output_path + ".base");
}

void restore_generation_base(const std::string& output_path) {
    replace_with_hard_link(output_path + ".base", output_path);
}

GenerationView::GenerationView(const std::string& path,
                               SnapshotValidation validation) {
    if (!generation_is_overlay(path)) {
        base_ = std::make_shared<SnapshotView>(path, validation);
        base_records_ = base_->records_data();
        base_shadow_records_ = base_->shadow_records_data();
        base_strings_ = base_->strings_data();
        base_search_texts_ = base_->search_texts_data();
        return;
    }

    overlay_fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (overlay_fd_ < 0) {
        throw std::runtime_error(std::string("open generation: ") +
                                 std::strerror(errno));
    }
    struct stat status {};
    if (::fstat(overlay_fd_, &status) != 0 || status.st_size < 0) {
        close_overlay();
        throw std::runtime_error("cannot stat generation");
    }
    overlay_mapping_size_ = static_cast<std::size_t>(status.st_size);
    if (overlay_mapping_size_ < sizeof(GenerationHeader)) {
        close_overlay();
        throw std::runtime_error("generation is smaller than its header");
    }
    overlay_mapping_ =
        ::mmap(nullptr, overlay_mapping_size_, PROT_READ, MAP_SHARED,
               overlay_fd_, 0);
    if (overlay_mapping_ == MAP_FAILED) {
        overlay_mapping_ = nullptr;
        close_overlay();
        throw std::runtime_error(std::string("mmap generation: ") +
                                 std::strerror(errno));
    }
    header_ = static_cast<const GenerationHeader*>(overlay_mapping_);
    if (std::memcmp(header_->magic, kGenerationMagic.data(),
                    kGenerationMagic.size()) != 0 ||
        header_->version != kGenerationVersion ||
        header_->header_size != sizeof(GenerationHeader)) {
        close_overlay();
        throw std::runtime_error("unsupported generation format");
    }

    const std::string base_path = path + ".base";
    try {
        base_ = std::make_shared<SnapshotView>(base_path, validation);
    } catch (...) {
        close_overlay();
        throw;
    }
    base_records_ = base_->records_data();
    base_shadow_records_ = base_->shadow_records_data();
    base_strings_ = base_->strings_data();
    base_search_texts_ = base_->search_texts_data();
    struct stat base_status {};
    if (::fstat(base_->file_descriptor(), &base_status) != 0 ||
        base_status.st_size < 0 ||
        static_cast<std::uint64_t>(base_status.st_size) !=
            header_->base_file_size ||
        static_cast<std::uint64_t>(base_->created_at()) !=
            header_->base_created_at) {
        close_overlay();
        base_.reset();
        throw std::runtime_error("generation base identity does not match");
    }

    const std::uint64_t file_size = overlay_mapping_size_;
    const std::uint64_t branch_refs_size =
        byte_size<GenerationBranchRef>(header_->branch_count);
    const std::uint64_t package_refs_size =
        byte_size<GenerationPackageRef>(header_->package_count);
    const std::uint64_t shadow_refs_size =
        byte_size<GenerationPackageRef>(header_->shadow_package_count);
    const std::uint64_t inline_branches_size =
        byte_size<InlineBranchRecord>(header_->inline_branch_count);
    const std::uint64_t inline_packages_size =
        byte_size<InlinePackageRecord>(header_->inline_package_count);
    if (!region_valid(header_->branch_refs_offset, branch_refs_size, file_size) ||
        !region_valid(header_->package_refs_offset, package_refs_size,
                      file_size) ||
        !region_valid(header_->shadow_package_refs_offset, shadow_refs_size,
                      file_size) ||
        !region_valid(header_->inline_branches_offset, inline_branches_size,
                      file_size) ||
        !region_valid(header_->inline_packages_offset, inline_packages_size,
                      file_size) ||
        !region_valid(header_->strings_offset, header_->strings_size,
                      file_size) ||
        !region_valid(header_->search_texts_offset, header_->search_texts_size,
                      file_size) ||
        !region_valid(header_->packages_gz_offset, header_->packages_gz_size,
                      file_size)) {
        close_overlay();
        base_.reset();
        throw std::runtime_error("generation contains an invalid region");
    }

    const auto* bytes = static_cast<const std::byte*>(overlay_mapping_);
    branch_refs_ = reinterpret_cast<const GenerationBranchRef*>(
        bytes + header_->branch_refs_offset);
    package_refs_ = reinterpret_cast<const GenerationPackageRef*>(
        bytes + header_->package_refs_offset);
    shadow_package_refs_ = reinterpret_cast<const GenerationPackageRef*>(
        bytes + header_->shadow_package_refs_offset);
    inline_branches_ = reinterpret_cast<const InlineBranchRecord*>(
        bytes + header_->inline_branches_offset);
    inline_packages_ = reinterpret_cast<const InlinePackageRecord*>(
        bytes + header_->inline_packages_offset);
    strings_ = reinterpret_cast<const char*>(bytes + header_->strings_offset);
    search_texts_ =
        reinterpret_cast<const char*>(bytes + header_->search_texts_offset);
    packages_gz_ = bytes + header_->packages_gz_offset;

    try {
        for (std::size_t i = 0; i < header_->inline_branch_count; ++i) {
            if (!slice_valid(inline_branches_[i].name, header_->strings_size)) {
                throw std::runtime_error(
                    "generation contains an invalid inline branch");
            }
        }
        for (std::size_t i = 0; i < header_->inline_package_count; ++i) {
            const InlinePackageRecord& record = inline_packages_[i];
            if (!slice_valid(record.name, header_->strings_size) ||
                !slice_valid(record.json, header_->strings_size) ||
                !slice_valid(record.search, header_->search_texts_size)) {
                throw std::runtime_error(
                    "generation contains an invalid inline package");
            }
        }
        bool have_base_branch = false;
        std::uint32_t last_base_branch = 0;
        for (std::size_t i = 0; i < branch_count(); ++i) {
            const GenerationBranchRef& ref = branch_ref(i);
            const GenerationSource source = checked_source(ref.source);
            if ((source == GenerationSource::base_branch &&
                 ref.index >= base_->branch_count()) ||
                (source == GenerationSource::inline_record &&
                 ref.index >= header_->inline_branch_count) ||
                (source != GenerationSource::base_branch &&
                 source != GenerationSource::inline_record)) {
                throw std::runtime_error(
                    "generation contains an invalid branch reference");
            }
            if (source == GenerationSource::base_branch) {
                if (have_base_branch && ref.index <= last_base_branch) {
                    throw std::runtime_error(
                        "generation base branches are not ordered");
                }
                have_base_branch = true;
                last_base_branch = ref.index;
            } else {
                if ((i != 0 && branch_name(i - 1) >= branch_name(i)) ||
                    (i + 1 < branch_count() &&
                     branch_name(i) >= branch_name(i + 1))) {
                    throw std::runtime_error(
                        "generation branches are not sorted");
                }
            }
        }
        auto validate_package_ref = [&](const GenerationPackageRef& ref) {
            const GenerationSource source = checked_source(ref.source);
            const bool valid =
                (source == GenerationSource::base_active &&
                 ref.index < base_->package_count()) ||
                (source == GenerationSource::base_shadow &&
                 ref.index < base_->shadow_package_count()) ||
                (source == GenerationSource::inline_record &&
                 ref.index < header_->inline_package_count);
            if (!valid || ref.branch_index >= branch_count()) {
                throw std::runtime_error(
                    "generation contains an invalid package reference");
            }
        };
        bool have_base_active = false;
        std::uint32_t last_base_active = 0;
        for (std::size_t i = 0; i < package_count(); ++i) {
            const GenerationPackageRef& ref = package_ref(i);
            validate_package_ref(ref);
            if (checked_source(ref.source) == GenerationSource::base_active) {
                if (have_base_active && ref.index <= last_base_active) {
                    throw std::runtime_error(
                        "generation base packages are not ordered");
                }
                have_base_active = true;
                last_base_active = ref.index;
            } else if ((i != 0 && name(i - 1) >= name(i)) ||
                       (i + 1 < package_count() &&
                        name(i) >= name(i + 1))) {
                throw std::runtime_error(
                    "generation active packages are not sorted");
            }
        }
        for (std::size_t i = 0; i < shadow_package_count(); ++i) {
            validate_package_ref(shadow_package_ref(i));
            if (i != 0 && shadow_name(i - 1) > shadow_name(i)) {
                throw std::runtime_error(
                    "generation shadow packages are not sorted");
            }
        }
    } catch (...) {
        close_overlay();
        base_.reset();
        throw;
    }

    base_active_kept_.assign(base_->package_count(), 0);
    base_to_current_.assign(base_->package_count(),
                            std::numeric_limits<std::uint32_t>::max());
    extra_active_.reserve(package_count());
    for (std::size_t i = 0; i < package_count(); ++i) {
        const GenerationPackageRef& ref = package_ref(i);
        if (checked_source(ref.source) == GenerationSource::base_active) {
            if (base_active_kept_[ref.index] != 0) {
                close_overlay();
                base_.reset();
                throw std::runtime_error(
                    "generation reuses a base active package");
            }
            base_active_kept_[ref.index] = 1;
            base_to_current_[ref.index] = static_cast<std::uint32_t>(i);
        } else {
            extra_active_.push_back(static_cast<std::uint32_t>(i));
        }
    }
    base_dirty_search_blocks_.assign(base_->search_block_count(), 0);
    const std::size_t block_packages = base_->search_block_packages();
    for (std::size_t i = 0; i < base_active_kept_.size(); ++i) {
        if (base_active_kept_[i] == 0) {
            base_dirty_search_blocks_[i / block_packages] = 1;
        }
    }
}

GenerationView::~GenerationView() {
    close_overlay();
}

bool GenerationView::is_overlay() const {
    return header_ != nullptr;
}

const SnapshotView& GenerationView::base_snapshot() const {
    return *base_;
}

std::size_t GenerationView::branch_count() const {
    return is_overlay() ? static_cast<std::size_t>(header_->branch_count)
                        : base_->branch_count();
}

std::size_t GenerationView::package_count() const {
    return is_overlay() ? static_cast<std::size_t>(header_->package_count)
                        : base_->package_count();
}

std::size_t GenerationView::shadow_package_count() const {
    return is_overlay()
               ? static_cast<std::size_t>(header_->shadow_package_count)
               : base_->shadow_package_count();
}

std::int64_t GenerationView::created_at() const {
    return is_overlay() ? header_->created_at : base_->created_at();
}

const GenerationBranchRef& GenerationView::branch_ref(
    std::size_t index) const {
    return branch_refs_[index];
}

const GenerationPackageRef& GenerationView::package_ref(
    std::size_t index) const {
    return package_refs_[index];
}

const GenerationPackageRef& GenerationView::shadow_package_ref(
    std::size_t index) const {
    return shadow_package_refs_[index];
}

const InlineBranchRecord& GenerationView::inline_branch(
    std::size_t index) const {
    return inline_branches_[index];
}

const InlinePackageRecord& GenerationView::inline_package(
    std::size_t index) const {
    return inline_packages_[index];
}

std::string_view GenerationView::string_slice(Slice32 slice) const {
    return {strings_ + slice.offset, slice.size};
}

std::string_view GenerationView::search_slice(Slice32 slice) const {
    return {search_texts_ + slice.offset, slice.size};
}

std::string_view GenerationView::base_string_slice(Slice32 slice) const {
    return {base_strings_ + slice.offset, slice.size};
}

std::string_view GenerationView::base_search_slice(Slice32 slice) const {
    return {base_search_texts_ + slice.offset, slice.size};
}

std::string_view GenerationView::branch_name(std::size_t index) const {
    if (!is_overlay()) {
        return base_->branch_name(index);
    }
    const GenerationBranchRef& ref = branch_ref(index);
    return checked_source(ref.source) == GenerationSource::base_branch
               ? base_->branch_name(ref.index)
               : string_slice(inline_branch(ref.index).name);
}

const std::array<std::byte, 20>& GenerationView::branch_oid(
    std::size_t index) const {
    if (!is_overlay()) {
        return base_->branch_record(index).oid;
    }
    const GenerationBranchRef& ref = branch_ref(index);
    return checked_source(ref.source) == GenerationSource::base_branch
               ? base_->branch_record(ref.index).oid
               : inline_branch(ref.index).oid;
}

std::int64_t GenerationView::branch_updated_at(std::size_t index) const {
    if (!is_overlay()) {
        return base_->branch_record(index).updated_at;
    }
    const GenerationBranchRef& ref = branch_ref(index);
    return checked_source(ref.source) == GenerationSource::base_branch
               ? base_->branch_record(ref.index).updated_at
               : inline_branch(ref.index).updated_at;
}

BranchInfo GenerationView::branch_info(std::size_t index) const {
    if (!is_overlay()) {
        return base_->branch_info(index);
    }
    const GenerationBranchRef& ref = branch_ref(index);
    if (checked_source(ref.source) == GenerationSource::base_branch) {
        return base_->branch_info(ref.index);
    }
    const InlineBranchRecord& record = inline_branch(ref.index);
    return BranchInfo{std::string(string_slice(record.name)), record.oid,
                      record.updated_at};
}

GenerationLocation GenerationView::branch_location(std::size_t index) const {
    if (!is_overlay()) {
        return {GenerationSource::base_branch,
                static_cast<std::uint32_t>(index)};
    }
    const GenerationBranchRef& ref = branch_ref(index);
    return {checked_source(ref.source), ref.index};
}

std::string_view GenerationView::package_name(
    const GenerationPackageRef& ref) const {
    switch (checked_source(ref.source)) {
        case GenerationSource::base_active:
            return base_string_slice(base_records_[ref.index].name);
        case GenerationSource::base_shadow:
            return base_string_slice(base_shadow_records_[ref.index].name);
        case GenerationSource::inline_record:
            return string_slice(inline_package(ref.index).name);
        case GenerationSource::base_branch:
            break;
    }
    throw std::runtime_error("invalid package source");
}

std::string_view GenerationView::package_search(
    const GenerationPackageRef& ref) const {
    switch (checked_source(ref.source)) {
        case GenerationSource::base_active:
            return base_search_slice(base_records_[ref.index].search);
        case GenerationSource::base_shadow:
            return base_search_slice(base_shadow_records_[ref.index].search);
        case GenerationSource::inline_record:
            return search_slice(inline_package(ref.index).search);
        case GenerationSource::base_branch:
            break;
    }
    throw std::runtime_error("invalid package source");
}

std::string_view GenerationView::package_json(
    const GenerationPackageRef& ref) const {
    switch (checked_source(ref.source)) {
        case GenerationSource::base_active:
            return base_string_slice(base_records_[ref.index].json);
        case GenerationSource::base_shadow:
            return base_string_slice(base_shadow_records_[ref.index].json);
        case GenerationSource::inline_record:
            return string_slice(inline_package(ref.index).json);
        case GenerationSource::base_branch:
            break;
    }
    throw std::runtime_error("invalid package source");
}

std::int64_t GenerationView::package_updated_at(
    const GenerationPackageRef& ref) const {
    switch (checked_source(ref.source)) {
        case GenerationSource::base_active:
            return base_records_[ref.index].updated_at;
        case GenerationSource::base_shadow:
            return base_shadow_records_[ref.index].updated_at;
        case GenerationSource::inline_record:
            return inline_package(ref.index).updated_at;
        case GenerationSource::base_branch:
            break;
    }
    throw std::runtime_error("invalid package source");
}

std::string_view GenerationView::name(std::size_t index) const {
    return is_overlay()
               ? package_name(package_ref(index))
               : base_string_slice(base_records_[index].name);
}

std::optional<std::size_t> GenerationView::find_package(
    std::string_view wanted) const {
    std::size_t first = 0;
    std::size_t last = base_->package_count();
    while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        if (base_string_slice(base_records_[middle].name) < wanted) {
            first = middle + 1;
        } else {
            last = middle;
        }
    }
    if (first < base_->package_count() &&
        base_string_slice(base_records_[first].name) == wanted) {
        if (!is_overlay()) {
            return first;
        }
        const std::uint32_t current = base_to_current_[first];
        if (current != std::numeric_limits<std::uint32_t>::max()) {
            return current;
        }
    }
    if (!is_overlay()) {
        return std::nullopt;
    }
    const auto extra = std::lower_bound(
        extra_active_.begin(), extra_active_.end(), wanted,
        [this](std::uint32_t index, std::string_view name) {
            return package_name(package_ref(index)) < name;
        });
    if (extra != extra_active_.end() &&
        package_name(package_ref(*extra)) == wanted) {
        return static_cast<std::size_t>(*extra);
    }
    return std::nullopt;
}

std::string_view GenerationView::search_text(std::size_t index) const {
    return is_overlay() ? package_search(package_ref(index))
                        : base_search_slice(base_records_[index].search);
}

std::string_view GenerationView::json(std::size_t index) const {
    return is_overlay()
               ? package_json(package_ref(index))
               : base_string_slice(base_records_[index].json);
}

std::int64_t GenerationView::updated_at(std::size_t index) const {
    return is_overlay() ? package_updated_at(package_ref(index))
                        : base_records_[index].updated_at;
}

GenerationLocation GenerationView::location(std::size_t index) const {
    if (!is_overlay()) {
        return {GenerationSource::base_active,
                static_cast<std::uint32_t>(index)};
    }
    const GenerationPackageRef& ref = package_ref(index);
    return {checked_source(ref.source), ref.index};
}

std::uint32_t GenerationView::package_branch_index(
    std::size_t index) const {
    return is_overlay() ? package_ref(index).branch_index
                        : base_records_[index].branch_index;
}

std::string_view GenerationView::shadow_name(std::size_t index) const {
    return is_overlay() ? package_name(shadow_package_ref(index))
                        : base_string_slice(base_shadow_records_[index].name);
}

std::string_view GenerationView::shadow_search_text(
    std::size_t index) const {
    return is_overlay() ? package_search(shadow_package_ref(index))
                        : base_search_slice(base_shadow_records_[index].search);
}

std::string_view GenerationView::shadow_json(std::size_t index) const {
    return is_overlay() ? package_json(shadow_package_ref(index))
                        : base_string_slice(base_shadow_records_[index].json);
}

std::int64_t GenerationView::shadow_updated_at(std::size_t index) const {
    return is_overlay() ? package_updated_at(shadow_package_ref(index))
                        : base_shadow_records_[index].updated_at;
}

GenerationLocation GenerationView::shadow_location(
    std::size_t index) const {
    if (!is_overlay()) {
        return {GenerationSource::base_shadow,
                static_cast<std::uint32_t>(index)};
    }
    const GenerationPackageRef& ref = shadow_package_ref(index);
    return {checked_source(ref.source), ref.index};
}

std::uint32_t GenerationView::shadow_branch_index(
    std::size_t index) const {
    return is_overlay() ? shadow_package_ref(index).branch_index
                        : base_shadow_records_[index].branch_index;
}

void GenerationView::search(std::string_view normalized_query,
                            std::size_t max_results,
                            std::vector<std::size_t>& results,
                            SearchStats* stats) const {
    if (!is_overlay()) {
        search_packages(*base_, normalized_query, max_results, results,
                        stats);
        return;
    }
    search_packages_kept(*base_, normalized_query, max_results,
                         base_active_kept_, base_dirty_search_blocks_, results,
                         stats);
    for (std::size_t& base_index : results) {
        const std::uint32_t current = base_to_current_[base_index];
        if (current == std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error(
                "kept base package has no generation index");
        }
        base_index = current;
    }
    for (const std::uint32_t current : extra_active_) {
        if (!search_text(current).contains(normalized_query)) {
            continue;
        }
        const std::string_view extra_name = name(current);
        const auto position = std::lower_bound(
            results.begin(), results.end(), extra_name,
            [this](std::size_t index, std::string_view wanted) {
                return name(index) < wanted;
            });
        if (results.size() < max_results) {
            results.insert(position, current);
        } else if (position != results.end()) {
            results.pop_back();
            const auto replacement = std::lower_bound(
                results.begin(), results.end(), extra_name,
                [this](std::size_t index, std::string_view wanted) {
                    return name(index) < wanted;
                });
            results.insert(replacement, current);
        }
    }
    if (stats != nullptr) {
        stats->packages_scanned += extra_active_.size();
    }
}

std::span<const std::byte> GenerationView::packages_gz() const {
    if (!is_overlay()) {
        return base_->packages_gz();
    }
    return {packages_gz_,
            static_cast<std::size_t>(header_->packages_gz_size)};
}

int GenerationView::packages_gz_file_descriptor() const {
    return is_overlay() ? overlay_fd_ : base_->file_descriptor();
}

std::uint64_t GenerationView::packages_gz_file_offset() const {
    return is_overlay() ? header_->packages_gz_offset
                        : base_->packages_gz_file_offset();
}

void GenerationView::close_overlay() {
    if (overlay_mapping_ != nullptr) {
        ::munmap(overlay_mapping_, overlay_mapping_size_);
    }
    if (overlay_fd_ >= 0) {
        ::close(overlay_fd_);
    }
    overlay_fd_ = -1;
    overlay_mapping_ = nullptr;
    overlay_mapping_size_ = 0;
    header_ = nullptr;
    branch_refs_ = nullptr;
    package_refs_ = nullptr;
    shadow_package_refs_ = nullptr;
    inline_branches_ = nullptr;
    inline_packages_ = nullptr;
    strings_ = nullptr;
    search_texts_ = nullptr;
    packages_gz_ = nullptr;
}

void write_generation_overlay(
    const std::string& path,
    const SnapshotView& base,
    std::span<const BranchInfo> branches,
    std::span<const GenerationLocation> branch_locations,
    std::span<const GenerationPackageInput> packages,
    std::span<const GenerationPackageInput> shadow_packages,
    std::span<const GenerationPackageDetails> package_details) {
    if (branches.size() != branch_locations.size()) {
        throw std::runtime_error("branch location count does not match");
    }
    if (branches.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("too many branches for generation");
    }

    std::vector<GenerationBranchRef> branch_refs;
    std::vector<GenerationPackageRef> package_refs;
    std::vector<GenerationPackageRef> shadow_refs;
    std::vector<InlineBranchRecord> inline_branches;
    std::vector<InlinePackageRecord> inline_packages;
    std::string strings;
    std::string search_texts;
    branch_refs.reserve(branches.size());
    package_refs.reserve(packages.size());
    shadow_refs.reserve(shadow_packages.size());

    for (std::size_t i = 0; i < branches.size(); ++i) {
        const GenerationLocation location = branch_locations[i];
        if (location.source == GenerationSource::base_branch) {
            if (location.index >= base.branch_count()) {
                throw std::runtime_error("invalid base branch location");
            }
            branch_refs.push_back(GenerationBranchRef{
                source_value(GenerationSource::base_branch), location.index});
        } else {
            InlineBranchRecord record{};
            append_slice(strings, record.name, branches[i].name);
            record.oid = branches[i].oid;
            record.updated_at = branches[i].updated_at;
            const std::uint32_t index =
                static_cast<std::uint32_t>(inline_branches.size());
            inline_branches.push_back(record);
            branch_refs.push_back(GenerationBranchRef{
                source_value(GenerationSource::inline_record), index});
        }
    }

    auto append_package = [&](const GenerationPackageInput& input,
                              std::vector<GenerationPackageRef>& refs) {
        if (input.branch_index >= branches.size()) {
            throw std::runtime_error(
                "generation package references an invalid branch");
        }
        GenerationLocation location = input.location;
        const bool reusable =
            (location.source == GenerationSource::base_active &&
             location.index < base.package_count()) ||
            (location.source == GenerationSource::base_shadow &&
             location.index < base.shadow_package_count());
        if (!reusable) {
            if (input.details_index >= package_details.size()) {
                throw std::runtime_error(
                    "generation inline package lacks details");
            }
            const GenerationPackageDetails& details =
                package_details[input.details_index];
            InlinePackageRecord record{};
            append_slice(strings, record.name, input.name);
            append_slice(search_texts, record.search, details.search);
            append_slice(strings, record.json, details.json);
            record.updated_at = details.updated_at;
            location = {
                GenerationSource::inline_record,
                static_cast<std::uint32_t>(inline_packages.size()),
            };
            inline_packages.push_back(record);
        }
        refs.push_back(GenerationPackageRef{
            source_value(location.source),
            location.index,
            input.branch_index,
            0,
        });
    };
    for (const GenerationPackageInput& package : packages) {
        append_package(package, package_refs);
    }
    for (const GenerationPackageInput& package : shadow_packages) {
        append_package(package, shadow_refs);
    }
    const std::vector<std::byte> packages_gz = gzip_names(packages);

    struct stat base_status {};
    if (::fstat(base.file_descriptor(), &base_status) != 0 ||
        base_status.st_size < 0) {
        throw std::runtime_error("cannot stat generation base");
    }
    GenerationHeader header{};
    std::memcpy(header.magic, kGenerationMagic.data(),
                kGenerationMagic.size());
    header.version = kGenerationVersion;
    header.header_size = sizeof(GenerationHeader);
    header.base_created_at = static_cast<std::uint64_t>(base.created_at());
    header.base_file_size = static_cast<std::uint64_t>(base_status.st_size);
    header.branch_count = branches.size();
    header.package_count = packages.size();
    header.shadow_package_count = shadow_packages.size();
    header.branch_refs_offset = align_up(sizeof(GenerationHeader), 8);
    header.package_refs_offset =
        align_up(header.branch_refs_offset +
                     byte_size<GenerationBranchRef>(branch_refs.size()),
                 8);
    header.shadow_package_refs_offset =
        align_up(header.package_refs_offset +
                     byte_size<GenerationPackageRef>(package_refs.size()),
                 8);
    header.inline_branches_offset =
        align_up(header.shadow_package_refs_offset +
                     byte_size<GenerationPackageRef>(shadow_refs.size()),
                 8);
    header.inline_branch_count = inline_branches.size();
    header.inline_packages_offset =
        align_up(header.inline_branches_offset +
                     byte_size<InlineBranchRecord>(inline_branches.size()),
                 8);
    header.inline_package_count = inline_packages.size();
    header.strings_offset =
        align_up(header.inline_packages_offset +
                     byte_size<InlinePackageRecord>(inline_packages.size()),
                 8);
    header.strings_size = strings.size();
    header.search_texts_offset =
        align_up(header.strings_offset + header.strings_size, 8);
    header.search_texts_size = search_texts.size();
    header.packages_gz_offset =
        align_up(header.search_texts_offset + header.search_texts_size, 8);
    header.packages_gz_size = packages_gz.size();
    header.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    const std::string temporary =
        path + ".tmp." + std::to_string(::getpid());
    int fd = ::open(temporary.c_str(),
                    O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0) {
        throw std::runtime_error(std::string("open generation output: ") +
                                 std::strerror(errno));
    }
    try {
        std::uint64_t position = 0;
        auto write_region = [&](std::uint64_t offset, const void* data,
                                std::size_t size) {
            write_padding(fd, offset - position);
            if (size != 0) {
                write_all(fd, data, size);
            }
            position = offset + size;
        };
        write_region(0, &header, sizeof(header));
        write_region(header.branch_refs_offset, branch_refs.data(),
                     branch_refs.size() * sizeof(GenerationBranchRef));
        write_region(header.package_refs_offset, package_refs.data(),
                     package_refs.size() * sizeof(GenerationPackageRef));
        write_region(header.shadow_package_refs_offset, shadow_refs.data(),
                     shadow_refs.size() * sizeof(GenerationPackageRef));
        write_region(header.inline_branches_offset, inline_branches.data(),
                     inline_branches.size() * sizeof(InlineBranchRecord));
        write_region(header.inline_packages_offset, inline_packages.data(),
                     inline_packages.size() * sizeof(InlinePackageRecord));
        write_region(header.strings_offset, strings.data(), strings.size());
        write_region(header.search_texts_offset, search_texts.data(),
                     search_texts.size());
        write_region(header.packages_gz_offset, packages_gz.data(),
                     packages_gz.size());
        if (::fsync(fd) != 0) {
            throw std::runtime_error(std::string("fsync generation: ") +
                                     std::strerror(errno));
        }
        ::close(fd);
        fd = -1;
        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            throw std::runtime_error(std::string("rename generation: ") +
                                     std::strerror(errno));
        }
    } catch (...) {
        if (fd >= 0) {
            ::close(fd);
        }
        ::unlink(temporary.c_str());
        throw;
    }
}

}  // namespace aurhub
