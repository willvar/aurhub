#include "srcinfo.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "srcinfo test failed: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

bool has_diagnostic(const aurhub::SrcinfoResult& result,
                    aurhub::SrcinfoSeverity severity,
                    aurhub::SrcinfoDiagnosticCode code) {
    return std::ranges::any_of(result.diagnostics,
        [severity, code](const aurhub::SrcinfoDiagnostic& diagnostic) {
            return diagnostic.severity == severity && diagnostic.code == code;
        });
}

void require_values(const std::vector<std::string>& actual,
                    std::initializer_list<std::string_view> expected,
                    std::string_view message) {
    if (actual.size() != expected.size()) {
        fail(message);
    }
    std::size_t index = 0;
    for (const std::string_view value : expected) {
        if (actual[index] != value) {
            fail(message);
        }
        ++index;
    }
}

void basic_and_rpc_fields() {
    const aurhub::SrcinfoResult result = aurhub::parse_srcinfo(R"(
pkgbase = alpha
pkgdesc = Alpha package
pkgver = 1.2.3
pkgrel = 4
arch = x86_64
groups = tools
license = MIT
depends = libc
replaces = alpha-old
pkgname = alpha
)");
    require(!result.has_fatal(), "basic document must parse");
    require(result.diagnostics.empty(), "basic document must be clean");
    require(result.packages.size() == 1, "basic package count");
    const aurhub::Package& package = result.packages.front();
    require(package.version == "1.2.3-4", "basic version");
    require_values(package.depends, {"libc"}, "basic depends");
    require_values(package.groups, {"tools"}, "basic groups");
    require_values(package.replaces, {"alpha-old"}, "basic replaces");
    const std::string json = aurhub::package_json(package);
    require(json.contains("\"Groups\":[\"tools\"]"),
            "Groups must be serialized");
    require(json.contains("\"Replaces\":[\"alpha-old\"]"),
            "Replaces must be serialized");
}

void final_package_values_do_not_append_base() {
    const aurhub::SrcinfoResult result = aurhub::parse_srcinfo(R"(
pkgbase = split
pkgver = 1
pkgrel = 1
arch = x86_64
depends = global
depends_x86_64 = base-x86
pkgname = split
depends = global
depends = local
depends_x86_64 = package-x86
)");
    require(!result.has_fatal(), "split override must parse");
    require_values(result.packages.front().depends,
                   {"global", "local", "package-x86"},
                   "package section contains final values");
}

void architecture_dimensions_inherit_independently() {
    const aurhub::SrcinfoResult result = aurhub::parse_srcinfo(R"(
pkgbase = arch-merge
pkgver = 1
pkgrel = 1
arch = x86_64
depends = global
depends_x86_64 = base-x86
pkgname = arch-merge
depends_x86_64 = package-x86
)");
    require(!result.has_fatal(), "architecture override must parse");
    require_values(result.packages.front().depends,
                   {"package-x86", "global"},
                   "only matching architecture dimension is replaced");

    const aurhub::SrcinfoResult package_arch = aurhub::parse_srcinfo(R"(
pkgbase = package-arch
pkgver = 1
pkgrel = 1
arch = x86_64
pkgname = package-arch
arch = aarch64
depends_aarch64 = package-only
)");
    require(!package_arch.has_fatal(),
            "package architecture override controls package suffixes");
    require_values(package_arch.packages.front().depends, {"package-only"},
                   "package architecture dependency");
}

void explicit_empty_clears_inheritance() {
    const aurhub::SrcinfoResult result = aurhub::parse_srcinfo(R"(
pkgbase = empty-override
pkgdesc = inherited description
pkgver = 1
pkgrel = 1
url = https://example.invalid
arch = any
groups = inherited-group
license = MIT
depends = inherited-dependency
pkgname = empty-override
pkgdesc =
url =
groups =
license =
depends =
)");
    require(!result.has_fatal(), "empty overrides must parse");
    const aurhub::Package& package = result.packages.front();
    require(package.description.empty(), "empty description clears base");
    require(package.url.empty(), "empty URL clears base");
    require(package.groups.empty(), "empty groups clear base");
    require(package.license.empty(), "empty license clears base");
    require(package.depends.empty(), "empty depends clear base");
}

void warning_fields_are_ignored_without_quarantine() {
    const aurhub::SrcinfoResult result = aurhub::parse_srcinfo(R"(
pkgbase = warning-only
pkgver = 1
pkgrel = 1
arch = x86_64
source_aarch64 = source.tar.gz
pkgname = warning-only
source = package-source.tar.gz
sha256sums = SKIP
)");
    require(!result.has_fatal(), "non-RPC field errors are warnings");
    require(result.packages.size() == 1, "warning branch remains indexed");
    require(has_diagnostic(result, aurhub::SrcinfoSeverity::warning,
                           aurhub::SrcinfoDiagnosticCode::unsupported_arch),
            "unsupported source architecture warning");
    require(has_diagnostic(result, aurhub::SrcinfoSeverity::warning,
                           aurhub::SrcinfoDiagnosticCode::ignored_package_field),
            "package source warning");
}

