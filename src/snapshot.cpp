#include "snapshot.hpp"

#include "srcinfo.hpp"

#include <algorithm>
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

void assign_slice(std::uint64_t& section_size,
                  Slice32& slice,
                  std::string_view value) {
    if (section_size > std::numeric_limits<std::uint32_t>::max() ||
        value.size() > std::numeric_limits<std::uint32_t>::max() ||
        value.size() > std::numeric_limits<std::uint32_t>::max() - section_size) {
        throw std::runtime_error("snapshot string section exceeds 4 GiB");
    }
    slice.offset = static_cast<std::uint32_t>(section_size);
    slice.size = static_cast<std::uint32_t>(value.size());
    section_size += value.size();
}

std::vector<std::byte> gzip_names(
    std::span<const CompiledPackageView> packages) {
    std::string names;
    for (const CompiledPackageView& package : packages) {
        names.append(package.name).push_back('\n');
    }

    z_stream stream{};
    if (deflateInit2(&stream, Z_BEST_SPEED, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }

    std::vector<std::byte> output;
    output.resize(compressBound(static_cast<uLong>(names.size())) + 64U);
    stream.next_in = reinterpret_cast<Bytef*>(names.data());
    stream.avail_in = static_cast<uInt>(names.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    const int status = deflate(&stream, Z_FINISH);
    if (status != Z_STREAM_END) {
        deflateEnd(&stream);
        throw std::runtime_error("gzip compression failed");
    }
    output.resize(stream.total_out);
    deflateEnd(&stream);
    return output;
}

void write_all(int fd, const void* data, std::size_t size) {
    const auto* cursor = static_cast<const std::byte*>(data);
    while (size != 0) {
        const ssize_t written = ::write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("write: ") + std::strerror(errno));
        }
        cursor += written;
        size -= static_cast<std::size_t>(written);
    }
}

off_t checked_file_offset(std::uint64_t offset) {
    if (offset >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        throw std::runtime_error("snapshot exceeds the platform file offset");
    }
    return static_cast<off_t>(offset);
}

void write_all_at(int fd,
                  const void* data,
                  std::size_t size,
                  std::uint64_t offset) {
    const auto* cursor = static_cast<const std::byte*>(data);
    while (size != 0) {
        const ssize_t written =
            ::pwrite(fd, cursor, size, checked_file_offset(offset));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("pwrite: ") +
                                     std::strerror(errno));
        }
        const std::size_t count = static_cast<std::size_t>(written);
        cursor += count;
        size -= count;
        offset += count;
    }
}

class BufferedWriter {
public:
    explicit BufferedWriter(int fd) : fd_(fd) {}

    void append(std::string_view value) {
        while (!value.empty()) {
            if (used_ == buffer_.size()) {
                flush();
            }
            const std::size_t chunk =
                std::min(value.size(), buffer_.size() - used_);
            std::memcpy(buffer_.data() + used_, value.data(), chunk);
            used_ += chunk;
            value.remove_prefix(chunk);
        }
    }

    void flush() {
        if (used_ != 0) {
            write_all(fd_, buffer_.data(), used_);
            used_ = 0;
        }
    }

private:
    int fd_;
    std::array<std::byte, static_cast<std::size_t>(1024) * 1024> buffer_{};
    std::size_t used_ = 0;
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void write_padding(int fd, std::uint64_t bytes) {
    static constexpr std::array<std::byte, 64> zeros{};
    while (bytes != 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(bytes, zeros.size()));
        write_all(fd, zeros.data(), chunk);
        bytes -= chunk;
    }
}

bool region_valid(std::uint64_t offset,
                  std::uint64_t size,
                  std::uint64_t file_size) {
    return offset <= file_size && size <= file_size - offset;
}

