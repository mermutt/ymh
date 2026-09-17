# 18 — Context Inspector Errata (`/context`: color grid + MCP/tools inventory)

```
Status: written · verified: — · reviewer: —
Component: 18 (errata) — additive to 10-supervisor-tui.md and 13-context-compaction.md
Depends on: 10-supervisor-tui.md (verified), 13-context-compaction.md (verified),
            05-transport.md + 11-m2-errata.md (frozen method catalog),
            15-mcp-adapter.md (MCP status), 16-daemon-ownership.md (ownership),
            01-session.md (deriveMessages), 06-agent-loop.md (assembly),
            04-workspace-host-daemon.md (WorkspaceRuntime), UI_SURFACE_INVENTORY.md
Scope: one read-only assembled-context snapshot + the `/context` overlay command
       on the live `ui::run_supervisor` path (RB-06)
```

## 1. Purpose, scope, and precedence

This errata adds a **read-only context inspector**: a daemon-produced snapshot of
the context the model is about to see, and a supervisor `/context` overlay that
renders it as a color grid plus an MCP/tools inventory. It is **additive**: it
pins one new snapshot type, one new RPC, one new overlay, new invariants, and
tests. It introduces **no change to context assembly, compaction policy, or the
event log** — the snapshot is a projection over existing state.

**Amended clauses (explicit).** This errata **extends**:

- `10-supervisor-tui.md` §8.1 (`:908-936`, render hierarchy — adds a
  `ContextOverlay` under `OverlayManager`), §8.2 (`:937-973`, renderers — adds
  `render_context_overlay`), §9.2 (`:1044-1063`, keybindings — overlay-local keys
  only, no new global binding), §4.1/§4.3 (`:326-417`, `UiModel`/`SessionUiState`
  — adds `UiModel::context`).
- `13-context-compaction.md` §6.7 (`:993-1143`, `/compact` UI — a sibling
  `/context` command; the compaction contract is untouched) and §6.8
  (`:1144-1293`, transport — an additive `context.show` method).

**Untouched (pinned).** Spec 10 §3 (threading/invalidation), §5 (event
adaptation), §6 (aggregate/flash), §7 (switcher), §10 (subagents), §11
(permission dialog); spec 13 §3 (policy), §4 (`ContextCompaction` event), §5
(interfaces), §8.2 (`C-F#`). Spec 16's ownership model is untouched: the snapshot
is per-daemon, per-session, and read-only.

**Precedence (pinned).** This errata wins over 10 and 13 for the `/context`
surface only. Where it meets 16 on cross-supervisor visibility, **16 wins**.
Spec 11's frozen catalog is **extended additively** (§3.4), never rewritten.

**Gate (per `AGENTS.md`).** Independent Oracle review must mark this `verified`
with no open HIGH/MEDIUM findings **before any code**. Nothing here is verified.

**Coexistence note.** Two concurrent errata touch the same shared surface and
have now landed: `19-session-rename-errata.md` adds `/rename` and the wire method
`session.rename` (commit `162fa8641`), and `20-skills.md` adds `/skills` +
`/skill` and `skills.list`/`skills.show` (commit `6761e62ef`). `/export` also
landed (`38c8a6214`) in the **UI-local** module `src/ui/session_export.*`
(`include/ymh/ui/session_export.hpp`) — it adds **no** wire method. All touch
the shared
`CommandRegistry`/`run_supervisor` surface (`src/ui/command_registry.cpp`,
`src/ui/supervisor.cpp`). This errata adds a **distinct** `CommandContext::context`
callback and a **distinct** `context` command; it does not touch
`export_session`, the UI-local `src/ui/session_export.*`, `rename_session`,
`skills`/`skill`, or
`UiMode`'s existing values. They are orthogonal and compose. The **wire-catalog
landing order** is a real dependency and is pinned in §3.4.1.

### 1.1 Honesty note — recon vs. code

Every `file:line` below was re-read in the working tree at HEAD `6761e62ef`
(2026-09-16). `docs/design/REQUIREMENTS_BACKLOG.md` RB-06 is stale and is **not**
trusted; the code is authoritative. Corrections:

- RB-06 cites `StatusModel` token fields at `ui_model.hpp:158-166`; they are at
  **`ui_model.hpp:172-180`**.
- RB-06 cites token rendering at `ui_render.cpp:262-269`; the actual render is
  **`ui_render.cpp:322-326`**.
- RB-06 cites `ui_event_adapter.cpp:147,211` for compaction; the actual
  `ContextCompaction` handling is `UiEventAdapter::adapt`'s
  `case EventType::ContextCompaction` at **`ui_event_adapter.cpp:146-151`**
  (UiEvent) and the maintenance fold at **`:210-220`**.
- RB-06 implies MCP status is directly available to the UI. In fact the UI holds
  only a **single bounded string**, `UiModel::mcp_status` (`ui_model.hpp:346`,
  set by `UiModel::setMcpStatus` at `ui_model.cpp:463-464`), fed by a host notice
  (`HostRuntime::mcp_status_detail`, `host_runtime.cpp:175-192`); there is **no
  per-server inventory** on the UI side and **no RPC** that returns one.
- RB-06 says "MCP server status + tools are enumerable: `statuses()`…". True of
  `McpManager` (`mcp_manager.hpp:56`, `mcp_manager.cpp:325-345`), but the daemon's
  `WorkspaceRuntime` exposes **no** accessor to it (verified: `mcp_` is private —
  constructed at `workspace_runtime.cpp:128`, declared at `:173`; no `mcp*`
  accessor in `workspace_runtime.hpp`).

The last two points are the core gaps this errata closes.

### 1.2 Citation audit (2026-09-16, HEAD `6761e62ef`, round-6 revision)

**Citation convention (durable, pinned).** A spec outlives the code it cites, so
a bare `file:line` anchor rots on the next commit — as this errata learned twice.
Every code reference in this document therefore pairs the **symbol** (function,
member, struct, or constant) with its file as the primary anchor; for the files
that moved in this round the symbol is written inline with the anchor (e.g.
`Session::append` in `src/session/session.cpp`, `HostRuntime::agentPrompt`,
`kMethodCatalog` in `src/transport/protocol.cpp`), and in the tables it is the
row's subject. A line number is always a **secondary hint**, qualified with the
commit it was read at (`as of 6761e62ef`); a reviewer re-resolves the symbol,
never the number. The `file:line` pairs in this errata were all re-derived under
this rule; a line number that cannot be re-resolved from its symbol is a defect.
Line-only anchors survive in exactly one place — references to another spec's own
sections (`10-supervisor-tui.md:937-973`), which move only when that spec moves.

**Audit basis (re-derived, not carried over).** Every anchor below was read from
the working tree at `6761e62ef` (2026-09-16) — *after* the two commits that moved
the files this errata had already audited:

- `162fa8641` (RB-03, session auto-name/rename) added lines to
  `src/session/session.cpp`, `include/ymh/session/session.hpp`,
  `src/host/host_runtime.cpp`, `include/ymh/host/host_runtime.hpp`,
  `src/ui/supervisor.cpp`, `src/transport/protocol.cpp`,
  `src/transport/protocol_server.cpp`, `src/ui/command_registry.cpp`,
  `include/ymh/ui/command_registry.hpp`, `src/ui/ui_model.cpp`,
  `src/ui/ui_event_adapter.cpp`, `include/ymh/ui/ui_event.hpp`, and
  `include/ymh/session/events.hpp`.
- `6761e62ef` (RB-05, `/skills`) added lines to
  `src/transport/protocol.cpp`, `src/transport/protocol_server.cpp`,
  `src/ui/supervisor.cpp`, `src/config/config.cpp`,
  `src/host/host_runtime.cpp`, `src/agent/workspace_runtime.cpp`,
  `include/ymh/agent/workspace_runtime.hpp`, `src/cli/wiring.cpp`, and
  `include/ymh/config/config.hpp`.

**Withdrawn claim (S18-R5-H6).** The earlier assertion that `src/session/`,
`src/transport/`, and `src/host/` were "unmodified and therefore stable" was
**false** and is retracted — **between them, the two commits touched all three**
(`162fa8641` touched `src/session/`, `src/transport/`, and `src/host/`;
`6761e62ef` touched `src/transport/` and `src/host/` but **not** `src/session/`).
The round-4 numbers for those files (and for `src/ui/supervisor.cpp`, `src/ui/command_registry.cpp`,
`src/agent/workspace_runtime.cpp`, and `src/cli/wiring.cpp`) were therefore
stale; all are re-derived below. For every other cited file
(`src/agent/agent_loop.cpp`, `src/agent/compactor.cpp`,
`src/agent/context_assembler.cpp`, `src/llm/*`, `src/mcp/*`, `src/tools/*`,
`src/core/event_bus.cpp`, `src/ui/ui_render.cpp`,
`include/ymh/agent/message.hpp`, `include/ymh/agent/compactor.hpp`,
`include/ymh/agent/agent.hpp`, `include/ymh/llm/*`, `include/ymh/mcp/*`,
`include/ymh/ui/ui_model.hpp`, `include/ymh/ui/theme.hpp`),
`git diff --name-only 38c8a6214 6761e62ef` proves **no change**, so the round-4
anchors are valid and are carried forward unchanged. Anchors were obtained with
`grep -n`/`sed -n` on the tree, never from memory or from
`REQUIREMENTS_BACKLOG.md`.

**Corrections retained from prior rounds (re-anchored).** The substantive
clarifications from rounds 3–4 survive; only their numbers changed:

- **Spec-19 cross-reference.** The catalog landing rule lives in spec 19 **§5.4**
  (`### 5.4 Wire method (RN-A9)`, heading `19-session-rename-errata.md:839`),
  operative text `:847-856` (the `kMethodCatalog` landing-order paragraph). Both
  references (§3.4.1 and here) name §5.4 and that span. Spec 19 is unchanged by
  the two commits, so its own line numbers still hold.
- **Fallback row arithmetic.** The overlay chrome is pinned to exactly
  `kContextOverlayChromeRows = 10` rows in every mode and view — border (2) +
  title/status (1) + body (6) + bottom line (1). The **body** is the six legend
  rows at `view == 0` and a six-row inventory viewport at `view == 1` (M1, §5.4);
  the **bottom line** is the `note` when non-empty, else the totals row in grid
  mode or the fallback hint in fallback mode (§5.1 CX-D12), and no separator/rule
  row is ever rendered.
- **`workspace_runtime.cpp` estimator.** The `DefaultTokenEstimator` **member**
  is `estimator_` at `:179`; `:146` is the `services_.estimator = &estimator_;`
  wiring. Both §3.4 and §3.5 now read "member `:179`, wired `:146`".
- **MCP lock entry point.** The additive mutex is `McpManager`'s private state;
  the acquiring read entry point is `McpManager::statuses()`
  (`mcp_manager.hpp:56`, impl `mcp_manager.cpp:325-345`). The additive
  `WorkspaceRuntime::mcp_statuses()` is a different class and only **forwards**
  to it, holding no lock itself (§3.5 C8, CTX-F6).
- **Small-terminal golden size.** The §10.2 renderable fallback golden uses
  `TerminalSize{52, 10}` (the geometry probe still uses `{32, 8}`).

---

## 2. Verified current state (what already exists)

### 2.1 Token usage

| Fact | Anchor |
|---|---|
| Canonical usage value type `Usage{input_tokens, output_tokens, cached_tokens, reasoning_tokens}` | `include/ymh/agent/message.hpp:131-138` |
| Durable carrier `payload::TokenUsage{Usage, optional<TurnId>}` | `include/ymh/session/events.hpp:167-170` |
| Appended by the loop (main turn) | `src/agent/agent_loop.cpp:702`, `:719`; compaction usage `:306-307` |
| UI event `TokenUsageUpdated{SessionId, Usage}` | `include/ymh/ui/ui_event.hpp:166-169` |
| Adapter maps `EventType::TokenUsage` → `TokenUsageUpdated` | `src/ui/ui_event_adapter.cpp:125-128` |
| Applied to `StatusModel` | `src/ui/ui_model.cpp:649-653`; also `:538-542` |
| Rendered (`↑in ↓out ⚡cached`) | `src/ui/ui_render.cpp:322-326` |
| `StatusModel` fields | `include/ymh/ui/ui_model.hpp:172-180` |

**Consequence.** The UI already has provider-reported usage for the *last* call.
It does **not** have a per-segment breakdown, the system-prompt size, the tool
schema size, or the context budget.

### 2.2 Compaction boundary

| Fact | Anchor |
|---|---|
| `payload::ContextCompaction{boundary, summary, tokenEstimate, model, createdAt}` | `include/ymh/session/events.hpp:159-165` |
| Projection fold: drop messages with origin `<= boundary`, prepend the summary as a `Role::System` message | `deriveMessages`' `case EventType::ContextCompaction` at `src/session/session.cpp:463-478` |
| `tokenEstimate = estimator.estimate(compacted)` (summary + kept messages) | `src/agent/compactor.cpp:285-297` |
| Compaction trigger (`estimate > threshold`) | `src/agent/agent_loop.cpp:563-579` |
| `CompactionPolicy{threshold_tokens, threshold_ratio, context_window_tokens, reserve_output_tokens, …}` | `include/ymh/agent/compactor.hpp:30-58` |
| `effective_threshold_tokens()` | `include/ymh/agent/compactor.hpp:48-57` |
| `ContextCompactor::policy()` accessor | `include/ymh/agent/compactor.hpp:127` |
| UI event `CompactionMarker{boundary, tokenEstimate, model, summary}` | `include/ymh/ui/ui_event.hpp:176-182` |
| Adapter maps it | `src/ui/ui_event_adapter.cpp:146-151` |
| Applied as a system transcript entry | `src/ui/ui_model.cpp:657-664` |
| Provider capability field `max_context_tokens` (optional) | `include/ymh/llm/llm_provider.hpp:36` |
| The OpenAI-compatible provider **does not set it** (`nullopt` ⇒ unknown) | `src/llm/provider_registry.cpp:90-99` |

**Consequence.** The UI knows the *last* compaction boundary and estimate, but the
supervisor never sees the boundary's effect on the current message list; only the
daemon can project it. The **only** budget source in the shipped binary is
`CompactionSettings::context_window_tokens` (`config.hpp:48`, default `0`);
`ProviderCapabilities::max_context_tokens` is never populated by the built-in
provider (`provider_registry.cpp:90-99`), so an unconfigured session has an
**unknown** budget — which CTX6/CTX-F3 require the UI to state honestly.

### 2.3 MCP statuses and tools

