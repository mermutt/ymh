// 46-D3/D9/D11/D13: the empty-switcher notice, the turn spinner, prompt-history
// hydration, and Ctrl+E. The supervisor-level cases drive the real private
// handlers through the additive `SupervisorHarness` seam; the model-level cases
// drive `UiModel` directly; the editor cases point `$EDITOR` at a hermetic
// script.

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "support/short_temp.hpp"
#include "ymh/agent/provenance.hpp"
#include "ymh/transport/protocol.hpp"
#include "ymh/ui/session_catalog.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace ymh::test;
using namespace std::chrono_literals;

constexpr std::array<const char*, 8> kFrames = {
    "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧",
};

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
    std::string              current;
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

std::string line_with(const std::string& text, const std::string& needle) {
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        const std::string line =
            text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.find(needle) != std::string::npos) {
            return line;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return {};
}

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata46-ui"},
                          protocol::ClientRole::Supervisor};
}

WorkspaceModel live_workspace(const WorkspaceId& id, const std::string& title) {
    WorkspaceModel workspace;
    workspace.id = id;
    workspace.title = title;
    workspace.cwd = "/" + id.value;
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live = true;
    return workspace;
}

UiModel model_with_active_session(AgentState state = AgentState::Idle) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws"};
    model.workspaces.emplace(model.activeWorkspaceId, live_workspace(model.activeWorkspaceId, "alpha"));
    model.focusSessionIn(model.activeWorkspaceId, SessionId{"s"});
    SessionUiState* session = model.session(SessionId{"s"});
    session->status.model = "test-model";
    session->agent_state  = state;
    return model;
}

std::string status_line(const std::string& rendered) {
    return line_with(rendered, "0 active");
}

// ── 46-D3 ───────────────────────────────────────────────────────────────────

TEST(Errata46D3, UI46_D3_NoOtherWorkspaceShowsNotice) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Notice);
    EXPECT_TRUE(harness->model().message.open);
    EXPECT_EQ(harness->model().message.text, "No other workspaces available");
    EXPECT_TRUE(harness->model().switcher.workspaces.empty());
}

TEST(Errata46D3, UI46_D3_NoticeEnterDismisses) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, SessionId{"s1"});
    harness->open_switcher();
    ASSERT_EQ(harness->model().mode, UiMode::Notice);

    EXPECT_TRUE(harness->dispatch_key("enter"));

    EXPECT_EQ(harness->model().mode, UiMode::Conversation);
    EXPECT_FALSE(harness->model().message.open);
}

TEST(Errata46D3, UI46_D3_NoticeEscapeDismisses) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, SessionId{"s1"});
    harness->open_switcher();
    ASSERT_EQ(harness->model().mode, UiMode::Notice);

    EXPECT_TRUE(harness->dispatch_key("escape"));

    EXPECT_EQ(harness->model().mode, UiMode::Conversation);
    EXPECT_FALSE(harness->model().message.open);
}

TEST(Errata46D3, UI46_D3_NoticeSwallowsOtherKeys) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, SessionId{"s1"});
    harness->open_switcher();
    ASSERT_EQ(harness->model().mode, UiMode::Notice);

    for (const std::string key : {"x", "/", "j", "k", "1"}) {
        EXPECT_TRUE(harness->dispatch_key(key));
        EXPECT_EQ(harness->model().mode, UiMode::Notice) << key;
        EXPECT_TRUE(harness->model().message.open) << key;
    }
}

TEST(Errata46D3, UI46_D3_OtherLiveWorkspaceShowsSwitcher) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, SessionId{"s1"});
    harness->seed_workspace(live_workspace(WorkspaceId{"ws-b"}, "beta"));

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Switcher);
    EXPECT_FALSE(harness->model().message.open);
    EXPECT_EQ(harness->model().switcher.workspaces.size(), 2u);
}

