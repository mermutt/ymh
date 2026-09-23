// 53-D6/D7: the durable `session/model` event, the daemon-side
// `ModelSelectionController`, the `ModelCatalog`, the canonical `display_model`,
// and the `/model` picker list source.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/model_selection.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/ui/ui_event.hpp"
#include "ymh/ui/ui_event_adapter.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

ModelSelection make_selection(const std::string& wire_id, const std::string& name) {
    ModelSelection selection;
    selection.model      = wire_id;
    selection.model_name = name;
    return selection;
}

ModelSettings model_settings(const std::string& endpoint, const std::string& model) {
    ModelSettings settings;
    settings.endpoint = endpoint;
    settings.model    = model;
    return settings;
}

std::size_t model_event_count(const std::shared_ptr<Session>& session) {
    std::size_t count = 0;
    for (const EventRecord& record : session->events()) {
        if (record.event.type == EventType::SessionModelChanged) {
            ++count;
        }
    }
    return count;
}

struct ModelEnv {
    test::MemorySessionStore store;
    EventBus                 bus;
    SessionManager           manager{store, bus};
    SessionId                id;
    ModelSelectionController controller;

    ModelEnv()
        : controller(
              [this](const SessionId& session_id, payload::SessionModelChanged change) {
                  if (std::shared_ptr<Session> session = manager.sessionPtr(session_id);
                      session != nullptr) {
                      session->append(change);
                  }
              },
              [](const std::string& wire_id) -> std::optional<ModelSelection> {
                  if (wire_id == "wire-a") {
                      return make_selection("wire-a", "alpha");
                  }
                  if (wire_id == "wire-b") {
                      return make_selection("wire-b", "beta");
                  }
                  return std::nullopt;
              }) {
        SessionOptions options;
        options.cwd           = std::filesystem::temp_directory_path();
        options.serverProfile = "interactive";
        options.model         = "test-model";
        options.title         = "model";
        id                    = manager.createSession(options);
    }

    std::shared_ptr<Session> session() { return manager.sessionPtr(id); }
};

// 53-U19 (53-D7/H4): the controller carries the selected entry's parameters,
// profile, and provider, not just the id.
TEST(ModelSelectionTest, UI53_D7_SelectionCarriesParams) {
    ModelEnv env;
    ModelSelection selection = make_selection("wire-a", "alpha");
    selection.parameters.reasoning_effort = "high";
    selection.parameters.temperature      = 0.3;
    selection.profile.id                  = "prof";
    selection.provider                    = "prov";

    EXPECT_EQ(env.controller.set(*env.session(), false, selection), ModelSetResult::Committed);

    const std::optional<ModelSelection> effective = env.controller.effective(*env.session());
    ASSERT_TRUE(effective.has_value());
    EXPECT_EQ(effective->model, "wire-a");
    EXPECT_EQ(effective->parameters.reasoning_effort, "high");
    ASSERT_TRUE(effective->parameters.temperature.has_value());
    EXPECT_DOUBLE_EQ(*effective->parameters.temperature, 0.3);
    EXPECT_EQ(effective->profile.id, "prof");
    EXPECT_EQ(effective->provider, "prov");
    EXPECT_EQ(env.session()->header().model, "wire-a");
}

// 53-U10 (53-D5/53-I7): an open turn queues the change; nothing is appended until
// the step boundary.
TEST(ModelSelectionTest, UI53_D5_SetModelQueued) {
    ModelEnv env;
    EXPECT_EQ(env.controller.set(*env.session(), true, make_selection("wire-a", "alpha")),
              ModelSetResult::Queued);
    EXPECT_EQ(model_event_count(env.session()), 0u);

    EXPECT_TRUE(env.controller.apply_pending_at_step_start(*env.session()));
    EXPECT_EQ(model_event_count(env.session()), 1u);
    EXPECT_EQ(env.session()->header().model, "wire-a");
}

// 53-U14 (53-F8): erasing a deleted session drops its pending change.
TEST(ModelSelectionTest, UI53_D7_ControllerEraseOnDelete) {
    ModelEnv env;
    EXPECT_EQ(env.controller.set(*env.session(), true, make_selection("wire-a", "alpha")),
              ModelSetResult::Queued);
    env.controller.erase(env.id);
    EXPECT_FALSE(env.controller.effective(*env.session()).has_value());
}

