// 66-D2..66-D5/D6: hermetic bracketed-paste coverage. The supervisor cases
// replay the exact FTXUI event run a terminal produces for a bracketed paste
// (open marker, body, close marker) through the real `handle_event` via the
// additive `SupervisorHarness` seam; the terminal cases pin the DECSET/DECRST
// 2004 bytes.

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include <ftxui/component/event.hpp>
#include <ftxui/component/receiver.hpp>
#include <ftxui/component/task.hpp>

#include "ftxui/component/terminal_input_parser.hpp"

#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using namespace std::chrono_literals;

const char* kPasteOpen  = "\x1b[200~";
const char* kPasteClose = "\x1b[201~";

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata66-ui"},
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

std::unique_ptr<SupervisorHarness> make_harness() {
    SupervisorRunOptions options;
    options.identity = test_identity();
    return make_supervisor_harness(std::move(options));
}

const std::string& draft_of(const SupervisorHarness& harness, const SessionId& session) {
    return harness.model().session(session)->input.draft;
}

void dispatch_body(SupervisorHarness& harness, const std::string& text) {
    for (const char character : text) {
        if (character == '\n') {
            harness.dispatch_event(ftxui::Event::Return);
        } else {
            harness.dispatch_event(ftxui::Event::Character(std::string(1, character)));
        }
    }
}

TEST(Errata66Paste, BulkInsertPreservesNewlinesAndNeverSubmits) {
    const std::unique_ptr<SupervisorHarness> harness = make_harness();
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);

    EXPECT_TRUE(harness->dispatch_event(ftxui::Event::Special(kPasteOpen)));
    dispatch_body(*harness, "line1\nline2");

    // 66-D4: nothing reaches the draft until the close marker — one bulk insert.
    EXPECT_TRUE(draft_of(*harness, session).empty());

    EXPECT_TRUE(harness->dispatch_event(ftxui::Event::Special(kPasteClose)));
    EXPECT_EQ(draft_of(*harness, session), "line1\nline2");
    EXPECT_EQ(harness->model().session(session)->input.cursor, std::string("line1\nline2").size());
    // 66-I2/66-F2: the embedded newline is literal; it never submits.
    EXPECT_EQ(harness->submitted_count("agent.prompt"), 0u);
}

TEST(Errata66Paste, MultiLinePasteDoesNotSubmitMidWay) {
    const std::unique_ptr<SupervisorHarness> harness = make_harness();
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);

    harness->dispatch_event(ftxui::Event::Special(kPasteOpen));
    dispatch_body(*harness, "alpha\nbeta\ngamma");
    EXPECT_EQ(harness->submitted_count("agent.prompt"), 0u);

    harness->dispatch_event(ftxui::Event::Special(kPasteClose));
    EXPECT_EQ(harness->submitted_count("agent.prompt"), 0u);
    EXPECT_EQ(draft_of(*harness, session), "alpha\nbeta\ngamma");
}

TEST(Errata66Paste, PasteInsertsAtCaretAndIsOneEntry) {
    const std::unique_ptr<SupervisorHarness> harness = make_harness();
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);
    harness->dispatch_event(ftxui::Event::Character("A"));
    harness->dispatch_event(ftxui::Event::Character("B"));

    harness->dispatch_event(ftxui::Event::Special(kPasteOpen));
    dispatch_body(*harness, "x\ny");
    harness->dispatch_event(ftxui::Event::Special(kPasteClose));
    EXPECT_EQ(draft_of(*harness, session), "ABx\ny");
}

TEST(Errata66Paste, TruncatedPasteFlushesAndDoesNotSwallowLaterKeys) {
    const auto now = std::make_shared<std::chrono::steady_clock::time_point>();
    SupervisorRunOptions options;
    options.identity        = test_identity();
    options.monotonic_clock = [now] { return *now; };
    const std::unique_ptr<SupervisorHarness> harness =
        make_supervisor_harness(std::move(options));
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);

    harness->dispatch_event(ftxui::Event::Special(kPasteOpen));
    dispatch_body(*harness, "x");
    // No close marker arrives; an idle gap must flush the buffer and let the
    // next key through normally (66-F4).
    *now += 600ms;
    harness->dispatch_event(ftxui::Event::Character("y"));
    EXPECT_EQ(draft_of(*harness, session), "xy");
    EXPECT_EQ(harness->submitted_count("agent.prompt"), 0u);
}

TEST(Errata66Paste, PasteSurvivesModalClose) {
    const std::unique_ptr<SupervisorHarness> harness = make_harness();
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);

    harness->open_switcher();
    harness->dispatch_event(ftxui::Event::Special(kPasteOpen));
    dispatch_body(*harness, "abc");
    harness->dispatch_event(ftxui::Event::Special(kPasteClose));
    EXPECT_EQ(draft_of(*harness, session), "abc");

    // 66-F3 (M1): closing the modal restores the composer snapshot; the paste
    // must have been mirrored into that snapshot, not discarded.
    harness->dispatch_event(ftxui::Event::Escape);
    EXPECT_EQ(draft_of(*harness, session), "abc");
}

