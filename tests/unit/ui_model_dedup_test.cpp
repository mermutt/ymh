#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <utility>

#include "ymh/agent/provenance.hpp"
#include "ymh/ui/ui_event.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const SessionId kSession{"session-1"};
const WorkspaceId kWorkspace{"workspace"};

UiModel make_model() {
    UiModel model;
    model.activeWorkspaceId = kWorkspace;
    WorkspaceModel workspace;
    workspace.id           = kWorkspace;
    workspace.cwd          = "/tmp/workspace";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live         = true;
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(kWorkspace, kSession);
    model.dirty.clear();
    return model;
}

TEST(UiModelDedup, ReplayedUserAndContextIdsAppendOnce) {
    UiModel model = make_model();
    model.apply(UiEvent{UserMessage{kSession, "u1", "first"}});
    model.apply(UiEvent{UserMessage{kSession, "u1", "first"}});
    model.apply(UiEvent{UserMessage{kSession, "u2", "second"}});
    model.apply(UiEvent{
        ContextInjected{kSession, "c1", Role::User, "ctx", MessageSource{}, ContextFormed{}}});
    model.apply(UiEvent{
        ContextInjected{kSession, "c1", Role::User, "ctx", MessageSource{}, ContextFormed{}}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    std::size_t users    = 0;
    std::size_t contexts = 0;
    for (const ConversationEntry& entry : state->conversation.entries) {
        if (entry.role == ConversationRole::User) {
            ++users;
        }
        if (entry.role == ConversationRole::Context) {
            ++contexts;
        }
    }
    EXPECT_EQ(users, 2u);
    EXPECT_EQ(contexts, 1u);
}

TEST(UiModelDedup, PluginUserMessageRendersWithoutHistory) {
    UiModel model = make_model();

    MessageSource nudge_source;
    nudge_source.kind   = MessageSource::Kind::Plugin;
    nudge_source.plugin = "agent-nudge";
    model.apply(UiEvent{UserMessage{kSession, "n1", "nudge", nudge_source}});

    model.apply(UiEvent{UserMessage{kSession, "u1", "typed prompt"}});

    const SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    std::size_t users = 0;
    for (const ConversationEntry& entry : state->conversation.entries) {
        if (entry.role == ConversationRole::User) {
            ++users;
        }
    }
    EXPECT_EQ(users, 2u);
    ASSERT_EQ(state->input.history.size(), 1u);
    EXPECT_EQ(state->input.history.front(), "typed prompt");
}

} // namespace
