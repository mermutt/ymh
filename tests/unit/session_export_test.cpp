#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/core/event.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"
#include "ymh/ui/command_registry.hpp"
#include "ymh/ui/session_export.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;
using ymh::test::TempWorkspace;

std::chrono::system_clock::time_point at_ms(std::int64_t ms) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

ContentBlock text_block(ContentBlockKind kind, std::string text) {
    ContentBlock block;
    block.kind = kind;
    block.text = std::move(text);
    return block;
}

EventRecord record(Sequence seq, std::int64_t ms, EventType type, nlohmann::json payload) {
    Event event;
    event.id         = EventId{"event-" + std::to_string(seq)};
    event.session_id = SessionId{"s1"};
    event.timestamp  = at_ms(ms);
    event.type       = type;
    event.payload    = std::move(payload);
    return EventRecord{seq, std::move(event)};
}

SessionHeader make_header() {
    SessionHeader header;
    header.id            = SessionId{"11111111-1111-4111-8111-111111111111"};
    header.cwd           = "/workspace";
    header.createdAt     = 1700000000000;
    header.title         = "Fix login bug";
    header.model         = "deepseek-flash";
    header.serverProfile = "default";
    header.kind          = SessionKind::Root;
    return header;
}

EventRange make_events() {
    EventRange events;
    events.push_back(record(1, 1700000000000, EventType::SessionStarted,
                            payload::SessionStarted{"deepseek-flash", "default", "Fix login bug"}));
    events.push_back(record(2, 1700000000500, EventType::TurnStarted,
                            payload::TurnStarted{1, payload::TurnOrigin::User}));
    events.push_back(record(
        3, 1700000001000, EventType::UserMessage,
        payload::UserMessage{"m1", {text_block(ContentBlockKind::Text, "Fix the login bug")}}));
    events.push_back(record(
        4, 1700000002000, EventType::AssistantMessage,
        payload::AssistantMessage{
            "m2",
            {text_block(ContentBlockKind::Reasoning, "Check the auth path."),
             text_block(ContentBlockKind::Text, "I will inspect the file.")},
            std::nullopt,
            {},
            std::nullopt}));
    events.push_back(record(5, 1700000003000, EventType::ToolCall,
                            payload::ToolCall{"call-1", 1, 1, "read",
                                              {{"path", "src/auth.cpp"}},
                                              at_ms(1700000003000)}));
    events.push_back(record(
        6, 1700000004000, EventType::ToolResult,
        payload::ToolResult{"call-1", "read", payload::ToolOutcome::Ok, "int main() {}", false,
                            std::nullopt, std::chrono::milliseconds{10}}));
    events.push_back(record(7, 1700000005000, EventType::TurnEnded, payload::TurnEnded{1}));
    return events;
}

TEST(SessionExport, SanitizesTitleStem) {
    EXPECT_EQ(sanitize_export_stem("Fix the /etc/passwd bug!"), "Fix-the-etc-passwd-bug");
    EXPECT_EQ(sanitize_export_stem("  spaced  out  "), "spaced-out");
    EXPECT_EQ(sanitize_export_stem("***"), "");
    EXPECT_EQ(sanitize_export_stem(""), "");

    const std::string path_stem = sanitize_export_stem("../../etc/passwd");
    EXPECT_EQ(path_stem.find('/'), std::string::npos);
    EXPECT_EQ(path_stem.find(' '), std::string::npos);
}

TEST(SessionExport, CapsStemLength) {
    const std::string stem = sanitize_export_stem(std::string(200, 'a'));
    EXPECT_EQ(stem.size(), kMaxExportStem);
}

TEST(SessionExport, DefaultFilenameUsesUtcTimestamp) {
    // 1700000000 == 2023-11-14 22:13:20 UTC
    EXPECT_EQ(export_filename("Fix login", 1700000000), "Fix-login-20231114-221320.md");
    EXPECT_EQ(export_filename("", 1700000000), "session-20231114-221320.md");
    EXPECT_EQ(export_filename("///", 1700000000), "session-20231114-221320.md");
}

TEST(SessionExport, ResolvesPathUnderRoot) {
    TempWorkspace    workspace("export_resolve");
    LocalEnvironment env(workspace.path());

    EXPECT_EQ(resolve_export_path(env, "notes/out.md", "", 0),
              env.root() / "notes" / "out.md");
    EXPECT_EQ(resolve_export_path(env, "", "Fix login", 1700000000),
              env.root() / "Fix-login-20231114-221320.md");
}

TEST(SessionExport, RejectsPathEscape) {
    TempWorkspace    workspace("export_escape");
    LocalEnvironment env(workspace.path());

    try {
        (void)resolve_export_path(env, "../escape.md", "", 0);
        FAIL() << "expected PathEscape for a parent-relative path";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::PathEscape);
    }

    try {
        (void)resolve_export_path(env, "/etc/passwd", "", 0);
        FAIL() << "expected PathEscape for an absolute path outside the root";
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::PathEscape);
    }
}

TEST(SessionExport, RendersDeterministicMarkdown) {
    const std::string markdown = render_session_markdown(make_header(), make_events());
    const std::string expected = R"MD(# Fix login bug

- **Session**: `11111111-1111-4111-8111-111111111111`
- **Created**: 2023-11-14 22:13:20 UTC
- **Model**: `deepseek-flash`
- **Profile**: `default`
- **Workspace**: `/workspace`

## Turn 1

### User — 2023-11-14 22:13:21 UTC

Fix the login bug

### Assistant — 2023-11-14 22:13:22 UTC

<details>
<summary>Reasoning</summary>

Check the auth path.

</details>

I will inspect the file.

### Tool call: `read` — 2023-11-14 22:13:23 UTC

<details>
<summary>Arguments</summary>

```json
{
  "path": "src/auth.cpp"
}
```

</details>

<details>
<summary>Output (ok)</summary>

```
int main() {}
```

</details>

)MD";
    EXPECT_EQ(markdown, expected);
}

TEST(SessionExport, LlmRequestHeaderProducesNoRow) {
    const std::string baseline = render_session_markdown(make_header(), make_events());

    EventRange events = make_events();
    payload::LlmRequestHeader header;
    header.turn       = 1;
    header.step       = 1;
    header.session_id = SessionId{"s1"};
    events.push_back(record(99, 1700000009000, EventType::LlmRequestHeader, header));

    EXPECT_EQ(render_session_markdown(make_header(), events), baseline);
}

TEST(SessionExport, RegistryDispatchesExport) {
    const CommandRegistry registry = CommandRegistry::builtin();
    const Command*        command  = registry.find("export");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->description,
              "write the session transcript to a markdown file (--edit to open it)");

    const std::vector<const Command*> matches = registry.complete("exp");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches.front()->name, "export");

    UiModel        model;
    CommandContext context{model};
    std::string    received;
    context.export_session = [&received](const std::string& args) {
        received = args;
        return "exported to out.md";
    };
    EXPECT_TRUE(registry.dispatch("/export notes.md --edit", context));
    EXPECT_EQ(received, "notes.md --edit");
}

} // namespace
