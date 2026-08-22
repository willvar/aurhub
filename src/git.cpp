#include "git.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

namespace aurhub {

namespace {

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

}  // namespace

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

std::vector<BranchInfo> list_branches(const std::string& repo,
                                      bool include_timestamp) {
    std::vector<std::string> args{
        "git", "-C", repo, "for-each-ref",
        "--sort=refname",
        include_timestamp
            ? "--format=%(refname:short)%09%(objectname)%09%(committerdate:unix)"
            : "--format=%(refname:short)%09%(objectname)",
        "refs/heads/"};
    const std::string output = capture_command(std::move(args));

    std::vector<BranchInfo> branches;
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
        BranchInfo branch;
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

std::vector<BranchInfo> list_branches_fast(const std::string& repo) {
    std::vector<BranchInfo> packed;
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
        BranchInfo branch;
        branch.oid = parse_oid(std::string_view(line).substr(0, space));
        branch.name.assign(line.data() + space + 1 + kHeadsPrefix.size(),
                           line.size() - space - 1 - kHeadsPrefix.size());
        packed.push_back(std::move(branch));
    }

    std::vector<BranchInfo> loose;
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
            BranchInfo branch;
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

    const auto by_name = [](const BranchInfo& left, const BranchInfo& right) {
        return left.name < right.name;
    };
    std::sort(packed.begin(), packed.end(), by_name);
    std::sort(loose.begin(), loose.end(), by_name);

    std::vector<BranchInfo> branches;
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

GitBatch::GitBatch(const std::string& repo) {
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

GitBatch::~GitBatch() {
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

std::optional<GitObject> GitBatch::read_object(std::string_view spec) {
    write_request(spec);
    flush_requests();
    return read_response();
}

std::pair<std::optional<GitObject>, std::optional<GitObject>>
GitBatch::read_pair(std::string_view first, std::string_view second) {
    write_request(first);
    write_request(second);
    flush_requests();
    std::optional<GitObject> first_result = read_response();
    std::optional<GitObject> second_result = read_response();
    return {std::move(first_result), std::move(second_result)};
}

std::optional<std::string> GitBatch::read_blob(std::string_view spec) {
    std::optional<GitObject> object = read_object(spec);
    if (!object.has_value()) {
        return std::nullopt;
    }
    if (object->type != "blob") {
        throw std::runtime_error("expected blob for " + std::string(spec));
    }
    return std::move(object->content);
}

void GitBatch::write_request(std::string_view spec) {
    if (::fwrite(spec.data(), 1, spec.size(), input_) != spec.size() ||
        ::fputc('\n', input_) == EOF) {
        throw std::runtime_error("cannot write git cat-file request");
    }
}

void GitBatch::flush_requests() {
    if (::fflush(input_) != 0) {
        throw std::runtime_error("cannot flush git cat-file requests");
    }
}

std::optional<GitObject> GitBatch::read_response() {
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
                                               BranchInfo& branch,
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

}  // namespace aurhub
