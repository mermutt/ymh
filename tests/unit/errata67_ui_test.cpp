// 67-D2..67-D4: supervisor-level `/rename` semantics that spec 19 left to the
// registry unit tests: the full trimmed argument (spaces) reaches the wire, an
// empty argument makes no wire call, a rejected rename is visible, and a rename
// in a workspace with no live supervisor connection fails visibly rather than
// silently. Driven through the real `dispatch_command` via `SupervisorHarness`.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata67-ui"},
                          protocol::ClientRole::Supervisor};
}

WorkspaceModel live_workspace(const WorkspaceId& id, const std::string& title) {
    WorkspaceModel workspace;
    workspace.id           = id;
    workspace.title        = title;
    workspace.cwd          = "/" + id.value;
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live         = true;
    return workspace;
}

struct Fixture {
    std::unique_ptr<SupervisorHarness> harness;
    WorkspaceId                        workspace{"ws-a"};
    SessionId                          session{"s1"};
};

Fixture make_fixture() {
    SupervisorRunOptions options;
    options.identity = test_identity();
    Fixture fixture;
    fixture.harness = make_supervisor_harness(std::move(options));
    fixture.harness->seed_active_workspace(live_workspace(fixture.workspace, "alpha"));
    fixture.harness->activate_session(fixture.workspace, fixture.session);
    return fixture;
}

const ConversationEntry& last_entry(const Fixture& fixture) {
    const SessionUiState* state = fixture.harness->model().session(fixture.session);
    return state->conversation.entries.back();
}

TEST(Errata67Rename, SpacesReachTheWireAsTheWholeTrimmedTitle) {
    Fixture fixture = make_fixture();
    EXPECT_TRUE(fixture.harness->dispatch_command_line("/rename my new title"));

    EXPECT_EQ(fixture.harness->submitted_count("session.rename"), 1u);
    const std::optional<nlohmann::json> params =
        fixture.harness->last_submitted_params("session.rename");
    ASSERT_TRUE(params.has_value());
    EXPECT_EQ((*params)["title"].get<std::string>(), "my new title");
    EXPECT_EQ((*params)["session"].get<std::string>(), fixture.session.value);
}

TEST(Errata67Rename, EmptyOrWhitespaceArgMakesNoWireCallAndShowsUsage) {
    Fixture fixture = make_fixture();
    EXPECT_TRUE(fixture.harness->dispatch_command_line("/rename"));
    EXPECT_TRUE(fixture.harness->dispatch_command_line("/rename    "));

    EXPECT_EQ(fixture.harness->submitted_count("session.rename"), 0u);
    EXPECT_NE(last_entry(fixture).text.find("usage"), std::string::npos);
}

TEST(Errata67Rename, RejectionIsVisible) {
    Fixture fixture = make_fixture();
    fixture.harness->install_method_error_reply("session.rename", -32602,
                                                "InvalidParams");
    EXPECT_TRUE(fixture.harness->dispatch_command_line("/rename " + std::string(160, 'x')));
    fixture.harness->drain_actions();

    EXPECT_NE(last_entry(fixture).text.find("rename failed: InvalidParams"),
              std::string::npos);
}

TEST(Errata67Rename, NoLiveConnectionFailsVisibly) {
    Fixture fixture = make_fixture();
    EXPECT_TRUE(fixture.harness->dispatch_command_line("/rename detached"));
    fixture.harness->drain_actions();

    EXPECT_NE(last_entry(fixture).text.find("rename failed: no supervisor connection"),
              std::string::npos);
}

} // namespace
