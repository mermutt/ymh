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

## Current state (Milestone 1 MVP implemented)

- The design is **verified**: `docs/design/00-architecture.md` + component specs
  `01`–`10`, tracked in `docs/design/DESIGN_STATUS.md`.
- The **Milestone 1 single-process MVP is implemented** in `include/ymh/` +
  `src/` with tests in `tests/`: core event bus, session event log + SQLite
  persistence/lease, LLM provider (OpenAI-compatible, DeepSeek default) +
  `FakeLLM`, execution environment + tools (read/write/edit/grep/glob/shell/git),
  permissions, agent loop, CLI + headless (`ymh run`), FTXUI TUI, and
  markdown/syntax/diff rendering.
- The **Milestone 2 split is implemented**: supervisor TUI + per-workspace
  `WorkspaceHost` daemons, shared `registry.db`, and JSON-RPC 2.0 over a
  length-prefixed Unix domain socket. `ymh` (no args) attaches to / spawns the
  cwd workspace daemon; `ymh --host …` is the daemon entry. See
  `docs/design/11-m2-errata.md` (the frozen M2 interfaces) and
  `docs/design/12-m1-drift-errata.md`.
- `00-architecture.md` is the single authoritative architecture source; the raw
  `drafts/` files were merged into it and are optional reference only.

## Build & test (exact commands)

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

- Warnings are errors (`-Wall -Wextra -Wpedantic -Werror`) — never suppress them.
- Deps: system SQLite3 / nlohmann_json / spdlog / fmt / libcurl / libgit2 /
  cmark-gfm; CMake **FetchContent** for FTXUI / Asio / toml++ / CLI11 / GoogleTest.
- **Live tests** (real LLM) are opt-in: set `YMH_LIVE_LLM=1` and
  `DEEPSEEK_API_KEY` (the key lives in `~/.apikey.deepseek`, an
  `export DEEPSEEK_API_KEY=…` snippet); they skip otherwise. Model
  `deepseek-flash`, `reasoning_effort=low`.
- `ymh run "<task>"` is the headless path; `ymh` (no args) launches the TUI.

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

- **Two milestones, both implemented.** Milestone 1 = single-process MVP;
  Milestone 2 = supervisor TUI + N × `WorkspaceHost` daemons (one daemon per
  workspace, owns its cwd, survives TUI exit). `ymh` (no args) attaches/spawns
  the cwd daemon; `ymh --host …` is the daemon entry.
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

## Testing

Strategy is §44: unit tests, integration tests (fake LLM / fake FS / fake shell),
golden TUI render tests, and replay tests. A deterministic **Fake LLM (§45)**
backs the hermetic suite. **Live tests** (real DeepSeek) are opt-in via
`YMH_LIVE_LLM=1`; they drive the real binary under a PTY.

## Immediate next steps

Milestone 2 is complete and green (supervisor + per-workspace daemons, registry,
Unix-socket JSON-RPC). Next candidates: remote SSH/TCP transport (§47 Mode B),
MCP/LSP/PTY (Phase 2, §51), and multi-workspace switcher / aggregate-flash
polish. See `HANDOFF.md` §9.