| Fact | Anchor |
|---|---|
| `McpServerStatus{id, state, server_name, server_version, protocol_version, tool_count, skipped_tools, last_error}` | `include/ymh/mcp/mcp_types.hpp:125-134` |
| `McpServerState{Disabled, Starting, Ready, Degraded, Disconnected, Failed, Stopped}` | `include/ymh/mcp/mcp_types.hpp:41-49` |
| `McpManager::statuses()` (public, sorted by id) | `include/ymh/mcp/mcp_manager.hpp:56`; impl `src/mcp/mcp_manager.cpp:325-345` |
| MCP tools registered into the **shared** `ToolRegistry` under `mcp.<server>.` | `src/mcp/mcp_manager.cpp:212` (`openAdapter("mcp." + id + ".")`) |
| MCP tool names are `mcp.<server>.<sanitized>` | `src/mcp/schema_translation.cpp:240` |
| `ToolRegistry::names()` / `schemas()` (mutex-guarded) | `src/tools/tool_registry.cpp:291`, `:303` |
| MCP status is **live-only**, never appended (empty `SessionId`, bus publish only) | `src/mcp/mcp_manager.cpp:125-139`; ignored by the projection (`deriveMessages`, `case EventType::McpServerStatusChanged`, `src/session/session.cpp:379`) |
| Bounded single-line status token sent to the UI | `HostRuntime::mcp_status_detail` `src/host/host_runtime.cpp:175-192`; `HostNoticeKind::McpServerStatus` `protocol.hpp:235`; `ProtocolServer::onMcpServerStatus` `protocol_server.cpp:859-871` |
| UI stores only that last string | `include/ymh/ui/ui_model.hpp:346`; `src/ui/ui_model.cpp:463-464` |

**Consequence (pinned).** MCP tools are **already in the tool schemas the model
sees**: `McpManager` registers them into the same `ToolRegistry`
(`mcp_manager.cpp:212`) that `SessionContextAssembler::tools()` returns
(`context_assembler.cpp:52-54`), so they are part of `LLMRequest.tools`
(`agent_loop.cpp:328-330`). There is **no separate MCP tool list on the wire**.
Provenance is derivable only from the `mcp.` name prefix. Per-server *state* is
**not** reachable from the supervisor and is not in the durable log.

### 2.4 The context-assembly path (the thing to inspect)

| Fact | Anchor |
|---|---|
| System prompt default | `src/cli/wiring.cpp:48-52`; effective assignment `to_agent_config` `:172-173` |
| `AgentConfig.system_prompt` | `include/ymh/agent/agent.hpp:103` |
| Assembler built with that exact prompt | `src/agent/workspace_runtime.cpp:109` (`assembler_(tools_, agent_config_.system_prompt)`) |
| `assemble()` = `session.deriveMessages()` + prepend system message | `src/agent/context_assembler.cpp:36-50` |
| `tools()` = `tools_.schemas()` | `src/agent/context_assembler.cpp:52-54` |
| `deriveMessages(header, events)` is a **pure free function** | `include/ymh/session/session.hpp:183`; `src/session/session.cpp:362-487` |
| `Session::deriveMessages()` delegates to it | `src/session/session.cpp:551-553` |
| `LLMRequest{model, messages, tools, parameters}` | `include/ymh/llm/llm_request.hpp:54-61` |
| Request built from assembler output | `src/agent/agent_loop.cpp:324-333`, `:557`, `:581` |
| Estimator: `bytes/4 + 4·messages` (messages only) | `src/agent/context_assembler.cpp:11-31` |
| Tool wire form: `{type:"function", function:{name, description, parameters}}` | `src/llm/openai_adapter.cpp:714-724` |

### 2.5 What does **not** exist (the gaps this errata closes)

1. No `/context` command. `CommandRegistry::builtin()`
   (`src/ui/command_registry.cpp:117-229`) registers `new`, `clear`, `model`,
   `compact`, `export`, `exit`, `help`, plus the concurrent `/rename` (spec 19)
   and `/skills` + `/skill` (spec 20/RB-05); none opens a context view.
2. No snapshot type, no per-segment token accounting, no tool-schema accounting.
3. No RPC to fetch either: the frozen catalog has 32 methods today
   (`kMethodCatalog`, `src/transport/protocol.cpp:612-625`; the landing size is
   relative, §3.4.1) and none returns context or MCP status.
4. No `WorkspaceRuntime` accessor for the compactor policy, the estimator, or the
   MCP manager (verified: the accessor block `include/ymh/agent/workspace_runtime.hpp:124-149`
   has `context()`/`agent_config()`/`skills()` but no `estimator()`,
   `compaction_policy()`, or `mcp_statuses()`).
5. `Session` is **not** thread-safe for concurrent reads: only
   `Session::appendEvent` takes `appendMutex_` (`src/session/session.cpp:555-557`),
   while `Session::events()`/`Session::deriveMessages()` read `log_` unlocked
   (`:536-553`). The RPC thread must never touch a live `Session`.

---

## 3. The assembled-context snapshot

### 3.1 Where it is produced, and why daemon-side

The snapshot is produced **in the daemon** by a **pure builder** fed from
`WorkspaceRuntime`, and crosses to the supervisor as one additive RPC.

Daemon-side is **correct** because the truth is daemon-local:

- the effective system prompt is `WorkspaceRuntime::agent_config().system_prompt`
  (accessor `workspace_runtime.cpp:300`; wired into the assembler at `:109`),
  which the supervisor never receives;
- the tool schemas are `ContextAssembler::tools()` (`context_assembler.cpp:52-54`),
  which the supervisor never receives;
- the MCP manager is daemon-owned (`mcp_` constructed at
  `workspace_runtime.cpp:128`, declared `:173`), and MCP status is live-only
  (`mcp_manager.cpp:125-139`);
- the resolved message list is a projection over the store the daemon owns
  (`deriveMessages`, `session.cpp:362-487`).

Supervisor-side reconstruction would require shipping the system prompt, schemas,
and message bodies to the supervisor — which §3.6/CTX8 forbid — and would still
be a *second* assembly path that can drift from `assemble()`. Producing it
daemon-side guarantees the snapshot is computed from the same inputs as the real
request.

### 3.2 C++ interface sketch (agent layer, new)

```cpp
// include/ymh/agent/context_snapshot.hpp  (NEW — 18 §3)
#pragma once

// Read-only assembled-context inspection (18-context-errata.md). Pure: it takes
// a resolved session view (header + events), the effective system prompt, the
// frozen tool schemas, and an MCP status snapshot, and returns advisory token
// estimates per segment. It never reads a live `Session`, never appends, never
// logs content, and never calls a provider.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/compactor.hpp"          // CompactionPolicy
#include "ymh/agent/context_assembler.hpp"  // TokenEstimator
#include "ymh/agent/message.hpp"            // Message, Role
#include "ymh/mcp/mcp_types.hpp"            // McpServerStatus
#include "ymh/session/session.hpp"          // SessionHeader, EventRange
#include "ymh/tools/tool.hpp"               // ToolSchema

namespace ymh {

// Fixed order; `FreeSpace` is always last (18 §5.2).
enum class ContextSegmentKind : std::uint8_t {
    SystemPrompt,
    ToolSchemas,
    McpToolSchemas,
    Conversation,
    CompactionSummary,
    FreeSpace,
};

[[nodiscard]] std::string_view context_segment_token(ContextSegmentKind kind) noexcept;
[[nodiscard]] std::optional<ContextSegmentKind> parse_context_segment(
    std::string_view token) noexcept;

struct ContextSegment {
    ContextSegmentKind kind = ContextSegmentKind::Conversation;
    std::string        provenance;   // human-readable origin label
    std::uint64_t      tokens = 0;   // advisory estimate
    std::size_t        items = 0;    // messages / schemas / summaries
};

struct ContextToolEntry {
    std::string   name;              // e.g. "read_file" or "mcp.alpha.search"
    std::string   provenance;        // "builtin" | "mcp"
    std::uint64_t schema_tokens = 0; // advisory estimate
};

struct ContextServerEntry {
    std::string id;
    std::string state;               // mcp_state_token()
    std::size_t tool_count = 0;
    std::size_t skipped = 0;
    // M8: the raw `McpServerStatus::last_error` is NEVER shipped (unbounded
    // std::exception::what(); see §6.2/CTX8). Only this bool is.
    bool        has_error = false;
};

struct ContextBudget {
    std::uint64_t window_tokens = 0;              // 0 => unknown
    std::uint64_t reserve_output_tokens = 0;
    std::uint64_t effective_threshold_tokens = 0;
};

// All references must outlive the call. Callers bind vectors to named locals
// first (never to temporaries — see 18 §3.3 note).
struct ContextSnapshotInputs {
    const SessionHeader&                header;
    const EventRange&                   events;         // resolved view (store read)
    std::string_view                    system_prompt;  // effective, may be empty
    const std::vector<ToolSchema>&      tools;          // frozen schemas
    const std::vector<McpServerStatus>& mcp_servers;
    const TokenEstimator&               estimator;
    ContextBudget                       budget;
    std::size_t                         max_tools = 256;
    // False when the daemon could not read MCP status (CTX-F6): the builder
    // omits the server section and records the degradation in `note`.
    bool                                mcp_available = true;
};

struct ContextSnapshot {
    SessionId                       session;         // = header.id (wire `session`)
    std::uint64_t                   used_tokens = 0;
    ContextBudget                   budget;
    std::vector<ContextSegment>     segments;        // fixed order, FreeSpace last
    std::vector<ContextToolEntry>   tools;
    std::vector<ContextServerEntry> mcp_servers;
    std::uint64_t                   captured_sequence = 0;  // last resolved seq
    bool                            truncated = false;
    std::string                     note;            // e.g. "budget unknown"
};

// Pure. `events` is the resolved session view; `header` its SessionHeader.
[[nodiscard]] ContextSnapshot build_context_snapshot(const ContextSnapshotInputs& inputs);

// Advisory size of one schema in the OpenAI-compatible wire form
// (openai_adapter.cpp:714-724): name + description + parameters. Not noexcept:
// `nlohmann::json::dump()` may throw on invalid UTF-8.
[[nodiscard]] std::uint64_t estimate_tool_schema_tokens(const ToolSchema& schema);

void to_json(nlohmann::json& json, const ContextSnapshot& snapshot);
void from_json(const nlohmann::json& json, ContextSnapshot& snapshot);

} // namespace ymh
```

### 3.3 Segment classification and token accounting (normative)

Let `E` be the estimator, `M = deriveMessages(header, events)` the same pure
function the assembler uses (`session.cpp:362-487`), and `S` the summary of the
**last** `ContextCompaction` event in `events` (the fold in `session.cpp:463-478`
keeps only the latest summary; boundaries advance monotonically per 13 §3.6).
`system_message(x)` denotes a `Role::System` message with one `Text` block built
exactly as `SessionContextAssembler::assemble` builds it
(`context_assembler.cpp:40-48`).

0. `snapshot.session = header.id`.
1. **SystemPrompt.** `tokens = system_prompt.empty() ? 0 : E({system_message(system_prompt)})`;
   `items = system_prompt.empty() ? 0 : 1`. `provenance = "agent.system_prompt"`.
   (An empty prompt contributes **zero** tokens and zero items — it is not a
   present-but-empty message.)
2. **ToolSchemas.** All `tools` whose `name.value` does **not** start with
   `"mcp."`; `tokens = Σ estimate_tool_schema_tokens(schema)`;
   `items = count`. `provenance = "ToolRegistry::schemas()"`.
3. **McpToolSchemas.** All `tools` whose `name.value` **does** start with
   `"mcp."`; same accounting. `provenance = "ToolRegistry::schemas() (mcp.)"`.
4. **CompactionSummary.** `tokens = S ? E({system_message(S)}) : 0`;
   `items = S ? 1 : 0`. `provenance = "payload::ContextCompaction.summary"`.
5. **Conversation.** The compaction fold inserts the surviving summary at index
   **0** of the post-boundary list (`session.cpp:475-476`), and later events only
   append, so the summary is always `M[0]` when `S` exists. Therefore:
   `tokens = S ? E(M[1..]) : E(M)`, `items = S ? M.size()−1 : M.size()`.
   `provenance = "deriveMessages(header, log)"`.
   **Do not compute this by subtracting the summary estimate from `E(M)`**:
   `DefaultTokenEstimator` is `floor(bytes/4) + 4·count`
   (`context_assembler.cpp:27-31`), and integer floor division is not
   distributive, so the subtraction can differ by up to one token. Estimating the
   tail directly is exact.
6. `used_tokens = Σ segments[0..4].tokens` (SystemPrompt + ToolSchemas +
   McpToolSchemas + Conversation + CompactionSummary).
7. **FreeSpace.** `tokens = budget.window_tokens > used_tokens ? budget.window_tokens − used_tokens : 0`;
   `items = 0`. `provenance = "window − used"`.
8. `captured_sequence = events.empty() ? 0 : events.back().seq`.
8a. **`tools` list (L2).** For every schema in `inputs.tools` — the frozen,
    name-sorted `ToolRegistry::schemas()` output (verified: `schemas()`
    `std::sort`s by `ToolName`, `tool_registry.cpp`) — append
    `ContextToolEntry{ name = schema.name.value, provenance =
    schema.name.value.starts_with("mcp.") ? "mcp" : "builtin", schema_tokens =
    estimate_tool_schema_tokens(schema) }`. The list is built **full** here (all
    schemas, MCP included) and is truncated to the first `max_tools` by step 10.
    `provenance` is derived **only** from the `mcp.` prefix (CTX5) — never from a
    second registry — so the list and the `ToolSchemas`/`McpToolSchemas`
    partition (steps 2–3) can never disagree. This is the list the inventory
    renders (§6.3).
8b. **`mcp_servers` list (L2).** If `inputs.mcp_available`, for every
    `McpServerStatus` in `inputs.mcp_servers` (already id-sorted by
    `McpManager::statuses()`, `mcp_manager.cpp:325-345`) append
    `ContextServerEntry{ id = status.id.value, state =
    std::string{mcp_state_token(status.state)}, tool_count = status.tool_count,
    skipped = status.skipped_tools.size(), has_error =
    !status.last_error.empty() }`. If `!inputs.mcp_available`, the list stays
    **empty** and step 11 records the degradation. The `last_error` **text** is
    never copied — only the derived `has_error` bool is (M8, CTX8). This is the
    list the inventory renders (§6.2).
9. If `budget.window_tokens == 0`, append `"budget unknown"` to `note` and
   `FreeSpace.tokens = 0` (CTX6).
