#include "srcinfo.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <simdjson.h>
#include <string>
#include <unordered_set>
#include <utility>

namespace aurhub {
namespace {

std::string_view trim(std::string_view value) {
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

bool unicode_blank(std::string_view value) {
    while (!value.empty()) {
        const unsigned char first = static_cast<unsigned char>(value.front());
        if (std::isspace(first) != 0) {
            value.remove_prefix(1);
            continue;
        }
        if (value.size() >= 2U && first == 0xc2U &&
            static_cast<unsigned char>(value[1]) == 0xa0U) {
            value.remove_prefix(2);
            continue;
        }
        return false;
    }
    return true;
}

template <typename T>
struct PresentValue {
    bool present = false;
    T value;
};

struct ArchDimension {
    std::string arch;
    std::vector<std::string> values;
    std::uint32_t line = 0;
};

struct ArchField {
    std::vector<ArchDimension> dimensions;
};

struct Fields {
    PresentValue<std::string> description;
    PresentValue<std::string> url;
    PresentValue<std::string> epoch;
    PresentValue<std::string> pkgver;
    PresentValue<std::string> pkgrel;
    PresentValue<std::vector<std::string>> arches;
    PresentValue<std::vector<std::string>> license;
    PresentValue<std::vector<std::string>> groups;
    ArchField depends;
    ArchField make_depends;
    ArchField check_depends;
    ArchField opt_depends;
    ArchField provides;
    ArchField conflicts;
    ArchField replaces;
};

struct PackageBlock {
    std::string name;
    std::uint32_t line = 0;
    Fields fields;
};

struct IgnoredArchUse {
    std::string key;
    std::string arch;
    std::uint32_t line = 0;
    std::size_t package_index = std::numeric_limits<std::size_t>::max();
};

enum class FieldId : std::uint8_t {
    pkgdesc,
    pkgver,
    pkgrel,
    epoch,
    url,
    install,
    changelog,
    arch,
    groups,
    license,
    checkdepends,
    makedepends,
    depends,
    optdepends,
    provides,
    conflicts,
    replaces,
    noextract,
    options,
    backup,
    source,
    validpgpkeys,
    checksum,
    unknown,
};

struct ParsedKey {
    FieldId field = FieldId::unknown;
    std::string_view arch;
};

bool checksum_name(std::string_view key) {
    return key == "cksums" || key == "md5sums" || key == "sha1sums" ||
           key == "sha224sums" || key == "sha256sums" ||
           key == "sha384sums" || key == "sha512sums" || key == "b2sums";
}

FieldId exact_field(std::string_view key) {
    if (key == "pkgdesc") { return FieldId::pkgdesc; }
    if (key == "pkgver") { return FieldId::pkgver; }
    if (key == "pkgrel") { return FieldId::pkgrel; }
    if (key == "epoch") { return FieldId::epoch; }
    if (key == "url") { return FieldId::url; }
    if (key == "install") { return FieldId::install; }
    if (key == "changelog") { return FieldId::changelog; }
    if (key == "arch") { return FieldId::arch; }
    if (key == "groups") { return FieldId::groups; }
    if (key == "license") { return FieldId::license; }
    if (key == "checkdepends") { return FieldId::checkdepends; }
    if (key == "makedepends") { return FieldId::makedepends; }
    if (key == "depends") { return FieldId::depends; }
    if (key == "optdepends") { return FieldId::optdepends; }
    if (key == "provides") { return FieldId::provides; }
    if (key == "conflicts") { return FieldId::conflicts; }
    if (key == "replaces") { return FieldId::replaces; }
    if (key == "noextract") { return FieldId::noextract; }
    if (key == "options") { return FieldId::options; }
    if (key == "backup") { return FieldId::backup; }
    if (key == "source") { return FieldId::source; }
    if (key == "validpgpkeys") { return FieldId::validpgpkeys; }
    if (checksum_name(key)) { return FieldId::checksum; }
    return FieldId::unknown;
}

bool architecture_capable(FieldId field) {
    switch (field) {
        case FieldId::checkdepends:
        case FieldId::makedepends:
        case FieldId::depends:
        case FieldId::optdepends:
        case FieldId::options:
        case FieldId::provides:
        case FieldId::conflicts:
        case FieldId::replaces:
        case FieldId::source:
        case FieldId::checksum:
            return true;
        default:
            return false;
    }
}

ParsedKey parse_key(std::string_view key) {
    const FieldId exact = exact_field(key);
    if (exact != FieldId::unknown) {
        return ParsedKey{exact, {}};
    }
    const std::size_t underscore = key.find('_');
    if (underscore == std::string_view::npos || underscore + 1U == key.size()) {
        return {};
    }
    const FieldId base = exact_field(key.substr(0, underscore));
    if (!architecture_capable(base)) {
        return {};
    }
    return ParsedKey{base, key.substr(underscore + 1U)};
}

bool projected_field(FieldId field) {
    switch (field) {
        case FieldId::pkgdesc:
        case FieldId::pkgver:
        case FieldId::pkgrel:
        case FieldId::epoch:
        case FieldId::url:
        case FieldId::arch:
        case FieldId::groups:
        case FieldId::license:
        case FieldId::checkdepends:
        case FieldId::makedepends:
        case FieldId::depends:
        case FieldId::optdepends:
        case FieldId::provides:
        case FieldId::conflicts:
        case FieldId::replaces:
            return true;
        default:
            return false;
    }
}

bool allowed_in_package(FieldId field) {
    switch (field) {
        case FieldId::pkgdesc:
        case FieldId::url:
        case FieldId::install:
        case FieldId::changelog:
        case FieldId::arch:
        case FieldId::groups:
        case FieldId::license:
        case FieldId::depends:
        case FieldId::optdepends:
        case FieldId::options:
        case FieldId::provides:
        case FieldId::conflicts:
        case FieldId::replaces:
        case FieldId::backup:
            return true;
        default:
            return false;
    }
}

void add_diagnostic(std::vector<SrcinfoDiagnostic>& diagnostics,
                    SrcinfoSeverity severity,
                    SrcinfoDiagnosticCode code,
                    std::uint32_t line,
                    std::string_view key = {}) {
    diagnostics.push_back(
        SrcinfoDiagnostic{severity, code, line, std::string(key)});
}

void assign_scalar(PresentValue<std::string>& field,
                   std::string_view value,
                   std::uint32_t line,
                   std::string_view key,
                   std::vector<SrcinfoDiagnostic>& diagnostics) {
    if (field.present) {
        add_diagnostic(diagnostics, SrcinfoSeverity::fatal,
                       SrcinfoDiagnosticCode::duplicate_scalar, line, key);
    }
    field.present = true;
    field.value.assign(value);
}

void append_plain(PresentValue<std::vector<std::string>>& field,
                  std::string_view value) {
    field.present = true;
    if (!value.empty()) {
        field.value.emplace_back(value);
    }
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void append_arch(ArchField& field,
                 std::string_view arch,
                 std::string_view value,
                 std::uint32_t line) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    auto found = std::find_if(
        field.dimensions.begin(), field.dimensions.end(),
        [&](const ArchDimension& dimension) { return dimension.arch == arch; });
    if (found == field.dimensions.end()) {
        field.dimensions.push_back(
            ArchDimension{std::string(arch), {}, line});
        found = std::prev(field.dimensions.end());
    }
    if (!value.empty()) {
        found->values.emplace_back(value);
    }
}

ArchField* arch_field(Fields& fields, FieldId field) {
    switch (field) {
        case FieldId::depends: return &fields.depends;
        case FieldId::makedepends: return &fields.make_depends;
        case FieldId::checkdepends: return &fields.check_depends;
        case FieldId::optdepends: return &fields.opt_depends;
        case FieldId::provides: return &fields.provides;
        case FieldId::conflicts: return &fields.conflicts;
        case FieldId::replaces: return &fields.replaces;
        default: return nullptr;
    }
}

void assign_projected_field(Fields& fields,
                            ParsedKey parsed,
                            std::string_view key,
                            std::string_view value,
                            std::uint32_t line,
                            std::vector<SrcinfoDiagnostic>& diagnostics) {
    switch (parsed.field) {
        case FieldId::pkgdesc:
            assign_scalar(fields.description, value, line, key, diagnostics);
            return;
        case FieldId::url:
            assign_scalar(fields.url, value, line, key, diagnostics);
            return;
        case FieldId::epoch:
            assign_scalar(fields.epoch, value, line, key, diagnostics);
            return;
        case FieldId::pkgver:
            assign_scalar(fields.pkgver, value, line, key, diagnostics);
            return;
        case FieldId::pkgrel:
            assign_scalar(fields.pkgrel, value, line, key, diagnostics);
            return;
        case FieldId::arch:
            append_plain(fields.arches, value);
            return;
        case FieldId::license:
            append_plain(fields.license, value);
            return;
        case FieldId::groups:
            append_plain(fields.groups, value);
            return;
        default:
            break;
    }
    if (ArchField* field = arch_field(fields, parsed.field); field != nullptr) {
        append_arch(*field, parsed.arch, value, line);
    }
}

bool contains_arch(const std::vector<std::string>& arches,
                   std::string_view arch) {
    return std::any_of(arches.begin(), arches.end(),
                       [&](const std::string& value) { return value == arch; });
}

void validate_arch_field(const ArchField& field,
                         const std::vector<std::string>& arches,
                         std::string_view key,
                         SrcinfoSeverity severity,
                         std::vector<SrcinfoDiagnostic>& diagnostics) {
    for (const ArchDimension& dimension : field.dimensions) {
        if (dimension.arch.empty()) {
            continue;
        }
        std::string full_key(key);
        full_key.push_back('_');
        full_key.append(dimension.arch);
        if (dimension.arch == "any") {
            add_diagnostic(diagnostics, severity,
                           SrcinfoDiagnosticCode::invalid_arch_any,
                           dimension.line, full_key);
        } else if (!contains_arch(arches, dimension.arch)) {
            add_diagnostic(diagnostics, severity,
                           SrcinfoDiagnosticCode::unsupported_arch,
                           dimension.line, full_key);
        }
    }
}

void validate_core_arches(const Fields& fields,
                          const std::vector<std::string>& arches,
                          std::vector<SrcinfoDiagnostic>& diagnostics) {
    validate_arch_field(fields.depends, arches, "depends",
                        SrcinfoSeverity::fatal, diagnostics);
    validate_arch_field(fields.make_depends, arches, "makedepends",
                        SrcinfoSeverity::fatal, diagnostics);
    validate_arch_field(fields.check_depends, arches, "checkdepends",
                        SrcinfoSeverity::fatal, diagnostics);
    validate_arch_field(fields.opt_depends, arches, "optdepends",
                        SrcinfoSeverity::fatal, diagnostics);
    validate_arch_field(fields.provides, arches, "provides",
                        SrcinfoSeverity::fatal, diagnostics);
    validate_arch_field(fields.conflicts, arches, "conflicts",
                        SrcinfoSeverity::fatal, diagnostics);
    validate_arch_field(fields.replaces, arches, "replaces",
                        SrcinfoSeverity::fatal, diagnostics);
}

std::vector<std::string> flatten(const ArchField& field) {
    std::size_t size = 0;
    for (const ArchDimension& dimension : field.dimensions) {
        size += dimension.values.size();
    }
    std::vector<std::string> result;
    result.reserve(size);
    for (const ArchDimension& dimension : field.dimensions) {
        result.insert(result.end(), dimension.values.begin(),
                      dimension.values.end());
    }
    return result;
}

bool overrides_arch(const ArchField& field, std::string_view arch) {
    return std::any_of(
        field.dimensions.begin(), field.dimensions.end(),
        [&](const ArchDimension& dimension) { return dimension.arch == arch; });
}

std::vector<std::string> merge_arch(const ArchField& base,
                                    const ArchField& local) {
    std::size_t size = 0;
    for (const ArchDimension& dimension : local.dimensions) {
        size += dimension.values.size();
    }
    for (const ArchDimension& dimension : base.dimensions) {
        if (!overrides_arch(local, dimension.arch)) {
            size += dimension.values.size();
        }
    }
    std::vector<std::string> result;
    result.reserve(size);
    for (const ArchDimension& dimension : local.dimensions) {
        result.insert(result.end(), dimension.values.begin(),
                      dimension.values.end());
    }
    for (const ArchDimension& dimension : base.dimensions) {
        if (!overrides_arch(local, dimension.arch)) {
            result.insert(result.end(), dimension.values.begin(),
                          dimension.values.end());
        }
    }
    return result;
}

std::string make_version(const Fields& fields) {
    std::string version;
    if (fields.epoch.present && !fields.epoch.value.empty() &&
        fields.epoch.value != "0") {
        version.append(fields.epoch.value).push_back(':');
    }
    version.append(fields.pkgver.value);
    version.push_back('-');
    version.append(fields.pkgrel.value);
    return version;
}

void append_json_array(simdjson::builder::string_builder& sb,
                       std::string_view name,
                       const std::vector<std::string>& values) {
    sb.append_raw(",\"");
    sb.append_raw(name);
    sb.append_raw("\":");
    sb.start_array();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) { sb.append_comma(); }
        sb.escape_and_append_with_quotes(values[i]);
    }
    sb.end_array();
}

}  // namespace

