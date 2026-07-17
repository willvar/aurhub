#pragma once

#include "model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aurhub {

enum class SrcinfoSeverity : std::uint8_t {
    warning,
    fatal,
};

enum class SrcinfoDiagnosticCode : std::uint8_t {
    malformed_line,
    empty_key,
    field_before_pkgbase,
    duplicate_pkgbase,
    empty_pkgbase,
    duplicate_pkgname,
    empty_pkgname,
    unknown_field,
    duplicate_scalar,
    field_not_allowed_in_package,
    ignored_package_field,
    invalid_arch_any,
    unsupported_arch,
    missing_pkgbase,
    missing_pkgname,
    missing_pkgver,
    missing_pkgrel,
    missing_arch,
    count,
};

struct SrcinfoDiagnostic {
    SrcinfoSeverity severity = SrcinfoSeverity::fatal;
    SrcinfoDiagnosticCode code = SrcinfoDiagnosticCode::malformed_line;
    std::uint32_t line = 0;
    std::string key;
};

struct SrcinfoResult {
    std::vector<Package> packages;
    std::vector<SrcinfoDiagnostic> diagnostics;

    bool has_fatal() const;
    std::size_t warning_count() const;
    std::size_t fatal_count() const;
};

constexpr std::size_t srcinfo_diagnostic_code_count() {
    return static_cast<std::size_t>(SrcinfoDiagnosticCode::count);
}

std::string_view srcinfo_severity_name(SrcinfoSeverity severity);
std::string_view srcinfo_diagnostic_code_name(SrcinfoDiagnosticCode code);
SrcinfoResult parse_srcinfo(std::string_view input);
std::string package_json(const Package& package);
std::string normalized_search_text(const Package& package);

}  // namespace aurhub
