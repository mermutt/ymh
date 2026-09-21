#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "support/agent_test_env.hpp"
#include "support/replay_harness.hpp"
#include "support/test_env.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/tools/builtin_tools.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

EventRecord record(Sequence seq, const SessionId& session, EventType type, nlohmann::json payload) {
    EventRecord result;
    result.seq              = seq;
    result.event.id         = make_event_id();
    result.event.session_id = session;
    result.event.timestamp  = std::chrono::system_clock::now();
    result.event.type       = type;
    result.event.payload    = std::move(payload);
    return result;
}

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

ContentBlock tool_use_block(std::string id, std::string name, nlohmann::json arguments) {
    ContentBlock block;
    block.kind         = ContentBlockKind::ToolUse;
    block.tool_call_id = std::move(id);
    block.tool_name    = std::move(name);
    block.arguments    = std::move(arguments);
    return block;
}

struct LeakedRecordedAttempt {
    ToolRegistry              registry;
    RegistrationKeeper        keeper;
    payload::LlmRequestHeader header;
    EventRange                log;
    EventRange                prefix;
    Sequence                  settlement_seq = 0;
    SessionId                 session;

    explicit LeakedRecordedAttempt(ToolEnv& env) : keeper(registry) {
        for (std::unique_ptr<Tool>& tool : make_builtin_tools()) {
            keeper.add(std::move(tool));
        }
        registry.freeze();

        const std::string prompt = "you are a replay test";
        session                  = env.header.id;

        LLMRequest request;
        request.model      = "test-model";
        request.session_id = session;
        Message system;
        system.role = Role::System;
        system.content.push_back(text_block(prompt));
        request.messages.push_back(std::move(system));
        request.tools = registry.schemas();

        LlmCallConfig config;
        config.provider = "fake";
        config.model    = "test-model";
        FrozenRequest frozen = FrozenRequest::freeze(std::move(request), config);

        header.turn       = 1;
        header.step       = 1;
        header.session_id = session;
        header.config     = config;
        header.system_prompt        = prompt;
        header.system_prompt_digest = sha256_hex(prompt);
        header.template_digest      = frozen.template_digest();
        for (const ToolSchema& schema : registry.schemas()) {
            header.tool_names.push_back(schema.name.value);
            header.tool_schema_digests.push_back(tool_schema_digest(schema));
        }

        log.push_back(record(10, session, EventType::LlmRequestHeader, header));
        log.push_back(record(11, session, EventType::UserMessage,
                             payload::UserMessage{"u1", {text_block("hi")}}));

        payload::AssistantMessage message;
        message.id = "a1";
        message.content = {
            text_block("<tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"a\"}}"
                       "</tool_call>"),
            tool_use_block("leaked_0", "read_file", nlohmann::json{{"path", "a"}})};
        log.push_back(record(12, session, EventType::AssistantMessage, message));
        settlement_seq = 12;
        prefix         = EventRange{log[0], log[1]};
    }

    ReplayEnv env() const { return ReplayEnv{&registry, std::nullopt}; }
};

} // namespace

TEST(MuseReplay, LeakedTurnDoesNotReexecute) {
    ToolEnv session_env("muse_replay_leaked");
    LeakedRecordedAttempt attempt(session_env);

    const ReplayReport report = assert_reconstructable(
        session_env.header, attempt.log, attempt.prefix, attempt.settlement_seq, attempt.header,
        attempt.env());
    EXPECT_EQ(report.status, ReplayStatus::Verified);
    EXPECT_FALSE(report.canonical_json.empty());

    const std::vector<Message> messages = reconstruct_messages(
        session_env.header, attempt.log, attempt.prefix, attempt.settlement_seq, attempt.header,
        attempt.env());
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].role, Role::System);
    EXPECT_EQ(messages[1].role, Role::User);

    bool saw_durable_tool_use = false;
    for (const EventRecord& entry : attempt.log) {
        if (entry.event.type != EventType::AssistantMessage) {
            continue;
        }
        for (const ContentBlock& block :
             entry.event.payload.get<payload::AssistantMessage>().content) {
            if (block.kind == ContentBlockKind::ToolUse) {
                saw_durable_tool_use = true;
                EXPECT_EQ(block.tool_call_id, "leaked_0");
                EXPECT_EQ(block.tool_name, "read_file");
            }
        }
    }
    EXPECT_TRUE(saw_durable_tool_use);
}
