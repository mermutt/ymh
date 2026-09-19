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

struct TurnContext {
    TurnId                      turn = 0;
    StepId                      step = 0;
    std::vector<Message>        inbox;
    std::vector<ContextMessage> injected;
};

class ContextAssembler {
public:
    virtual ~ContextAssembler() = default;

    virtual std::vector<Message> assemble(const Session&, const TurnContext&) const = 0;
    virtual std::vector<ToolSchema> tools() const = 0;
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

    std::vector<Message> assemble(const Session&, const TurnContext&) const override;
    std::vector<ToolSchema> tools() const override;

private:
    ToolRegistry&                              tools_;
    std::string                                systemPrompt_;
    std::function<std::string(const Session&)> plan_policy_;
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