10. If `tools.size() > max_tools`, the `tools` **list** keeps the first
    `max_tools` entries (stable order from `ToolRegistry::schemas()`),
    `truncated = true`, and `"tools truncated"` is appended to `note` (CTX7).
    **Truncation affects the `tools` list only.** `ToolSchemas.items` and
    `McpToolSchemas.items` remain the **full** counts and their `tokens` remain
    the full sums, so the segments always reflect the true totals even when the
    per-tool list is cut (L8).
11. If `!inputs.mcp_available`, `snapshot.mcp_servers` is empty and
    `"mcp status unavailable"` is appended to `note`; the rest of the snapshot is
    unaffected (M7, CTX-F6).

**`note` join rule (pinned, C7).** `note` starts as the empty string. Every
fragment is joined to the existing text with the exact separator `"; "`
(semicolon + one space); there is **no** leading, trailing, or doubled
separator. Steps 9→11 run in the order written, so the maximal note is
`"budget unknown; tools truncated; mcp status unavailable"`. `append_note` is the
single writer — no other code path assigns `note`:

```cpp
void append_note(std::string& note, std::string_view fragment) {
    if (!note.empty()) {
        note += "; ";
    }
    note.append(fragment);
}
```

Steps 9, 10, and 11 call `append_note(note, …)`. The wire carries the joined
string verbatim, the overlay renders it verbatim in its pinned bottom-line slot
(§5.1 CX-D12), and it is never re-parsed or split.

**`estimate_tool_schema_tokens` (pinned formula).** The function is:

```cpp
std::uint64_t estimate_tool_schema_tokens(const ToolSchema& schema) {
    constexpr std::size_t kBytesPerToken = 4;   // same divisor as DefaultTokenEstimator
    constexpr std::size_t kSchemaOverhead = 8;  // the {"type":"function","function":{…}} wrapper keys
    const std::size_t bytes = schema.name.value.size()
                            + schema.description.size()
                            + schema.input_schema.dump().size();
    return static_cast<std::uint64_t>(bytes / kBytesPerToken + kSchemaOverhead);
}
```

The byte source mirrors the OpenAI-compatible wire form
(`openai_adapter.cpp:714-724`: `name` + `description` + `parameters`). Concrete
fixture: `ToolSchema{name="read_file", description="Read a file",
input_schema={"type":"object","properties":{"path":{"type":"string"}},"required":["path"]}}`
→ `dump()` is 77 bytes, so `(9 + 11 + 77)/4 + 8 = 97/4 + 8 = 24 + 8 = 32` tokens.
The worked example in §5.6 takes 3,980 (builtin) and 2,240 (MCP) as **snapshot
inputs** — they are produced by summing this formula over the fixture schemas,
not by the renderer; the golden pins the snapshot directly.

**Note (verified gap, not fixed here).** The loop's compaction threshold uses
`E(messages)` (`agent_loop.cpp:564`), which **includes** the assembler's system
prompt but **excludes** tool schemas. `/context` therefore shows the schemas the
threshold ignores. This errata does **not** change the threshold (spec 13 §3.4
owns it); it makes the gap visible.

### 3.4 Crossing the supervisor/daemon boundary (additive RPC)

```text
session.compact (13 §6.8, frozen)      context.show (this errata, additive)
Interactive only                       Interactive + Automation (read-only)
```

**Wire contract.**

```text
context.show   params: { "session": "<uuid>" }
               result: { session, captured_sequence, used_tokens,
                         budget: { window_tokens, reserve_output_tokens,
                                   effective_threshold_tokens },
                         segments: [ { kind, provenance, tokens, items } ],
                         tools:    [ { name, provenance, schema_tokens } ],
                         mcp_servers: [ { id, state, tool_count, skipped,
                                          has_error } ],
                         truncated, note }
```

`kind` uses `context_segment_token` (`system_prompt`, `tool_schemas`,
`mcp_tool_schemas`, `conversation`, `compaction_summary`, `free_space`). `state`
uses `mcp_state_token` (`mcp_types.hpp:56`).

**Additive changes (pinned).**

- `include/ymh/transport/protocol.hpp`: add
  `inline constexpr std::string_view kContextShow = "context.show";` to the
  `method` namespace (`:476-509`, as of `6761e62ef`) and add it to the catalog.
- `src/transport/protocol.cpp`: **this errata adds exactly one entry to
  `kMethodCatalog`; the catalog size at landing is `C + 1`, where `C` is the size
  immediately before it lands.** At the audit baseline the catalog is
  `std::array<std::string_view, 32>` (`:612-625`, as of `6761e62ef`; the count
  includes the already-landed `session.rename`, `skills.list`, and `skills.show`)
  and the test pins `32u` (`tests/unit/transport_protocol_test.cpp:41`), so the
  landing total is `33` if nothing else lands first. The implementer must
  **re-count `kMethodCatalog` at landing** and set the array's size template
  argument and the test assertion to `C + 1` — never a hard-coded number. §3.4.1
  pins the relative rule. `is_method_allowed` (`:642-648`) already permits
  `context.show` in both profiles because it is **read-only** (do **not** add it
  to the deny list).
- `include/ymh/transport/host.hpp`: add
  `virtual nlohmann::json showContext(const SessionId& id) = 0;` (a pure virtual;
  `tests/support/fake_transport_host.hpp` and `HostRuntime` gain an override).
  **Rejected:** a typed `protocol::ContextSnapshot` DTO — it would either
  duplicate the agent snapshot or force `transport → agent` coupling; JSON is
  already a transport payload (`SessionDetail.header`, `protocol.hpp:366`;
  `HostNotice.detail`), and the supervisor already parses JSON replies
  (`refresh_sessions` reply lambda, `supervisor.cpp:755-764`).
- `src/transport/protocol_server.cpp`: add a `kContextShow` branch next to
  `kSessionShow` (`:393-394`) that responds with `host_.showContext(session)`.

#### 3.4.1 Catalog landing order (pinned)

`kMethodCatalog` grows whenever any additive-method errata lands (spec 18
`context.show`, spec 19 `session.rename`, spec 20 `skills.list`/`skills.show`),
so an absolute size is never stable and this errata does **not** pin one. It pins
a **relative** rule:

- At the audit baseline the catalog is `std::array<std::string_view, 32>`
  (`kMethodCatalog`, `src/transport/protocol.cpp:612-625`, as of `6761e62ef`)
  and `tests/unit/transport_protocol_test.cpp:41` pins `32u`. Let `C` be the
  catalog size **immediately before this errata lands** — measured by re-counting
  `kMethodCatalog` at landing, never assumed from here.
- **This errata adds exactly one entry; the catalog size at landing is `C + 1`.**
  At the baseline `C = 32`, so the landing total is `33` — but `C` is not fixed:
  any other additive-method errata that lands first raises it.
- **Landing-order dependency (explicit).** Because the total depends on order,
  each errata computes its own landing total from the live catalog: set the
  `std::array` size template argument and the
  `EXPECT_EQ(protocol::all_methods().size(), …u)` assertion to `C + 1`, where `C`
  is the count immediately before that errata's own commit. Neither may hard-code
  a number. Spec 19's §5.4 (RN-A9) (`19-session-rename-errata.md:839-856`, the
  `kMethodCatalog` landing-order paragraph, which cites this §3.4.1 in return)
  already adopted the same relative principle; this section restates it in terms
  of the measured size `C` so it survives any number of intervening errata.
- No additive-method errata may renumber or reorder existing entries; each only
  appends its own constant at the end of the `method` namespace
  (`protocol.hpp:476-509`) and its own catalog entry.
- **No method name may collide**: `context.show`, `session.rename`,
  `skills.list`, and `skills.show` are distinct; an errata adding any existing
  name is a duplicate.

**Daemon adapter (sketch).** `HostRuntime` already reads the store on the
dispatch thread (`HostRuntime::showSession`, `host_runtime.cpp:470-491`;
`HostRuntime::readEvents`, `:836-847`), so this is the established threading
contract.

```cpp
// src/host/host_runtime.cpp (additive; sketch). `translate`/`throw_mapped`/
// `WireError` are the file's existing error-mapping helpers (host_runtime.hpp:84,
// :195); the body is wrapped in `translate(...)` and the unknown-session code
// mirrors `showSession` (host_runtime.cpp:473-475) exactly (L5).
nlohmann::json HostRuntime::showContext(const SessionId& id) {
    return translate([&]() -> nlohmann::json {
        const std::optional<SessionHeader> header = runtime_.store().load(id);
        if (!header.has_value()) {
            throw_mapped(WireError{protocol::code_value(protocol::AppCode::UnknownSession),
                                   "UnknownSession"});
        }
        const EventRange events = runtime_.store().read(id, 0);

        // Bind to named locals: ContextSnapshotInputs holds references, and a
        // temporary would dangle before build_context_snapshot() runs.
        const std::vector<ToolSchema> tools = runtime_.context().tools();
        const CompactionPolicy*       policy = runtime_.compaction_policy();

        // MCP status is best-effort (CTX-F6): a failure degrades to an empty
        // server section plus a note, never to a failed snapshot (M7).
        std::vector<McpServerStatus> servers;
        bool mcp_available = true;
        try {
            servers = runtime_.mcp_statuses();
        } catch (const std::exception&) {
            mcp_available = false;
        }

        const ContextBudget budget{
            policy != nullptr ? policy->context_window_tokens : 0,
            policy != nullptr ? policy->reserve_output_tokens : 0,
            policy != nullptr ? policy->effective_threshold_tokens() : 0,
        };
        const ContextSnapshotInputs inputs{*header, events,
                                           runtime_.agent_config().system_prompt,
                                           tools, servers, runtime_.estimator(), budget,
                                           256, mcp_available};
        const ContextSnapshot snapshot = build_context_snapshot(inputs);
        nlohmann::json out;
        to_json(out, snapshot);
        return out;
    });
}
```

**Additive `WorkspaceRuntime` accessors (pinned).**

```cpp
// include/ymh/agent/workspace_runtime.hpp (additive)
[[nodiscard]] const TokenEstimator&       estimator() const noexcept;
[[nodiscard]] const CompactionPolicy*     compaction_policy() const noexcept; // null if no provider
[[nodiscard]] std::vector<McpServerStatus> mcp_statuses() const;
```

These are the minimum needed to reach the already-owned objects
(`estimator_` **member** at `workspace_runtime.cpp:179`, wired into
`AgentServices` at `:146`; compactor at `:153-155`; MCP manager at `:128`/`:173`)
without exposing the confined `Session`.

### 3.5 Threading (normative)

- The snapshot is built on the daemon's **dispatch/io thread** from
  `SessionStore` reads only. It **never** calls `Session::deriveMessages()`
  (`session.hpp:207`) or `Session::events()` (`:203`) off the executor thread:
  those read `log_` unlocked (`session.cpp:536-553`) while `Session::appendEvent`
  mutates it under `appendMutex_` (`:555-557`).
- `ToolRegistry::names()/schemas()` are mutex-guarded
  (`tool_registry.cpp:291,303`) and safe on any thread.
- `estimator()` returns the stateless `DefaultTokenEstimator`
  (`workspace_runtime.cpp:179` member, wired at `:146`), and
  `compaction_policy()` returns the `ContextCompactor`'s `policy_`, which is
  constructed once and never mutated (`compactor.hpp:127,143`). Both are
  read-only after construction, so reading them on the dispatch thread is safe.
- **MCP status is the one hazard.** `McpManager` has **no** internal
  synchronization (verified: no `mutex`/`atomic` in `mcp_manager.hpp` or
  `mcp_manager.cpp`), and `refresh()` is reachable from the reconnect path
  (`mcp_manager.cpp:234`), which can run off the dispatch thread. This errata
  therefore pins an **additive internal mutex in `McpManager`** guarding
  `slots_` (no public-interface change; spec 15 implementation detail), so
  `mcp_statuses()` is safe on the dispatch thread. **The mutex must be
  `mutable`** because `statuses()` is `const` (`mcp_manager.hpp:56`);
  `McpManager::statuses()` takes a `std::lock_guard` over it and copies the
  `ServerSlot` fields under the lock. The additive
  `WorkspaceRuntime::mcp_statuses()` is a member of a different class and holds
  no `McpManager` lock of its own — it only **forwards** to
  `McpManager::statuses()`. Absent the `McpManager` lock, the daemon must call
  `mcp_statuses()` only on the MCP owner thread. `CTX-F6` covers the degraded
  path.
- **C8 — the mutex covers every `slots_` access.** The additive
  `mutable std::mutex` (non-recursive) is the **sole** synchronization for
  `slots_`. Every read and every write of `slots_` or of a `ServerSlot` field is
  performed while that lock is held. The acquiring public entry points are
  `McpManager`'s `start`, `refresh`, `shutdown`, and `statuses()`
  (`mcp_manager.hpp:56`, impl `mcp_manager.cpp:325-345`). The additive
  `WorkspaceRuntime::mcp_statuses()` accessor does **not** acquire the lock
  itself: it is a member of a different class and cannot reach `McpManager`'s
  private mutex; it only **forwards** to `McpManager::statuses()`, which is the
  entry point that takes the `std::lock_guard`. The synchronous helpers
  `findSlot`, `setState`, `emitStatus`, and
  `installTools` are **lock-held helpers**: they never acquire it themselves and
  are only called with it already held. This matters because `setState` calls
  `emitStatus`: with a non-recursive `std::mutex`, a helper that re-locked would
  self-deadlock. `findSlot` returns a `ServerSlot*` used only inside the same
  critical section, so no pointer escapes the lock.
- **C8 — the lock never spans blocking client I/O.** `start`/`refresh`/`shutdown`
  return `Task<void>`, but they drive the client with **blocking** `.get()` calls,
  not `co_await` (`mcp_manager.cpp:234`, `:254-255`, `:287-290`);
  `DefaultMcpClient::start` itself blocks for up to the handshake timeout
  (`mcp_client.cpp:81,93-94,133`). A `std::mutex` locked on one thread must be
  unlocked by that same thread, and a later wave may make `Task` a real coroutine
  that resumes on a different thread, so the lock is held only in short,
  **non-suspending** critical sections: mutate/copy the `ServerSlot` state needed
  and call the lock-held helpers under the lock, release, then call `.get()`/
  `co_await`. A `ServerSlot*` obtained before the client call is never
  dereferenced after it; the slot is re-located by `id` under a fresh lock. No
  `.get()`, `co_await`, or other suspension occurs while the lock is held.
