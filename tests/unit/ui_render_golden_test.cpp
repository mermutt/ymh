#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/event.hpp"
#include "ymh/session/events.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const SessionId kSession{"golden-session"};

std::string strip_ansi(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\x1b' && index + 1 < input.size() && input[index + 1] == '[') {
            index += 2;
            while (index < input.size() &&
                   (input[index] == ';' || (input[index] >= '0' && input[index] <= '9'))) {
                ++index;
            }
            continue;
        }
        output.push_back(input[index]);
    }
    return output;
}

std::string normalize(const std::string& input) {
    std::vector<std::string> lines;
    std::string current;
    for (const char character : strip_ansi(input)) {
        if (character == '\r') {
            continue;
        }
        if (character == '\n') {
            while (!current.empty() && current.back() == ' ') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    while (!current.empty() && current.back() == ' ') {
        current.pop_back();
    }
    lines.push_back(current);
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            result += '\n';
        }
        result += lines[index];
    }
    return result;
}

Event make_event(EventType type, const nlohmann::json& payload) {
    Event event;
    event.id = EventId{"event"};
    event.session_id = kSession;
    event.timestamp = std::chrono::system_clock::now();
    event.type = type;
    event.payload = payload;
    return event;
}

template <class P>
Event typed_event(EventType type, const P& payload) {
    return make_event(type, nlohmann::json(payload));
}

UiModel build_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel workspace;
    workspace.id = model.activeWorkspaceId;
    workspace.cwd = "/work";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.activeSessionId = kSession;
    model.workspaces.emplace(workspace.id, workspace);
    model.ensureSession(kSession);
    model.session(kSession)->status.model = "test-model";

    UiEventAdapter adapter(model);
    adapter.onEvent(typed_event(EventType::UserMessage, [] {
        payload::UserMessage message;
        message.id = "m1";
        ContentBlock block;
        block.kind = ContentBlockKind::Text;
        block.text = "hello there";
        message.content.push_back(block);
        return message;
    }()));
    payload::AssistantChunk chunk;
    chunk.message = "a1";
    chunk.text = "Hello world";
    adapter.onEvent(typed_event(EventType::AssistantChunk, chunk));
    adapter.onEvent(typed_event(EventType::AssistantMessage, [] {
        payload::AssistantMessage message;
        message.id = "a1";
        ContentBlock block;
        block.kind = ContentBlockKind::Text;
        block.text = "Hello world";
        message.content.push_back(block);
        Usage usage;
        usage.input_tokens = 12;
        usage.output_tokens = 3;
        message.usage = usage;
        return message;
    }()));
    adapter.onEvent(typed_event(EventType::ToolCall, [] {
        payload::ToolCall call;
        call.id = "t1";
        call.name = "read_file";
        call.arguments = {{"path", "hello.txt"}};
        return call;
    }()));
    adapter.onEvent(typed_event(EventType::ToolResult, [] {
        payload::ToolResult result;
        result.id = "t1";
        result.name = "read_file";
        result.outcome = payload::ToolOutcome::Ok;
        result.output = "file-body";
        return result;
    }()));
    adapter.onEvent(typed_event(EventType::TurnEnded, payload::TurnEnded{}));
    return model;
}

const char* kGolden = R"GOLDEN(╭──────────────────────────────────────────────────────────────────────╮
│ymh · /work                                                           │
├──────────────────────────────────────────────────────────────────────┤
│you:                                                                  │
│hello there                                                           │
│assistant                                                             │
│Hello world                                                           │
│tool: read_file                                                       │
│file-body                                                             │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
│                                                                      │
├──────────────────────────────────────────────────────────────────────┤
│[golden-s o]                                                          │
│> _                                                                   │
│idle · test-model                                 0 active · 0 waiting│
╰──────────────────────────────────────────────────────────────────────╯)GOLDEN";

TEST(UiRenderGolden, ConversationSnapshot) {
    const UiModel model = build_model();
    const std::string rendered = normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    std::cerr << "\n---GOLDEN-BEGIN---\n" << rendered << "\n---GOLDEN-END---\n";
    SCOPED_TRACE(rendered);
    EXPECT_EQ(rendered, std::string(kGolden));
}

TEST(UiRenderGolden, RenderIsPure) {
    const UiModel model = build_model();
    const std::string first = render_to_ansi(model, TerminalSize{72, 20}, Theme{false});
    const std::string second = render_to_ansi(model, TerminalSize{72, 20}, Theme{false});
    EXPECT_EQ(first, second);
}

TEST(UiRenderGolden, ContainsConversationAndToolLines) {
    const UiModel model = build_model();
    const std::string rendered = render_to_ansi(model, TerminalSize{72, 20}, Theme{false});
    EXPECT_NE(rendered.find("hello there"), std::string::npos);
    EXPECT_NE(rendered.find("Hello world"), std::string::npos);
    EXPECT_NE(rendered.find("read_file"), std::string::npos);
    EXPECT_NE(rendered.find("0 active"), std::string::npos);
}

TEST(UiRenderGolden, PermissionDialogOverlay) {
    UiModel model = build_model();
    UiEventAdapter adapter(model);
    PermissionRequest request;
    request.session = kSession;
    request.tool = "shell";
    request.arguments = {{"command", "rm -rf build"}};
    adapter.onPermissionRequest(kSession, PermissionRequestId{"p1"}, request);
    const std::string rendered = render_to_ansi(model, TerminalSize{72, 24}, Theme{false});
    EXPECT_NE(rendered.find("Permission required"), std::string::npos);
    EXPECT_NE(rendered.find("shell"), std::string::npos);
}

TEST(UiRenderGolden, LayoutModes) {
    EXPECT_EQ(calculate_layout(60), LayoutMode::Narrow);
    EXPECT_EQ(calculate_layout(90), LayoutMode::Normal);
    EXPECT_EQ(calculate_layout(130), LayoutMode::Normal);
    EXPECT_EQ(calculate_layout(160), LayoutMode::Wide);
}

} // namespace