TEST(Errata46D3, UI46_D3_OtherSessionShowsSwitcher) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));

    const WorkspaceId workspace{"ws-a"};
    const SessionId   focused{"s1"};
    const SessionId   other{"s2"};
    WorkspaceModel    model = live_workspace(workspace, "alpha");
    for (const SessionId& id : {focused, other}) {
        SessionCell cell;
        cell.id = id;
        model.sessions.push_back(cell);
    }
    harness->seed_active_workspace(model);
    harness->activate_session(workspace, focused);

    SessionCatalogSnapshot snapshot;
    WorkspaceHistory       history;
    history.id            = workspace;
    history.title         = "alpha";
    history.canonicalPath = "/ws-a";
    SessionHistoryEntry entry;
    entry.id   = other;
    entry.kind = "root";
    history.sessions.push_back(entry);
    snapshot.workspaces.push_back(std::move(history));
    snapshot.generation = 1;
    harness->on_catalog_snapshot(std::move(snapshot));

    harness->open_switcher();

    EXPECT_EQ(harness->model().mode, UiMode::Switcher);
    EXPECT_FALSE(harness->model().message.open);
}

TEST(Errata46D3, UI46_D3_NoticeBlocksComposer) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);
    ASSERT_TRUE(harness->dispatch_key("a"));

    harness->open_switcher();
    ASSERT_EQ(harness->model().mode, UiMode::Notice);

    EXPECT_TRUE(harness->dispatch_key("b"));

    ASSERT_NE(harness->model().session(session), nullptr);
    EXPECT_EQ(harness->model().session(session)->input.draft, "a");
    EXPECT_EQ(harness->model().mode, UiMode::Notice);
}

TEST(Errata46D3, UI46_D3_StaleNoticeStaysUntilDismissed) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    const std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, SessionId{"s1"});
    harness->open_switcher();
    ASSERT_EQ(harness->model().mode, UiMode::Notice);

    harness->seed_workspace(live_workspace(WorkspaceId{"ws-b"}, "beta"));
    EXPECT_TRUE(harness->dispatch_key("x"));
    EXPECT_EQ(harness->model().mode, UiMode::Notice);
    EXPECT_TRUE(harness->model().message.open);

    EXPECT_TRUE(harness->dispatch_key("enter"));
    EXPECT_EQ(harness->model().mode, UiMode::Conversation);
}

// ── 46-D9 ───────────────────────────────────────────────────────────────────

TEST(Errata46D9, UI46_D9_SpinnerOnWhileTurnActive) {
    UiModel model = model_with_active_session(AgentState::Thinking);
    EXPECT_TRUE(model.has_active_turn());

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    const std::string line = status_line(rendered);
    SCOPED_TRACE(rendered);
    EXPECT_NE(line.find(kFrames[0]), std::string::npos);
}

TEST(Errata46D9, UI46_D9_SpinnerOffOnIdle) {
    UiModel model = model_with_active_session(AgentState::Idle);
    EXPECT_FALSE(model.has_active_turn());

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    const std::string line = status_line(rendered);
    SCOPED_TRACE(rendered);
    for (const char* frame : kFrames) {
        EXPECT_EQ(line.find(frame), std::string::npos) << frame;
    }
}

TEST(Errata46D9, UI46_D9_SpinnerOffOnWaitingForPermission) {
    UiModel model = model_with_active_session(AgentState::WaitingForPermission);
    EXPECT_FALSE(model.has_active_turn());

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    const std::string line = status_line(rendered);
    SCOPED_TRACE(rendered);
    for (const char* frame : kFrames) {
        EXPECT_EQ(line.find(frame), std::string::npos) << frame;
    }
}

TEST(Errata46D9, UI46_D9_SpinnerAdvancesOnTick) {
    UiModel model = model_with_active_session(AgentState::Thinking);
    const std::uint32_t before = model.spinner.frame;

    EXPECT_TRUE(model.advance_spinner(120ms));
    EXPECT_EQ(model.spinner.frame, before + 1);
    EXPECT_FALSE(model.advance_spinner(60ms));
    EXPECT_EQ(model.spinner.frame, before + 1);

    UiModel idle = model_with_active_session(AgentState::Idle);
    EXPECT_FALSE(idle.advance_spinner(500ms));
    EXPECT_EQ(idle.spinner.frame, 0u);
}

TEST(Errata46D9, UI46_D9_SpinnerBeforeModeSegment) {
    UiModel model = model_with_active_session(AgentState::Thinking);
    model.spinner.frame = 0;

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    const std::string line = status_line(rendered);
    SCOPED_TRACE(rendered);
    EXPECT_EQ(line.find("│⠋  · build"), 0u);
}