- **C8 — the bus is published only after the `slots_` lock is released.** The
  additive mutex is non-recursive and `EventBus::publish` invokes its handlers
  synchronously (`event_bus.cpp:192-214`), so a handler that called back into
  `McpManager` would self-deadlock if `slots_` were still held. `emitStatus`
  therefore **builds** the `payload::McpServerStatusChanged` event under the lock
  and appends it to a caller-owned `std::vector<Event>& pending` threaded through
  `setState`/`installTools` (it never calls `bus_.publish()`); the entry points
  `start`/`refresh`/`shutdown` publish `pending` **after** releasing the lock.
  Pinned lock order: `McpManager::slots_` and `EventBus` are never held at the
  same time, so no bus handler ever runs under `slots_` (the daemon handler only
  forwards to the transport, `HostRuntime::handleCommittedEvent`,
  `host_runtime.cpp:337-366`). This is the lock-order rule CTX-F6 tests.

### 3.6 Rejected alternatives

1. **Supervisor-side reconstruction** from `TokenUsageUpdated` + `CompactionMarker`.
   Rejected: the supervisor lacks the system prompt, tool schemas, and message
   list; a re-derivation would drift from `assemble()`.
2. **Overload `session.show`** to include the snapshot. Rejected: it would change
   the response shape of a frozen M2 method (`SessionDetail`, `protocol.hpp:364-368`,
   `HostRuntime::showSession`, `host_runtime.cpp:470-491`); spec 13 §6.8 set the
   precedent of a new method.
3. **Agent-loop-published last-request cache** (loop stores the last
   `LLMRequest` metadata in a shared object). Rejected: it couples the UI to loop
   internals, adds cross-thread mutable state, and is empty before the first
   turn. It is also *stale by design* ("last sent" vs. "current projection").
   Kept as a possible future optimization only.
4. **A typed `protocol::ContextSnapshot` DTO.** Rejected (see §3.4).
5. **Shipping content to the supervisor** (prompt text, schema bodies, message
   bodies) for a richer view. Rejected: leaks prompts and violates spec 10 U15 /
   §40. CTX8 pins counts-and-names-only.

---

## 4. The `/context` command

### 4.1 Registration

Add one callback to `CommandContext` and one command to `builtin()`:

```cpp
// include/ymh/ui/command_registry.hpp (additive; in `CommandContext`, after the
// last existing callback `skill` at :33)
// Opens the read-only context overlay for the active session (18 §4).
std::function<void()> context;
```

```cpp
// src/ui/command_registry.cpp, inside CommandRegistry::builtin() (additive)
registry.add(Command{
    "context", "show the assembled context window (grid, tools, MCP)",
    [](CommandContext& context, const std::string&) {
        if (context.context) {
            context.context();
        }
    }});
```

`CommandRegistry::dispatch` (`command_registry.cpp:95-115`) needs no change: the
leading-`/` detection and Tab completion (`CommandRegistry::complete`, `:64-73`)
are generic, so `/con<Tab>` completes to `/context` for free.

### 4.2 Output mode: overlay (pinned)

`/context` opens a **modal overlay** (`UiMode::Context`), not a transcript entry.

- Rationale: the color grid is a 2-D, per-cell-colored element. A transcript
  entry is a plain `ConversationEntry{role, text}` (`ui_model.hpp:80-86`) with no
  color, and `/clear` erases it; the overlay is the established pattern
  (`render_dialog`, `ui_render.cpp:344-370`, and the switcher, `:409`).
- **Rejected:** a transcript entry with an ASCII grid — loses color, clutters the
  durable-looking view, and is erased by `/clear`.
- The overlay **does not** append any event or transcript entry (CTX1).

```cpp
// include/ymh/ui/ui_model.hpp (additive)
struct ContextOverlayModel {
    bool            open = false;
    bool            loaded = false;
    SessionId       session;
    ContextSnapshot snapshot;   // ymh::ContextSnapshot (agent value type, 18 §3)
    int             view = 0;   // 0 = grid+legend, 1 = tools/servers
    int             scroll = 0;
    // The note string the overlay renders verbatim in its bottom-line slot
    // (§5.1 CX-D12) when non-empty. On success it is the daemon's
    // `snapshot.note` (e.g. "budget unknown"); on a UI-local failure it is
    // "malformed snapshot" or the transport error. Empty means no note.
    std::string     note;
};
```

Add `ContextOverlayModel context;` to `UiModel` (`ui_model.hpp:336-347`) and
`Context` to `UiMode` (`ui_event.hpp:47-52`). `ui_model.hpp` adds
`#include "ymh/agent/context_snapshot.hpp"`. The UI already includes agent value
headers (`ui_event.hpp:15-22` includes `agent/compactor.hpp`), so holding a
`ContextSnapshot` introduces no new layer.

`build_ui` (`ui_render.cpp:498-537`) gains one branch, alongside the existing
`dbox` overlays (`:527-535`):

```cpp
if (model.mode == UiMode::Context && model.context.open) {
    return ftxui::dbox({main, render_context_overlay(model, size, theme)});
}
```

### 4.3 Wiring and refresh

```cpp
// src/ui/supervisor.cpp — dispatch_command (additive, with the other
// CommandContext callbacks, :1105-1147)
context.context = [this] { open_context(); };
```

**M9 — generation guard (pinned).** A `context.show` reply can arrive after the
user pressed `Esc`, or after a newer `r` request superseded it; applying it would
re-open (or overwrite) a dismissed overlay. Every request is therefore tagged with
a monotonically increasing **generation**; a reply is applied only if its
generation still matches, and **closing or superseding bumps the generation**.

```cpp
// src/ui/supervisor.cpp — new members (sketch). <atomic> is required (C1).
// `context_generation_` is written on the UI thread (open/close) and read on
// the pump thread (reply early-out), so it MUST be atomic — a plain uint64_t is
// a data race (UB, TSAN-visible). It guards no other memory, so relaxed
// ordering suffices; see the C1 note below.
std::atomic<std::uint64_t> context_generation_{0};  // bumped on open, refresh, close

void open_context() {
    WorkspaceModel* workspace = model_.activeWorkspace();
    SessionUiState* state     = active();
    if (workspace == nullptr || state == nullptr) {
        return;
    }
    const SessionId   session    = state->id;
    const std::uint64_t generation =
        context_generation_.fetch_add(1, std::memory_order_relaxed) + 1;  // supersedes in-flight
    submit_to(workspace->id, std::string(protocol::method::kContextShow),
              nlohmann::json{{"session", session.value}},
              [this, session, generation](SupervisorReply reply) {
                  if (generation != context_generation_.load(std::memory_order_relaxed)) {
                      return;   // closed or superseded before the reply arrived
                  }
                  ContextOverlayModel overlay;
                  overlay.session = session;
                  if (reply.ok) {
                      overlay.loaded = parse_context_snapshot(reply.result, overlay.snapshot);
                      if (!overlay.loaded) {
                          overlay.note = "malformed snapshot";
                      } else {
                          overlay.note = overlay.snapshot.note;  // daemon note (C7)
                      }
                  } else {
                      overlay.note = reply.error;
                  }
                  enqueue([this, overlay, generation]() mutable {
                      if (generation != context_generation_.load(std::memory_order_relaxed)) {
                          return;   // second check on the UI thread
                      }
                      model_.context = std::move(overlay);
                      model_.context.open = true;
                      model_.mode = UiMode::Context;
                      model_.dirty.markAggregate();
                  });
              });
}

void close_context() {
    context_generation_.fetch_add(1, std::memory_order_relaxed);  // invalidates in-flight
    model_.context.open = false;
    model_.mode = UiMode::Conversation;
    model_.dirty.markAggregate();
}
```

The guard is checked **twice**: on the pump thread before building the overlay
(cheap early-out) and again inside the `enqueue` action on the UI thread
(authoritative, since the user may close between the two). `Esc`/`q` and the
`r` refresh both route through `close_context`/`open_context`, so both bump the
generation.

**C1 — memory ordering (pinned).** `context_generation_` is
`std::atomic<std::uint64_t>`; the bump is
`fetch_add(1, std::memory_order_relaxed)` and both checks are
`load(std::memory_order_relaxed)`. **Relaxed is provably sufficient** here: the
counter orders nothing but itself — the reply payload is captured by value in
the lambda, the built overlay is moved through `enqueue`, and every write to
`model_` happens on the UI thread. The pump-thread check is only an optimization
(a stale relaxed load costs at most one wasted overlay build), and the UI-thread
check is authoritative because both the bump and that load execute on the same
thread, so program order guarantees the load observes the latest bump. No
acquire/release edge is needed and no other shared state is touched across
threads. The alternative — dropping the pump-thread early-out and relying on the
UI-thread check alone — is also race-free, but this errata keeps the early-out
(the reply can be large, and parsing it off the hot path is cheap insurance) and
pays for it with one relaxed atomic.

`submit_to` (`supervisor.cpp:863`) and `enqueue` (`:583`) are the pinned async
pattern; the reply runs on the pump thread and is marshalled to the UI thread
exactly like `refresh_sessions` (`supervisor.cpp:748-796`: reply parse
`:755-764`, `enqueue` marshal `:772-794`).

`parse_context_snapshot` is a thin, non-throwing wrapper over the agent
`from_json` (§3.2) so a malformed reply degrades instead of aborting the pump:

```cpp
// src/ui/supervisor.cpp (file-local; sketch)
bool parse_context_snapshot(const nlohmann::json& json, ContextSnapshot& out) {
    try {
        out = json.get<ContextSnapshot>();
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}
```

**Refresh policy (pinned).** The snapshot is a **point-in-time projection**.

- Requested on **every open** (`/context`), not cached from events.
- `r` re-requests (same code path).
- It is **not** auto-refreshed on `TokenUsageUpdated`/`CompactionMarker`: that
  would spam RPCs and mutate the model mid-render. The overlay shows
  `captured_sequence` in the title/status row (pinned slot `  @<seq>`, §5.1
  CX-D12, §5.3) so the user knows how fresh it is (CTX-F10).
- Closing discards the snapshot; nothing is written anywhere.

### 4.4 Keybindings (overlay-local)

| Key | Action |
|---|---|
| `Esc`, `q`, `Ctrl+C` | close via `close_context()` (bumps the generation, M9) |
| `r` | re-request the snapshot via `open_context()` (bumps the generation) |
| `g` | grid + legend view (`view = 0`) |
| `t` | tools/servers view (`view = 1`) |
| `Tab` | toggle view |
| `↑`/`↓`, `PageUp`/`PageDown` | scroll the tools/servers list |

`Esc`/`q`/`Ctrl+C` must call `close_context()` (not just clear `open`), so an
in-flight reply is invalidated (M9).

`handle_event` (`supervisor.cpp:1448-1461`) gains, before the switcher branch:

```cpp
if (model_.mode == UiMode::Context && model_.context.open) {
    return handle_context(event);
}
```

This preserves **U8** (exactly one keyboard focus owner): the `InputView` is
never reached while the overlay is open.

---

## 5. The color grid

**Displayed-element inventory and the 10-row decision (normative, CX-D12).**
Before the geometry, every element the overlay is required to display is assigned
a slot. The table is exhaustive; the grid is sized to absorb whatever the fixed
chrome does not use, so no element ever overflows the terminal.

| # | Element | Rows | Slot | Present when |
|---|---|---|---|---|
| 1 | window border (top + bottom) | 2 | first and last | always |
| 2 | budget title (known or unknown form, §5.3 L1) | 1 | row 2 (first inside the border), left field | always |
| 3 | `captured_sequence` | 0 | same row (2), right field `  @<seq>` | always |
| 4 | color grid | `rows = clamp(height − 10, 0, 12)` | rows 3 … 2+rows (between title and body) | grid mode, both views |
| 5 | body: legend (one row per segment) | 6 | after the grid | `view == 0` |
| 6 | body: inventory viewport | 6 | after the grid (blank-padded) | `view == 1` |
| 7 | — inventory section header `servers` / `tools` | ≤ 2 | inside the six body rows | `view == 1` |
| 8 | — server row (`id  state  tools=<n>[  skipped=<n>][  !]`) | ≤ 5 | inside the six body rows, scrollable | `view == 1` |
| 9 | — tool row (`name  provenance  ~<tok> tok`) | ≤ 5 | inside the six body rows, scrollable | `view == 1` |
| 10 | `note` | 0 or 1 | bottom line, **when non-empty** | `note != ""` |
| 11 | totals row | 0 or 1 | bottom line, when `note == ""` and grid mode | grid mode |
| 12 | fallback hint | 0 or 1 | bottom line, when `note == ""` and fallback mode | fallback mode |

**Row cost, by view and size (proves every pinned size fits).** The fixed chrome
is border (2) + title/status (1) + body (6) + bottom line (1) = **10** rows and
never changes; the grid is the only variable element, consuming exactly `rows =
height − 10` (clamped to `[0, 12]`). Therefore:

- **Grid mode, `view == 0` or `view == 1`:** `2 + 1 + rows + 6 + 1 = 10 + rows =
  height`. At `{66, 20}`: `2+1+10+6+1 = 20` (fills the height exactly). At
  `{100, 30}`: `2+1+12+6+1 = 22 ≤ 30` (grid capped at 12; the overlay is 22 rows).
- **Fallback mode (grid suppressed), `view == 0` or `view == 1`:** `2 + 1 + 0 +
  6 + 1 = 10`. At `{52, 10}`: exactly 10 (fills the height exactly). At
  `{32, 8}`: the 10-row fallback exceeds height 8 and the **terminal** clips it —
  the documented `height < 10` concession (CTX11), not a renderer defect.

Elements 7–9 share the six body rows (they are the viewport, not extra rows); `!`
(element 8) and `provenance` (element 9) are fields inside a body row. The bottom
line is the single row immediately above the bottom border, and it is always
occupied by exactly one of elements 10–12.

**CX-D12 — the single decision, applied everywhere.** The chrome stays at
`kContextOverlayChromeRows = 10` rows in every view and every size; it is **not**
raised. The two elements that previously had no slot are folded into existing
chrome rows:

- `captured_sequence` is appended to the **title/status row** as `  @<seq>` (two
  spaces), always present (§5.3).
- `note`, when non-empty, **takes the bottom line**, displacing the totals row
  (grid mode) or the fallback hint (fallback mode). When `note == ""` the bottom
  line is exactly as before.

