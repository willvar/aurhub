#include "generation.hpp"
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

std::vector<char*> make_argv(std::vector<std::string>& args) {
    std::vector<char*> result;
    result.reserve(args.size() + 1);
    for (std::string& arg : args) {
        result.push_back(arg.data());
    }
    result.push_back(nullptr);
    return result;
}

std::string capture_command(std::vector<std::string> args) {
    int output_pipe[2];
    if (::pipe2(output_pipe, O_CLOEXEC) != 0) {
        throw std::runtime_error(std::string("pipe2: ") + std::strerror(errno));
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) {
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        std::vector<char*> argv = make_argv(args);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    ::close(output_pipe[1]);
    std::string output;
    std::array<char, static_cast<std::size_t>(64) * 1024> buffer{};
    while (true) {
        const ssize_t count = ::read(output_pipe[0], buffer.data(), buffer.size());
        if (count > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            ::close(output_pipe[0]);
            throw std::runtime_error(std::string("read child output: ") +
                                     std::strerror(errno));
        }
    }
    ::close(output_pipe[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("child command failed");
    }
    return output;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') { return ch - '0'; }
    if (ch >= 'a' && ch <= 'f') { return ch - 'a' + 10; }
    if (ch >= 'A' && ch <= 'F') { return ch - 'A' + 10; }
    return -1;
}

std::array<std::byte, 20> parse_oid(std::string_view value) {
    if (value.size() != 40) {
        throw std::runtime_error("invalid Git object ID length");
    }
    std::array<std::byte, 20> oid{};
    for (std::size_t i = 0; i < oid.size(); ++i) {
        const int high = hex_value(value[i * 2]);
        const int low = hex_value(value[i * 2 + 1]);
        if (high < 0 || low < 0) {
            throw std::runtime_error("invalid Git object ID");
        }
        oid[i] = static_cast<std::byte>((high << 4) | low);
    }
    return oid;
}

std::string oid_string(const std::array<std::byte, 20>& oid) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string value(40, '0');
    for (std::size_t i = 0; i < oid.size(); ++i) {
        const unsigned int byte = std::to_integer<unsigned int>(oid[i]);
        value[i * 2] = kHex[byte >> 4U];
        value[i * 2 + 1] = kHex[byte & 0x0fU];
    }
    return value;
}

std::vector<aurhub::BranchInfo> list_branches(const std::string& repo,
                                               bool include_timestamp) {
    std::vector<std::string> args{
        "git", "-C", repo, "for-each-ref",
        "--sort=refname",
        include_timestamp
            ? "--format=%(refname:short)%09%(objectname)%09%(committerdate:unix)"
            : "--format=%(refname:short)%09%(objectname)",
        "refs/heads/"};
    const std::string output = capture_command(std::move(args));

    std::vector<aurhub::BranchInfo> branches;
    std::string_view remaining = output;
    while (!remaining.empty()) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos
                                          ? remaining
                                          : remaining.substr(0, newline);
        remaining = newline == std::string_view::npos
                        ? std::string_view{}
                        : remaining.substr(newline + 1);
        if (line.empty()) {
            continue;
        }
        const std::size_t first_tab = line.find('\t');
        if (first_tab == std::string_view::npos) {
            continue;
        }
        const std::size_t second_tab = line.find('\t', first_tab + 1);
        aurhub::BranchInfo branch;
        branch.name.assign(line.substr(0, first_tab));
        const std::string_view oid =
            second_tab == std::string_view::npos
                ? line.substr(first_tab + 1)
                : line.substr(first_tab + 1, second_tab - first_tab - 1);
        branch.oid = parse_oid(oid);
        if (include_timestamp) {
            if (second_tab == std::string_view::npos) {
                continue;
            }
            const std::string_view timestamp = line.substr(second_tab + 1);
            const auto parsed = std::from_chars(
                timestamp.data(), timestamp.data() + timestamp.size(),
                branch.updated_at);
            if (parsed.ec != std::errc{}) {
                continue;
            }
        }
        branches.push_back(std::move(branch));
    }
    return branches;
}