// 53-U26 (53-F12): a queued change that is never applied is not durable: a cold
// controller (a restarted daemon) reverts to the last durable value.
TEST(ModelSelectionTest, UI53_F12_PendingLostOnDaemonDeath) {
    ModelEnv env;
    EXPECT_EQ(env.controller.set(*env.session(), true, make_selection("wire-a", "alpha")),
              ModelSetResult::Queued);
    EXPECT_EQ(model_event_count(env.session()), 0u);

    ModelSelectionController restarted(
        [&env](const SessionId& session_id, payload::SessionModelChanged change) {
            if (std::shared_ptr<Session> session = env.manager.sessionPtr(session_id);
                session != nullptr) {
                session->append(change);
            }
        },
        [](const std::string&) -> std::optional<ModelSelection> { return std::nullopt; });
    EXPECT_FALSE(restarted.effective(*env.session()).has_value());
}

// 53-U21 (53-I6): the last `session/model` wins on fold/reload.
TEST(ModelSelectionTest, UI53_D6_FoldLastWins) {
    ModelEnv env;
    std::shared_ptr<Session> session = env.session();
    session->append(payload::SessionModelChanged{"wire-a", "alpha"});
    EXPECT_EQ(session->header().model, "wire-a");
    session->append(payload::SessionModelChanged{"wire-b", "beta"});
    EXPECT_EQ(session->header().model, "wire-b");

    // A fresh manager over the same store folds the log on load: the last event
    // wins.
    SessionManager reloaded{env.store, env.bus};
    EXPECT_EQ(reloaded.resumeSession(env.id), env.id);
    EXPECT_EQ(reloaded.sessionPtr(env.id)->header().model, "wire-b");
}

// 53-U11 (53-D6): the event wire name round-trips and JSON round-trips.
TEST(ModelSelectionTest, UI53_D6_WireAndJsonRoundTrip) {
    EXPECT_EQ(wire_name(EventType::SessionModelChanged), "session/model");
    const std::optional<EventType> parsed = parse_event_type("session/model");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, EventType::SessionModelChanged);
    EXPECT_NE(std::find(all_event_types().begin(), all_event_types().end(),
                        EventType::SessionModelChanged),
              all_event_types().end());

    const payload::SessionModelChanged value{"wire-a", "alpha"};
    const nlohmann::json               json = value;
    const payload::SessionModelChanged back = json.get<payload::SessionModelChanged>();
    EXPECT_EQ(back.model, "wire-a");
    EXPECT_EQ(back.model_name, "alpha");
}

// 53-U23 (53-F11): concurrent switches serialize; the last append wins on fold.
TEST(ModelSelectionTest, UI53_F11_ConcurrentSetModel) {
    ModelEnv env;
    std::thread first(
        [&env] { (void)env.controller.set(*env.session(), false, make_selection("wire-a", "alpha")); });
    std::thread second(
        [&env] { (void)env.controller.set(*env.session(), false, make_selection("wire-b", "beta")); });
    first.join();
    second.join();

    EXPECT_EQ(model_event_count(env.session()), 2u);
    const std::string model = env.session()->header().model;
    EXPECT_TRUE(model == "wire-a" || model == "wire-b");
}

// 53-U18 (53-D3.1): `display_model` is the single name-else-id rule.
TEST(ModelSelectionTest, UI53_D3_DisplayModelCanonical) {
    Config config;
    config.llm.models["balanced"] = model_settings("ds", "Muse-Glimmer-30B");
    EXPECT_EQ(display_model(config, "Muse-Glimmer-30B"), "balanced");
    EXPECT_EQ(display_model(config, "unknown-model"), "unknown-model");
    EXPECT_EQ(display_model(config, ""), "");

    ResolvedModel resolved;
    resolved.model_name = "balanced";
    resolved.model_id   = "Muse-Glimmer-30B";
    EXPECT_EQ(display_model(resolved), "balanced");
    resolved.model_name = "";
    EXPECT_EQ(display_model(resolved), "Muse-Glimmer-30B");
}