bool SrcinfoResult::has_fatal() const {
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [](const SrcinfoDiagnostic& diagnostic) {
            return diagnostic.severity == SrcinfoSeverity::fatal;
        });
}

std::size_t SrcinfoResult::warning_count() const {
    return static_cast<std::size_t>(std::count_if(
        diagnostics.begin(), diagnostics.end(),
        [](const SrcinfoDiagnostic& diagnostic) {
            return diagnostic.severity == SrcinfoSeverity::warning;
        }));
}

std::size_t SrcinfoResult::fatal_count() const {
    return static_cast<std::size_t>(std::count_if(
        diagnostics.begin(), diagnostics.end(),
        [](const SrcinfoDiagnostic& diagnostic) {
            return diagnostic.severity == SrcinfoSeverity::fatal;
        }));
}

std::string_view srcinfo_severity_name(SrcinfoSeverity severity) {
    switch (severity) {
        case SrcinfoSeverity::warning: return "warning";
        case SrcinfoSeverity::fatal: return "fatal";
    }
    return "unknown";
}

std::string_view srcinfo_diagnostic_code_name(SrcinfoDiagnosticCode code) {
    switch (code) {
        case SrcinfoDiagnosticCode::malformed_line: return "malformed_line";
        case SrcinfoDiagnosticCode::empty_key: return "empty_key";
        case SrcinfoDiagnosticCode::field_before_pkgbase:
            return "field_before_pkgbase";
        case SrcinfoDiagnosticCode::duplicate_pkgbase:
            return "duplicate_pkgbase";
        case SrcinfoDiagnosticCode::empty_pkgbase: return "empty_pkgbase";
        case SrcinfoDiagnosticCode::duplicate_pkgname:
            return "duplicate_pkgname";
        case SrcinfoDiagnosticCode::empty_pkgname: return "empty_pkgname";
        case SrcinfoDiagnosticCode::unknown_field: return "unknown_field";
        case SrcinfoDiagnosticCode::duplicate_scalar:
            return "duplicate_scalar";
        case SrcinfoDiagnosticCode::field_not_allowed_in_package:
            return "field_not_allowed_in_package";
        case SrcinfoDiagnosticCode::ignored_package_field:
            return "ignored_package_field";
        case SrcinfoDiagnosticCode::invalid_arch_any:
            return "invalid_arch_any";
        case SrcinfoDiagnosticCode::unsupported_arch:
            return "unsupported_arch";
        case SrcinfoDiagnosticCode::missing_pkgbase: return "missing_pkgbase";
        case SrcinfoDiagnosticCode::missing_pkgname: return "missing_pkgname";
        case SrcinfoDiagnosticCode::missing_pkgver: return "missing_pkgver";
        case SrcinfoDiagnosticCode::missing_pkgrel: return "missing_pkgrel";
        case SrcinfoDiagnosticCode::missing_arch: return "missing_arch";
        case SrcinfoDiagnosticCode::count: break;
    }
    return "unknown";
}

