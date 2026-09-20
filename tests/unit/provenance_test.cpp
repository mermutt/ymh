#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/message.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/ids.hpp"
#include "ymh/session/session.hpp"

namespace {

using namespace ymh;

ContentBlock text_block(std::string text) {
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    return block;
}

SessionHeader make_header() {
    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = "/workspace";
    header.createdAt     = 1000;
    header.updatedAt     = 1000;
    header.model         = "test-model";
    header.serverProfile = "interactive";
    header.kind          = SessionKind::Root;
    return header;
}

template <class P>
EventRecord record(Sequence seq, const SessionId& session, const P& payload) {
    TypedEvent<P> typed;
    typed.id         = make_event_id();
    typed.session_id = session;
    typed.timestamp  = std::chrono::system_clock::time_point{std::chrono::milliseconds{seq * 10}};
    typed.payload    = payload;
    return EventRecord{seq, encode(typed)};
}

MessageSource model_source(std::string provider, std::string model) {
    MessageSource source;
    source.kind     = MessageSource::Kind::Model;
    source.provider = std::move(provider);
    source.model    = std::move(model);
    return source;
}

ContextFormed instructions_form() {
    return ContextFormed{ContextForm::Instructions};
}

ContextFormed snapshot_form() {
    return ContextFormed{ContextForm::Snapshot,
                         std::vector<ContextSnapshotSection>{{"runtime-context", "body"}}};
}

MessageSource plugin_source(std::string plugin, ContextFormed context) {
    MessageSource source;
    source.kind    = MessageSource::Kind::Plugin;
    source.plugin  = std::move(plugin);
    source.context = std::move(context);
    return source;
}

TEST(ProvenanceTypes, ContextFormedCouplingFailsAtConstruction) {
    EXPECT_THROW(ContextFormed{ContextForm::Snapshot}, std::invalid_argument);
    EXPECT_THROW(ContextFormed{ContextForm::Notice}, std::invalid_argument);
    EXPECT_NO_THROW(ContextFormed{ContextForm::Instructions});
    EXPECT_NO_THROW(ContextFormed{ContextForm::None});
    EXPECT_NO_THROW((ContextFormed{ContextForm::Snapshot, {{"section", "text"}}}));
    EXPECT_NO_THROW((ContextFormed{ContextForm::Notice, {}, "summary"}));
}

TEST(ProvenanceTypes, MessageSourceGating) {
    MessageSource user;
    user.plugin = "nope";
    EXPECT_THROW(user.validate(), std::invalid_argument);

    MessageSource model;
    model.kind     = MessageSource::Kind::Model;
    model.provider = "deepseek";
    model.model    = "deepseek-flash";
    EXPECT_NO_THROW(model.validate());

    MessageSource model_plugin;
    model_plugin.kind   = MessageSource::Kind::Model;
    model_plugin.plugin = "nope";
    EXPECT_THROW(model_plugin.validate(), std::invalid_argument);

    MessageSource tool;
    tool.kind = MessageSource::Kind::Tool;
    tool.call = "call-1";
    EXPECT_NO_THROW(tool.validate());

    MessageSource tool_context;
    tool_context.kind    = MessageSource::Kind::Tool;
    tool_context.context = instructions_form();
    EXPECT_THROW(tool_context.validate(), std::invalid_argument);

    MessageSource plugin;
    plugin.kind   = MessageSource::Kind::Plugin;
    plugin.plugin = "runtime-context";
    EXPECT_NO_THROW(plugin.validate());

    MessageSource plugin_call;
    plugin_call.kind = MessageSource::Kind::Plugin;
    plugin_call.call = "call-1";
    EXPECT_THROW(plugin_call.validate(), std::invalid_argument);
}

TEST(ProvenanceCodec, ContextFormedRoundTrips) {
    for (const ContextFormed& original :
         {ContextFormed{}, instructions_form(), snapshot_form(),
          ContextFormed{ContextForm::Notice, {}, "removed"}}) {
        const nlohmann::json json = original;
        const ContextFormed back = json.get<ContextFormed>();
        EXPECT_TRUE(back == original);
    }
}

TEST(ProvenanceCodec, MessageSourceRoundTrips) {
    const std::vector<MessageSource> sources{
        MessageSource{},
        plugin_source("runtime-context", snapshot_form()),
        model_source("deepseek", "deepseek-flash"),
        MessageSource{MessageSource::Kind::Tool, {}, {}, "call-9", {}, {}, {}},
    };
    for (const MessageSource& original : sources) {
        const nlohmann::json json = original;
        const MessageSource back = json.get<MessageSource>();
        EXPECT_TRUE(back == original);
    }
}

TEST(ProvenanceCodec, UnknownEnumsFailLoud) {
    const nlohmann::json bad_form = {{"form", "bogus"}};
    EXPECT_THROW(static_cast<void>(bad_form.get<ContextFormed>()), nlohmann::json::exception);

    const nlohmann::json bad_kind = {{"kind", "bogus"}};
    EXPECT_THROW(static_cast<void>(bad_kind.get<MessageSource>()), nlohmann::json::exception);
}

TEST(ProvenanceCodec, NonGatedKeyFailsLoud) {
    const nlohmann::json model_with_plugin = {{"kind", "model"}, {"plugin", "nope"}};
    EXPECT_THROW(static_cast<void>(model_with_plugin.get<MessageSource>()),
                 std::invalid_argument);

    const nlohmann::json tool_with_context = {
        {"kind", "tool"}, {"context", {{"form", "instructions"}}}};
    EXPECT_THROW(static_cast<void>(tool_with_context.get<MessageSource>()),
                 std::invalid_argument);
}

TEST(ProvenanceCodec, DefaultsOmitSourceAndContext) {
    const nlohmann::json user = payload::UserMessage{};
    EXPECT_FALSE(user.contains("source"));

    const nlohmann::json assistant = payload::AssistantMessage{};
    EXPECT_FALSE(assistant.contains("source"));

    const nlohmann::json tool = payload::ToolResult{};
    EXPECT_FALSE(tool.contains("source"));
    EXPECT_FALSE(tool.contains("context"));

    const nlohmann::json injected = payload::ContextInjected{};
    EXPECT_FALSE(injected.contains("source"));
    EXPECT_FALSE(injected.contains("context"));
}

TEST(ProvenanceCodec, DataBearingPayloadsEmitSourceAndContext) {
    payload::AssistantMessage assistant;
    assistant.source = model_source("deepseek", "deepseek-flash");
    const nlohmann::json assistant_json = assistant;
    ASSERT_TRUE(assistant_json.contains("source"));
    EXPECT_EQ(assistant_json["source"]["kind"], "model");
    EXPECT_EQ(assistant_json["source"]["provider"], "deepseek");
    EXPECT_EQ(assistant_json["source"]["model"], "deepseek-flash");

    payload::ToolResult tool;
    tool.id          = "t1";
    tool.source.call = tool.id;
    const nlohmann::json tool_json = tool;
    ASSERT_TRUE(tool_json.contains("source"));
    EXPECT_EQ(tool_json["source"]["kind"], "tool");
    EXPECT_EQ(tool_json["source"]["call"], "t1");

    payload::ContextInjected injected;
    injected.source  = plugin_source("runtime-context", snapshot_form());
    injected.context = injected.source.context;
    const nlohmann::json injected_json = injected;
    ASSERT_TRUE(injected_json.contains("source"));
    ASSERT_TRUE(injected_json.contains("context"));
    EXPECT_EQ(injected_json["source"]["plugin"], "runtime-context");
    EXPECT_EQ(injected_json["source"]["context"]["form"], "snapshot");
    EXPECT_EQ(injected_json["context"]["form"], "snapshot");
}

TEST(ProvenanceCodec, StructuralDefaults) {
    EXPECT_EQ(payload::UserMessage{}.source.kind, MessageSource::Kind::User);
    EXPECT_EQ(payload::AssistantMessage{}.source.kind, MessageSource::Kind::Model);
    EXPECT_EQ(payload::ToolResult{}.source.kind, MessageSource::Kind::Tool);
    EXPECT_FALSE(payload::ToolResult{}.source.call.has_value());
    EXPECT_EQ(payload::ContextInjected{}.source.kind, MessageSource::Kind::Plugin);
    EXPECT_EQ(payload::ContextInjected{}.role, Role::User);
}

TEST(ProvenanceCodec, NestedSchemaSpellings) {
    const nlohmann::json snapshot = snapshot_form();
    EXPECT_EQ(snapshot["form"], "snapshot");
    ASSERT_TRUE(snapshot.contains("sections"));
    EXPECT_EQ(snapshot["sections"][0]["name"], "runtime-context");

    const nlohmann::json notice = ContextFormed{ContextForm::Notice, {}, "gone"};
    EXPECT_EQ(notice["form"], "notice");
    EXPECT_EQ(notice["summary"], "gone");
    EXPECT_FALSE(notice.contains("sections"));

    const nlohmann::json catalog = ContextFormed{ContextForm::Catalog};
    EXPECT_EQ(catalog["form"], "catalog");
    EXPECT_FALSE(catalog.contains("sections"));
    EXPECT_FALSE(catalog.contains("summary"));
}

TEST(ProvenanceCodec, PayloadRoundTripsWithProvenance) {
    payload::UserMessage user;
    user.id      = "u1";
    user.content = {text_block("hi")};
    const payload::UserMessage user_back = nlohmann::json(user).get<payload::UserMessage>();
    EXPECT_TRUE(user_back.source == user.source);

    payload::AssistantMessage assistant;
    assistant.id     = "a1";
    assistant.content = {text_block("hello")};
    assistant.source = model_source("deepseek", "deepseek-flash");
    const payload::AssistantMessage assistant_back =
        nlohmann::json(assistant).get<payload::AssistantMessage>();
    EXPECT_TRUE(assistant_back.source == assistant.source);

    payload::ToolResult tool;
    tool.id          = "t1";
    tool.name        = "read_file";
    tool.output      = "body";
    tool.source.call = tool.id;
    const payload::ToolResult tool_back = nlohmann::json(tool).get<payload::ToolResult>();
    EXPECT_TRUE(tool_back.source == tool.source);
    EXPECT_TRUE(tool_back.context == tool.context);

    payload::ContextInjected injected;
    injected.id      = "c1";
    injected.text    = "instructions";
    injected.source  = plugin_source("agent-instructions", instructions_form());
    injected.context = injected.source.context;
    const payload::ContextInjected injected_back =
        nlohmann::json(injected).get<payload::ContextInjected>();
    EXPECT_TRUE(injected_back.source == injected.source);
    EXPECT_TRUE(injected_back.context == injected.context);
}

TEST(ProvenanceCodec, MessageRoundTripsAndDecodesOldShape) {
    Message message;
    message.role         = Role::Assistant;
    message.content      = {text_block("answer")};
    message.tool_call_id = "t1";
    message.source       = model_source("deepseek", "deepseek-flash");
    message.context      = snapshot_form();
    const Message back = nlohmann::json(message).get<Message>();
    EXPECT_TRUE(back == message);

    const nlohmann::json old = {
        {"role", "tool"}, {"content", nlohmann::json::array()}, {"tool_call_id", "t9"}};
    const Message decoded = old.get<Message>();
    EXPECT_EQ(decoded.role, Role::Tool);
    EXPECT_EQ(decoded.tool_call_id, "t9");
    EXPECT_FALSE(decoded.source.has_value());
    EXPECT_FALSE(decoded.context.has_value());
}

TEST(ProvenanceCodec, MessageUnknownSourceEnumFailsLoud) {
    const nlohmann::json bad = {
        {"role", "assistant"},
        {"content", nlohmann::json::array()},
        {"tool_call_id", ""},
        {"source", {{"kind", "bogus"}}}};
    EXPECT_THROW(static_cast<void>(bad.get<Message>()), nlohmann::json::exception);
}

TEST(ProvenanceProjection, CarriesSourceAndContext) {
    const SessionHeader header = make_header();
    const SessionId     session = header.id;

    payload::ContextInjected injected;
    injected.id      = "c1";
    injected.role    = Role::User;
    injected.text    = "instructions";
    injected.source  = plugin_source("agent-instructions", instructions_form());
    injected.context = injected.source.context;

    payload::AssistantMessage assistant;
    assistant.id     = "a1";
    assistant.content = {text_block("answer")};
    assistant.source = model_source("deepseek", "deepseek-flash");

    payload::ToolResult tool;
    tool.id          = "t1";
    tool.name        = "read_file";
    tool.output      = "body";
    tool.source.call = tool.id;

    const EventRange events{
        record(1, session, payload::UserMessage{MessageId{"u1"}, {text_block("hi")}}),
        record(2, session, injected),
        record(3, session, assistant),
        record(4, session, tool),
    };

    const std::vector<Message> messages = deriveMessages(header, events);
    ASSERT_EQ(messages.size(), 4u);

    EXPECT_EQ(messages[0].role, Role::User);
    ASSERT_TRUE(messages[0].source.has_value());
    EXPECT_EQ(messages[0].source->kind, MessageSource::Kind::User);

    EXPECT_EQ(messages[1].role, Role::User);
    ASSERT_TRUE(messages[1].source.has_value());
    EXPECT_EQ(messages[1].source->plugin, "agent-instructions");
    ASSERT_TRUE(messages[1].context.has_value());
    EXPECT_EQ(messages[1].context->form, ContextForm::Instructions);

    EXPECT_EQ(messages[2].role, Role::Assistant);
    ASSERT_TRUE(messages[2].source.has_value());
    EXPECT_EQ(messages[2].source->provider, "deepseek");
    EXPECT_EQ(messages[2].source->model, "deepseek-flash");

    EXPECT_EQ(messages[3].role, Role::Tool);
    ASSERT_TRUE(messages[3].source.has_value());
    EXPECT_EQ(messages[3].source->kind, MessageSource::Kind::Tool);
    ASSERT_TRUE(messages[3].source->call.has_value());
    EXPECT_EQ(*messages[3].source->call, "t1");
    EXPECT_EQ(messages[3].tool_call_id, "t1");
}

TEST(ProvenanceProjection, SyntheticToolMessageCarriesToolSource) {
    const SessionHeader header = make_header();
    const SessionId     session = header.id;

    payload::AssistantMessage assistant;
    assistant.id = "a1";
    ContentBlock use;
    use.kind         = ContentBlockKind::ToolUse;
    use.tool_call_id = "t7";
    use.tool_name    = "read_file";
    assistant.content = {use};

    const EventRange events{
        record(1, session, payload::TurnStarted{1, payload::TurnOrigin::User}),
        record(2, session, assistant),
        record(3, session, payload::TurnCancelled{1, "stop"}),
    };

    const std::vector<Message> messages = deriveMessages(header, events);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[1].role, Role::Tool);
    EXPECT_EQ(messages[1].tool_call_id, "t7");
    ASSERT_TRUE(messages[1].source.has_value());
    EXPECT_EQ(messages[1].source->kind, MessageSource::Kind::Tool);
    ASSERT_TRUE(messages[1].source->call.has_value());
    EXPECT_EQ(*messages[1].source->call, "t7");
}

TEST(ProvenanceProjection, ContextRoleDefaultAndExplicit) {
    const SessionHeader header = make_header();
    const SessionId     session = header.id;

    payload::ContextInjected defaulted;
    defaulted.id   = "c1";
    defaulted.text = "default";

    payload::ContextInjected explicit_system;
    explicit_system.id   = "c2";
    explicit_system.role = Role::System;
    explicit_system.text = "system";

    const EventRange events{
        record(1, session, defaulted),
        record(2, session, explicit_system),
    };

    const std::vector<Message> messages = deriveMessages(header, events);
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages[0].role, Role::User);
    EXPECT_EQ(messages[1].role, Role::System);
}

TEST(ProvenanceProjection, SystemMessageIsNotProjected) {
    const SessionHeader header = make_header();
    const SessionId     session = header.id;

    payload::ContextInjected injected;
    injected.id   = "c1";
    injected.text = "instructions";

    const EventRange events{record(1, session, injected)};
    const std::vector<Message> messages = deriveMessages(header, events);
    for (const Message& message : messages) {
        EXPECT_NE(message.role, Role::System);
    }
}

} // namespace
