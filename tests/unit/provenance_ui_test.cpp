#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/provenance.hpp"
#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const SessionId kSession{"prov-ui-session"};

Event make_event(EventType type, const nlohmann::json& payload) {
    Event event;
    event.id         = EventId{"prov-ui-event"};
    event.session_id = kSession;
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = type;
    event.payload    = payload;
    return event;
}

template <class P>
Event typed_event(EventType type, const P& payload) {
    return make_event(type, nlohmann::json(payload));
}

UiModel make_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"prov-workspace"};
    WorkspaceModel workspace;
    workspace.id            = model.activeWorkspaceId;
    workspace.cwd           = "/tmp/prov";
    workspace.daemonStatus  = DaemonStatus::Attached;
    workspace.live          = true;
    workspace.activeSessionId = kSession;
    model.workspaces.emplace(workspace.id, workspace);
    model.ensureSession(kSession);
    return model;
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

MessageSource model_source() {
    MessageSource source;
    source.kind     = MessageSource::Kind::Model;
    source.provider = "deepseek";
    source.model    = "deepseek-flash";
    return source;
}

MessageSource tool_source(std::string call) {
    MessageSource source;
    source.kind = MessageSource::Kind::Tool;
    source.call = std::move(call);
    return source;
}

TEST(ProvenanceUi, AdapterContextInjectedCopiesProvenance) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::ContextInjected payload;
    payload.id      = "c1";
    payload.role    = Role::User;
    payload.text    = "instructions body";
    payload.context = instructions_form();
    payload.source  = plugin_source("agent-instructions", payload.context);

    const std::vector<UiEvent> events =
        adapter.adapt(typed_event(EventType::ContextInjected, payload));
    ASSERT_EQ(events.size(), 1u);
    const auto* context = std::get_if<ContextInjected>(&events[0].value);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(context->session, kSession);
    EXPECT_EQ(context->id, "c1");
    EXPECT_EQ(context->role, Role::User);
    EXPECT_EQ(context->text, "instructions body");
    EXPECT_TRUE(context->source == payload.source);
    EXPECT_TRUE(context->context == payload.context);
}

TEST(ProvenanceUi, AdapterCopiesSourceOnUserAndAssistant) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::UserMessage user;
    user.id     = "u1";
    user.source = MessageSource{};
    const std::vector<UiEvent> user_events =
        adapter.adapt(typed_event(EventType::UserMessage, user));
    const auto* user_view = std::get_if<UserMessage>(&user_events[0].value);
    ASSERT_NE(user_view, nullptr);
    EXPECT_TRUE(user_view->source == user.source);

    payload::AssistantMessage assistant;
    assistant.id     = "a1";
    assistant.source = model_source();
    const std::vector<UiEvent> assistant_events =
        adapter.adapt(typed_event(EventType::AssistantMessage, assistant));
    const auto* assistant_view =
        std::get_if<AssistantMessageFinished>(&assistant_events[0].value);
    ASSERT_NE(assistant_view, nullptr);
    EXPECT_TRUE(assistant_view->source == assistant.source);
}

TEST(ProvenanceUi, AdapterCopiesSourceAndContextOnToolResult) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::ToolResult result;
    result.id      = "t1";
    result.name    = "read_file";
    result.output  = "body";
    result.source  = tool_source("t1");
    result.context = ContextFormed{ContextForm::Notice, {}, "truncated"};

    const std::vector<UiEvent> events =
        adapter.adapt(typed_event(EventType::ToolResult, result));
    ASSERT_FALSE(events.empty());
    const auto* tool = std::get_if<ToolFinished>(&events[0].value);
    ASSERT_NE(tool, nullptr);
    EXPECT_TRUE(tool->source == result.source);
    EXPECT_TRUE(tool->context == result.context);
}

