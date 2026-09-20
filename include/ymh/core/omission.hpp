#pragma once

// Omission metadata for retained output (40-output-retention.md §3.2, §3.3).
// Spec 40 owns the exact representation. Kept dependency-light so the durable
// `payload::ToolResult` codec (01) can include it without pulling in the whole
// retention library.
//
// `count` is meaningful only for `Exact`; dsh is a discriminated union and ymh
// keeps the discriminant explicit (26-dsh-alignment-part2.md T-L3).

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ymh {

enum class OmittedKind : std::uint8_t {
    None,
    Exact,
    Unknown,
};

struct Omitted {
    OmittedKind kind  = OmittedKind::None;
    std::size_t count = 0;
};

[[nodiscard]] inline std::string_view omitted_kind_name(OmittedKind kind) noexcept {
    switch (kind) {
        case OmittedKind::None:
            return "none";
        case OmittedKind::Exact:
            return "exact";
        case OmittedKind::Unknown:
            return "unknown";
    }
    return {};
}

[[nodiscard]] inline OmittedKind parse_omitted_kind(std::string_view value) {
    if (value == "none") {
        return OmittedKind::None;
    }
    if (value == "exact") {
        return OmittedKind::Exact;
    }
    if (value == "unknown") {
        return OmittedKind::Unknown;
    }
    throw std::runtime_error("unknown omitted kind: " + std::string{value});
}

} // namespace ymh