void core_errors_quarantine_the_document() {
    const aurhub::SrcinfoResult package_version = aurhub::parse_srcinfo(R"(
pkgbase = bad-version
pkgver = 1
pkgrel = 1
arch = any
pkgname = bad-version
pkgver = 2
)");
    require(package_version.has_fatal(), "package pkgver must be fatal");
    require(package_version.packages.empty(), "fatal document is quarantined");
    require(has_diagnostic(
                package_version, aurhub::SrcinfoSeverity::fatal,
                aurhub::SrcinfoDiagnosticCode::field_not_allowed_in_package),
            "package pkgver diagnostic");

    const aurhub::SrcinfoResult unknown = aurhub::parse_srcinfo(R"(
pkgbase = bad-unknown
pkgver = 1
pkgrel = 1
arch = any
depend = libc
pkgname = bad-unknown
)");
    require(unknown.has_fatal(), "unknown field must be fatal");
    require(has_diagnostic(unknown, aurhub::SrcinfoSeverity::fatal,
                           aurhub::SrcinfoDiagnosticCode::unknown_field),
            "unknown field diagnostic");

    const aurhub::SrcinfoResult duplicate = aurhub::parse_srcinfo(R"(
pkgbase = duplicate
pkgver = 1
pkgrel = 1
arch = any
pkgname = duplicate
pkgbase = duplicate
pkgname = duplicate
)");
    require(duplicate.has_fatal(), "duplicate headers must be fatal");
    require(has_diagnostic(duplicate, aurhub::SrcinfoSeverity::fatal,
                           aurhub::SrcinfoDiagnosticCode::duplicate_pkgbase),
            "duplicate pkgbase diagnostic");
    require(has_diagnostic(duplicate, aurhub::SrcinfoSeverity::fatal,
                           aurhub::SrcinfoDiagnosticCode::duplicate_pkgname),
            "duplicate pkgname diagnostic");
}

void required_fields_and_arch_suffixes_are_strict() {
    const aurhub::SrcinfoResult empty_required = aurhub::parse_srcinfo(R"(
pkgbase = empty-required
pkgver =
pkgrel = 1
arch =
pkgname = empty-required
)");
    require(empty_required.has_fatal(), "empty required fields must fail");
    require(has_diagnostic(empty_required, aurhub::SrcinfoSeverity::fatal,
                           aurhub::SrcinfoDiagnosticCode::missing_pkgver),
            "empty pkgver diagnostic");
    require(has_diagnostic(empty_required, aurhub::SrcinfoSeverity::fatal,
                           aurhub::SrcinfoDiagnosticCode::missing_arch),
            "empty arch diagnostic");

    const aurhub::SrcinfoResult bad_arch = aurhub::parse_srcinfo(R"(
pkgbase = bad-arch
pkgver = 1
pkgrel = 1
arch = x86_64
depends_any = invalid
pkgname = bad-arch
)");
    require(bad_arch.has_fatal(), "depends_any must fail");
    require(has_diagnostic(bad_arch, aurhub::SrcinfoSeverity::fatal,
                           aurhub::SrcinfoDiagnosticCode::invalid_arch_any),
            "depends_any diagnostic");
}

void epoch_zero_is_not_rendered() {
    const aurhub::SrcinfoResult result = aurhub::parse_srcinfo(R"(
pkgbase = epoch-zero
pkgver = 2
pkgrel = 3
epoch = 0
arch = any
pkgname = epoch-zero
)");
    require(!result.has_fatal(), "epoch zero document must parse");
    require(result.packages.front().version == "2-3",
            "epoch zero must be omitted");
}

void unicode_blank_lines_are_ignored() {
    const aurhub::SrcinfoResult result = aurhub::parse_srcinfo(
        "pkgbase = unicode-blank\n"
        "pkgver = 1\n"
        "pkgrel = 1\n"
        "arch = any\n"
        "\xc2\xa0\n"
        "pkgname = unicode-blank\n");
    require(!result.has_fatal(), "Unicode blank line must be ignored");
    require(result.packages.size() == 1, "Unicode blank package count");
}

}  // namespace

int main() {
    basic_and_rpc_fields();
    final_package_values_do_not_append_base();
    architecture_dimensions_inherit_independently();
    explicit_empty_clears_inheritance();
    warning_fields_are_ignored_without_quarantine();
    core_errors_quarantine_the_document();
    required_fields_and_arch_suffixes_are_strict();
    epoch_zero_is_not_rendered();
    unicode_blank_lines_are_ignored();
    std::cout << "srcinfo tests passed\n";
    return 0;
}