TEST(ProvenanceUi, ModelAppliesContextRow) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::ContextInjected payload;
    payload.id      = "c1";
    payload.role    = Role::User;
    payload.text    = "catalog body";
    payload.context = ContextFormed{ContextForm::Catalog};
    payload.source  = plugin_source("skill-catalog", payload.context);

    adapter.onEvent(typed_event(EventType::ContextInjected, payload));

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->conversation.entries.size(), 1u);
    const ConversationEntry& entry = state->conversation.entries[0];
    EXPECT_EQ(entry.role, ConversationRole::Context);
    EXPECT_EQ(entry.text, "catalog body");
    ASSERT_TRUE(entry.source.has_value());
    EXPECT_EQ(entry.source->plugin, "skill-catalog");
    ASSERT_TRUE(entry.context.has_value());
    EXPECT_EQ(entry.context->form, ContextForm::Catalog);
    EXPECT_TRUE(model.dirty.peek(kSession) != UiDirtyFlag::None);
}

TEST(ProvenanceUi, ContextRowKeepsTwoAxes) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::ContextInjected instructions;
    instructions.id      = "c1";
    instructions.text    = "one";
    instructions.context = instructions_form();
    instructions.source  = plugin_source("agent-instructions", instructions.context);

    payload::ContextInjected snapshot;
    snapshot.id      = "c2";
    snapshot.text    = "two";
    snapshot.context = snapshot_form();
    snapshot.source  = plugin_source("runtime-context", snapshot.context);

    adapter.onEvent(typed_event(EventType::ContextInjected, instructions));
    adapter.onEvent(typed_event(EventType::ContextInjected, snapshot));

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->conversation.entries.size(), 2u);
    EXPECT_EQ(state->conversation.entries[0].role, ConversationRole::Context);
    EXPECT_EQ(state->conversation.entries[1].role, ConversationRole::Context);
    EXPECT_EQ(state->conversation.entries[0].context->form, ContextForm::Instructions);
    EXPECT_EQ(state->conversation.entries[1].context->form, ContextForm::Snapshot);
}

TEST(ProvenanceUi, ToolNoticeDataPath) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::ToolCall call;
    call.id        = "t1";
    call.name      = "read_file";
    call.arguments = {{"path", "hello.txt"}};
    adapter.onEvent(typed_event(EventType::ToolCall, call));

    payload::ToolResult result;
    result.id      = "t1";
    result.name    = "read_file";
    result.output  = "body";
    result.source  = tool_source("t1");
    result.context = ContextFormed{ContextForm::Notice, {}, "retention notice"};
    adapter.onEvent(typed_event(EventType::ToolResult, result));

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    const std::size_t index = state->tools.find("t1");
    ASSERT_NE(index, kNoEntry);
    ASSERT_TRUE(state->tools.calls[index].notice.has_value());
    EXPECT_EQ(state->tools.calls[index].notice->summary, "retention notice");

    bool saw_tool = false;
    for (const ConversationEntry& entry : state->conversation.entries) {
        if (entry.role != ConversationRole::Tool) {
            continue;
        }
        saw_tool = true;
        ASSERT_TRUE(entry.source.has_value());
        EXPECT_EQ(entry.source->kind, MessageSource::Kind::Tool);
        ASSERT_TRUE(entry.context.has_value());
        EXPECT_EQ(entry.context->form, ContextForm::Notice);
    }
    EXPECT_TRUE(saw_tool);
}

TEST(ProvenanceUi, ToolWithoutNoticeSetsNoNotice) {
    UiModel model = make_model();
    UiEventAdapter adapter(model);

    payload::ToolCall call;
    call.id   = "t2";
    call.name = "read_file";
    adapter.onEvent(typed_event(EventType::ToolCall, call));

    payload::ToolResult result;
    result.id     = "t2";
    result.name   = "read_file";
    result.output = "body";
    result.source = tool_source("t2");
    adapter.onEvent(typed_event(EventType::ToolResult, result));

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);
    const std::size_t index = state->tools.find("t2");
    ASSERT_NE(index, kNoEntry);
    EXPECT_FALSE(state->tools.calls[index].notice.has_value());
    for (const ConversationEntry& entry : state->conversation.entries) {
        if (entry.role != ConversationRole::Tool) {
            continue;
        }
        EXPECT_FALSE(entry.context.has_value());
        ASSERT_TRUE(entry.source.has_value());
    }
}

} // namespace
