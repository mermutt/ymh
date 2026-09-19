#pragma once

// TEST-ONLY request-reconstruction harness, pinned by
// 34-assembler-replay-errata.md §9 (34-D9, GAP 3). It is not installed and is
// included by no production translation unit. It turns the `28 §4.4`
// reconstruction guarantee and the `26 §5.2` replay negative tests into a
// callable assertion surface.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh::test {

enum class ReplayMismatchKind : std::uint8_t {
    MissingHeader,           // no LlmRequestHeader precedes the attempt (26-F15)
    ToolNameSetMismatch,     // registry names != header.tool_names
    ToolSchemaMismatch,      // re-derived schema digest != header.tool_schema_digests[i]
    PromptDigestMismatch,    // sha256(rendered) != header.system_prompt_digest
    TemplateDigestMismatch,  // rebuilt.template_digest() != header.template_digest
    PrefixMismatch,          // candidate prefix != the attempt's dispatch prefix (§9.2)
    MessageDivergence,       // two rebuilds of canonical_json() differ
};

class ReplayMismatch : public std::runtime_error {
public:
    ReplayMismatch(ReplayMismatchKind kind, std::string detail)
        : std::runtime_error(detail), kind_(kind), detail_(std::move(detail)) {}

    [[nodiscard]] ReplayMismatchKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

private:
    ReplayMismatchKind kind_;
    std::string        detail_;
};

struct ReplayEnv {
    const ToolRegistry*        registry = nullptr;
    std::optional<std::string> rendered_system_prompt;
};

enum class ReplayStatus : std::uint8_t {
    Verified,
    PromptUnavailable,
};

struct ReplayReport {
    ReplayStatus status = ReplayStatus::Verified;
    FrozenRequest rebuilt;
    std::string   canonical_json;
};

[[nodiscard]] FrozenRequest rebuild_template(const payload::LlmRequestHeader& header,
                                             const ReplayEnv& env);

[[nodiscard]] std::vector<Message> reconstruct_messages(const SessionHeader& session_header,
                                                        const EventRange& log,
                                                        const EventRange& prefix,
                                                        Sequence settlement_seq,
                                                        const payload::LlmRequestHeader& header,
                                                        const ReplayEnv& env);

[[nodiscard]] ReplayReport assert_reconstructable(const SessionHeader& session_header,
                                                  const EventRange& log,
                                                  const EventRange& prefix,
                                                  Sequence settlement_seq,
                                                  const payload::LlmRequestHeader& header,
                                                  const ReplayEnv& env);

} // namespace ymh::test