std::uint32_t trigram_hash(std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

void add_trigram(SearchBlock& block, std::uint32_t trigram) {
    const std::uint32_t mixed = trigram_hash(trigram);
    const std::uint16_t first = static_cast<std::uint16_t>(mixed);
    const std::uint16_t second = static_cast<std::uint16_t>(mixed >> 16U);
    block.trigrams[first >> 6U] |= std::uint64_t{1} << (first & 63U);
    block.trigrams[second >> 6U] |= std::uint64_t{1} << (second & 63U);
}

void add_search_text(SearchBlock& block, std::string_view text) {
    for (const unsigned char byte : text) {
        block.bytes[byte >> 6U] |= std::uint64_t{1} << (byte & 63U);
    }
    for (std::size_t i = 1; i < text.size(); ++i) {
        const auto high = static_cast<std::uint16_t>(
            static_cast<unsigned char>(text[i - 1]));
        const auto low =
            static_cast<std::uint16_t>(static_cast<unsigned char>(text[i]));
        const std::uint16_t bigram = static_cast<std::uint16_t>((high << 8U) | low);
        block.bigrams[bigram >> 6U] |= std::uint64_t{1} << (bigram & 63U);
    }
    for (std::size_t i = 2; i < text.size(); ++i) {
        const auto first = static_cast<std::uint32_t>(
            static_cast<unsigned char>(text[i - 2]));
        const auto second = static_cast<std::uint32_t>(
            static_cast<unsigned char>(text[i - 1]));
        const auto third =
            static_cast<std::uint32_t>(static_cast<unsigned char>(text[i]));
        add_trigram(block, (first << 16U) | (second << 8U) | third);
    }
}

std::uint64_t block_count(std::size_t packages,
                          std::uint32_t packages_per_block) {
    return packages / packages_per_block +
           (packages % packages_per_block != 0 ? 1U : 0U);
}

void write_search_blocks(int fd,
                         std::span<const CompiledPackageView> packages,
                         const SnapshotHeader& header) {
    SearchBlock fine{};
    SearchBlock coarse{};
    std::uint64_t fine_index = 0;
    std::uint64_t coarse_index = 0;
    for (std::size_t package_index = 0; package_index < packages.size();
         ++package_index) {
        if (fine.package_count == 0) {
            fine.first_package =
                static_cast<std::uint32_t>(package_index);
        }
        if (header.search_super_block_packages != 0 &&
            coarse.package_count == 0) {
            coarse.first_package =
                static_cast<std::uint32_t>(package_index);
        }

        add_search_text(fine, packages[package_index].search);
        ++fine.package_count;
        if (header.search_super_block_packages != 0) {
            add_search_text(coarse, packages[package_index].search);
            ++coarse.package_count;
        }

        if (fine.package_count == header.search_block_packages ||
            package_index + 1 == packages.size()) {
            write_all_at(
                fd, &fine, sizeof(fine),
                header.search_blocks_offset +
                    fine_index * sizeof(SearchBlock));
            ++fine_index;
            fine = SearchBlock{};
        }
        if (header.search_super_block_packages != 0 &&
            (coarse.package_count ==
                 header.search_super_block_packages ||
             package_index + 1 == packages.size())) {
            write_all_at(
                fd, &coarse, sizeof(coarse),
                header.search_super_blocks_offset +
                    coarse_index * sizeof(SearchBlock));
            ++coarse_index;
            coarse = SearchBlock{};
        }
    }
    if (fine_index != header.search_block_count ||
        coarse_index != header.search_super_block_count) {
        throw std::runtime_error("search block count changed while writing");
    }
}

bool slice_valid(Slice32 slice, std::uint64_t section_size) {
    return slice.offset <= section_size &&
           slice.size <= section_size - slice.offset;
}

}  // namespace

