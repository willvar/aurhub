#pragma once

#include "model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace aurhub {

struct GitObject {
    std::string type;
    std::string content;
};

class GitBatch {
public:
    explicit GitBatch(const std::string& repo);
    ~GitBatch();

    GitBatch(const GitBatch&) = delete;
    GitBatch& operator=(const GitBatch&) = delete;
    GitBatch(GitBatch&&) = delete;
    GitBatch& operator=(GitBatch&&) = delete;

    std::optional<GitObject> read_object(std::string_view spec);
    std::pair<std::optional<GitObject>, std::optional<GitObject>> read_pair(
        std::string_view first, std::string_view second);
    std::optional<std::string> read_blob(std::string_view spec);

private:
    void write_request(std::string_view spec);
    void flush_requests();
    std::optional<GitObject> read_response();

    pid_t pid_ = -1;
    FILE* input_ = nullptr;
    FILE* output_ = nullptr;
    char* line_buffer_ = nullptr;
    std::size_t line_capacity_ = 0;
};

std::array<std::byte, 20> parse_oid(std::string_view value);
std::string oid_string(const std::array<std::byte, 20>& oid);

std::vector<BranchInfo> list_branches(const std::string& repo,
                                      bool include_timestamp);
std::vector<BranchInfo> list_branches_fast(const std::string& repo);

std::int64_t commit_timestamp(std::string_view commit);
std::optional<std::string> read_branch_srcinfo(GitBatch& batch,
                                               BranchInfo& branch,
                                               bool load_timestamp);

}  // namespace aurhub
