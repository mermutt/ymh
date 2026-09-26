#pragma once

// Secret redaction for provider-derived strings (08-llm-provider.md §8, L12).
// Applied to every provider message and diagnostic before it is surfaced or
// logged. Prompts and API keys are never logged by default.

#include <string>
#include <string_view>

namespace ymh {

// Redacts bearer tokens, `Authorization`/`api-key` header values, and common
// API-key shapes. Returns the input unchanged when nothing matches.
[[nodiscard]] std::string redact_secrets(std::string_view text);

// Redacts a single header value when the header name is secret-bearing.
[[nodiscard]] std::string redact_header_value(std::string_view name,
                                              std::string_view value);

} // namespace ymh
