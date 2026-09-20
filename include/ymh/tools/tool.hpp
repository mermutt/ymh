#pragma once

// The Tool contract, ToolSchema, ToolArguments, and the tool-layer error
// taxonomy (07 §2-§3, §14.1). `ToolRegistry` (tool_registry.hpp) validates and
// dispatches; a `Tool` never appends durable events, never touches the UI, and
// never decides policy (X6/X8).

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ymh/core/retention.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {

class ToolContext;

using ToolResult = payload::ToolResult;

// dsh's "live concurrency mode" (40-output-retention.md §2.2). Default is the
// SAFE value: a tool that has not declared itself parallel-safe is an exclusive
// barrier.
enum class ToolConcurrencyMode : std::uint8_t { Exclusive, ParallelSafe };

// Stable, provider-visible tool identifier. Grammar:
// [a-z][a-z0-9_]* ( "." [a-z0-9_]+ )*
struct ToolName {
    std::string value;
    auto operator<=>(const ToolName&) const = default;
};

struct ToolVersion {
    std::uint32_t major{1};
    std::uint32_t minor{0};
    auto operator<=>(const ToolVersion&) const = default;
};

struct ToolSchema {
    ToolName       name;
    ToolVersion    version;             // contract version, not binary version
    std::string    description;         // model-facing; bounded (§4.4)
    nlohmann::json input_schema;        // JSON Schema object (pinned subset)
    bool           destructive{false};  // hint for the permission layer (§19)
    ToolConcurrencyMode concurrency{ToolConcurrencyMode::Exclusive};  // 40 §2.2
};

struct ToolArguments {
    nlohmann::json value;    // always a JSON object (08 §4.2, L10)
};

class Tool {
public:
    virtual ~Tool() = default;

    // §14.1 (preserved): the model-facing contract.
    virtual ToolSchema schema() const = 0;

    // §14.1 (preserved): pure execution; no append, no UI, no policy decision.
    virtual Task<ToolResult> execute(const ToolContext&,
                                     const ToolArguments&) = 0;

    virtual ToolName    name() const { return schema().name; }
    virtual ToolVersion version() const { return schema().version; }
};

// ToolName grammar, validated at registration (07 §2.1).
[[nodiscard]] bool is_valid_tool_name(std::string_view name) noexcept;

// Pinned JSON Schema subset check (07 §3.2). Throws
// ToolRegistryError{InvalidSchema} on violation. Exposed so registration and
// tests share one implementation.
void validate_input_schema(const nlohmann::json& schema);

// Deterministic validation of an argument object against a pinned-subset
// schema (07 §3.3). Pure; no I/O.
[[nodiscard]] bool schema_validate(const nlohmann::json& schema,
                                   const nlohmann::json& arguments);

// Apply the Wave-4 retention cap to `result.output` (40-output-retention.md
// §3.4). Replaces the retired clamp: `max_bytes` is the serialized
// `payload::ToolResult` budget and the escaping-aware `TextRetainer` budget is
// derived from it minus envelope/notice headroom (40-D6, 40-I6). Sets
// `omitted_kind`/`omitted_count` and derives `truncated` (40-I7).
void retain_tool_result(ToolResult& result, std::size_t max_bytes);

} // namespace ymh
