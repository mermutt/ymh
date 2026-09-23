// 53-D2/D3/D4: the eager attach auto-create, the `/model` picker keymap and
// no-session preference path, the unknown-name error, and the read-only refusal.
// The supervisor-level cases drive the real private handlers through the additive
// `SupervisorHarness` seam.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ymh/config/config.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata53-ui"},
                          protocol::ClientRole::Supervisor};
}

ModelSettings model_settings(const std::string& endpoint, const std::string& model) {
    ModelSettings settings;
    settings.endpoint = endpoint;
    settings.model    = model;
    return settings;
}

Config config_with_models() {
    Config config;
    config.llm.endpoints["ds"]    = EndpointSettings{};
    config.llm.models["balanced"] = model_settings("ds", "m-a");
    config.llm.models["fast"]     = model_settings("ds", "m-b");
    return config;
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

SupervisorWorkspace spec_for(const WorkspaceId& id) {
    SupervisorWorkspace spec;
    spec.id          = id;
    spec.cwd         = "/" + id.value;
    spec.title       = id.value;
    spec.socket_path = "/tmp/" + id.value + "/host.sock";
    spec.boot_id     = "boot-a";
    return spec;
}

// 53-U3 (53-D2/53-I2): an `Attached` workspace with no live session issues
// `session.create`; `status.model` becomes non-empty.
TEST(Errata53, UI53_U3_AttachAutoCreatesSession) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->on_scan({spec_for(workspace)});
    harness->drain_actions();

    harness->install_session_list_reply(nlohmann::json::array(), 0);
    harness->install_method_reply(std::string(protocol::method::kSessionCreate),
                                  nlohmann::json{{"session", "s1"}}, 0);
    harness->on_link_state(workspace, SupervisorLinkState::Attached, "test");
    harness->drain_actions();
    harness->drain_actions();
    harness->drain_actions();

    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kSessionCreate)), 1U);
    const SessionUiState* state = harness->model().session(SessionId{"s1"});
    ASSERT_NE(state, nullptr);
    EXPECT_FALSE(state->status.model.empty());
}

// 53-U7 (53-D4): with no session, `Enter` on a picker row sets the preference and
// the resolved model; no RPC is issued.
TEST(Errata53, UI53_U7_NoSessionSetsPreference) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    options.config   = config_with_models();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    EXPECT_TRUE(harness->dispatch_command_line("/model"));
    EXPECT_EQ(harness->model().mode, UiMode::ModelPicker);

    EXPECT_TRUE(harness->dispatch_key("down"));
    EXPECT_TRUE(harness->dispatch_key("enter"));

    EXPECT_EQ(harness->model().mode, UiMode::Conversation);
    EXPECT_FALSE(harness->model().resolved_model.empty());
    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kSessionSetModel)), 0U);
}

// 53-U8 (53-F5): `/model bogus` appends an error and issues no RPC.
TEST(Errata53, UI53_U8_UnknownNameErrors) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    options.config   = config_with_models();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    EXPECT_TRUE(harness->dispatch_command_line("/model bogus"));

    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_NE(harness->model().notices.back().text.find("unknown model: bogus"),
              std::string::npos);
    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kSessionSetModel)), 0U);
}

// 53-U16 (53-I13/53-F9): `/model` on a read-only session shows the lockout
// notice and issues no RPC.
TEST(Errata53, UI53_U16_ReadOnlyRefused) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    options.config   = config_with_models();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);
    harness->mutable_model().setSessionReadOnly(session, true);

    EXPECT_TRUE(harness->dispatch_command_line("/model balanced"));

    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kSessionSetModel)), 0U);
    const SessionUiState* state = harness->model().session(session);
    ASSERT_NE(state, nullptr);
    bool found = false;
    for (const ConversationEntry& entry : state->conversation.entries) {
        if (entry.text.find("read-only") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

// 53-U6 (53-I10): arrows/`j`/`k` wrap; Enter applies; Esc dismisses; Tab is a
// no-op.
TEST(Errata53, UI53_U6_PickerKeys) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    options.config   = config_with_models();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    EXPECT_TRUE(harness->dispatch_command_line("/model"));
    ASSERT_EQ(harness->model().mode, UiMode::ModelPicker);
    ASSERT_TRUE(harness->model().model_picker.visible);
    const std::size_t rows = harness->model().model_picker.rows.size();
    ASSERT_GE(rows, 2U);

    EXPECT_TRUE(harness->dispatch_key("tab"));
    EXPECT_EQ(harness->model().mode, UiMode::ModelPicker);

    const std::size_t before = harness->model().model_picker.selected;
    EXPECT_TRUE(harness->dispatch_key("down"));
    EXPECT_NE(harness->model().model_picker.selected, before);
    EXPECT_TRUE(harness->dispatch_key("up"));
    EXPECT_EQ(harness->model().model_picker.selected, before);

    EXPECT_TRUE(harness->dispatch_key("enter"));
    EXPECT_EQ(harness->model().mode, UiMode::Conversation);
    EXPECT_FALSE(harness->model().resolved_model.empty());

    EXPECT_TRUE(harness->dispatch_command_line("/model"));
    ASSERT_EQ(harness->model().mode, UiMode::ModelPicker);
    EXPECT_TRUE(harness->dispatch_key("escape"));
    EXPECT_EQ(harness->model().mode, UiMode::Conversation);
}

// 53-U27 (53-F7): a lost `session.set_model` reply never changes `status.model`
// optimistically; the durable `ModelChanged` projection reconciles it.
TEST(Errata53, UI53_F7_ReplyLostNoOptimisticModel) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    options.config   = config_with_models();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);
    harness->mutable_model().session(session)->status.model = "alpha";

    // No connection and no canned reply: `submit_to` replies failure.
    EXPECT_TRUE(harness->dispatch_command_line("/model beta"));
    harness->drain_actions();
    EXPECT_EQ(harness->model().session(session)->status.model, "alpha");

    harness->mutable_model().apply(UiEvent{ModelChanged{session, "beta-wire", "beta"}});
    EXPECT_EQ(harness->model().session(session)->status.model, "beta");
}

// 53-U27 (53-F13): a failed eager auto-create degrades to the 53-D8 no-session
// path; the next prompt retries the create.
TEST(Errata53, UI53_F13_EagerCreateRetryFallback) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    options.config   = config_with_models();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->on_scan({spec_for(workspace)});
    harness->drain_actions();

    harness->install_session_list_reply(nlohmann::json::array(), 0);
    harness->install_method_error_reply(std::string(protocol::method::kSessionCreate), -32015,
                                        "store unavailable");
    harness->on_link_state(workspace, SupervisorLinkState::Attached, "test");
    harness->drain_actions();
    harness->drain_actions();
    harness->drain_actions();

    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_NE(harness->model().notices.back().text.find("cannot create session"), std::string::npos);
    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kSessionCreate)), 1U);

    // The next prompt retries through the retained lazy `submit()` path.
    harness->install_method_reply(std::string(protocol::method::kSessionCreate),
                                  nlohmann::json{{"session", "s1"}}, 0);
    harness->submit("hi");
    harness->drain_actions();
    EXPECT_EQ(harness->submitted_count(std::string(protocol::method::kSessionCreate)), 2U);
}

} // namespace