The displaced row is the **only** element ever dropped, and only when
`note != ""`. Rationale: `note` is set only on a degraded snapshot
(CTX-F2/F3/F6/F7), where it is the more important signal; the title still carries
`used`/`window`/`pct`, so only `threshold`/`reserve` are hidden, and only in that
one case. `note` is rendered verbatim on one row (never split, never prefixed —
§3.3 C7) and is a fixed-width row like the hint and the totals row: it is emitted
whole and the terminal may clip it below `inner_width` (§5.4 L2).

This decision is the single source of truth for §5.1, §5.3, §5.4, §4.3,
CTX-F10, §10.2/§10.5/§10.6/§10.7 and every golden/PTY assertion.

### 5.1 Budget → cells mapping (normative, pure)

Inputs: the snapshot and the **whole-terminal** `TerminalSize{width, height}`
(the same `size` `build_ui` receives, `ui_render.cpp:498`). The geometry is a
pure function, exposed for testing (M5/M6):

```cpp
// include/ymh/ui/ui_render.hpp (additive)
struct ContextGridGeometry { int cols = 0; int rows = 0; };
[[nodiscard]] ContextGridGeometry context_grid_geometry(int width, int height) noexcept;
```

with the pinned constants:

```text
kContextGridMaxCols        = 72
kContextGridMaxRows        = 12
kContextGridMinCols        = 40
kContextGridMinRows        = 10
kContextOverlayChromeRows  = 10   // border (2) + title/status (1) + body (6) + ONE bottom line
                                  // (title/status = budget title + "  @<captured_sequence>";
                                  //  body = legend at view 0, inventory at view 1;
                                  //  bottom line = note, else totals in grid mode, else fallback hint; no rule row)
kContextOverlayBorderCols  = 2    // the ftxui::window() border
inner_width = max(width - kContextOverlayBorderCols, 0)
cols = clamp(inner_width, 0, kContextGridMaxCols)
rows = clamp(height - kContextOverlayChromeRows, 0, kContextGridMaxRows)
cells_total = rows * cols
grid_total  = budget.window_tokens > 0 ? budget.window_tokens : used_tokens
```

`chrome` is thus **defined** as `kContextOverlayChromeRows = 10` rows: window
border (2) + **title/status** (1) + **body** (6) + **one** bottom line. There is
**no** separator/rule row. The **title/status** row carries the budget title
(§5.3 L1) followed by the freshness field `  @<captured_sequence>`; it is the
only header row. The **body** is view-dependent and always exactly six rows:
the six segment legend rows at `view == 0` (§5.3), and a six-row inventory
viewport at `view == 1` (§6, M1) — the inventory **replaces** the legend, so the
chrome is 10 rows in both views; when the inventory holds fewer than six rows the
remaining body rows are blank, so the body is always exactly six rows. The
**bottom line** is the `note` when non-empty, otherwise the totals row in grid
mode or the fallback hint in fallback mode (§5.4) — the hint **replaces** the
totals row so the fallback fits the pinned 10-row minimum exactly. If
`cols < kContextGridMinCols` or `rows < kContextGridMinRows` or `cells_total == 0`
or `grid_total == 0`, render the **text-only fallback** (§5.4) and draw no grid.

Otherwise, for each cell index `c ∈ [0, cells_total)`:

```text
lo = c * grid_total / cells_total          // integer floor
cell(c) = the segment whose [seg_lo, seg_hi) contains lo,
          where seg_lo = Σ tokens of preceding segments
          if no segment contains lo -> FreeSpace
```

This is deterministic, monotone in reading order, and partitions all cells. It
requires no floating point and is trivially unit-testable (CTX9). Segments fill
row-major, top-left → bottom-right: system prompt, tools, MCP tools, conversation,
summary, then free space.

**Cell mapping** is a pure, exposed function (M6), the single implementation the
renderer and the test share:

```cpp
// include/ymh/ui/ui_render.hpp (additive)
// Returns the segment owning cell `cell_index` of `cells_total`. `grid_total` is
// derived from the snapshot (window > 0 ? window : used). Degenerate inputs
// (cells_total <= 0, cell_index out of [0, cells_total), grid_total == 0) return
// ContextSegmentKind::FreeSpace; the caller must not draw a grid for them.
[[nodiscard]] ContextSegmentKind context_cell_kind(const ContextSnapshot& snapshot,
                                                   int cells_total, int cell_index) noexcept;
```

**Sub-cell segments (L2).** A segment whose token share is smaller than one cell
(`tokens * cells_total < grid_total`) can receive **zero** cells; the grid cannot
represent it. This is not an error: its size remains visible in the legend
(legend-only visibility), and CTX11's fallback is reserved for geometry, not for
small segments.

### 5.2 Renderer surface, color, and glyph

```cpp
// src/ui/ui_render.cpp (additive, file-local)
ftxui::Element render_context_overlay(const UiModel& model, TerminalSize size,
                                      const Theme& theme);
ftxui::Element render_context_grid(const ContextSnapshot& snapshot, int cols, int rows,
                                   const Theme& theme);
ftxui::Element render_context_legend(const ContextSnapshot& snapshot, const Theme& theme);
ftxui::Element render_context_inventory(const ContextSnapshot& snapshot, int scroll,
                                        const Theme& theme);

ftxui::Color context_segment_color(ContextSegmentKind kind) {
    switch (kind) {
        case ContextSegmentKind::SystemPrompt:      return ftxui::Color::Blue;
        case ContextSegmentKind::ToolSchemas:       return ftxui::Color::Green;
        case ContextSegmentKind::McpToolSchemas:    return ftxui::Color::Magenta;
        case ContextSegmentKind::Conversation:      return ftxui::Color::Default;
        case ContextSegmentKind::CompactionSummary: return ftxui::Color::Cyan;
        case ContextSegmentKind::FreeSpace:         return ftxui::Color::GrayDark;
    }
    return ftxui::Color::Default;
}

char context_segment_glyph(ContextSegmentKind kind) {
    switch (kind) {
        case ContextSegmentKind::SystemPrompt:      return 'S';
        case ContextSegmentKind::ToolSchemas:       return 'T';
        case ContextSegmentKind::McpToolSchemas:    return 'M';
        case ContextSegmentKind::Conversation:      return 'C';
        case ContextSegmentKind::CompactionSummary: return '~';
        case ContextSegmentKind::FreeSpace:         return '.';
    }
    return '?';
}
```

Each cell is `paint(ftxui::text(std::string(1, glyph)), color, theme)`
(`paint` is the existing fg-only helper, `ui_render.cpp:23-28`). **The glyph is
drawn in both modes**; color is additive, never the sole carrier (CTX10). The
rejected alternative — uniform `█` cells distinguished by color only — fails for
color-blind users and makes the monochrome path a separate renderer.

### 5.3 Legend (the `view == 0` body)

At `view == 0` the **body** is one row per segment, in fixed order; the overlay's
**bottom line** is the `note` when non-empty, otherwise the totals row in grid
mode or the fallback hint in fallback mode (§5.4 — the hint **replaces** the
totals row; the note **replaces** either). **No separator/rule row is
rendered** in either mode; the overlay's chrome is exactly
`kContextOverlayChromeRows = 10` rows (§5.1), so the six legend rows are followed
immediately by the one bottom line. At `view == 1` the body is the six-row
inventory viewport instead and this legend is not drawn (§6, M1). **Every
percentage in the title, the legend, and the totals row is
`tokens * 100 / window_tokens`, truncated (floor) to one decimal — never
rounded** (C5). For the §5.6 fixture:

```text
S  system prompt          412 tok    0.6%
T  tool schemas          3,980 tok   6.2%
M  mcp tool schemas      2,240 tok   3.5%
C  conversation         33,540 tok  52.4%
~  compaction summary    1,128 tok   1.7%
.  free                 22,700 tok  35.4%
used 41,300 / 64,000 (64.5%)  threshold 47,923  reserve 4,096
```

(The six legend rows above are exactly the legend; the totals line is the
single bottom chrome row. A horizontal rule is **not** part of the rendered
output — the overlay is budgeted for 10 chrome rows, not 11.)

The summary is `1128·100/64000 = 1.7625%` and the free row is
`22700·100/64000 = 35.46875%`; both truncate to the values shown. Rounding would
give `1.8%`/`35.5%` and is **not** used (C5).

The glyph is painted with `context_segment_color`. There is **no separate header
row**: the `title/status (1)` chrome row (§5.1) is the only title/header, and its
exact text depends on the budget and the freshness suffix (L1, M1):

- **Known budget** (`window_tokens > 0`): `used <used> / window <window> (<pct>%)`
  — for the §5.6 fixture, `used 41,300 / window 64,000 (64.5%)` (35 chars).
- **Unknown budget** (`window_tokens == 0`): `used tokens (budget unknown)`
  (28 chars), the canonical string (CTX6, C3); the percentage column and the free
  row are replaced by `—`.

**Freshness suffix (pinned, M1).** The title/status row is exactly
`<budget title>  @<captured_sequence>` — the canonical budget string above,
followed by **two spaces** and `@`, then `captured_sequence` in decimal. For the
§5.6 fixture (`captured_sequence = 1842`) the known-budget title row is
`used 41,300 / window 64,000 (64.5%)  @1842` (42 chars) and the unknown-budget
title row is `used tokens (budget unknown)  @1842` (35 chars). This is the
**only** place `captured_sequence` is displayed (CTX-F10); it is always present,
even when the snapshot is stale or degraded. Both forms fit `inner_width` at the
pinned sizes (42 ≤ 50 at `{52, 10}`, 42 ≤ 64 at `{66, 20}`); a narrower terminal
clips at the right edge like the other fixed-width rows (L2).

No other title/header string exists. To supply a budget, the user sets
`agent.compaction.context_window_tokens`.

**Pinned totals format** (L7). The totals row is exactly:

```text
used <used> / <window> (<pct>%)  threshold <threshold>  reserve <reserve>
```

where `<used>`, `<window>`, `<threshold>`, `<reserve>` are thousands-separated
integers and `<pct>` is `used*100/window` truncated (floor) to one decimal — the
same truncation rule as the legend, so the two can never disagree. When
`used_tokens > window_tokens` the **computed** `<pct>` is shown as-is (e.g.
`109.3%`); it is **not** clamped to a literal `>100%` (C6). The unknown variant
is:

```text
used <used> tokens (budget unknown)  threshold —  reserve —
```

**Row widths (pinned).** Every row must fit `inner_width`; a row that exceeds it
would wrap or clip. At the §5.6 fixture (`TerminalSize{66, 20}`,
`inner_width = 64`), the title/status row is
`used 41,300 / window 64,000 (64.5%)  @1842` — **42 chars** — the totals row is
exactly `used 41,300 / 64,000 (64.5%)  threshold 47,923  reserve 4,096` —
**61 chars** — and the over-budget fixture (`used = 70,000`) is **62 chars**
(`used 70,000 / 64,000 (109.3%)  threshold 47,923  reserve 4,096`); the unknown
totals variant is **59 chars** and the unknown title/status row is **35 chars**.
Every legend segment row is ≤ 45 chars, and the maximal joined `note` is
**55 chars** (§3.3 C7). All stay ≤ 64, so nothing wraps or clips at the pinned
size.

**L3 — unknown budget must not read as a full window.** When `window_tokens == 0`,
`grid_total = used_tokens` (§5.1), so the grid is **100% used segments and has no
free cells**. That is expected, but the title/status row's budget field must read
exactly `used tokens (budget unknown)` (the full row is
`used tokens (budget unknown)  @<seq>`) instead of any `n% of window`, and the
free row shows `—`; the grid is never labelled as a percentage of an unknown
window.

### 5.4 Terminals too small (fallback)

If the grid minimum is not met, render the **fallback**: window border, title, the
six-row **body**, and the bottom line — **no grid**. The fallback chrome is
exactly the same `kContextOverlayChromeRows = 10` rows as grid mode, in **both**
views: border (2) + title/status (1) + body (6) + bottom line (1) = **10**. The
bottom line is the `note` when non-empty and the fallback hint otherwise (§5.1
CX-D12); the totals row is never drawn in fallback mode. The **body** is
view-dependent (§5.1): the six legend rows at `view == 0`, or the
six-row inventory viewport at `view == 1` (M1). At `view == 1` the inventory
therefore does **not** add rows beyond the budget: it is bounded to the same six
body rows (scrollable with `↑`/`↓`/`PageUp`/`PageDown`, §4.4) and the legend is
not drawn. There is no separator/rule row. This is a deliberate degradation, not
an error (CTX11).

**M1 — the `view == 1` fallback row budget (pinned).** The fallback is exactly 10
rows at every view. `view == 0`: border (2) + title/status (1) + legend (6) +
bottom line (1). `view == 1`: border (2) + title/status (1) + inventory viewport
(6) + bottom line (1). The inventory shows at most the six body rows (section
headers count as rows) and scrolls when the list is longer; shorter content is
padded with blank body rows so the body is always exactly six rows. The bottom
line is the `note` when non-empty, else the pinned hint. This keeps the
`{52, 10}` golden valid at both views and is asserted by
`ContextOverlaySmallTerminal` (view 0) and
`ContextOverlaySmallTerminalInventory` (view 1) in §10.2.

The pinned fallback hint text (L6) is exactly:

```text
terminal too small for the grid (need >= 40x10)
```

(ASCII `>=`/`x`, no Unicode, so it is byte-identical under the monochrome golden.)

**L2 — narrow-width behavior (pinned).** The overlay's **grid and legend rows**
are always composed to fit `inner_width`, and the grid is never partially drawn.
Three rows are fixed-width and are emitted whole regardless: the fallback hint
(47 bytes), the totals row (≤ 62 bytes at the pinned fixtures, §5.3), and the
`note` row (≤ 55 bytes — the maximal joined note of §3.3 C7). Showing the hint
whole needs `inner_width ≥ 47`, i.e. `width ≥ 47 +
kContextOverlayBorderCols = 49`. When the terminal is narrower than such a row,
the row is still emitted as a single whole string and the **terminal** clips it at
the right edge — exactly the concession already made for `height < 10` below. The
renderer itself never truncates a row (no ellipsis, no partial glyph), so the
"never a clipped or partial row" rule means: the renderer never **composes** a
clipped row, and it never partially draws the grid. The recommended minimum is
`49 × 10` for the fallback and the `{66, 20}` fixture (`inner_width = 64`) for the
full grid; a narrower terminal degrades to terminal-level clipping, which is not a
rendering defect (CTX11).

