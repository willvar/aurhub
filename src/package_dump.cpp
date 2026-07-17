#include "generation.hpp"

#include <cstddef>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: " << argv[0]
                  << " SNAPSHOT OUTPUT.gz [OUTPUT.jsonl]\n";
        return 2;
    }
    try {
        const aurhub::GenerationView generation(
            argv[1], aurhub::SnapshotValidation::records_only);
        const std::span<const std::byte> gzip = generation.packages_gz();
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot open output file");
        }
        output.write(reinterpret_cast<const char*>(gzip.data()),
                     static_cast<std::streamsize>(gzip.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("cannot write output file");
        }
        if (argc == 4) {
            std::ofstream json(argv[3], std::ios::binary | std::ios::trunc);
            if (!json) {
                throw std::runtime_error("cannot open JSON output file");
            }
            for (std::size_t i = 0; i < generation.package_count(); ++i) {
                json << generation.json(i) << '\n';
            }
            json.close();
            if (!json) {
                throw std::runtime_error("cannot write JSON output file");
            }
        }
        std::cerr << "wrote " << generation.package_count()
                  << " package names\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "aurhub-package-dump: " << error.what() << '\n';
        return 1;
    }
}