TEST(Errata46D9, UI46_D9_SpinnerSharesFrameTable) {
    UiModel model = model_with_active_session(AgentState::Thinking);
    model.spinner.frame = 3;
    ConversationEntry reasoning;
    reasoning.role      = ConversationRole::Reasoning;
    reasoning.text      = "thinking";
    reasoning.streaming = true;
    model.session(SessionId{"s"})->conversation.entries.push_back(std::move(reasoning));
    EXPECT_TRUE(model.active_has_streaming_reasoning());

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    const std::string line = status_line(rendered);
    SCOPED_TRACE(rendered);
    EXPECT_NE(line.find("⠸  · build"), std::string::npos);
    EXPECT_NE(rendered.find("⠸ Thinking"), std::string::npos);
}

TEST(Errata46D9, UI46_D9_SpinnerOnlyActiveSession) {
    UiModel model = model_with_active_session(AgentState::Idle);
    const SessionId background{"bg"};
    model.ensureSessionIn(WorkspaceId{"ws-bg"}, background);
    model.session(background)->agent_state = AgentState::Thinking;
    ConversationEntry reasoning;
    reasoning.role      = ConversationRole::Reasoning;
    reasoning.text      = "background";
    reasoning.streaming = true;
    model.session(background)->conversation.entries.push_back(std::move(reasoning));

    EXPECT_FALSE(model.has_active_turn());
    EXPECT_TRUE(model.has_streaming_reasoning());
    EXPECT_FALSE(model.active_has_streaming_reasoning());
    const std::uint32_t before = model.spinner.frame;
    EXPECT_FALSE(model.advance_spinner(500ms));
    EXPECT_EQ(model.spinner.frame, before);

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}));
    const std::string line = status_line(rendered);
    SCOPED_TRACE(rendered);
    for (const char* frame : kFrames) {
        EXPECT_EQ(line.find(frame), std::string::npos) << frame;
    }
}

// ── 46-D11 ──────────────────────────────────────────────────────────────────

UiModel hydration_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"ws"};
    model.workspaces.emplace(model.activeWorkspaceId, live_workspace(model.activeWorkspaceId, "alpha"));
    return model;
}

TEST(Errata46D11, UI46_D11_HistoryHydratedOnReplay) {
    UiModel model = hydration_model();
    const SessionId session{"s"};

    model.apply(UiEvent{UserMessage{session, "m1", "first prompt"}});
    model.apply(UiEvent{UserMessage{session, "m2", "second prompt"}});

    SessionUiState* state = model.session(session);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->input.history.size(), 2u);
    EXPECT_EQ(state->input.history[0], "first prompt");
    EXPECT_EQ(state->input.history[1], "second prompt");
    EXPECT_TRUE(state->input.history_up());
    EXPECT_EQ(state->input.draft, "second prompt");
}

TEST(Errata46D11, UI46_D11_FirstPromptOfNewSessionRecallable) {
    UiModel model = hydration_model();
    const SessionId session{"s"};
    ASSERT_EQ(model.session(session), nullptr);

    model.apply(UiEvent{UserMessage{session, "m1", "first prompt"}});

    SessionUiState* state = model.session(session);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->input.history.size(), 1u);
    EXPECT_TRUE(state->input.history_up());
    EXPECT_EQ(state->input.draft, "first prompt");
}

TEST(Errata46D11, UI46_D11_NoDuplicateForLivePrompt) {
    UiModel model = hydration_model();
    const SessionId session{"s"};
    model.ensureSessionIn(WorkspaceId{"ws"}, session).input.push_history("hello");

    model.apply(UiEvent{UserMessage{session, "m1", "hello"}});

    SessionUiState* state = model.session(session);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->input.history.size(), 1u);
    EXPECT_EQ(state->input.history[0], "hello");
}

TEST(Errata46D11, UI46_D11_HistoryOrderMatchesLog) {
    UiModel model = hydration_model();
    const SessionId session{"s"};

    model.apply(UiEvent{UserMessage{session, "m1", "one"}});
    model.apply(UiEvent{UserMessage{session, "m2", "two"}});
    model.apply(UiEvent{UserMessage{session, "m3", "three"}});

    SessionUiState* state = model.session(session);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->input.history.size(), 3u);
    EXPECT_EQ(state->input.history[0], "one");
    EXPECT_EQ(state->input.history[1], "two");
    EXPECT_EQ(state->input.history[2], "three");
}