SrcinfoResult parse_srcinfo(std::string_view input) {
    SrcinfoResult result;
    std::string base_name;
    bool has_pkgbase = false;
    Fields base_fields;
    std::vector<PackageBlock> blocks;
    PackageBlock* current = nullptr;
    std::unordered_set<std::string> package_names;
    std::vector<IgnoredArchUse> ignored_arches;
    std::uint32_t line_number = 0;

    while (!input.empty()) {
        ++line_number;
        const std::size_t newline = input.find('\n');
        std::string_view line = newline == std::string_view::npos
                                    ? input
                                    : input.substr(0, newline);
        input = newline == std::string_view::npos
                    ? std::string_view{}
                    : input.substr(newline + 1U);
        line = trim(line);
        if (line.empty() || unicode_blank(line) || line.front() == '#') {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                           SrcinfoDiagnosticCode::malformed_line, line_number);
            continue;
        }
        const std::string_view key = trim(line.substr(0, equals));
        const std::string_view value = trim(line.substr(equals + 1U));
        if (key.empty()) {
            add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                           SrcinfoDiagnosticCode::empty_key, line_number);
            continue;
        }

        if (key == "pkgbase") {
            if (has_pkgbase) {
                add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                               SrcinfoDiagnosticCode::duplicate_pkgbase,
                               line_number, key);
                continue;
            }
            has_pkgbase = true;
            base_name.assign(value);
            if (value.empty()) {
                add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                               SrcinfoDiagnosticCode::empty_pkgbase,
                               line_number, key);
            }
            continue;
        }
        if (key == "pkgname") {
            if (!has_pkgbase) {
                add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                               SrcinfoDiagnosticCode::field_before_pkgbase,
                               line_number, key);
                continue;
            }
            if (value.empty()) {
                add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                               SrcinfoDiagnosticCode::empty_pkgname,
                               line_number, key);
            } else if (!package_names.emplace(value).second) {
                add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                               SrcinfoDiagnosticCode::duplicate_pkgname,
                               line_number, key);
            }
            blocks.push_back(PackageBlock{std::string(value), line_number, {}});
            current = &blocks.back();
            continue;
        }
        if (!has_pkgbase) {
            add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                           SrcinfoDiagnosticCode::field_before_pkgbase,
                           line_number, key);
            continue;
        }

        const ParsedKey parsed = parse_key(key);
        if (parsed.field == FieldId::unknown) {
            add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                           SrcinfoDiagnosticCode::unknown_field, line_number,
                           key);
            continue;
        }
        const bool in_package = current != nullptr;
        if (in_package && !allowed_in_package(parsed.field)) {
            const SrcinfoSeverity severity = projected_field(parsed.field)
                                                 ? SrcinfoSeverity::fatal
                                                 : SrcinfoSeverity::warning;
            const SrcinfoDiagnosticCode code = projected_field(parsed.field)
                                                   ? SrcinfoDiagnosticCode::field_not_allowed_in_package
                                                   : SrcinfoDiagnosticCode::ignored_package_field;
            add_diagnostic(result.diagnostics, severity, code, line_number,
                           key);
            continue;
        }

        if (!projected_field(parsed.field)) {
            if (!parsed.arch.empty()) {
                ignored_arches.push_back(IgnoredArchUse{
                    std::string(key), std::string(parsed.arch), line_number,
                    in_package ? blocks.size() - 1U
                               : std::numeric_limits<std::size_t>::max()});
            }
            continue;
        }
        assign_projected_field(in_package ? current->fields : base_fields,
                               parsed, key, value, line_number,
                               result.diagnostics);
    }

    if (!has_pkgbase) {
        add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                       SrcinfoDiagnosticCode::missing_pkgbase, 0, "pkgbase");
    }
    if (blocks.empty()) {
        add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                       SrcinfoDiagnosticCode::missing_pkgname, 0, "pkgname");
    }
    if (!base_fields.pkgver.present || base_fields.pkgver.value.empty()) {
        add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                       SrcinfoDiagnosticCode::missing_pkgver, 0, "pkgver");
    }
    if (!base_fields.pkgrel.present || base_fields.pkgrel.value.empty()) {
        add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                       SrcinfoDiagnosticCode::missing_pkgrel, 0, "pkgrel");
    }
    if (!base_fields.arches.present || base_fields.arches.value.empty()) {
        add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                       SrcinfoDiagnosticCode::missing_arch, 0, "arch");
    }

    validate_core_arches(base_fields, base_fields.arches.value,
                         result.diagnostics);
    for (const PackageBlock& block : blocks) {
        const std::vector<std::string>& arches = block.fields.arches.present
                                                     ? block.fields.arches.value
                                                     : base_fields.arches.value;
        if (block.fields.arches.present && arches.empty()) {
            add_diagnostic(result.diagnostics, SrcinfoSeverity::fatal,
                           SrcinfoDiagnosticCode::missing_arch, block.line,
                           "arch");
        }
        validate_core_arches(block.fields, arches, result.diagnostics);
    }
    for (const IgnoredArchUse& use : ignored_arches) {
        const std::vector<std::string>* arches = &base_fields.arches.value;
        if (use.package_index != std::numeric_limits<std::size_t>::max()) {
            const Fields& package_fields = blocks[use.package_index].fields;
            if (package_fields.arches.present) {
                arches = &package_fields.arches.value;
            }
        }
        if (use.arch == "any") {
            add_diagnostic(result.diagnostics, SrcinfoSeverity::warning,
                           SrcinfoDiagnosticCode::invalid_arch_any, use.line,
                           use.key);
        } else if (!contains_arch(*arches, use.arch)) {
            add_diagnostic(result.diagnostics, SrcinfoSeverity::warning,
                           SrcinfoDiagnosticCode::unsupported_arch, use.line,
                           use.key);
        }
    }

    if (result.has_fatal()) {
        return result;
    }

    result.packages.reserve(blocks.size());
    for (const PackageBlock& block : blocks) {
        Package package;
        package.name = block.name;
        package.base = base_name;
        package.version = make_version(base_fields);
        package.description = block.fields.description.present
                                  ? block.fields.description.value
                                  : base_fields.description.value;
        package.url = block.fields.url.present ? block.fields.url.value
                                               : base_fields.url.value;
        package.license = block.fields.license.present
                              ? block.fields.license.value
                              : base_fields.license.value;
        package.groups = block.fields.groups.present
                             ? block.fields.groups.value
                             : base_fields.groups.value;
        package.depends = merge_arch(base_fields.depends, block.fields.depends);
        package.make_depends = flatten(base_fields.make_depends);
        package.check_depends = flatten(base_fields.check_depends);
        package.opt_depends =
            merge_arch(base_fields.opt_depends, block.fields.opt_depends);
        package.provides =
            merge_arch(base_fields.provides, block.fields.provides);
        package.conflicts =
            merge_arch(base_fields.conflicts, block.fields.conflicts);
        package.replaces =
            merge_arch(base_fields.replaces, block.fields.replaces);
        result.packages.push_back(std::move(package));
    }
    return result;
}