void write_snapshot(const std::string& path,
                    std::span<const CompiledPackageView> packages,
                    std::span<const CompiledPackageView> shadow_packages,
                    const std::vector<BranchInfo>& branches,
                    std::uint32_t search_block_packages,
                    std::uint32_t search_super_block_packages) {
    if (search_block_packages == 0 ||
        search_block_packages > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error("search block package count must be 1..65535");
    }
    if (search_super_block_packages != 0 &&
        (search_super_block_packages >
             std::numeric_limits<std::uint16_t>::max() ||
         search_super_block_packages < search_block_packages ||
         search_super_block_packages % search_block_packages != 0)) {
        throw std::runtime_error(
            "search super block count must be zero or a multiple of the fine block");
    }
    if (packages.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("snapshot contains too many active packages");
    }
    std::vector<PackageRecord> records;
    records.reserve(packages.size());
    std::vector<PackageRecord> shadow_records;
    shadow_records.reserve(shadow_packages.size());
    std::vector<BranchRecord> branch_records;
    branch_records.reserve(branches.size());
    std::uint64_t strings_size = 0;
    std::uint64_t search_texts_size = 0;
    for (const BranchInfo& branch : branches) {
        BranchRecord record{};
        assign_slice(strings_size, record.name, branch.name);
        record.oid = branch.oid;
        record.updated_at = branch.updated_at;
        branch_records.push_back(record);
    }
    for (const auto& package : packages) {
        if (package.branch_index >= branches.size()) {
            throw std::runtime_error("package references an invalid branch");
        }
        PackageRecord record{};
        assign_slice(strings_size, record.name, package.name);
        assign_slice(search_texts_size, record.search, package.search);
        assign_slice(strings_size, record.json, package.json);
        record.branch_index = package.branch_index;
        record.updated_at = package.updated_at;
        records.push_back(record);
    }
    for (const CompiledPackageView& package : shadow_packages) {
        if (package.branch_index >= branches.size()) {
            throw std::runtime_error(
                "shadow package references an invalid branch");
        }
        PackageRecord record{};
        assign_slice(strings_size, record.name, package.name);
        assign_slice(search_texts_size, record.search, package.search);
        assign_slice(strings_size, record.json, package.json);
        record.branch_index = package.branch_index;
        record.updated_at = package.updated_at;
        shadow_records.push_back(record);
    }
    const std::vector<std::byte> packages_gz = gzip_names(packages);

    SnapshotHeader header{};
    std::memcpy(header.magic, kSnapshotMagic.data(), kSnapshotMagic.size());
    header.version = kSnapshotVersion;
    header.header_size = sizeof(SnapshotHeader);
    header.package_count = packages.size();
    header.shadow_package_count = shadow_packages.size();
    header.branch_count = branches.size();
    header.records_offset = align_up(sizeof(SnapshotHeader), 8);
    const std::uint64_t records_size =
        static_cast<std::uint64_t>(records.size() * sizeof(PackageRecord));
    header.shadow_records_offset =
        align_up(header.records_offset + records_size, 8);
    const std::uint64_t shadow_records_size = static_cast<std::uint64_t>(
        shadow_records.size() * sizeof(PackageRecord));
    header.branches_offset =
        align_up(header.shadow_records_offset + shadow_records_size, 8);
    const std::uint64_t branches_size = static_cast<std::uint64_t>(
        branch_records.size() * sizeof(BranchRecord));
    header.search_blocks_offset =
        align_up(header.branches_offset + branches_size, 8);
    header.search_super_blocks_offset = header.search_blocks_offset;
    header.search_super_block_count =
        search_super_block_packages == 0
            ? 0
            : block_count(packages.size(), search_super_block_packages);
    const std::uint64_t search_super_blocks_size =
        header.search_super_block_count * sizeof(SearchBlock);
    header.search_blocks_offset = align_up(
        header.search_super_blocks_offset + search_super_blocks_size, 8);
    header.search_block_count =
        block_count(packages.size(), search_block_packages);
    const std::uint64_t search_blocks_size =
        header.search_block_count * sizeof(SearchBlock);
    header.strings_offset =
        align_up(header.search_blocks_offset + search_blocks_size, 8);
    header.strings_size = strings_size;
    header.search_texts_offset =
        align_up(header.strings_offset + header.strings_size, 8);
    header.search_texts_size = search_texts_size;
    header.packages_gz_offset = align_up(
        header.search_texts_offset + header.search_texts_size, 8);
    header.packages_gz_size = packages_gz.size();
    header.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    header.search_block_packages = search_block_packages;
    header.search_super_block_packages = search_super_block_packages;

    const std::string temporary = path + ".tmp." + std::to_string(::getpid());
    int fd = ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        throw std::runtime_error(std::string("open snapshot: ") +
                                 std::strerror(errno));
    }

    try {
        const std::uint64_t final_size =
            header.packages_gz_offset + header.packages_gz_size;
        if (::ftruncate(fd, checked_file_offset(final_size)) != 0) {
            throw std::runtime_error(std::string("ftruncate snapshot: ") +
                                     std::strerror(errno));
        }
        write_all(fd, &header, sizeof(header));
        write_padding(fd, header.records_offset - sizeof(header));
        if (!records.empty()) {
            write_all(fd, records.data(), records.size() * sizeof(PackageRecord));
        }
        write_padding(fd, header.shadow_records_offset -
                              (header.records_offset + records_size));
        if (!shadow_records.empty()) {
            write_all(fd, shadow_records.data(),
                      shadow_records.size() * sizeof(PackageRecord));
        }
        write_padding(fd, header.branches_offset -
                              (header.shadow_records_offset +
                               shadow_records_size));
        if (!branch_records.empty()) {
            write_all(fd, branch_records.data(),
                      branch_records.size() * sizeof(BranchRecord));
        }
        write_padding(fd, header.search_super_blocks_offset -
                              (header.branches_offset + branches_size));
        write_search_blocks(fd, packages, header);
        if (::lseek(fd, checked_file_offset(header.strings_offset), SEEK_SET) < 0) {
            throw std::runtime_error(std::string("lseek snapshot: ") +
                                     std::strerror(errno));
        }
        BufferedWriter buffered(fd);
        for (const BranchInfo& branch : branches) {
            buffered.append(branch.name);
        }
        for (const CompiledPackageView& package : packages) {
            buffered.append(package.name);
            buffered.append(package.json);
        }
        for (const CompiledPackageView& package : shadow_packages) {
            buffered.append(package.name);
            buffered.append(package.json);
        }
        buffered.flush();
        write_padding(fd, header.search_texts_offset -
                              (header.strings_offset + header.strings_size));
        for (const CompiledPackageView& package : packages) {
            buffered.append(package.search);
        }
        for (const CompiledPackageView& package : shadow_packages) {
            buffered.append(package.search);
        }
        buffered.flush();
        write_padding(fd, header.packages_gz_offset -
                              (header.search_texts_offset +
                               header.search_texts_size));
        if (!packages_gz.empty()) {
            write_all(fd, packages_gz.data(), packages_gz.size());
        }
        if (::fsync(fd) != 0) {
            throw std::runtime_error(std::string("fsync snapshot: ") +
                                     std::strerror(errno));
        }
        ::close(fd);
        fd = -1;
        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            throw std::runtime_error(std::string("rename snapshot: ") +
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

SnapshotView::SnapshotView(const std::string& path,
                           SnapshotValidation validation)
    : fd_(::open(path.c_str(), O_RDONLY)) {
    if (fd_ < 0) {
        throw std::runtime_error(std::string("open snapshot: ") +
                                 std::strerror(errno));
    }

    struct stat status {};
    if (::fstat(fd_, &status) != 0 || status.st_size < 0) {
        close();
        throw std::runtime_error("cannot stat snapshot");
    }
    mapping_size_ = static_cast<std::size_t>(status.st_size);
    if (mapping_size_ < sizeof(SnapshotHeader)) {
        close();
        throw std::runtime_error("snapshot is smaller than its header");
    }
    mapping_ = ::mmap(nullptr, mapping_size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (mapping_ == MAP_FAILED) {
        mapping_ = nullptr;
        close();
        throw std::runtime_error(std::string("mmap snapshot: ") +
                                 std::strerror(errno));
    }

    header_ = static_cast<const SnapshotHeader*>(mapping_);
    const std::uint64_t file_size = mapping_size_;
    if (std::memcmp(header_->magic, kSnapshotMagic.data(), kSnapshotMagic.size()) !=
            0 ||
        header_->version != kSnapshotVersion ||
        header_->header_size != sizeof(SnapshotHeader)) {
        close();
        throw std::runtime_error("unsupported snapshot format");
    }
    if (header_->package_count >
        std::numeric_limits<std::uint64_t>::max() / sizeof(PackageRecord)) {
        close();
        throw std::runtime_error("invalid package count");
    }
    const std::uint64_t records_size =
        header_->package_count * sizeof(PackageRecord);
    if (header_->shadow_package_count >
        std::numeric_limits<std::uint64_t>::max() / sizeof(PackageRecord)) {
        close();
        throw std::runtime_error("invalid shadow package count");
    }
    const std::uint64_t shadow_records_size =
        header_->shadow_package_count * sizeof(PackageRecord);
    if (header_->branch_count >
        std::numeric_limits<std::uint64_t>::max() / sizeof(BranchRecord)) {
        close();
        throw std::runtime_error("invalid branch count");
    }
    const std::uint64_t branches_size =
        header_->branch_count * sizeof(BranchRecord);
    if (header_->search_block_count >
        std::numeric_limits<std::uint64_t>::max() / sizeof(SearchBlock)) {
        close();
        throw std::runtime_error("invalid search block count");
    }
    const std::uint64_t search_blocks_size =
        header_->search_block_count * sizeof(SearchBlock);
    if (header_->search_super_block_count >
        std::numeric_limits<std::uint64_t>::max() / sizeof(SearchBlock)) {
        close();
        throw std::runtime_error("invalid search super block count");
    }
    const std::uint64_t search_super_blocks_size =
        header_->search_super_block_count * sizeof(SearchBlock);
    if (!region_valid(header_->records_offset, records_size, file_size) ||
        !region_valid(header_->shadow_records_offset, shadow_records_size,
                      file_size) ||
        !region_valid(header_->branches_offset, branches_size, file_size) ||
        !region_valid(header_->search_super_blocks_offset,
                      search_super_blocks_size, file_size) ||
        !region_valid(header_->search_blocks_offset, search_blocks_size,
                      file_size) ||
        !region_valid(header_->strings_offset, header_->strings_size, file_size) ||
        !region_valid(header_->search_texts_offset, header_->search_texts_size,
                      file_size) ||
        !region_valid(header_->packages_gz_offset, header_->packages_gz_size,
                      file_size) ||
        header_->search_block_packages == 0 ||
        (header_->search_super_block_packages != 0 &&
         (header_->search_super_block_packages <
              header_->search_block_packages ||
          header_->search_super_block_packages %
                  header_->search_block_packages !=
              0))) {
        close();
        throw std::runtime_error("snapshot contains an invalid region");
    }
    const std::uint64_t expected_search_blocks =
        header_->package_count / header_->search_block_packages +
        (header_->package_count % header_->search_block_packages != 0 ? 1U
                                                                      : 0U);
    const std::uint64_t expected_search_super_blocks =
        header_->search_super_block_packages == 0
            ? 0
            : header_->package_count /
                      header_->search_super_block_packages +
                  (header_->package_count %
                               header_->search_super_block_packages !=
                           0
                       ? 1U
                       : 0U);
    if (header_->search_block_count != expected_search_blocks ||
        header_->search_super_block_count != expected_search_super_blocks) {
        close();
        throw std::runtime_error("snapshot contains an invalid search layout");
    }

    const auto* bytes = static_cast<const std::byte*>(mapping_);
    records_ = reinterpret_cast<const PackageRecord*>(
        bytes + header_->records_offset);
    shadow_records_ = reinterpret_cast<const PackageRecord*>(
        bytes + header_->shadow_records_offset);
    branches_ = reinterpret_cast<const BranchRecord*>(
        bytes + header_->branches_offset);
    search_super_blocks_ = reinterpret_cast<const SearchBlock*>(
        bytes + header_->search_super_blocks_offset);
    search_blocks_ = reinterpret_cast<const SearchBlock*>(
        bytes + header_->search_blocks_offset);
    strings_ = reinterpret_cast<const char*>(bytes + header_->strings_offset);
    search_texts_ =
        reinterpret_cast<const char*>(bytes + header_->search_texts_offset);
    packages_gz_ = bytes + header_->packages_gz_offset;

    try {
        for (std::size_t i = 0; i < package_count(); ++i) {
            if (!slice_valid(records_[i].name, header_->strings_size) ||
                !slice_valid(records_[i].json, header_->strings_size) ||
                !slice_valid(records_[i].search, header_->search_texts_size) ||
                records_[i].branch_index >= branch_count()) {
                throw std::runtime_error(
                    "snapshot contains an invalid string slice");
            }
        }
        for (std::size_t i = 0; i < shadow_package_count(); ++i) {
            if (!slice_valid(shadow_records_[i].name, header_->strings_size) ||
                !slice_valid(shadow_records_[i].json, header_->strings_size) ||
                !slice_valid(shadow_records_[i].search,
                             header_->search_texts_size) ||
                shadow_records_[i].branch_index >= branch_count()) {
                throw std::runtime_error(
                    "snapshot contains an invalid shadow package slice");
            }
        }
        for (std::size_t i = 0; i < branch_count(); ++i) {
            if (!slice_valid(branches_[i].name, header_->strings_size)) {
                throw std::runtime_error(
                    "snapshot contains an invalid branch name slice");
            }
            if (i != 0 && branch_name(i - 1) >= branch_name(i)) {
                throw std::runtime_error("snapshot branches are not sorted");
            }
        }
        if (validation == SnapshotValidation::full) {
            std::size_t expected_first = 0;
            for (std::size_t i = 0; i < search_block_count(); ++i) {
                const SearchBlock& block = search_blocks_[i];
                if (block.first_package != expected_first ||
                    expected_first > package_count() ||
                    block.package_count == 0 ||
                    block.package_count > header_->search_block_packages ||
                    block.package_count > package_count() - expected_first) {
                    throw std::runtime_error(
                        "snapshot contains an invalid search block");
                }
                expected_first += block.package_count;
            }
            if (expected_first != package_count()) {
                throw std::runtime_error(
                    "snapshot search blocks do not cover packages");
            }
            expected_first = 0;
            for (std::size_t i = 0; i < search_super_block_count(); ++i) {
                const SearchBlock& block = search_super_blocks_[i];
                if (block.first_package != expected_first ||
                    expected_first > package_count() ||
                    block.package_count == 0 ||
                    block.package_count >
                        header_->search_super_block_packages ||
                    block.package_count >
                        package_count() - expected_first) {
                    throw std::runtime_error(
                        "snapshot contains an invalid search super block");
                }
                expected_first += block.package_count;
            }
            if (header_->search_super_block_packages != 0 &&
                expected_first != package_count()) {
                throw std::runtime_error(
                    "snapshot search super blocks do not cover packages");
            }
        }
    } catch (...) {
        close();
        throw;
    }
}

SnapshotView::~SnapshotView() {
    close();
}

SnapshotView::SnapshotView(SnapshotView&& other) noexcept {
    *this = std::move(other);
}

SnapshotView& SnapshotView::operator=(SnapshotView&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        mapping_ = std::exchange(other.mapping_, nullptr);
        mapping_size_ = std::exchange(other.mapping_size_, 0);
        header_ = std::exchange(other.header_, nullptr);
        records_ = std::exchange(other.records_, nullptr);
        shadow_records_ = std::exchange(other.shadow_records_, nullptr);
        branches_ = std::exchange(other.branches_, nullptr);
        search_blocks_ = std::exchange(other.search_blocks_, nullptr);
        search_super_blocks_ =
            std::exchange(other.search_super_blocks_, nullptr);
        strings_ = std::exchange(other.strings_, nullptr);
        search_texts_ = std::exchange(other.search_texts_, nullptr);
        packages_gz_ = std::exchange(other.packages_gz_, nullptr);
    }
    return *this;
}

std::size_t SnapshotView::package_count() const {
    return static_cast<std::size_t>(header_->package_count);
}

std::size_t SnapshotView::shadow_package_count() const {
    return static_cast<std::size_t>(header_->shadow_package_count);
}

std::size_t SnapshotView::branch_count() const {
    return static_cast<std::size_t>(header_->branch_count);
}

std::size_t SnapshotView::search_block_packages() const {
    return header_->search_block_packages;
}

std::size_t SnapshotView::search_super_block_packages() const {
    return header_->search_super_block_packages;
}

std::size_t SnapshotView::search_block_count() const {
    return static_cast<std::size_t>(header_->search_block_count);
}

std::size_t SnapshotView::search_super_block_count() const {
    return static_cast<std::size_t>(header_->search_super_block_count);
}

std::int64_t SnapshotView::created_at() const {
    return header_->created_at;
}

const PackageRecord& SnapshotView::record(std::size_t index) const {
    return records_[index];
}

const PackageRecord& SnapshotView::shadow_record(std::size_t index) const {
    return shadow_records_[index];
}

const PackageRecord* SnapshotView::records_data() const {
    return records_;
}

const PackageRecord* SnapshotView::shadow_records_data() const {
    return shadow_records_;
}

const char* SnapshotView::strings_data() const {
    return strings_;
}

const char* SnapshotView::search_texts_data() const {
    return search_texts_;
}

const BranchRecord& SnapshotView::branch_record(std::size_t index) const {
    return branches_[index];
}

const SearchBlock& SnapshotView::search_block(std::size_t index) const {
    return search_blocks_[index];
}

const SearchBlock& SnapshotView::search_super_block(std::size_t index) const {
    return search_super_blocks_[index];
}

std::string_view SnapshotView::string_slice(Slice32 slice) const {
    return {strings_ + slice.offset, slice.size};
}

std::string_view SnapshotView::name(std::size_t index) const {
    return string_slice(record(index).name);
}

std::string_view SnapshotView::shadow_name(std::size_t index) const {
    return string_slice(shadow_record(index).name);
}

std::string_view SnapshotView::branch_name(std::size_t index) const {
    return string_slice(branch_record(index).name);
}

BranchInfo SnapshotView::branch_info(std::size_t index) const {
    const BranchRecord& record = branch_record(index);
    return BranchInfo{std::string(branch_name(index)), record.oid,
                      record.updated_at};
}

std::string_view SnapshotView::search_text(std::size_t index) const {
    const Slice32 slice = record(index).search;
    return {search_texts_ + slice.offset, slice.size};
}

std::string_view SnapshotView::shadow_search_text(std::size_t index) const {
    const Slice32 slice = shadow_record(index).search;
    return {search_texts_ + slice.offset, slice.size};
}

std::string_view SnapshotView::json(std::size_t index) const {
    return string_slice(record(index).json);
}

std::string_view SnapshotView::shadow_json(std::size_t index) const {
    return string_slice(shadow_record(index).json);
}

std::span<const std::byte> SnapshotView::packages_gz() const {
    return {packages_gz_, static_cast<std::size_t>(header_->packages_gz_size)};
}

int SnapshotView::file_descriptor() const {
    return fd_;
}

std::uint64_t SnapshotView::packages_gz_file_offset() const {
    return header_->packages_gz_offset;
}

void SnapshotView::close() {
    if (mapping_ != nullptr) {
        ::munmap(mapping_, mapping_size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = -1;
    mapping_ = nullptr;
    mapping_size_ = 0;
    header_ = nullptr;
    records_ = nullptr;
    shadow_records_ = nullptr;
    branches_ = nullptr;
    search_blocks_ = nullptr;
    search_super_blocks_ = nullptr;
    strings_ = nullptr;
    search_texts_ = nullptr;
    packages_gz_ = nullptr;
}

}  // namespace aurhub
