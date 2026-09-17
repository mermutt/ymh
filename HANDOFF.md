# ymh — Handoff & Design-First Policy

**Move this file into `~/prjs/github/ymh/` as `HANDOFF.md`** (along with the files listed in §3). It is the operating instructions for the repo until coding starts.

---

## 1. What this project is

**ymh** — a native C++23 terminal coding-agent harness (Claude Code / OpenCode-class), built on the DeepSeek Harness (dsh) architecture. Terminal-first, SSH-friendly, low-overhead, model-provider agnostic, plugin-extensible.

Core architecture (locked, dsh-aligned):

```
Supervisor (TUI) process
   └── N × WorkspaceHost daemons   (one daemon per project/workspace, owns its cwd)
        └── in-process Sessions     (conversations), Agents, subagents, tools
             └── shared WorkspaceRegistry (registry.db) + per-workspace session DBs
```

- **Workspace** = project = one daemon process (own cwd, **supervisor-owned** —
  the last supervisor's exit tears it down, per `16-daemon-ownership.md`).
- **Session** = conversation inside a workspace (immutable `SessionHeader.cwd`).
- **Shared registry** (`registry.db`): multi-row `workspaces` + `workspace_sessions`, `flock` single-writer, boot-nonce liveness.
- **Transport**: JSON-RPC 2.0 over length-prefixed Unix socket (Interactive + Automation profiles).
- **UI**: tree switcher (workspaces → sessions), aggregate status line (`2 active · 3 waiting`), ~1s edge-triggered flash.

---

## 2. The rule (non-negotiable)

> **No implementation code until the design is complete AND verified — at the top level first, then per component.**

Two gates, in order:

1. **Top-level gate** — the architecture document is complete and independently verified (no open HIGH/MEDIUM findings).
2. **Component gate** — each component's design spec is complete and verified **before any code is written for that component**.

Coding a component is permitted *only* after its spec is marked `verified` in `DESIGN_STATUS.md`.

---

## 3. Files to move into the repo

| Source (in this vault / /tmp) | Destination in `~/prjs/github/ymh/` |
|---|---|
| `YMH_HANDOFF.md` (this file) | `HANDOFF.md` |
| `cpp_coding_harness_design.md` | `docs/design/00-architecture.md` |
| *(optional)* `/tmp/opencode/dsh-design/audit.md` | `docs/design/drafts/audit.md` |
| *(optional)* `/tmp/opencode/dsh-design/core-ipc.md` | `docs/design/drafts/core-ipc.md` |
| *(optional)* `/tmp/opencode/dsh-design/ui.md` | `docs/design/drafts/ui.md` |

The three `drafts/` files are **already merged** into `00-architecture.md`; move them only if you want the raw team output for reference. The architecture doc is the single authoritative source.

---

## 4. Repo layout (proposed)

```text
ymh/
├── HANDOFF.md
├── AGENTS.md                      (operating manual — see §8)
├── docs/
│   └── design/
│       ├── 00-architecture.md     (the architecture baseline)
│       ├── 01-session.md          (to be written)
│       ├── 02-persistence.md
│       ├── 03-workspace-registry.md
│       ├── 04-workspace-host-daemon.md
│       ├── 05-transport.md
│       ├── 06-agent-loop.md
│       ├── 07-tools-execution.md
│       ├── 08-llm-provider.md
│       ├── 09-permissions.md
│       ├── 10-supervisor-tui.md
│       ├── DESIGN_STATUS.md       (tracker — see §6)
│       └── drafts/                (optional reference)
├── src/                           (created per-component, only after that spec is verified)
└── tests/
```

---

## 5. Top-level open items to close (before the top-level gate passes)

These were flagged as unresolved during the design pass and must be decided/closed:

1. **Naming** — **RESOLVED**: `ymh` is final for the project, CLI binary, state
   dir (`~/.local/state/ymh/`), per-workspace dir (`<workspace>/.ymh/`), source
   tree (`ymh/`, `include/ymh/`), and namespace (`ymh::`). The working name
   `txtcoder` is retired.
2. **Registry bootstrap discovery** (§9.10) — **RESOLVED**: first-run discovery
   walks the configured `workspace_roots` (default `["$HOME/prjs"]`, bounded
   depth 4, denylist), imports every `<dir>/.ymh/sessions.db`, and optionally
   migrates a legacy central store once. A process scan is not a discovery
   source.
3. **`SessionHeader` finalization** — **RESOLVED** (§9.2/§9.10): fields are `id`, `cwd`, `createdAt`, `updatedAt`, `title`, `model`, `serverProfile`, `kind` (root|fork|subagent), `parentSession?`, `seedLength?`, `metadata?`. No boot nonce in the header (liveness lives in the lease / host registration). `ordinal`/`archived` stay in the registry junction.
4. **Remote/SSH transport (TCP)**: deferred. Decide when §47 Mode B (local TUI + remote daemons) is in scope; the JSON-RPC protocol is transport-agnostic, so this is a scheduling decision, not a design blocker.
5. **Deferred dsh-items** (accepted for v1): no offline session cache (live host required); host-level events merged into the mux; no hot plugin reload (dynamic libs deferred).

Close these by editing `00-architecture.md`, then re-run the verification gate (§7) on the whole top-level doc.

---

## 6. Component design plan (the bulk of remaining design work)

Write one spec per component, in this order (dependency-ordered — **session first, it is the spine**):

| # | Spec | Covers (must include) |
|---|---|---|
| 01 | **Session & event log** | `Session` (append-only typed log), `SessionEventMap` + turn/step taxonomy, `deriveMessages()`, create/resume/fork/replay, `SessionHeader`. |
| 02 | **Persistence & write lease** | `SessionPersistence` seam, `SessionHandle`, `session_leases` (boot nonce, TTL, steal), `sessions/events/session_leases` schema, flush/checkpoint, crash recovery. |
| 03 | **WorkspaceRegistry** | shared `registry.db`, `workspaces` + `workspace_sessions` + `pending_mutation`, `flock` single-writer, WAL readers, heartbeat + boot nonce, bootstrap. |
| 04 | **WorkspaceHost daemon** | spawn/setsid/socket, attach/detach, supervisor-owned lifetime (16), crash/orphan handling, graceful shutdown. |
| 05 | **Transport & protocol** | JSON-RPC 2.0 over length-prefixed Unix socket, Interactive vs Automation profiles, full RPC method catalog, `SessionEnvelope`, event multiplexing, per-session ordered delivery. |
| 06 | **Agent & loop** | `Agent` handle (`dispose()`/`whenIdle()`), `AgentRegistry` create/resume transaction, inbox (`send`/`followup`/`steer`/`inject`), `AgentLoop`, scope. |
| 07 | **Tools & execution** | `ToolRegistry`, `ToolContext`, `ExecutionEnvironment` (rooted cwd, realpath canonicalization), sandbox modes, resource caps. |
| 08 | **LLM provider** | `LLMProvider` seam, streaming, model selection, provider adapters. |
| 09 | **Permissions** | `PermissionPolicy`, decision flow over transport, background permission policy. |
| 10 | **Supervisor/TUI** | `WorkspaceModel`/`SessionUiState`/`SessionEnvelope`, tree switcher, aggregate status + ~1s flash, FTXUI renderers + `TerminalLayer` (IXON), keybindings. |

Each spec must contain: **C++ interface sketches, invariants, failure modes (F#-tagged), dsh mapping, and a test plan.** Follow the style of `00-architecture.md`.

Track progress in `docs/design/DESIGN_STATUS.md`:

```markdown
# Design Status

| Spec | Written | Verified | Reviewer | Notes |
|---|---|---|---|---|
| 00-architecture | yes | in-progress | Oracle pass 1 (18 fixes applied) | close §5 open items |
| 01-session | no | — | — | |
| 02-persistence | no | — | — | |
| ... | | | | |
```

---

## 7. Definition of "verified"

A design artifact is **verified** when:

- an **independent reviewer** (Oracle / a review team) has read it and issued findings, **and**
- there are **no open HIGH or MEDIUM findings** (all resolved and re-checked), **and**
- the interface sketches are pinned (no further signature churn expected).

Only then does the component move from `docs/design/` to `src/`. The review loop is: **write → review → fix → re-check → mark verified → code.**

---

## 8. Suggested repo `AGENTS.md` (paste verbatim at repo root)

```markdown
# AGENTS.md — ymh

C++23 terminal coding-agent harness, dsh-aligned. Design-first workflow.

## The rule
DO NOT write implementation code for any component until its design spec in
docs/design/ is marked `verified` in DESIGN_STATUS.md. Top-level architecture
(00-architecture.md) must be verified first, then each component spec before
its code.

## Workflow
1. Read HANDOFF.md, then docs/design/00-architecture.md, then DESIGN_STATUS.md.
2. Design/implementation work targets a single component at a time.
3. Before coding a component, ensure its spec exists and is `verified`;
   if not, write/complete the spec and submit it for review first.
4. After significant design changes, update DESIGN_STATUS.md.

## Conventions
- Modern C++23; CMake + Ninja; FTXUI (UI), Asio (async), SQLite (persistence),
  nlohmann/json (wire), tree-sitter/cmark-gfm (rendering), libgit2 (selective).
- One daemon process per workspace; supervisor TUI attaches over Unix-socket
  JSON-RPC 2.0. Sessions are append-only event logs. Never `chdir()` cross-session;
  path resolution is root-relative with realpath canonicalization.
- No code before a verified design. No `as any`/`@ts-ignore`-style suppression.
```

---

## 9. Immediate next steps (in order)

**Milestone 1 MVP and Milestone 2 are implemented and green** — `include/ymh/` +
`src/` + `tests/`; `ctest` all passing (707 hermetic + 16 opt-in live); 0 orphaned
daemons. See `README.md` for build/run. The design is complete: specs `01`–`15`
verified plus `11-m2-errata.md` (frozen M2 interfaces) and `12-m1-drift-errata.md`.

Milestone 2 delivers the supervisor TUI + per-workspace `WorkspaceHost` daemons,
the shared `registry.db`, and JSON-RPC over a length-prefixed Unix socket.
`ymh` (no args) attaches to / spawns the cwd daemon; `ymh --host …` is the daemon
entry. Next candidates: remote SSH/TCP transport (§47 Mode B), MCP/LSP/PTY
(Phase 2, §51), and multi-workspace UI polish.

**Phase-2 features are now implemented too** (specs `13`–`15`, all Oracle-verified
and marked `verified` in `DESIGN_STATUS.md`): context compaction (`13`), the PTY
capability behind the execution-environment seam (`14`), and the MCP adapter into
the shared tool registry (`15`). Remaining candidates: LSP tools (§28), remote
SSH/TCP transport (§47 Mode B), worktrees, and multi-workspace UI polish.

**Spec `16-daemon-ownership.md` is implemented** (8 waves). It changes daemon
lifetime from "supervisor-independent" to **supervisor-owned**: the last
supervisor's clean exit prompts and tears the daemons down, and every crash path
is backstopped by a daemon-side owner watchdog. It **supersedes** `04` H8/H9,
`04` §3.2/§6.5/§14.1(f), `00` §54 D23 and §9.8/§9.9, `10` §2.1, and `11` §8.2
D20.3; it amends `03`/`04`/`05` (A15/A16) and adds a `supervisors` registry table
(schema 1→2). Invariants `O1–O22`, failure modes `O-F1–O-F16`. Gated over 5
rounds by an adversarial critic plus Oracle — **both PASS, zero open HIGH/MEDIUM**
(2737 lines) — then implemented and verified live with two supervisors against
real DeepSeek.

Two supporting registers were produced alongside it:
`REQUIREMENTS_BACKLOG.md` (RB-01–RB-11, triaged from `requirements_draft.txt` and
deduplicated against shipped code; six items need a spec/errata before code) and
`UI_SURFACE_INVENTORY.md` (the spec-16 ↔ RB-10/RB-11 UI seam; most
cross-supervisor visibility already ships, so spec 16's new UI is mainly the
last-supervisor exit prompt).