std::string package_json(const Package& package) {
    simdjson::builder::string_builder sb(576);
    sb.start_object();
    sb.append_key_value("Name", package.name);
    sb.append_comma();
    sb.append_key_value("PackageBase", package.base);
    sb.append_comma();
    sb.append_key_value("Version", package.version);
    sb.append_comma();
    sb.append_key_value("Description", package.description);
    sb.append_comma();
    sb.append_key_value("URL", package.url);
    sb.append_raw(",\"NumVotes\":0,\"Popularity\":0,\"OutOfDate\":null");
    sb.append_raw(",\"Maintainer\":\"aurhub\",\"FirstSubmitted\":");
    sb.append(package.updated_at);
    sb.append_raw(",\"LastModified\":");
    sb.append(package.updated_at);
    sb.append_raw(",\"URLPath\":\"/cgit/aur.git/snapshot/");
    sb.escape_and_append(package.name);
    sb.append_raw(".tar.gz\"");
    append_json_array(sb, "Depends", package.depends);
    append_json_array(sb, "MakeDepends", package.make_depends);
    append_json_array(sb, "CheckDepends", package.check_depends);
    append_json_array(sb, "OptDepends", package.opt_depends);
    append_json_array(sb, "Provides", package.provides);
    append_json_array(sb, "Conflicts", package.conflicts);
    append_json_array(sb, "Replaces", package.replaces);
    append_json_array(sb, "Groups", package.groups);
    append_json_array(sb, "License", package.license);
    append_json_array(sb, "Keywords", package.keywords);
    sb.end_object();
    return std::string(sb);
}

std::string normalized_search_text(const Package& package) {
    std::string out;
    out.reserve(package.name.size() + package.description.size() + 1U);
    out.append(package.name).push_back(' ');
    out.append(package.description);
    for (char& ch : out) {
        const unsigned char value = static_cast<unsigned char>(ch);
        if (value >= static_cast<unsigned char>('A') &&
            value <= static_cast<unsigned char>('Z')) {
            ch = static_cast<char>(value + ('a' - 'A'));
        }
    }
    return out;
}

}  // namespace aurhub
