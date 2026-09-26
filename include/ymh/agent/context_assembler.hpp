#pragma once

// Context assembly, compaction, and token-estimation seams (06-agent-loop.md
// §5.2-§5.3, §8). The assembler is the only place history is selected; the
// provider receives a complete message list. The estimator is advisory only and
// never overrides provider-reported `Usage`.

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/ids.hpp"
#include "ymh/session/session.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh {

class SystemPrompt;

struct TurnContext {
    TurnId                      turn = 0;
    StepId                      step = 0;
    std::vector<Message>        inbox;
    std::vector<ContextMessage> injected;
    // 52 review (3A): the session's mounted preset scope, when one exists. The
    // registry render and the tool waterfall resolve their scoped rows through
    // it; absent means the root layer only.
    std::optional<std::string>  scope;
};

class ContextAssembler {
public:
    virtual ~ContextAssembler() = default;

    virtual std::vector<Message> assemble(const Session&, const TurnContext&) const = 0;
    virtual std::vector<ToolSchema> tools() const = 0;
    // 52 review (3A): scope-aware tool waterfall. The default forwards to the
    // scope-free overload so existing implementations stay valid.
    virtual std::vector<ToolSchema> tools(const std::optional<std::string>& scope) const {
        (void)scope;
        return tools();
    }
};

class TokenEstimator {
public:
    virtual ~TokenEstimator() = default;

    virtual std::size_t estimate(const std::vector<Message>&) const = 0;
};

class Compactor {
public:
    virtual ~Compactor() = default;

    virtual std::optional<payload::ContextCompaction> run(const Session&,
                                                          const std::vector<Message>&,
                                                          CancellationToken) = 0;
};

class DefaultTokenEstimator final : public TokenEstimator {
public:
    std::size_t estimate(const std::vector<Message>& messages) const override;
};

class SessionContextAssembler final : public ContextAssembler {
public:
    SessionContextAssembler(ToolRegistry& tools, std::string systemPrompt);

    // 25-D3: optional. When set and it returns non-empty for the assembled
    // session, the text is appended to the system message as a paragraph.
    void set_plan_policy_provider(
        std::function<std::string(const Session&)> provider);

    // 36 §2.7: when set, the registry render replaces `systemPrompt_` and the
    // registry's `.tools` replaces `tools_.schemas()`. When null, the fixed
    // string and the raw registry order are used (the pre-Wave-3 behavior).
    void set_system_prompt(const SystemPrompt* prompt);

    std::vector<Message> assemble(const Session&, const TurnContext&) const override;
    std::vector<ToolSchema> tools() const override;
    std::vector<ToolSchema> tools(const std::optional<std::string>& scope) const override;

private:
    ToolRegistry&                              tools_;
    std::string                                systemPrompt_;
    std::function<std::string(const Session&)> plan_policy_;
    const SystemPrompt*                        prompt_ = nullptr;
};

class NullCompactor final : public Compactor {
public:
    std::optional<payload::ContextCompaction> run(const Session&,
                                                  const std::vector<Message>&,
                                                  CancellationToken) override {
        return std::nullopt;
    }
};

} // namespace ymh
