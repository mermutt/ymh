# AGENTS.md — ymh

C++23 terminal coding-agent harness (Claude Code / OpenCode-class), built on the
DeepSeek Harness (dsh) architecture. Terminal-first, SSH-friendly, low-overhead,
model-provider agnostic. **Design-first workflow.**

## The rule (non-negotiable)

DO NOT write implementation code for any component until its design spec is
`verified`. Two gates, in order:

1. **Top-level gate** — `docs/design/00-architecture.md` is complete and
   independently verified (no open HIGH/MEDIUM findings).
2. **Component gate** — the component's spec is complete and verified *before*
   any code for that component.

A spec is `verified` only after independent review (Oracle / review team), with
no open HIGH/MEDIUM findings and pinned interface sketches. Loop:
**write → review → fix → re-check → mark verified → code.**

## Current state (repo is design-only)

- Only `HANDOFF.md` and `docs/design/00-architecture.md` exist. There is **no**
  `src/`, `tests/`, `CMakeLists.txt`, or build/test/lint command yet — do not
  invent one.
- `docs/design/DESIGN_STATUS.md` (the written/verified tracker) is specified in
  `HANDOFF.md` §6 but **not yet created**. Create it from that template before
  or while starting spec work.
- `00-architecture.md` is the single authoritative architecture source; the raw
  `drafts/` files were merged into it and are optional reference only.

## Reading order

1. `HANDOFF.md` — operating instructions, the rule, component spec plan, open items.
2. `docs/design/00-architecture.md` — the architecture baseline (~5.2k lines).
3. `docs/design/DESIGN_STATUS.md` — per-spec tracker (create if missing).

## Naming (resolved)

The project, CLI binary, state dir, per-workspace dir, source tree, and namespace
are all **`ymh`**: binary `ymh`, state `~/.local/state/ymh/`, per-workspace
`<workspace>/.ymh/`, source tree `ymh/`, `include/ymh/`, namespace `ymh::`.
The earlier working name `txtcoder` is **retired** — do not reintroduce it.

## Architecture facts an agent will get wrong

- **Two milestones, not one topology.** Final target = supervisor TUI + N ×
  `WorkspaceHost` daemons (one daemon per workspace, owns its cwd, survives TUI
  exit). But MVP is a **single-process monolith** (Milestone 1). The daemon split
  is Milestone 2 and is *not optional* — §57 Step 13 / §58 define the fork point.
- **Sessions are event-sourced.** The append-only typed event log is the durable
  source of truth; the TUI is one consumer of the event stream, not the agent.
- **Transport** is JSON-RPC 2.0 over a **length-prefixed Unix domain socket**
  (Interactive + Automation profiles). Not stdio, not TCP — stdio is
  testing/debug only; TCP/SSH is deferred.
- **Path safety.** Never `chdir()` cross-session. A daemon chdirs to its
  workspace root once at startup; every tool/LSP/git/subprocess path goes through
  `ExecutionEnvironment::resolve()`, root-relative with realpath canonicalization.
  `getcwd()` is never a resolution base in tool code. The environment-root
  parameter must be baked into the §18 interface **before any tool is written**.
- **Persistence split.** Shared `${XDG_STATE_HOME}/.../registry.db` (open-set +
  host registration, single `flock` writer, WAL readers) is separate from each
  workspace's session DB. The focused/active session is supervisor-local state,
  not registry state.
- **Logging.** Never dump full prompts or sensitive tool output to normal logs by
  default; the session event log is the authoritative trace.

## Conventions

- Modern C++23; **CMake + Ninja**. Core deps: FTXUI, Asio, SQLite3,
  nlohmann/json, toml++, spdlog, CLI11. Add cmark-gfm / tree-sitter / libgit2
  only when the feature arrives. Do not build a giant umbrella dependency.
- **No warning/error suppression**: no `-w`, no blanket
  `#pragma GCC diagnostic ignored`, no casts/`reinterpret_cast` used to silence
  the compiler.
- Each component spec must contain: C++ interface sketches, invariants,
  F#-tagged failure modes (F1–F12, §54), dsh mapping, and a test plan. Match the
  style of `00-architecture.md`.
- Work targets **one component at a time**.
- No commit unless explicitly requested.

## Testing (specified, not yet built)

Strategy is §44: unit tests, integration tests (fake LLM / fake FS / fake shell),
golden TUI render tests, and replay tests. A deterministic **Fake LLM (§45)** is
required for agent-loop tests. Nothing is implemented yet.

## Immediate next steps

See `HANDOFF.md` §9: create `DESIGN_STATUS.md`, close the §5 top-level open
items in `00-architecture.md`, re-verify the top-level doc, then write
`01-session.md` (session first — it is the spine) before any `src/` code.