The fallback needs at least `47 + kContextOverlayBorderCols = 49` columns (the
47-char hint plus the border) and exactly `kContextOverlayChromeRows = 10` rows
(border 2 + title/status 1 + body 6 + bottom line 1). At height 10 the fallback fills the
terminal exactly and nothing clips; below 10 rows the terminal itself clips,
which is not a rendering defect. The hint's `40x10` names the **grid** minimum
(40 columns × 10 grid rows, §5.1); because `rows = height − 10` and
`cols = width − 2`, the grid renders only when `height ≥ 20` and `width ≥ 42`,
and the fallback renders otherwise (CTX-F8). The pinned golden therefore uses
`TerminalSize{52, 10}` (§10.2), never a smaller size.

### 5.5 Monochrome

`Theme::color == false` (`theme.hpp:5-12`): `paint` returns the element
unchanged, so cells and legend swatches are glyphs only. The glyph map (§5.2)
makes every segment distinguishable with zero color. `Theme::user_block` is
irrelevant to this overlay.

### 5.6 Worked example

Config: `agent.compaction.context_window_tokens = 64000`,
`reserve_output_tokens = 4096`, `threshold_ratio = 0.80` →
`effective_threshold_tokens = 0.80 · (64000 − 4096) = 47923`
(`compactor.hpp:48-57`). Snapshot: system prompt 412, builtin tools 3,980, MCP
tools 2,240, conversation 33,540, summary 1,128 → `used = 41,300`,
`free = 22,700`.

Fixture `TerminalSize{66, 20}`: `inner_width = 66 − 2 = 64` → `cols = min(64,72) = 64`;
`rows = clamp(20 − 10, 0, 12) = 10` → `cells_total = 640`. `grid_total = 64000`, so
`lo = c · 100`. The fixture's `captured_sequence = 1842`, so the title/status row
is `used 41,300 / window 64,000 (64.5%)  @1842` (42 chars ≤ 64) and the totals row
is `used 41,300 / 64,000 (64.5%)  threshold 47,923  reserve 4,096` (61 chars ≤ 64);
the note is empty, so the totals row is the bottom line. Cell assignment:

| Segment | token range | cells |
|---|---|---|
| SystemPrompt | `[0, 412)` | `c=0..4` (5) |
| ToolSchemas | `[412, 4392)` | `c=5..43` (39) |
| McpToolSchemas | `[4392, 6632)` | `c=44..66` (23) |
| Conversation | `[6632, 40172)` | `c=67..401` (335) |
| CompactionSummary | `[40172, 41300)` | `c=402..412` (11) |
| FreeSpace | `[41300, 64000)` | `c=413..639` (227) |

Total 640 cells. Rendered (color terminals show these glyphs in the palette of
§5.2; monochrome shows the same glyphs). Each row below is exactly 64 glyphs
(no separator characters):

```text
row 0  SSSSSTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTMMMMMMMMMMMMMMMMMMMM
row 1  MMMCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
row 2  CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
row 3  CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
row 4  CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
row 5  CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC
row 6  CCCCCCCCCCCCCCCCCC~~~~~~~~~~~...................................
row 7  ................................................................
row 8  ................................................................
row 9  ................................................................
```

(Row 0: 5×`S` + 39×`T` + 20×`M`; row 1: 3×`M` + 61×`C`; row 6: 18×`C` +
11×`~` + 35×`.`.)

---

## 6. The MCP/tools inventory view

`view == 1` lists two sections, both bounded. The inventory occupies the overlay's
six-row **body** (§5.1, M1) — the legend is not drawn at `view == 1` — and scrolls
when either section is longer; the title and bottom line are the same chrome rows
as `view == 0`.

### 6.1 What the model actually sees (verified)

**MCP tools are already inside the tool schemas.** `McpManager` registers each
translated tool into the shared `ToolRegistry` under the prefix
`mcp.<server>.` (`mcp_manager.cpp:212`), and `SessionContextAssembler::tools()`
returns `tools_.schemas()` (`context_assembler.cpp:52-54`), which is placed on
`LLMRequest.tools` (`agent_loop.cpp:328-330`). There is **no separate MCP tool
list on the wire**. The inventory therefore lists `ToolRegistry::schemas()`
(`tool_registry.cpp:303`) and tags provenance by the `mcp.` prefix.

### 6.2 Servers

One row per `ContextServerEntry`: `id`, `state` (`mcp_state_token`), `tool_count`,
`skipped` count, and `has_error`. The entry is built by §3.3 step 8b
(id-sorted; `skipped = skipped_tools.size()`, `has_error =
!last_error.empty()`, never the error text). Source: `McpManager::statuses()`
(`mcp_manager.cpp:325-345`) via the additive `WorkspaceRuntime::mcp_statuses()`
(§3.4). If the accessor is unavailable (see CTX-F6), the section is omitted and
`note` records it; the rest of the overlay still renders.

**Pinned row format** (L7):

```text
<id>  <state>  tools=<tool_count>[  skipped=<skipped>][  !]
```

where `  !` is appended iff `has_error`. Example: `alpha  ready  tools=2`.

**M8 decision — `last_error` is dropped from the wire.** `McpServerStatus::last_error`
is unbounded and is sourced from arbitrary `std::exception::what()` on the
reconnect path (`mcp_manager.cpp:300`, also `:245`, `:267`), or from a bounded
enum token (`:294`). Shipping it would violate the project rule against dumping
sensitive tool/provider output unbounded on the wire (§40, spec 10 U15). The wire
therefore carries `has_error` (a bool) and never the message text. The existing
UI signal remains the bounded 256-byte `mcp_status_detail` notice
(`host_runtime.cpp:175-192`), which already truncates; `/context` deliberately
does not surface more than the boolean.

### 6.3 Tools

One row per `ContextToolEntry`: `name`, `provenance` (`builtin` | `mcp`),
`schema_tokens`. The entry is built by §3.3 step 8a (`provenance` from the `mcp.`
prefix, `schema_tokens` from `estimate_tool_schema_tokens`); the list is
name-sorted because `ToolRegistry::schemas()` is already name-sorted. Bounded by
`max_tools` (§3.3 step 10).

**Pinned row format** (L7):

```text
<name>  <provenance>  ~<schema_tokens> tok
```

Example: `mcp.alpha.search  mcp  ~48 tok`.

### 6.4 Sensitive content

No schema body, description, message text, system prompt, summary text, or MCP
error text is shown or shipped. Tool **names** and server **ids/states** are
allowed: tool names already appear in transcripts (`ui_model.cpp:552-556`), and
server id/state already appear in MCP status notices (`host_runtime.cpp:175-192`).

---

## 7. Invariants

Numbered, testable, and cited. Any code that can violate one is a defect.

**CTX1 — Read-only.** Producing a snapshot appends no event, mutates no log, no
`Session`, and no agent state; it is a pure function of `(header, events, config,
frozen schemas, MCP status)`.

**CTX2 — No live `Session` off the executor thread.** The daemon reads the
resolved view via `SessionStore::load/read` (`HostRuntime::showSession`,
`host_runtime.cpp:470-491`; `HostRuntime::readEvents`, `:836-847`); it never calls
`Session::deriveMessages()`/`Session::events()` on the dispatch thread
(`session.hpp:203,207`; `session.cpp:536-553`).

