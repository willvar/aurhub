#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aurhub {

struct Package {
    std::string name;
    std::string base;
    std::string version;
    std::string description;
    std::string url;
    std::vector<std::string> license;
    std::vector<std::string> depends;
    std::vector<std::string> make_depends;
    std::vector<std::string> check_depends;
    std::vector<std::string> opt_depends;
    std::vector<std::string> provides;
    std::vector<std::string> conflicts;
    std::vector<std::string> replaces;
    std::vector<std::string> groups;
    std::vector<std::string> keywords;
    std::int64_t updated_at = 0;
};

struct BranchInfo {
    std::string name;
    std::array<std::byte, 20> oid{};
    std::int64_t updated_at = 0;
};

struct CompiledPackage {
    std::string name;
    std::string search;
    std::string json;
    std::uint32_t branch_index = 0;
    std::int64_t updated_at = 0;
};

struct CompiledPackageView {
    std::string_view name;
    std::string_view search;
    std::string_view json;
    std::uint32_t branch_index = 0;
    std::int64_t updated_at = 0;
};

inline CompiledPackageView package_view(const CompiledPackage& package) {
    return CompiledPackageView{
        package.name,
        package.search,
        package.json,
        package.branch_index,
        package.updated_at,
    };
}

}  // namespace aurhub