// 53-U12 (53-D6): the adapter emits `ModelChanged`; `apply` sets the display name.
TEST(ModelSelectionTest, UI53_D6_AdapterUpdatesStatus) {
    UiModel model;
    const WorkspaceId workspace{"ws"};
    model.activeWorkspaceId = workspace;
    WorkspaceModel ws;
    ws.id           = workspace;
    ws.daemonStatus = DaemonStatus::Attached;
    model.workspaces.emplace(workspace, ws);
    const SessionId session{"s-model"};
    model.ensureSessionIn(workspace, session);

    UiEventAdapter adapter(model);
    Event          event;
    event.id         = make_event_id();
    event.session_id = session;
    event.timestamp  = std::chrono::system_clock::now();
    event.type       = EventType::SessionModelChanged;
    event.payload    = payload::SessionModelChanged{"wire-a", "alpha"};
    adapter.onEvent(event);

    const SessionUiState* state = model.session(session);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->status.model, "alpha");
}

// 53-U5 (53-I4): the picker lists the `llm.models` entries sorted, with a
// synthetic current-literal row when no named entry is active.
TEST(ModelSelectionTest, UI53_D4_PickerListsModels) {
    Config config;
    config.llm.models["zeta"]  = model_settings("ds", "m-z");
    config.llm.models["alpha"] = model_settings("ds", "m-a");

    ModelPickerModel picker;
    picker.open(config, "alpha");
    ASSERT_EQ(picker.rows.size(), 3u);
    EXPECT_EQ(picker.rows[0].name, "");
    EXPECT_EQ(picker.rows[1].name, "alpha");
    EXPECT_EQ(picker.rows[2].name, "zeta");
    EXPECT_EQ(picker.selected, 1u);
    EXPECT_TRUE(picker.visible);

    picker.moveDown();
    EXPECT_EQ(picker.selected, 2u);
    picker.moveDown();
    EXPECT_EQ(picker.selected, 0u);
    picker.moveUp();
    EXPECT_EQ(picker.selected, 2u);
    picker.close();
    EXPECT_FALSE(picker.visible);
    EXPECT_TRUE(picker.rows.empty());
}

// 53-U5 (53-I4): a named active model suppresses the synthetic row.
TEST(ModelSelectionTest, UI53_D4_PickerNoSyntheticWhenNamed) {
    Config config;
    config.llm.endpoints["ds"] = EndpointSettings{};
    config.llm.models["alpha"] = model_settings("ds", "m-a");
    config.llm.active_model    = "alpha";

    ModelPickerModel picker;
    picker.open(config, "alpha");
    ASSERT_EQ(picker.rows.size(), 1u);
    EXPECT_EQ(picker.rows[0].name, "alpha");
    EXPECT_EQ(picker.rows[0].model_id, "m-a");
    EXPECT_EQ(picker.rows[0].endpoint, "ds");
    EXPECT_EQ(picker.selected, 0u);
}

// 53-U20 (53-D2/H5): the catalog resolves a name or a literal wire id to a full
// entry, and `default_entry` is the daemon's registered endpoint's model.
TEST(ModelSelectionTest, UI53_D5_CatalogResolvesNameAndLiteral) {
    Config config;
    config.llm.endpoints["ds"] = EndpointSettings{};
    config.llm.models["alpha"] = model_settings("ds", "m-a");
    config.llm.active_model    = "alpha";

    const ModelCatalog catalog = ModelCatalog::build(config);
    const std::optional<ModelCatalogEntry> by_name = catalog.find("alpha");
    ASSERT_TRUE(by_name.has_value());
    EXPECT_EQ(by_name->model_id, "m-a");
    EXPECT_EQ(by_name->endpoint.name, "ds");

    const std::optional<ModelCatalogEntry> by_id = catalog.find("m-a");
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_id->name, "alpha");
    EXPECT_FALSE(catalog.find("nope").has_value());
    EXPECT_EQ(catalog.default_entry().model_id, "m-a");
}

} // namespace