TEST(Errata46D11, UI46_D11_CommandsNotHydrated) {
    UiModel model = hydration_model();
    const SessionId session{"s"};

    model.apply(UiEvent{UserMessage{session, "m1", "run the tests"}});
    model.apply(UiEvent{AssistantMessageFinished{session, "a1", "/status", std::nullopt}});

    SessionUiState* state = model.session(session);
    ASSERT_NE(state, nullptr);
    ASSERT_EQ(state->input.history.size(), 1u);
    EXPECT_EQ(state->input.history[0], "run the tests");
    EXPECT_EQ(std::find(state->input.history.begin(), state->input.history.end(), "/status"),
              state->input.history.end());
}

TEST(Errata46D11, UI46_D11_PluginMessageNotHydrated) {
    UiModel model = hydration_model();
    const SessionId session{"s"};

    MessageSource plugin;
    plugin.kind   = MessageSource::Kind::Plugin;
    plugin.plugin = "job_wakeup";
    model.apply(UiEvent{UserMessage{session, "m1", "job woke up", plugin}});

    MessageSource goal;
    goal.kind = MessageSource::Kind::Goal;
    model.apply(UiEvent{UserMessage{session, "m2", "goal round", goal}});

    SessionUiState* state = model.session(session);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->input.history.empty());
}

// ── 46-D13 ──────────────────────────────────────────────────────────────────

class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* previous = std::getenv(name); previous != nullptr) {
            had_ = true;
            old_ = previous;
        }
        ::setenv(name, value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (had_) {
            ::setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string name_;
    std::string old_;
    bool        had_ = false;
};

std::filesystem::path write_editor_script(const std::filesystem::path& directory,
                                          const std::string& name, const std::string& body) {
    const std::filesystem::path path = directory / name;
    std::ofstream               out(path, std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\n" << body << "\n";
    out.close();
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);
    return path;
}

std::unique_ptr<SupervisorHarness> editor_harness(const WorkspaceId& workspace,
                                                  const SessionId& session) {
    SupervisorRunOptions options;
    options.identity = test_identity();
    std::unique_ptr<SupervisorHarness> harness = make_supervisor_harness(std::move(options));
    harness->install_prompt_editor_io();
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);
    return harness;
}

void type_text(SupervisorHarness& harness, const std::string& text) {
    for (const char character : text) {
        harness.dispatch_key(std::string(1, character));
    }
}

TEST(Errata46D13, UI46_D13_CtrlEOpensEditor) {
    const ShortTempRoot root("ymh-d13");
    const auto          marker = root.path() / "editor-ran";
    const auto script = write_editor_script(
        root.path(), "editor.sh", "touch \"" + marker.string() + "\"\nprintf '%s' 'edited' > \"$1\"");
    const ScopedEnv visual("VISUAL", script.string());
    const ScopedEnv editor("EDITOR", script.string());

    const WorkspaceId workspace{"ws"};
    const SessionId   session{"s"};
    const std::unique_ptr<SupervisorHarness> harness = editor_harness(workspace, session);
    type_text(*harness, "draft");

    EXPECT_TRUE(harness->dispatch_key("ctrl-e"));

    EXPECT_TRUE(std::filesystem::exists(marker));
    ASSERT_NE(harness->model().session(session), nullptr);
    EXPECT_EQ(harness->model().session(session)->input.draft, "edited");
}

TEST(Errata46D13, UI46_D13_EditorResultCopied) {
    const ShortTempRoot root("ymh-d13");
    const auto script = write_editor_script(root.path(), "editor.sh",
                                            "printf '%s' 'the edited prompt' > \"$1\"");
    const ScopedEnv visual("VISUAL", script.string());
    const ScopedEnv editor("EDITOR", script.string());

    const WorkspaceId workspace{"ws"};
    const SessionId   session{"s"};
    const std::unique_ptr<SupervisorHarness> harness = editor_harness(workspace, session);
    type_text(*harness, "original");

    EXPECT_TRUE(harness->dispatch_key("ctrl-e"));

    const SessionUiState* state = harness->model().session(session);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->input.draft, "the edited prompt");
    EXPECT_EQ(state->input.cursor, state->input.draft.size());
}

