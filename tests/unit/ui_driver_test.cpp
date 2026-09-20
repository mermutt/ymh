#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "support/agent_test_env.hpp"
#include "ymh/agent/agent.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;
using namespace ymh::ui;

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

FakeScript two_step_script() {
    FakeScript script;
    FakeResponseStep tool;
    tool.tool_calls.push_back(
        FakeToolCallStep{"read_file", {{"path", "hello.txt"}}, std::nullopt});
    tool.finish = FinishReason::ToolCalls;
    FakeResponseStep text;
    text.text = "all done";
    text.finish = FinishReason::Stop;
    script.steps = {tool, text};
    return script;
}

TEST(UiDriver, RendersScriptedTranscript) {
    AgentEnv env("ui_driver", std::make_unique<FakeLLM>(two_step_script()),
                 AgentConfig{}, allow_all_permission_config(), {}, true);
    env.workspace.write("hello.txt", "file-body");
    auto agent_owner = env.createAgent();
    Agent& agent = *agent_owner;

    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel workspace;
    workspace.id = model.activeWorkspaceId;
    workspace.cwd = env.workspace.path().string();
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(workspace.id, agent.session());

    UiEventAdapter adapter(model);
    Subscription subscription =
        env.bus.subscribe([&adapter](const Event& event) { adapter.onEvent(event); });

    ASSERT_EQ(agent.send(user_message("read it")), InboxResult::Accepted);
    subscription.unsubscribe();

    const std::string rendered =
        render_to_ansi(model, TerminalSize{100, 40}, Theme{false});
    EXPECT_NE(rendered.find("read it"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("all done"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("read_file"), std::string::npos) << rendered;
    EXPECT_EQ(model.aggregate.current.activeCount, 0u);
    EXPECT_EQ(model.aggregate.current.waitingCount, 0u);
}

} // namespace