**CTX3 — Faithful projection.** Messages are `deriveMessages(header, store.read(id))`
(the assembler's own function, `context_assembler.cpp:39`); the prompt is
`WorkspaceRuntime::agent_config().system_prompt` (`workspace_runtime.cpp:300`,
wired `:109`); tools are `ContextAssembler::tools()` (`context_assembler.cpp:52-54`).
No second assembly path exists.

**CTX4 — Advisory estimates.** Every token figure is an estimate, labelled as
such; it never overrides provider-reported `Usage` (`message.hpp:131-138`,
`agent_loop.cpp:702`).

**CTX5 — Schema partition.** `ToolSchemas` ∪ `McpToolSchemas` partitions
`tools()` by the `mcp.` prefix (`mcp_manager.cpp:212`); the two `items` sum to the
schema count and no tool is counted twice.

**CTX6 — Budget honesty.** `window_tokens == 0` means unknown; the UI shows
absolute counts and the title whose **budget field** is exactly
`used tokens (budget unknown)`, never a fabricated percentage or free space. With
a known window the budget field is exactly
`used <used> / window <window> (<pct>%)` (§5.3 L1). In both cases the title/status
row is `<budget field>  @<captured_sequence>` (§5.3, M1). The threshold and
reserve come from `CompactionPolicy` (`compactor.hpp:34-35,48-57`) and are never
recomputed.

**CTX7 — Bounded.** The snapshot has exactly six segments and at most `max_tools`
tool entries; the wire reply is bounded (≤ 256 KiB); `truncated` is set when a
bound is hit.

**CTX8 — No content egress.** The wire carries segment kinds, provenance labels,
token counts, tool names, and MCP server id/state/counts/`has_error` — never
message bodies, the system prompt text, summary text, schema bodies, or MCP
error text (M8; `McpServerStatus::last_error` is unbounded
`std::exception::what()`, `mcp_manager.cpp:300`).

**CTX9 — Grid purity and determinism.** The grid is a pure function of
`(snapshot, size, theme)`; identical inputs ⇒ byte-identical output; no clock, no
randomness, no I/O (U3, `§20.12`).

**CTX10 — Monochrome legibility.** With `theme.color == false` every segment is
distinguishable by glyph alone (`theme.hpp:5-12`); color is never the sole
carrier of meaning.

**CTX11 — Small-terminal degradation.** Below the pinned grid minimum (40 columns
× 10 grid rows) the overlay renders the title/status row and the six-row body,
plus the bottom line, in exactly `kContextOverlayChromeRows = 10` rows (border 2 +
title/status 1 + body 6 + bottom line 1). The body is the legend at `view == 0`
and the six-row inventory viewport at `view == 1` (M1, §5.4); the bottom line is
the `note` when non-empty, else the pinned fallback hint, so no totals or
separator/rule row is emitted, and the grid is never partially drawn. The hint
and the `note` are fixed-width rows (47 bytes and ≤ 55 bytes respectively): when
the terminal is narrower than `49` columns the terminal clips them (L2, §5.4),
exactly as `height < 10` clips vertically — the renderer never truncates a
composed row.

**CTX12 — Focus, cancellation, and late replies.** The overlay owns keyboard
focus while open (U8); `Esc`/`q` closes it and no key reaches the `InputView`;
closing cancels no agent work and touches no daemon state. A `context.show` reply
is applied **only** if its generation matches the current
`context_generation_` (a `std::atomic<std::uint64_t>` accessed with relaxed
ordering — it guards no other memory; §4.3 C1); closing or refreshing bumps the
generation, so a late reply can never re-open or overwrite a dismissed overlay
(M9).

---

## 8. Failure modes

### 8.1 Shared findings (F1–F12, `§54`)

| F# | Finding | Context-inspector handling |
|---|---|---|
| **F1** | path/process isolation | The snapshot performs no path resolution, no `chdir`, no subprocess; it reads the store and the frozen registry only. |
| **F2** | background permission | N/A — no permission is requested or decided. |
| **F3** | late event after close | A snapshot of an unknown/closed session returns `AppCode::UnknownSession` (CTX-F1); the overlay shows the error. |
| **F4** | edge-triggered attention | N/A — no `AgentState` is changed. |
| **F5** | output ring buffers | The snapshot is bounded by `max_tools` and a 256 KiB reply cap (CTX7). |
| **F6** | input/keybinding focus | **Owned here**: the overlay takes focus and the `InputView` receives nothing while open (`handle_event`, `supervisor.cpp:1448-1461`, CTX12). |
| **F7** | per-session dirty flags | Open/close/refresh mark only `Aggregate` (and `Layout` if the renderer needs it); no `Conversation` dirty is raised. |
| **F8** | resource caps | One store read + one registry read per request; no thread, no spawn, no unbounded allocation. |
| **F9** | cancellation scoping | The request is a single read; closing the overlay cancels nothing on the daemon and affects no other session. |
| **F10** | resume-suspended | A resumed/idle session produces a valid snapshot from the store; the overlay never starts a turn. |
| **F11** | subagent ID duality | N/A — the snapshot is per-session; no subagent ID is projected. |
| **F12** | flash clock in model | The renderer reads no clock; `captured_sequence` is data, not time (CTX9). |

### 8.2 Component-local failure modes (CTX-F1–CTX-F12)

These are **component-local** to the context inspector and are not part of the
shared F1–F12 set. They are named `CTX-F#` to avoid collision with the shared
set (the `13` `C-F#` convention). Each must be covered by a test (§10.6).

| CTX-F# | Failure | Detection | Required behaviour |
|---|---|---|---|
| **CTX-F1** | Unknown/closed session | `store().load(id)` returns `nullopt` | Throw `WireError{AppCode::UnknownSession, "UnknownSession"}` via `throw_mapped`, exactly like `showSession` (`host_runtime.cpp:473-475`); UI shows the error in `note`; no overlay grid. |
| **CTX-F2** | Daemon unreachable / link `Dead` | `SupervisorReply.ok == false` | Overlay opens with `loaded == false` and `note = reply.error` (rendered in the bottom-line slot, §5.1 CX-D12); `r` retries; no crash. |
| **CTX-F3** | Budget unknown (`window_tokens == 0`) | `budget.window_tokens == 0` | `note` contains `"budget unknown"`; no percentage; `FreeSpace.tokens = 0`; grid maps used tokens only; the title/status row reads exactly `used tokens (budget unknown)  @<seq>` and the `note` occupies the bottom line, displacing the totals row (CTX6, §5.3 C3, §5.1 CX-D12, M1). |
| **CTX-F4** | Used exceeds window | `used_tokens > window_tokens` | `FreeSpace.tokens = 0`; the last cell is labelled by the segment containing its `lo`; the totals row shows the **computed** percentage (e.g. `109.3%`), never a literal `>100%`; no negative cell count (C6). |
| **CTX-F5** | Estimator undercounts tools | Snapshot vs. provider `Usage` divergence | The snapshot is labelled an estimate (CTX4); tools are reported as a separate segment so the gap is visible; never claimed exact. |
| **CTX-F6** | MCP status race / read failure | Concurrent `refresh()` (`mcp_manager.cpp:234`) or a throw from `McpManager::statuses()` surfaced through `WorkspaceRuntime::mcp_statuses()` | Pin an additive `mutable` `McpManager` mutex around `slots_`; every `slots_` access is lock-held (the `McpManager` entry points acquire it — `statuses()` is the read entry point; `WorkspaceRuntime::mcp_statuses()` only forwards — and lock-held helpers never re-lock), the lock never spans a blocking client call, and the bus is published only after the lock is released (§3.5 C8). The daemon catches any exception, sets `mcp_available = false`, and the builder omits the server section and appends `"mcp status unavailable"` to `note` (rendered in the bottom-line slot, §5.1 CX-D12); the rest renders (§3.3 step 11, §3.4 sketch). |
| **CTX-F7** | Oversized snapshot | `tools.size() > max_tools` or serialized reply > 256 KiB | Keep the first `max_tools`, set `truncated = true`, append `"tools truncated"` to `note` (rendered in the bottom-line slot, §5.1 CX-D12); segment `items`/`tokens` stay full (§3.3 step 10); never an unbounded reply. |
| **CTX-F8** | Terminal too small | `cols < 40` or `rows < 10` | Text-only fallback; no partial grid rows (CTX11). |
| **CTX-F9** | Monochrome terminal | `theme.color == false` | Glyph-only rendering; legend retains the glyph map (CTX10). |
| **CTX-F10** | Stale snapshot | Session advanced after open | Point-in-time view with `captured_sequence` shown in the title/status row (`  @<seq>`, §5.1 CX-D12, §5.3); no auto-refresh; `r` refreshes; no mid-render model mutation. A late reply after `Esc`/refresh is dropped by the generation guard (M9, CTX12). |
| **CTX-F11** | Compaction in flight | Maintenance turn open at request time | The projection is either pre- or post-compaction — both are committed views; the snapshot shows the boundary; never a torn/partial list. |
| **CTX-F12** | Sensitive-content leak | Unit test scans the reply JSON | The wire schema carries no message/prompt/summary/schema bodies **and no MCP error text** (CTX8; `has_error` is a bool, M8); a test asserts absence; the `tui` log category is never used for the snapshot. |

---

## 9. dsh (DeepSeek Harness) mapping

dsh treats the session log as the traceable source of truth and context
management as an explicit, replayable projection (`00 §55`). The context
inspector maps onto that model:

```text
dsh concept                          ymh (this errata)
────────────────────────────────────────────────────────────────────────────
append-only session log          →   Session event log (01); the snapshot reads
                                     it, never rewrites it (CTX1)
context rebuilt from the log     →   deriveMessages(header, events) (01 §6.3),
                                     the same pure projection the assembler uses
context window management        →   CompactionPolicy window/threshold/reserve
                                     (13 §3.2) reported verbatim (CTX6)
tools as model-visible contract  →   ToolRegistry::schemas(); MCP tools are part
                                     of it under mcp.<server>. (15, CTX5)
per-run traceability             →   captured_sequence + boundary + token counts
                                     make the view auditable (18 §3.3)
frontend as a projection view    →   UiModel::context; Render() stays pure
                                     (10 U3, CTX9)
provider-agnostic                →   no provider-specific logic; estimates are
                                     local and advisory (CTX4)
```

The dsh-aligned insight is the same as compaction's: **inspection is a
projection, not a mutation.** The log stays complete and replayable, and the
context view is derived from it identically by any frontend.

---

## 10. Test plan

Strategy is `§44`: unit, integration (`FakeLLM`, fake host), golden, replay, and
a live PTY layer. The inspector is deterministic under the fake layers.

### 10.1 Unit tests (`tests/unit/context_snapshot_test.cpp`, new)

- **Builder partition (CTX5).** Given schemas `{read_file, shell, mcp.alpha.x,
  mcp.beta.y}`, `ToolSchemas.items == 2`, `McpToolSchemas.items == 2`, and the
  union equals 4. No name appears in both.
- **`tools` list construction (L2).** With the same four schemas,
  `snapshot.tools.size() == 4` in `ToolRegistry::schemas()` order (name-sorted);
  `read_file`/`shell` have `provenance == "builtin"`, `mcp.alpha.x`/`mcp.beta.y`
  have `provenance == "mcp"`, and each `schema_tokens ==
  estimate_tool_schema_tokens(schema)`. (Complements the partition test: the list
  and the segments agree.)
- **`mcp_servers` list construction (L2).** Given two `McpServerStatus` — one
  with a non-empty `last_error`, one with two `skipped_tools` — `snapshot.mcp_servers`
  is **id-sorted**, and each entry has `state == mcp_state_token(status.state)`,
  `tool_count == status.tool_count`, `skipped == status.skipped_tools.size()`,
  and `has_error == !status.last_error.empty()`; no error text appears anywhere.
- **Summary fold (CTX3).** Events with two `ContextCompaction` events: only the
  **last** summary contributes; `CompactionSummary.items == 1`; the surviving
  summary is `M[0]` and `Conversation.tokens == E(M[1..])` (estimated directly,
  **not** by subtraction — see §3.3 step 5).
- **System prompt.** Empty prompt ⇒ `SystemPrompt.items == 0`, `tokens == 0`; a
  non-empty prompt ⇒ `tokens == E({system_message(prompt)})`.
- **Budget unknown (CTX6).** `window_tokens == 0` ⇒ `note` contains
  `"budget unknown"`, `FreeSpace.tokens == 0`, and the snapshot reports
  `used_tokens` equal to the sum of used segments.
- **`note` join (C7).** Driving budget-unknown **and** tool truncation **and**
  `mcp_available = false` together yields exactly
  `"budget unknown; tools truncated; mcp status unavailable"` — the pinned `"; "`
  separator, no leading/trailing/doubled separator, fixed step order. Each
  fragment alone yields just that fragment (no separator).
- **Over-budget (CTX-F4).** `used > window` ⇒ `FreeSpace.tokens == 0` and
  `used_tokens` unchanged; the snapshot still reports the true `used_tokens`
  (the renderer computes the actual percentage, e.g. `109.3%`; §5.3 C6).
- **Tool truncation (CTX-F7).** `max_tools = 2` with 5 schemas ⇒ `tools.size() == 2`,
  `truncated == true`.
- **Schema estimate (M4).** The pinned fixture in §3.3
  (`read_file` / `"Read a file"` / the 3-key schema) ⇒ `estimate_tool_schema_tokens`
  returns exactly **32**; the value is stable across calls and equals
  `(name.size() + description.size() + input_schema.dump().size())/4 + 8`.
- **MCP unavailable (M7).** `mcp_available = false` ⇒ `mcp_servers.empty()` and
  `note` contains `"mcp status unavailable"`; the segments are unchanged.
- **`has_error` (M8).** A `McpServerStatus` with a non-empty `last_error` yields
  `ContextServerEntry.has_error == true` and the JSON contains **no** substring of
  the error text.
- **Geometry (M5).** `context_grid_geometry(66, 20) == {64, 10}`;
  `context_grid_geometry(100, 30) == {72, 12}`; `context_grid_geometry(32, 8)`
  has `cols < 40` or `rows < 10` (fallback). This is a pure geometry probe and
  renders nothing; the renderable fallback golden uses `{52, 10}` (§10.2).
- **Grid mapping (CTX9/M6).** For the §5.6 fixture, drive the exposed
  `context_cell_kind(snapshot, 640, c)` and assert the per-cell kind sequence:
  `c=0..4` `SystemPrompt`, `c=5..43` `ToolSchemas`, `c=44..66` `McpToolSchemas`,
  `c=67..401` `Conversation`, `c=402..412` `CompactionSummary`,
  `c=413..639` `FreeSpace`; the counts are exactly 5/39/23/335/11/227 = 640.
- **Determinism (CTX9).** Two calls with identical inputs produce identical
  snapshots (including segment order and JSON).
- **Serialization (CTX8).** `to_json` of a snapshot whose messages/prompt contain
  a sentinel string does **not** contain that sentinel; only names/counts/labels
  are present. Round-trip `to_json` → `from_json` is value-equal, including
  `session` (M2).

### 10.2 Golden-render tests (`tests/unit/ui_render_golden_test.cpp`)

Follow the existing pattern (`render_to_ansi(model, TerminalSize{…}, Theme{…})`,
`:168-183`, `:315-322`).

- **`ContextOverlayGridGolden`.** The §5.6 `ContextSnapshot` fixture
  (`captured_sequence = 1842`, empty `note`) + `TerminalSize{66, 20}` (which
  yields `inner_width=64`, `cols=64`, `rows=10`, 640 cells) + `Theme{color=false}`
  ⇒ a golden string containing the exact §5.6 grid, the title/status row
  `used 41,300 / window 64,000 (64.5%)  @1842`, and the totals row
  `used 41,300 / 64,000 (64.5%)  threshold 47,923  reserve 4,096`. The legend's
  truncation is asserted exactly: the summary row is `1.7%` and the free row is
  `35.4%` (not the rounded `1.8%`/`35.5%`; §5.3 C5). **Row arithmetic (proves the
  golden fits):** the overlay is border 2 + title/status 1 + grid 10 + legend 6 +
  bottom line 1 = 20 rows = the terminal height, and every row is ≤ 64 chars
  (title/status 42, totals 61, longest legend row 45, grid rows exactly 64), so
  the 20×64 overlay fills `{66, 20}` exactly with no wrap or clip.
  Assert `render_to_ansi` twice is byte-identical (CTX9).
- **`ContextOverlayMonochrome`.** `Theme{color=false}` ⇒ every segment glyph
  present; no ANSI color escapes in the output (CTX10).
- **`ContextOverlayColor`.** `Theme{color=true}` ⇒ the segment glyphs are wrapped
  in the pinned colors (assert the SGR sequences for Blue/Green/Magenta/Cyan).
- **`ContextOverlaySmallTerminal`** (`view == 0`, empty `note`).
  `TerminalSize{52, 10}` ⇒ `inner_width = 50` and
  `rows = clamp(10 − 10, 0, 12) = 0`, so no grid rows; the overlay renders the
  **fallback**: window border (2) + title/status (1, `… @<seq>`) + body (the six
  legend rows) + the exact hint `terminal too small for the grid (need >= 40x10)`
  (47 chars) = **10 rows exactly** — the hint is the bottom line, so the totals
  row and any separator/rule row are absent (CTX11/CTX-F8, L6). **Row
  arithmetic:** 10 content rows at `{52, 10}` fill the height exactly, and the
  widest row is the 47-char hint ≤ `inner_width = 50`; the 42-char title/status
  row also fits. The size is deliberately `≥ 49 × 10` so the hint is not clipped.
- **`ContextOverlaySmallTerminalNote`** (new, M1). The same `TerminalSize{52, 10}`
  with a non-empty `note` (e.g. `budget unknown`) ⇒ the fallback is still exactly
  10 rows, and the **bottom line is the note verbatim**, replacing the hint;
  `threshold`/`reserve` and the totals row are absent. This is the golden that
  pins the CX-D12 bottom-line precedence in fallback mode.
- **`ContextOverlaySmallTerminalInventory`** (`view == 1`, M1, empty `note`). The
  same `TerminalSize{52, 10}` with the overlay toggled to the inventory view ⇒ the
  fallback is still **exactly 10 rows**: window border (2) + title/status (1) + a
  **six-row inventory viewport** (the legend is **not** drawn) + the bottom-line
  hint (1). Assert the inventory's first rows appear, the legend rows do **not**,
  the viewport is exactly six rows (blank-padded if shorter), and the hint is the
  bottom line; scrolling moves the six-row window. This is the M1 regression test
  that the `view == 1` fallback never exceeds the 10-row chrome.
- **`ContextOverlayBudgetUnknown`.** `window_tokens == 0` ⇒ `budget unknown`
  appears, no `%` column, the **title/status row** reads exactly
  `used tokens (budget unknown)  @<seq>` and no other header row exists, the
  **bottom line is the note** `budget unknown` (the totals row is displaced,
  §5.1 CX-D12), and the grid is full with no free cells (CTX-F3, L3, C3, M1).
  **Row arithmetic:** the same 20-row `{66, 20}` layout as the grid golden
  (border 2 + title/status 1 + grid 10 + legend 6 + bottom line 1); the note
  replaces the totals row, so the row count is unchanged.
- **`ContextOverlayOverBudget`.** `used_tokens > window_tokens` (e.g. `70000` /
  `64000`; `note` empty, so the totals row is the bottom line) ⇒ the totals row
  shows the computed `109.3%` and the literal string `>100%` appears nowhere;
  `FreeSpace` has no cells (CTX-F4, C6). **Row arithmetic:** the same 20-row
  `{66, 20}` layout; the 62-char totals row ≤ `inner_width = 64`.
- **`ContextOverlayInventory`.** `view == 1` at `TerminalSize{66, 20}` ⇒ the
  inventory viewport (the six-row body, §5.1/M1) renders the pinned server row
  `alpha  ready  tools=2` and the pinned tool row
  `mcp.alpha.search  mcp  ~48 tok`; the body is exactly six rows (blank-padded
  when the list is shorter); a server with `has_error` renders a
  trailing `  !`; no error text appears anywhere; scrolling moves the window
  (CTX-F7, L7, M8). **Row arithmetic:** `2 + 1 + 10 + 6 + 1 = 20` rows = the
  terminal height; the server/tool rows (20 and 31 chars) ≤ `inner_width = 64`.
- **Purity probe.** A snapshot with a sentinel in a message body must never appear
  in the rendered string (CTX8).

### 10.3 Model tests (`tests/unit/ui_model_test.cpp`)

- **Command registration.** `find("context") != nullptr`; `complete("con")`
  includes `/context`; `complete("co")` still includes `/compact` (no regression,
  cf. the RB-08 completion tests).
- **Dispatch.** `dispatch("/context")` invokes `CommandContext::context` exactly
  once and appends no transcript entry; an unknown `/ctx` still emits the
  "unknown command" notice.
- **Overlay state.** `open_context`-equivalent model mutation sets
  `mode == UiMode::Context`, `context.open == true`, and marks only the aggregate
  dirty bit.

### 10.4 Integration tests

- **`tests/unit/host_runtime_test.cpp`** (fake host, `FakeLLM`): create a session,
  run one prompt, call `showContext`; assert `used_tokens > 0`, `Conversation.items
  > 0`, and `captured_sequence == store head`. Then force a compaction and assert
  `CompactionSummary.items == 1` and `Conversation` drops.
- **`tests/integration_mcp_test.cpp`** (`fake_mcp_server`): start one fake server
  with two tools; assert `mcp_servers` contains one entry with `state == "ready"`,
  `tool_count == 2`, and `has_error == false`, and that `McpToolSchemas.items == 2`
  while those two names appear in `tools` with `provenance == "mcp"` (CTX5).
  Force a reconnect failure and assert `has_error == true` with **no error text**
  in the serialized reply (M8).
- **`tests/unit/transport_protocol_test.cpp`**: `context.show` is present in
  `all_methods()` and `is_method_allowed` returns true for both `Interactive` and
  `Automation` (read-only). The size assertion is set to the **relative** `C + 1`
  (`C` = the catalog size immediately before this errata lands; re-count
  `kMethodCatalog`, §3.4.1). At the audit baseline `C = 32` (`32u`), so the
  expected value after this lands is `33u` — but the implementer must recompute
  `C + 1` and **never hard-code a specific landing total in this errata** (M10).
- **`tests/unit/supervisor_connection_test.cpp`**: a `context.show` request
  round-trips through the fake transport and the reply callback runs on the pump
  thread.
- **Late-reply guard (M9)** (new `tests/unit/ui_context_overlay_test.cpp`, or an
  added `supervisor_connection`/controller test): (a) `open_context()` then
  `close_context()` then deliver the reply ⇒ the overlay stays closed and
  `mode != UiMode::Context`; (b) `open_context()` twice then deliver the **first**
  reply ⇒ it is dropped and the second reply wins; (c) `Esc` between the pump
  callback and the `enqueue` action ⇒ the UI-thread check drops it. Run this test
  in the TSAN configuration (`-DYMH_TSAN=ON`): the pump thread and the UI thread
  both touch `context_generation_`, and the run must be race-free (C1).

### 10.5 PTY tests (`tests/unit/ui_supervisor_pty_test.cpp`)

Reuse `PtyChild` (`:136`) and the `HelpListAndHistoryRecall` pattern (`:364-421`).

- **`ContextOverlayOpensAndCloses`.** Spawn the supervisor, type `/context` +
  Enter; assert the rendered PTY output contains `system prompt`, `window`, and
  the freshness field `@` (the `captured_sequence` slot, §5.3 M1); press `Esc`;
  assert the overlay is gone and the input line is restored.
- **`ContextOverlayShowsNote`** (new, M1). With an **unconfigured** window
  (`agent.compaction.context_window_tokens` unset ⇒ `window_tokens == 0`), type
  `/context` + Enter; assert the PTY shows the title/status row
  `used tokens (budget unknown)  @` and the bottom line `budget unknown` (the
  note slot, §5.1 CX-D12). This is the PTY assertion for the note row.
- **`ContextShowsMcpInventory`.** With a configured fake MCP server, `/context`
  then `t` shows the server id and `tools=`; `g` returns to the grid.
- **`ContextRefreshKey`.** `r` re-issues `context.show` and does not change the
  overlay's `captured_sequence` unless the store advanced. The PTY spawns a real
  `ymh` daemon (there is no fake transport on this path), so the test asserts
  **PTY output only**: after `r` the overlay is still drawn (grid/legend present)
  and the `@<seq>` field in the title/status row (§5.3) is unchanged. The
  **request-count**
  assertion — that `r` emits exactly one `context.show` — belongs in the hermetic
  `tests/unit/supervisor_connection_test.cpp` (`ScriptedServer` counts the
  requests, §10.4), not here.

### 10.6 Failure-mode coverage matrix

| Mode | Test |
|---|---|
| CTX-F1 | `host_runtime_test`: `showContext` on an unknown id throws `AppCode::UnknownSession` / `"UnknownSession"` (not `InvalidParams`) |
| CTX-F2 | `supervisor_connection_test`: failed reply ⇒ overlay `note` set (bottom-line slot, §5.1 CX-D12) |
| CTX-F3 | `context_snapshot_test` + `ContextOverlayBudgetUnknown` (title/status row exact + note on the bottom line) |
| CTX-F4 | `context_snapshot_test` over-budget case + `ContextOverlayOverBudget` (computed `%`, no `>100%`) |
| CTX-F5 | `host_runtime_test`: compare estimate with FakeLLM usage (documented divergence) |
| CTX-F6 | `mcp_manager_test`: concurrent `statuses()`/`refresh()` under TSAN (all `slots_` accesses locked, none across a blocking client call, and no `bus_.publish` under the lock, §3.5 C8); `context_snapshot_test` `mcp_available=false` ⇒ note + empty section |
| CTX-F7 | `context_snapshot_test` truncation + `ContextOverlayInventory` |
| CTX-F8 | `ContextOverlaySmallTerminal` (view 0) + `ContextOverlaySmallTerminalNote` (note replaces hint) + `ContextOverlaySmallTerminalInventory` (view 1, M1) |
| CTX-F9 | `ContextOverlayMonochrome` |
| CTX-F10 | `ContextRefreshKey` (title/status `@<seq>` field) + `ContextOverlayOpensAndCloses` (PTY `@`) + late-reply guard (M9) |
| CTX-F11 | `host_runtime_test`: snapshot during a maintenance turn |
| CTX-F12 | `context_snapshot_test` sentinel + `ContextOverlayGridGolden` purity probe |

### 10.7 Invariant coverage

| Invariant | Tests |
|---|---|
| CTX1, CTX3 | builder unit tests; `host_runtime_test` asserts the store event count is unchanged across `showContext` (no append) |
| CTX2 | `host_runtime_test`: call `showContext` concurrently with an active turn under TSAN; the store-read path is race-free (the builder never touches `Session`) |
| CTX4 | estimate-vs-usage divergence test |
| CTX5 | partition test; MCP integration test; truncation test asserts segment `items` stay full |
| CTX6 | budget-unknown tests (`ContextOverlayBudgetUnknown` asserts the title/status row and the note bottom line; `ContextOverlaySmallTerminalNote` asserts the fallback note) |
| CTX7 | truncation test; reply-size bound |
| CTX8 | message/prompt sentinel tests **and** an MCP-error-text sentinel test (M8) |
| CTX9 | determinism test; golden render |
| CTX10 | monochrome golden |
| CTX11 | small-terminal goldens (exact hint and note bottom line, both views: `ContextOverlaySmallTerminal` + `ContextOverlaySmallTerminalNote` + `ContextOverlaySmallTerminalInventory`, M1/L2) |
| CTX12 | PTY open/close; `handle_event` ordering test; late-reply generation guard (M9) run under TSAN (atomic `context_generation_`, C1) |

---

## 11. Amendment register

| ID | Amends (clause) | Additive surface |
|---|---|---|
| CX-01 | 10 §8.1 (`:908-936`) | `ContextOverlay` under `OverlayManager` |
| CX-02 | 10 §8.2 (`:937-973`) | `render_context_overlay` + file-local grid/legend/inventory helpers |
| CX-03 | 10 §9.2 (`:1044-1063`) | overlay-local keys; no new global binding |
| CX-04 | 10 §4.1/§4.3 (`:326-417`) | `UiModel::context` (`ContextOverlayModel`) |
| CX-05 | 10 §5.1 (`:632-678`) | none — the snapshot is an RPC reply, not a `UiEvent` |
| CX-06 | 13 §6.7 (`:993-1143`) | sibling `/context` command; compaction semantics unchanged |
| CX-07 | 13 §6.8 (`:1144-1293`) | additive `context.show` RPC |
| CX-08 | 04 (`WorkspaceRuntime`) | `estimator()`, `compaction_policy()`, `mcp_statuses()` accessors |
| CX-09 | 15 (MCP) | additive internal **`mutable`** `McpManager` mutex around `slots_`; all `slots_` accesses are lock-held, never across a blocking client call, and the bus is published after the lock is released (implementation only, §3.5 C8) |
| CX-10 | 06 (agent loop) | none — pure consumer; threshold unchanged |
| CX-11 | 01 (session) | none — reuses the `deriveMessages` free function |
| CX-12 | 05/11 (catalog) | `context.show` **added as one entry** to `kMethodCatalog` (size is relative to landing order, §3.4.1); both profiles |
| CX-13 | 05 §1.4/§7.3 (transport seam) | additive `TransportHost::showContext(const SessionId&)` pure virtual (`include/ymh/transport/host.hpp`) + the `HostRuntime` and `tests/support/fake_transport_host.hpp` overrides; a read-only view alongside `showSession` (§3.4) |

**Untouched:** 10 §3/§6/§7/§10/§11; 13 §3/§4/§5/§8.2; all of 16; the `F1–F12`
shared set.

---

## 12. Decisions, rejected alternatives, open questions

### 12.1 Decisions (pinned by this errata)

- **CX-D1** — The snapshot is produced **daemon-side** by a pure builder (§3.1).
- **CX-D2** — The message list is the assembler's own `deriveMessages`
  projection over a store read; the snapshot never touches a live `Session`
  (§3.2, §3.5, CTX2).
- **CX-D3** — The boundary crossing is one additive RPC `context.show` returning
  JSON, permitted in both profiles because it is read-only (§3.4).
- **CX-D4** — `/context` is a **modal overlay**, not a transcript entry (§4.2).
- **CX-D5** — The grid maps the **window** (or used tokens when the window is
  unknown) onto cells by `lo = c · grid_total / cells_total`; glyphs are drawn in
  both modes (§5.1, §5.2).
- **CX-D6** — MCP tools are reported as part of the tool schemas, partitioned by
  the `mcp.` prefix; there is no separate MCP tool list (§6.1, CTX5).
- **CX-D7** — No content egress: counts, labels, names, and statuses only (§6.4,
  CTX8).
- **CX-D8** — MCP status is made safe by an additive internal **`mutable`**
  `McpManager` mutex; every `slots_` access is lock-held (entry points acquire
  it, helpers never re-lock), it is never held across a blocking client call, and
  the status event is published only after the lock is released (C8); otherwise
  the accessor is owner-thread-only (§3.5, CTX-F6).
- **CX-D9 (M8)** — `McpServerStatus::last_error` is **not** shipped; the wire
  carries `has_error` only. It is unbounded `std::exception::what()`
  (`mcp_manager.cpp:300`) and the project forbids unbounded sensitive output on
  the wire (§6.2, CTX8).
- **CX-D10 (M9)** — Every `context.show` request carries a generation; a reply is
  applied only if its generation matches, and closing/refreshing bumps the
  generation. The counter is `std::atomic<std::uint64_t>` with relaxed ordering
  (C1) because it is written on the UI thread and read on the pump thread, and
  guards no other memory. This prevents a late reply from re-opening a dismissed
  overlay (§4.3, CTX12).
- **CX-D11 (M10)** — The catalog size is **relative to landing order**: this
  errata adds exactly one entry, so the size at landing is `C + 1` where `C` is
  the catalog size immediately before it lands (re-count `kMethodCatalog`; `C =
  32` at the audit baseline). It never pins an absolute total. The landing-order
  dependency is explicit in §3.4.1.
- **CX-D12 (M1)** — The overlay chrome is **exactly 10 rows** in every view and
  every size: border (2) + title/status (1) + body (6) + bottom line (1). The
  title/status row carries the budget title plus the freshness field
  `  @<captured_sequence>`; `note`, when non-empty, takes the bottom line and
  displaces the totals row (grid mode) or the fallback hint (fallback mode). No
  element ever adds an eleventh row; the inventory scrolls inside the six body
  rows and is blank-padded. The full element inventory is §5.1 (§5.3, §5.4).

### 12.2 Rejected alternatives

See §3.6 (supervisor-side assembly; overloading `session.show`; loop-published
cache; typed protocol DTO; shipping content) and §4.2 (transcript entry) and
§5.2 (color-only cells). Additional:

- **A percentage of provider-reported `Usage` as the "used" figure.** Rejected:
  `Usage` is per-call (last request), not the current projection, and is
  unavailable before the first turn; the snapshot must be reconstructible from
  the log alone.
- **Auto-refresh on every event.** Rejected (RPC spam, mid-render mutation);
  explicit `r` instead (§4.3).
- **A `context.grid` RPC that ships cell colors.** Rejected: presentation belongs
  in the frontend (U2/U3); the daemon ships data, not pixels.
- **Shipping a redacted MCP error reason on the wire (M8 alternative).**
  Rejected: any non-enum text is arbitrary `std::exception::what()` output and
  cannot be bounded reliably. If a reason is needed later, ship only
  `McpErrorCode` tokens (`to_string(error.code())`, a bounded enum,
  `mcp_manager.cpp:294`), never `what()`. The existing bounded 256-byte
  `mcp_status_detail` notice (`host_runtime.cpp:175-192`) remains the only
  free-text reason surface.
- **Applying a late reply unconditionally (M9 alternative).** Rejected: it
  re-opens a dismissed overlay. A generation counter is cheaper and has no
  ownership/`shared_ptr` lifetime burden.
- **Raising the chrome budget to add a dedicated `note` row (M1 alternative).**
  Rejected: an 11-row chrome shrinks the grid (`rows = height − chrome`) and
  breaks the pinned `{52, 10}` fallback (which fills exactly 10 rows), forcing
  every grid/golden figure to be re-pinned. Folding `captured_sequence` into the
  title/status row and `note` into the bottom line keeps the chrome at 10 and
  changes no existing golden (§5.1 CX-D12).

### 12.3 Open questions (adjudicated)

- **Q1 — Should the reserve band be drawn separately?** Pinned **no** for v1: the
  reserve is shown as text in the totals row. Rationale: a separate band adds a
  seventh segment and complicates the golden; it can be added later without
  changing the wire (the reserve is already a field).
- **Q2 — Should the threshold be drawn as a marker?** Pinned **no** for v1; it is
  text in the totals row. A marker would require an overlay line inside the grid
  and a second mapping rule.
- **Q3 — Is `context.show` Automation-safe?** Pinned **yes** (read-only). If a
  future reviewer decides Automation must not observe prompts at all, gating it
  to Interactive is a one-line change in `is_method_allowed` (`protocol.cpp:642-648`).

---

## 13. References

- `AGENTS.md` — the design-first gate.
- `docs/design/10-supervisor-tui.md` — presentation model, renderers,
  `OverlayManager`, keybindings, invariants U1–U18.
- `docs/design/13-context-compaction.md` — compaction policy, `ContextCompaction`
  event, `/compact` UI, additive RPC precedent.
- `docs/design/11-m2-errata.md`, `docs/design/05-transport.md` — the frozen
  method catalog.
- `docs/design/15-mcp-adapter.md` — MCP status and tool registration.
- `docs/design/16-daemon-ownership.md` — daemon lifetime/ownership (untouched).
- `docs/design/01-session.md` — `deriveMessages` projection.
- `docs/design/06-agent-loop.md` — assembly and the compaction trigger.
- `docs/design/04-workspace-host-daemon.md` — `WorkspaceRuntime`.
- `docs/design/UI_SURFACE_INVENTORY.md` — current pixels.
- `docs/design/17-ui-transcript-errata.md` — the errata style this document follows.
