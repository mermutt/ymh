#include "ymh/agent/context_assembler.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ymh {
namespace {

std::size_t text_bytes(const std::vector<Message>& messages) {
    std::size_t bytes = 0;
    for (const Message& message : messages) {
        for (const ContentBlock& block : message.content) {
            bytes += block.text.size();
            bytes += block.tool_name.size();
            if (!block.arguments.is_null()) {
                bytes += block.arguments.dump().size();
            }
        }
    }
    return bytes;
}

} // namespace

std::size_t DefaultTokenEstimator::estimate(const std::vector<Message>& messages) const {
    constexpr std::size_t kBytesPerToken = 4;
    constexpr std::size_t kPerMessageOverhead = 4;
    return text_bytes(messages) / kBytesPerToken + messages.size() * kPerMessageOverhead;
}

SessionContextAssembler::SessionContextAssembler(ToolRegistry& tools, std::string systemPrompt)
    : tools_(tools), systemPrompt_(std::move(systemPrompt)) {}

void SessionContextAssembler::set_plan_policy_provider(
    std::function<std::string(const Session&)> provider) {
    plan_policy_ = std::move(provider);
}

std::vector<Message> SessionContextAssembler::assemble(const Session& session,
                                                       const TurnContext& context) const {
    (void)context;
    std::vector<Message> messages = session.deriveMessages();
    std::string          system_text = systemPrompt_;
    if (plan_policy_) {
        const std::string section = plan_policy_(session);
        if (!section.empty()) {
            if (!system_text.empty()) {
                system_text += "\n\n";
            }
            system_text += section;
        }
    }
    if (!system_text.empty()) {
        Message system;
        system.role = Role::System;
        ContentBlock block;
        block.kind = ContentBlockKind::Text;
        block.text = std::move(system_text);
        system.content.push_back(std::move(block));
        messages.insert(messages.begin(), std::move(system));
    }
    return messages;
}

std::vector<ToolSchema> SessionContextAssembler::tools() const {
    return tools_.schemas();
}

} // namespace ymh
