#pragma once

// The `/handoff` durable command (82-handoff-command-errata.md, verified Rev 3).
// `/handoff` distills the active session into a six-section Markdown summary,
// writes it to a durable doc under the workspace, and (unless `--no-seed`)
// creates a stored-only `kind='root'` seed session whose first non-lifecycle
// event is a `context/injected` carrying the summary.
//
// The summarization call is long, so `HandoffService::run` executes on the
// daemon's per-session turn executor (44-I17); the TUI only forwards
// `command.invoke` and returns on `{outcome:"Queued"}` (82-D1/82-D2, H8).
//
// This errata adds no EventType: it reuses `command/run`+`command/done` on the
// source session and `context/injected` on the seed (82-D14, H11).

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/agent/compactor.hpp"  // WallClock
#include "ymh/agent/message.hpp"
#include "ymh/core/cancellation.hpp"
#include "ymh/core/event.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

class ExecutionEnvironment;
class LLMPool;
class ModelCatalog;
class SessionManager;

// 82-D1/D10: the parsed `/handoff` options and the bounded policy. The policy
// mirrors `CompactionPolicy` (include/ymh/agent/compactor.hpp:43-58) and is
// built from the layered `[handoff]` config (82-D10) by `to_handoff_policy`.
struct HandoffOptions {
    bool        seed  = true;  // --no-seed clears
    std::string out;           // --out <root-relative path>; empty => default
    std::string title;         // --title; empty => "handoff: <source title>"
};

struct HandoffPolicy {
    bool        enabled = true;
    std::string summarizer_model;          // empty => runtime default route
    std::size_t max_summary_tokens = 2048;
    std::size_t max_summary_bytes  = 256u * 1024u;
    std::size_t max_input_bytes    = 256u * 1024u;
    std::size_t keep_recent_turns  = 2;
};

// 82-D3 file cap: `## Relevant Files` / the digest file list never exceeds this.
inline constexpr std::size_t kMaxHandoffFiles = 64;

// 82-D5: resolves the handoff doc path under `env.root()`. When `requested` is
// empty the default is `<default_dir>/<session-id>-<YYYYMMDD-HHMMSS>.md` (UTC),
// the directory is created with mkdir -p semantics, and an existing default file
// gets a deterministic `-2`, `-3`, ... suffix. Throws
// `ToolError{ToolErrorCode::PathEscape}` when the path resolves outside the
// workspace root (H3), mirroring `resolve_export_path`.
// Caller: `HandoffService::run`; unit tests HS-U6/U7/U8.
[[nodiscard]] std::filesystem::path resolve_handoff_path(
    const ExecutionEnvironment& env,
    const std::string&          requested,
    const SessionId&            session,
    std::time_t                 utc_now,
    const std::filesystem::path& default_dir);  // <ws>/.ymh/handoffs

// 82-D3: the six-section summarizer prompt. Every section is present and
// ordered; empty sections must render "(none)". Unit-testable without an LLM.
// Caller: `HandoffService::run`; HS-U1.
[[nodiscard]] std::string_view handoff_instruction() noexcept;

// 82-D3/D6: the deterministic evidence digest (pure over the log). Same header +
// same log => byte-identical string (H14). Caller: `HandoffService::run`; HS-U2.
[[nodiscard]] std::string build_handoff_evidence(const SessionHeader& header,
                                                 const EventRange&    events);

// The result of a `/handoff` attempt. Never thrown for expected failures.
struct HandoffResult {
    enum class Outcome : std::uint8_t {
        Ok,
        Empty,
        Disabled,
        BadOption,
        NoRoute,
        ContextTooLarge,
        WriteFailed,
        StoreUnavailable,
        Cancelled,  // HF10: CancellationToken fired (cf. CompactionError::Code)
        Internal,
    };

    Outcome                  outcome = Outcome::Ok;
    std::string              detail;         // command/done text + notice
    std::string              doc_relative;   // workspace-relative (Ok)
    std::optional<SessionId> seed_session;   // present when seeded
};

// One instance per `WorkspaceRuntime`, shared; thread-affine to the turn
// executor that calls it (44-I17). Holds injected seams; no global state.
// Construction caller: `WorkspaceRuntime` (src/agent/workspace_runtime.cpp).
// `run()` caller: the `handoff` CommandSpec handler from make_handoff_command.
class HandoffService {
public:
    HandoffService(LlmRuntime&          runtime,
                   LLMPool&             pool,
                   SessionManager&      sessions,
                   ExecutionEnvironment& environment,
                   ModelCatalog*        catalog,
                   HandoffPolicy        policy,
                   WallClock            clock = std::chrono::system_clock::now);

    HandoffService(const HandoffService&) = delete;
    HandoffService& operator=(const HandoffService&) = delete;

    // Long-running: builds the digest, calls the summarizer, writes the doc, and
    // (unless options.seed) creates the stored-only root seed (sec 4.2).
    // Never throws for expected failures; returns them in HandoffResult::outcome.
    HandoffResult run(const Session&              source,
                      const std::vector<Message>& view,
                      const HandoffOptions&       options,
                      CancellationToken           cancel);

    [[nodiscard]] SessionManager& sessions() noexcept { return sessions_; }
    [[nodiscard]] const HandoffPolicy& policy() const noexcept { return policy_; }

private:
    LlmRuntime&           runtime_;
    LLMPool&              pool_;
    SessionManager&       sessions_;
    ExecutionEnvironment& environment_;
    ModelCatalog*         catalog_ = nullptr;
    HandoffPolicy         policy_;
    WallClock             clock_;
};

} // namespace ymh