TEST(Errata66Paste, NestedOpenMarkerFlushesInsteadOfDropping) {
    const std::unique_ptr<SupervisorHarness> harness = make_harness();
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);

    harness->dispatch_event(ftxui::Event::Special(kPasteOpen));
    dispatch_body(*harness, "ab");
    harness->dispatch_event(ftxui::Event::Special(kPasteOpen));
    dispatch_body(*harness, "cd");
    harness->dispatch_event(ftxui::Event::Special(kPasteClose));
    EXPECT_EQ(draft_of(*harness, session), "abcd");
}

TEST(Errata66Paste, RawSequenceFromRealParserBecomesOneBulkInsert) {
    // 66-U7 (L2): feed the exact byte sequence a terminal emits to the *real*
    // FTXUI parser, then drive the resulting events through the real handler.
    // Guards the §3 root-cause mapping, not just a synthesised event list.
    const std::string raw = "\x1b[200~ab\ncd\x1b[201~";
    auto receiver         = ftxui::MakeReceiver<ftxui::Task>();
    {
        ftxui::TerminalInputParser parser(receiver->MakeSender());
        for (const char byte : raw) {
            parser.Add(byte);
        }
    }

    const std::unique_ptr<SupervisorHarness> harness = make_harness();
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);

    ftxui::Task task;
    while (receiver->Receive(&task)) {
        harness->dispatch_event(std::get<ftxui::Event>(task));
    }
    EXPECT_EQ(draft_of(*harness, session), "ab\ncd");
    EXPECT_EQ(harness->submitted_count("agent.prompt"), 0u);
}

TEST(Errata66Paste, ParserNormalizationOfLegacyFunctionKeysIsTracked) {
    // 66-I3 (R1): FTXUI's parser applies `g_uniformize` to every Special, so a
    // legacy F1 encoding inside a paste is delivered as `ESC OP` rather than
    // verbatim. The limitation is pinned here so it stays visible.
    const std::string raw = "\x1b[200~\x1b[11~\x1b[201~";
    auto receiver         = ftxui::MakeReceiver<ftxui::Task>();
    {
        ftxui::TerminalInputParser parser(receiver->MakeSender());
        for (const char byte : raw) {
            parser.Add(byte);
        }
    }

    const std::unique_ptr<SupervisorHarness> harness = make_harness();
    const WorkspaceId workspace{"ws-a"};
    const SessionId   session{"s1"};
    harness->seed_active_workspace(live_workspace(workspace, "alpha"));
    harness->activate_session(workspace, session);

    ftxui::Task task;
    while (receiver->Receive(&task)) {
        harness->dispatch_event(std::get<ftxui::Event>(task));
    }
    EXPECT_EQ(draft_of(*harness, session), "\x1bOP");
}

TEST(Errata66Paste, WriteBracketedPasteEmitsDecsetAndDecrst) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);

    ASSERT_TRUE(write_bracketed_paste(true, fds[1]));
    char buffer[64] = {};
    const ssize_t enable_bytes = ::read(fds[0], buffer, sizeof(buffer));
    ASSERT_GT(enable_bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(enable_bytes)),
              std::string(kBracketedPasteEnable));

    ASSERT_TRUE(write_bracketed_paste(false, fds[1]));
    const ssize_t disable_bytes = ::read(fds[0], buffer, sizeof(buffer));
    ASSERT_GT(disable_bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(disable_bytes)),
              std::string(kBracketedPasteDisable));

    EXPECT_FALSE(write_bracketed_paste(true, -1));

    ::close(fds[0]);
    ::close(fds[1]);
}

TEST(Errata66Paste, TerminalLayerRestoresDecrstOnDestruction) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT_GE(master, 0);
    ASSERT_EQ(::grantpt(master), 0);
    ASSERT_EQ(::unlockpt(master), 0);
    const char* slave_name = ::ptsname(master);
    ASSERT_NE(slave_name, nullptr);
    const int slave = ::open(slave_name, O_RDWR | O_NOCTTY);
    ASSERT_GE(slave, 0);

    const int saved_stdout = ::dup(STDOUT_FILENO);
    ASSERT_GE(saved_stdout, 0);
    ASSERT_EQ(::dup2(slave, STDOUT_FILENO), STDOUT_FILENO);
    {
        TerminalLayer layer(slave);
        EXPECT_TRUE(layer.enterBracketedPaste());
        EXPECT_TRUE(layer.bracketedPasteActive());
    }
    const int restored = ::dup2(saved_stdout, STDOUT_FILENO);
    ASSERT_EQ(restored, STDOUT_FILENO);
    ::close(saved_stdout);

    char buffer[64] = {};
    const ssize_t bytes = ::read(master, buffer, sizeof(buffer));
    ASSERT_GT(bytes, 0);
    EXPECT_EQ(std::string(buffer, static_cast<std::size_t>(bytes)),
              std::string(kBracketedPasteEnable) + std::string(kBracketedPasteDisable));

    ::close(slave);
    ::close(master);
}

} // namespace
