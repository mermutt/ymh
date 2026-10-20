#pragma once

// 93-DF1..DF13: the canonical, model-facing denial text and the provider
// Tool-message projection helper.

#include <cstddef>
#include <string>
#include <string_view>

#include "ymh/session/events.hpp"

namespace ymh {

inline constexpr std::string_view kDeniedToolMarker = "DENIED:";
inline constexpr std::string_view kDeniedNotExecuted = "was NOT executed";
inline constexpr std::size_t kMaxDenialToolBytes     = 128;
inline constexpr std::size_t kMaxDenialReasonBytes   = 256;
inline constexpr std::size_t kMaxDenialTextBytes     = 1024;

[[nodiscard]] std::string bounded_ascii(std::string_view text, std::size_t cap);

[[nodiscard]] std::string denial_detail(std::string_view reason);

[[nodiscard]] std::string denial_text(std::string_view tool, std::string_view reason);

[[nodiscard]] bool is_canonical_denial(std::string_view text) noexcept;

[[nodiscard]] std::string tool_message_content(const payload::ToolResult& result);

} // namespace ymh