std::vector<aurhub::BranchInfo> list_branches_fast(const std::string& repo) {
    std::vector<aurhub::BranchInfo> packed;
    const std::filesystem::path repo_path(repo);
    std::ifstream packed_refs(repo_path / "packed-refs");
    std::string line;
    static constexpr std::string_view kHeadsPrefix = "refs/heads/";
    while (std::getline(packed_refs, line)) {
        if (line.empty() || line.front() == '#' || line.front() == '^') {
            continue;
        }
        const std::size_t space = line.find(' ');
        if (space == std::string::npos ||
            !std::string_view(line).substr(space + 1).starts_with(kHeadsPrefix)) {
            continue;
        }
        aurhub::BranchInfo branch;
        branch.oid = parse_oid(std::string_view(line).substr(0, space));
        branch.name.assign(line.data() + space + 1 + kHeadsPrefix.size(),
                           line.size() - space - 1 - kHeadsPrefix.size());
        packed.push_back(std::move(branch));
    }

    std::vector<aurhub::BranchInfo> loose;
    const std::filesystem::path heads = repo_path / "refs" / "heads";
    std::error_code error;
    if (std::filesystem::exists(heads, error)) {
        for (std::filesystem::recursive_directory_iterator iterator(heads, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (!iterator->is_regular_file(error)) {
                continue;
            }
            std::ifstream input(iterator->path());
            std::string oid;
            input >> oid;
            if (oid.size() != 40) {
                continue;
            }
            aurhub::BranchInfo branch;
            branch.oid = parse_oid(oid);
            branch.name =
                std::filesystem::relative(iterator->path(), heads).generic_string();
            loose.push_back(std::move(branch));
        }
    }
    if (error) {
        throw std::runtime_error("cannot enumerate loose Git refs: " +
                                 error.message());
    }

    const auto by_name = [](const aurhub::BranchInfo& left,
                            const aurhub::BranchInfo& right) {
        return left.name < right.name;
    };
    std::sort(packed.begin(), packed.end(), by_name);
    std::sort(loose.begin(), loose.end(), by_name);

    std::vector<aurhub::BranchInfo> branches;
    branches.reserve(packed.size() + loose.size());
    std::size_t packed_index = 0;
    std::size_t loose_index = 0;
    while (packed_index < packed.size() || loose_index < loose.size()) {
        // NOLINTBEGIN(bugprone-branch-clone)
        if (packed_index == packed.size()) {
            branches.push_back(std::move(loose[loose_index++]));
        } else if (loose_index == loose.size()) {
            branches.push_back(std::move(packed[packed_index++]));
        } else if (packed[packed_index].name < loose[loose_index].name) {
            branches.push_back(std::move(packed[packed_index++]));
        } else if (loose[loose_index].name < packed[packed_index].name) {
            branches.push_back(std::move(loose[loose_index++]));
        } else {
            branches.push_back(std::move(loose[loose_index++]));
            ++packed_index;
        }
        // NOLINTEND(bugprone-branch-clone)
    }
    if (branches.empty()) {
        return list_branches(repo, false);
    }
    return branches;
}

struct GitObject {
    std::string type;
    std::string content;
};

class GitBatch {
public:
    explicit GitBatch(const std::string& repo) {
        int input_pipe[2];
        int output_pipe[2];
        if (::pipe2(input_pipe, O_CLOEXEC) != 0 ||
            ::pipe2(output_pipe, O_CLOEXEC) != 0) {
            throw std::runtime_error(std::string("pipe2: ") + std::strerror(errno));
        }

        pid_ = ::fork();
        if (pid_ < 0) {
            throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
        }
        if (pid_ == 0) {
            ::dup2(input_pipe[0], STDIN_FILENO);
            ::dup2(output_pipe[1], STDOUT_FILENO);
            ::close(input_pipe[0]);
            ::close(input_pipe[1]);
            ::close(output_pipe[0]);
            ::close(output_pipe[1]);
            ::execlp("git", "git", "-c",
                     "core.packedGitWindowSize=16m", "-c",
                     "core.packedGitLimit=64m", "-c",
                     "core.deltaBaseCacheLimit=32m", "-C", repo.c_str(),
                     "cat-file", "--batch", static_cast<char*>(nullptr));
            _exit(127);
        }

        ::close(input_pipe[0]);
        ::close(output_pipe[1]);
        input_ = ::fdopen(input_pipe[1], "w");
        output_ = ::fdopen(output_pipe[0], "r");
        if (input_ == nullptr || output_ == nullptr) {
            throw std::runtime_error("fdopen failed");
        }
        static_cast<void>(::setvbuf(input_, nullptr, _IOFBF, static_cast<std::size_t>(64) * 1024));
        static_cast<void>(::setvbuf(output_, nullptr, _IOFBF, static_cast<std::size_t>(64) * 1024));
    }

    ~GitBatch() {
        if (input_ != nullptr) {
            static_cast<void>(::fclose(input_));
        }
        if (output_ != nullptr) {
            static_cast<void>(::fclose(output_));
        }
        if (pid_ > 0) {
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        std::free(line_buffer_);
    }

    GitBatch(const GitBatch&) = delete;
    GitBatch& operator=(const GitBatch&) = delete;
    GitBatch(GitBatch&&) = delete;
    GitBatch& operator=(GitBatch&&) = delete;

    std::optional<GitObject> read_object(std::string_view spec) {
        write_request(spec);
        flush_requests();
        return read_response();
    }

    std::pair<std::optional<GitObject>, std::optional<GitObject>>
    read_pair(std::string_view first, std::string_view second) {
        write_request(first);
        write_request(second);
        flush_requests();
        std::optional<GitObject> first_result = read_response();
        std::optional<GitObject> second_result = read_response();
        return {std::move(first_result), std::move(second_result)};
    }

    std::optional<std::string> read_blob(std::string_view spec) {
        std::optional<GitObject> object = read_object(spec);
        if (!object.has_value()) {
            return std::nullopt;
        }
        if (object->type != "blob") {
            throw std::runtime_error("expected blob for " + std::string(spec));
        }
        return std::move(object->content);
    }

private:
    void write_request(std::string_view spec) {
        if (::fwrite(spec.data(), 1, spec.size(), input_) != spec.size() ||
            ::fputc('\n', input_) == EOF) {
            throw std::runtime_error("cannot write git cat-file request");
        }
    }

    void flush_requests() {
        if (::fflush(input_) != 0) {
            throw std::runtime_error("cannot flush git cat-file requests");
        }
    }

    std::optional<GitObject> read_response() {
        const ssize_t line_size =
            ::getline(&line_buffer_, &line_capacity_, output_);
        if (line_size < 0) {
            throw std::runtime_error("unexpected end of git cat-file output");
        }
        std::string_view header(line_buffer_, static_cast<std::size_t>(line_size));
        while (!header.empty() &&
               (header.back() == '\n' || header.back() == '\r')) {
            header.remove_suffix(1);
        }
        if (header.ends_with(" missing") || header.ends_with(" ambiguous")) {
            return std::nullopt;
        }

        const std::size_t size_separator = header.rfind(' ');
        const std::size_t type_separator =
            size_separator == std::string::npos
                ? std::string::npos
                : header.rfind(' ', size_separator - 1);
        if (type_separator == std::string::npos ||
            size_separator == std::string::npos) {
            throw std::runtime_error("invalid git cat-file header: " +
                                     std::string(header));
        }
        const std::string_view type(header.data() + type_separator + 1,
                                    size_separator - type_separator - 1);
        const std::string_view size_text(header.data() + size_separator + 1,
                                         header.size() - size_separator - 1);
        std::size_t size = 0;
        const auto parsed = std::from_chars(size_text.data(),
                                            size_text.data() + size_text.size(), size);
        if (parsed.ec != std::errc{}) {
            throw std::runtime_error("unexpected git cat-file object: " +
                                     std::string(header));
        }

        std::string content(size, '\0');
        std::size_t offset = 0;
        while (offset < size) {
            const std::size_t count =
                ::fread(content.data() + offset, 1, size - offset, output_);
            if (count == 0) {
                throw std::runtime_error("short read from git cat-file");
            }
            offset += count;
        }
        if (::fgetc(output_) != '\n') {
            throw std::runtime_error("git cat-file response is not terminated");
        }
        return GitObject{std::string(type), std::move(content)};
    }

    pid_t pid_ = -1;
    FILE* input_ = nullptr;
    FILE* output_ = nullptr;
    char* line_buffer_ = nullptr;
    std::size_t line_capacity_ = 0;
};

std::int64_t commit_timestamp(std::string_view commit) {
    std::size_t start = commit.starts_with("committer ")
                            ? 0
                            : commit.find("\ncommitter ");
    if (start == std::string_view::npos) {
        throw std::runtime_error("commit object has no committer line");
    }
    if (start != 0) {
        ++start;
    }
    const std::size_t end = commit.find('\n', start);
    const std::string_view line = commit.substr(start, end - start);
    const std::size_t timezone_space = line.rfind(' ');
    const std::size_t timestamp_space =
        timezone_space == std::string_view::npos
            ? std::string_view::npos
            : line.rfind(' ', timezone_space - 1);
    if (timestamp_space == std::string_view::npos ||
        timezone_space == std::string_view::npos) {
        throw std::runtime_error("invalid committer line");
    }
    const std::string_view value = line.substr(
        timestamp_space + 1, timezone_space - timestamp_space - 1);
    std::int64_t timestamp = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(),
                                        timestamp);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("invalid commit timestamp");
    }
    return timestamp;
}

