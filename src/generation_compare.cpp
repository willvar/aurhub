#include "generation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[noreturn]] void mismatch(std::string_view section, std::size_t index) {
    throw std::runtime_error(std::string(section) +
                             " differs at index " +
                             std::to_string(index));
}

void verify_exact_lookup(const aurhub::GenerationView& generation,
                         std::string_view label) {
    for (std::size_t i = 0; i < generation.package_count(); ++i) {
        const auto found = generation.find_package(generation.name(i));
        if (!found || *found != i) {
            throw std::runtime_error(std::string(label) +
                                     " exact lookup differs at index " +
                                     std::to_string(i));
        }
    }
    if (generation.find_package("\x01aurhub-definitely-not-a-package")) {
        throw std::runtime_error(std::string(label) +
                                 " exact lookup found a missing package");
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0]
                      << " GENERATION_A GENERATION_B\n";
            return 2;
        }
        const aurhub::GenerationView left(argv[1]);
        const aurhub::GenerationView right(argv[2]);
        verify_exact_lookup(left, "left");
        verify_exact_lookup(right, "right");
        if (left.branch_count() != right.branch_count() ||
            left.package_count() != right.package_count() ||
            left.shadow_package_count() != right.shadow_package_count()) {
            throw std::runtime_error("generation counts differ");
        }
        for (std::size_t i = 0; i < left.branch_count(); ++i) {
            if (left.branch_name(i) != right.branch_name(i) ||
                left.branch_oid(i) != right.branch_oid(i) ||
                left.branch_updated_at(i) != right.branch_updated_at(i)) {
                mismatch("branch", i);
            }
        }
        for (std::size_t i = 0; i < left.package_count(); ++i) {
            if (left.name(i) != right.name(i) ||
                left.search_text(i) != right.search_text(i) ||
                left.json(i) != right.json(i) ||
                left.updated_at(i) != right.updated_at(i) ||
                left.branch_name(left.package_branch_index(i)) !=
                    right.branch_name(right.package_branch_index(i))) {
                std::cerr << "left active: name=" << left.name(i)
                          << " branch="
                          << left.branch_name(left.package_branch_index(i))
                          << " json=" << left.json(i) << '\n';
                std::cerr << "right active: name=" << right.name(i)
                          << " branch="
                          << right.branch_name(right.package_branch_index(i))
                          << " json=" << right.json(i) << '\n';
                mismatch("active package", i);
            }
        }
        for (std::size_t i = 0; i < left.shadow_package_count(); ++i) {
            if (left.shadow_name(i) != right.shadow_name(i) ||
                left.shadow_search_text(i) != right.shadow_search_text(i) ||
                left.shadow_json(i) != right.shadow_json(i) ||
                left.shadow_updated_at(i) != right.shadow_updated_at(i) ||
                left.branch_name(left.shadow_branch_index(i)) !=
                    right.branch_name(right.shadow_branch_index(i))) {
                mismatch("shadow package", i);
            }
        }
        const auto left_gzip = left.packages_gz();
        const auto right_gzip = right.packages_gz();
        if (left_gzip.size() != right_gzip.size() ||
            !std::equal(left_gzip.begin(), left_gzip.end(),
                        right_gzip.begin())) {
            throw std::runtime_error("packages.gz differs");
        }
        std::cout << "generations are equivalent: branches="
                  << left.branch_count() << " packages="
                  << left.package_count() << " shadow="
                  << left.shadow_package_count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "aurhub-generation-compare: " << error.what() << '\n';
        return 1;
    }
}