TEST(Errata46D13, UI46_D13_EditorFailureKeepsDraft) {
    const ShortTempRoot root("ymh-d13");
    const auto script = write_editor_script(root.path(), "editor.sh", "exit 3");
    const ScopedEnv visual("VISUAL", script.string());
    const ScopedEnv editor("EDITOR", script.string());

    const WorkspaceId workspace{"ws"};
    const SessionId   session{"s"};
    const std::unique_ptr<SupervisorHarness> harness = editor_harness(workspace, session);
    type_text(*harness, "keepme");

    EXPECT_TRUE(harness->dispatch_key("ctrl-e"));

    const SessionUiState* state = harness->model().session(session);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->input.draft, "keepme");
    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_NE(harness->model().notices.back().text.find("3"), std::string::npos);
}

TEST(Errata46D13, UI46_D13_EditorExit127KeepsDraft) {
    const ShortTempRoot root("ymh-d13");
    const std::string   missing = (root.path() / "does-not-exist-editor").string();
    const ScopedEnv     visual("VISUAL", missing);
    const ScopedEnv     editor("EDITOR", missing);

    const WorkspaceId workspace{"ws"};
    const SessionId   session{"s"};
    const std::unique_ptr<SupervisorHarness> harness = editor_harness(workspace, session);
    type_text(*harness, "keepme");

    EXPECT_TRUE(harness->dispatch_key("ctrl-e"));

    const SessionUiState* state = harness->model().session(session);
    ASSERT_NE(state, nullptr);
    EXPECT_EQ(state->input.draft, "keepme");
    ASSERT_FALSE(harness->model().notices.empty());
    EXPECT_NE(harness->model().notices.back().text.find("127"), std::string::npos);
}

TEST(Errata46D13, UI46_D13_EmptyDraft) {
    const ShortTempRoot root("ymh-d13");
    const auto          script = write_editor_script(root.path(), "editor.sh", "exit 0");
    const ScopedEnv     visual("VISUAL", script.string());
    const ScopedEnv     editor("EDITOR", script.string());

    const WorkspaceId workspace{"ws"};
    const SessionId   session{"s"};
    const std::unique_ptr<SupervisorHarness> harness = editor_harness(workspace, session);

    EXPECT_TRUE(harness->dispatch_key("ctrl-e"));

    const SessionUiState* state = harness->model().session(session);
    ASSERT_NE(state, nullptr);
    EXPECT_TRUE(state->input.draft.empty());
    EXPECT_FALSE(harness->model().message.open);
}

TEST(Errata46D13, UI46_D13_ScratchFileUnlinked) {
    const ShortTempRoot root("ymh-d13");
    const auto          record = root.path() / "scratch-path";
    const auto          script = write_editor_script(
        root.path(), "editor.sh",
        "printf '%s' \"$1\" > \"" + record.string() + "\"\nprintf '%s' 'edited' > \"$1\"");
    const ScopedEnv visual("VISUAL", script.string());
    const ScopedEnv editor("EDITOR", script.string());

    const WorkspaceId workspace{"ws"};
    const SessionId   session{"s"};
    const std::unique_ptr<SupervisorHarness> harness = editor_harness(workspace, session);

    EXPECT_TRUE(harness->dispatch_key("ctrl-e"));

    std::ifstream record_stream(record, std::ios::binary);
    ASSERT_TRUE(record_stream);
    std::string scratch;
    std::getline(record_stream, scratch);
    ASSERT_FALSE(scratch.empty());
    EXPECT_NE(scratch.find("ymh-edit-"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(scratch));
}

TEST(Errata46D13, UI46_D13_HintsRefreshedAfterEdit) {
    const ShortTempRoot root("ymh-d13");
    const auto          script = write_editor_script(root.path(), "editor.sh",
                                                     "printf '%s' '/status' > \"$1\"");
    const ScopedEnv     visual("VISUAL", script.string());
    const ScopedEnv     editor("EDITOR", script.string());

    const WorkspaceId workspace{"ws"};
    const SessionId   session{"s"};
    const std::unique_ptr<SupervisorHarness> harness = editor_harness(workspace, session);

    EXPECT_TRUE(harness->dispatch_key("ctrl-e"));

    const SessionUiState* state = harness->model().session(session);
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->command_hints.empty());
    EXPECT_EQ(state->command_hints.front().name, "status");
}

} // namespace