std::optional<std::string> read_branch_srcinfo(GitBatch& batch,
                                               aurhub::BranchInfo& branch,
                                               bool load_timestamp) {
    const std::string oid = oid_string(branch.oid);
    if (!load_timestamp) {
        return batch.read_blob(oid + ":.SRCINFO");
    }

    auto [commit, srcinfo] =
        batch.read_pair(oid, oid + ":.SRCINFO");
    if (!commit.has_value() || commit->type != "commit") {
        throw std::runtime_error("cannot read commit " + oid);
    }
    branch.updated_at = commit_timestamp(commit->content);
    if (!srcinfo.has_value()) {
        return std::nullopt;
    }
    if (srcinfo->type != "blob") {
        throw std::runtime_error("expected .SRCINFO blob for " + oid);
    }
    return std::move(srcinfo->content);
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

void append_branch_packages(GitBatch& batch,
                            aurhub::BranchInfo& branch,
                            std::uint32_t branch_index,
                            bool load_timestamp,
                            std::vector<aurhub::CompiledPackage>& packages,
                            ParseStats& parse_stats) {
    const std::optional<std::string> srcinfo =
        read_branch_srcinfo(batch, branch, load_timestamp);
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
    GitBatch& batch,
    aurhub::BranchInfo& branch,
    std::uint32_t branch_index,
    bool load_timestamp,
    StringArena& arena,
    std::vector<aurhub::CompiledPackageView>& packages,
    ParseStats& parse_stats) {
    const std::optional<std::string> srcinfo =
        read_branch_srcinfo(batch, branch, load_timestamp);
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
            GitBatch batch(options.repo);
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

enum class MergeSource : std::uint8_t {
    previous_shadow,
    previous_active,
    changed,
};

constexpr std::uint32_t kNoPackageDetails =
    std::numeric_limits<std::uint32_t>::max();

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

void merge_generation_inputs(
    const aurhub::GenerationView& previous,
    const PackageNameTable& previous_names,
    std::span<const std::uint32_t> old_to_new,
    std::vector<aurhub::CompiledPackage>& changed_packages,
    std::vector<aurhub::GenerationPackageInput>& packages,
    std::vector<aurhub::GenerationPackageInput>& shadow_packages,
    std::vector<aurhub::GenerationPackageDetails>& package_details) {
    constexpr std::uint32_t kMissingBranch =
        std::numeric_limits<std::uint32_t>::max();
    std::stable_sort(
        changed_packages.begin(), changed_packages.end(),
        [](const aurhub::CompiledPackage& left,
           const aurhub::CompiledPackage& right) {
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
        const aurhub::CompiledPackage& package =
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
            aurhub::GenerationPackageDetails{search, json, updated_at});
        return index;
    };
    const auto make_input = [&](const MergeCandidate& candidate) {
        switch (candidate.source) {
            case MergeSource::previous_shadow: {
                const aurhub::GenerationLocation location =
                    previous.shadow_location(candidate.index);
                std::uint32_t details_index = kNoPackageDetails;
                if (location.source != aurhub::GenerationSource::base_active &&
                    location.source != aurhub::GenerationSource::base_shadow) {
                    details_index = add_details(
                        previous.shadow_search_text(candidate.index),
                        previous.shadow_json(candidate.index),
                        previous.shadow_updated_at(candidate.index));
                }
                return aurhub::GenerationPackageInput{
                    candidate.name, location, candidate.branch_index,
                    details_index,
                };
            }
            case MergeSource::previous_active: {
                const aurhub::GenerationLocation location =
                    previous.location(candidate.index);
                std::uint32_t details_index = kNoPackageDetails;
                if (location.source != aurhub::GenerationSource::base_active &&
                    location.source != aurhub::GenerationSource::base_shadow) {
                    details_index = add_details(
                        previous.search_text(candidate.index),
                        previous.json(candidate.index),
                        previous.updated_at(candidate.index));
                }
                return aurhub::GenerationPackageInput{
                    candidate.name, location, candidate.branch_index,
                    details_index,
                };
            }
            case MergeSource::changed: {
                const aurhub::CompiledPackageView package =
                    aurhub::package_view(changed_packages[candidate.index]);
                return aurhub::GenerationPackageInput{
                    package.name,
                    {aurhub::GenerationSource::inline_record, 0},
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
    std::optional<aurhub::GenerationPackageInput> winner;
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
        const aurhub::GenerationPackageInput input = make_input(*candidate);
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

    PackageNameTable previous_names;
    decode_package_names(previous.packages_gz(), previous.package_count(),
                         previous_names);

    GitBatch batch(options.repo);
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
    merge_generation_inputs(previous, previous_names, old_to_new,
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
                if (package.details_index != kNoPackageDetails) {
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
                             " +refs/heads/*:refs/heads/* --prune")
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
                        list_branches_fast(options.repo);
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
            list_branches_fast(options.repo);
        full_build(options, std::move(branches), started);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "aurhub-indexer: " << error.what() << '\n';
        return 1;
    }
}
