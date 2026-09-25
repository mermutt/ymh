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
- **Phase-2 features are implemented** (specs `13`–`15`, all Oracle-verified):
  context compaction (`13`), the PTY capability behind the execution-environment
  seam (`14`), and the MCP adapter into the shared tool registry (`15`).
- **Spec `16-daemon-ownership.md` is implemented** (8 waves). Daemons are
  **supervisor-owned**: the last supervisor's clean exit prompts and tears the
  daemons down, and every crash path is backstopped by a daemon-side owner
  watchdog. It **supersedes** `04` H8/H9 and §3.2/§6.5/§14.1(f), `00` §54 D23 and
  §9.8/§9.9, `10` §2.1, `11` §8.2 D20.3, and amends `03`/`04`/`05` (A15/A16) with a
  new `supervisors` registry table (schema 1→2). Invariants `O1–O22`, failure modes
  `O-F1–O-F16`. Verified over 5 gate rounds by an adversarial critic plus Oracle
  (both PASS, zero open HIGH/MEDIUM), then implemented and verified live with two
  supervisors against real DeepSeek.
- **Architecture facts below describe the SHIPPED code**; where they conflict with
  `16-daemon-ownership.md` about daemon lifetime and ownership, spec 16 wins (the
  facts below are updated for it).
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
  cmark-gfm; CMake **FetchContent** for FTXUI / Asio / CLI11 / GoogleTest.
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
  workspace, owns its cwd, is **supervisor-owned** — the last supervisor's exit
  tears it down, with a daemon-side owner watchdog backstopping every crash path;
  see `16-daemon-ownership.md`). `ymh` (no args) attaches/spawns
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
- **Workspace vs session, in the UI.** A **workspace** is a live daemon plus its
  durable row in `registry.db`; a **session** is stored per-workspace event-log
  history on disk (`<workspace>/.ymh/sessions.db`). The **Ctrl-S switcher is
  live-only** (live daemon with `Attached`/`Stopping`; entries are evicted when
  the daemon dies; `NotRunning`/`Unreachable` never render), ordered
  effective-root-first, then last-usage descending (`max(sessions.updated_at)`),
  tie-broken by title ascending (case-insensitive) then `canonical_path` (57-D5,
  superseding 22-D1's workspace-order half). **`/sessions`** lists
  stored sessions read **directly from disk** for every **registered** workspace,
  live or not (no daemon needed; orphan DBs are not scanned). Selecting a session
  in a non-running workspace spawns/attaches its daemon then resumes it, and a
  failure surfaces a status-bar notice, never a phantom workspace. **`ymh
  --resume <id>` works in TUI mode** (unknown id → exit 1). See
  `docs/design/22-switcher-sessions-errata.md`.
- **Config is JSONC; TOML is retired.** The sole config file is `config.jsonc`
  in both slots: `$XDG_CONFIG_HOME/ymh/config.jsonc` (else
  `$HOME/.config/ymh/config.jsonc`) and `<workspace>/.ymh/config.jsonc`. ymh has
  **no TOML awareness** — a leftover `config.toml` is invisible (never read,
  probed, or warned about), and `toml++` is not a dependency. The **global layer
  is required** (empty/absent/non-regular ⇒ `ConfigError`, exit 2); the workspace
  layer is optional. First run scaffolds only the conventional global path: an
  explicit `--config <path>` is **never** auto-created. Config loads only for
  `ymh`, `ymh run`, `ymh list`, `ymh show`, `ymh replay`, `ymh fork`;
  `workspace`/`config`/`version` never load it. The supervisor passes its
  effective path to the daemon (`ymh --host … --config <path>`). See
  `docs/design/21-config-jsonc-errata.md` §6.
- **Logging.** Never dump full prompts or sensitive tool output to normal logs by
  default; the session event log is the authoritative trace.

## Conventions

- Modern C++23; **CMake + Ninja**. Core deps: FTXUI, Asio, SQLite3,
  nlohmann/json, spdlog, CLI11. Add cmark-gfm / tree-sitter / libgit2
  only when the feature arrives. Do not build a giant umbrella dependency.
- **No warning/error suppression**: no `-w`, no blanket
  `#pragma GCC diagnostic ignored`, no casts/`reinterpret_cast` used to silence
  the compiler.
- Each component spec must contain: C++ interface sketches, invariants,
  F#-tagged failure modes (F1–F12, §54), dsh mapping, and a test plan. Match the
  style of `00-architecture.md`. **Every non-mirror cell in the dsh-mapping
  table carries its justification with a concrete anchor (56-D6).**
- **Fidelity divergences carry a reason.** When a spec records a dsh (or other
  reference) **non-mirror** — a capability deliberately not mirrored — it must
  state *why* the divergence is acceptable (an architectural absence, an
  unbuilt feature with its seam and missing consumer, or a deliberate scope
  decision with a citation), not merely record that it is not mirrored.
  "Recorded as not mirrored" is insufficient; the reason goes inline in the
  dsh-mapping table's non-mirror cell **and cites a concrete anchor** — a
  decision ID (`00` §55's omitted list, a `55-OQ-n`, a `52-D15`, …) or a
  `file:line`. A cell whose reason restates the omission ("not in scope",
  "coarser") or cites nothing is a finding, not a disposition: independent
  review **must log it** and the spec **cannot be marked `verified`** while it
  is open — the rule applies **before a spec is marked `verified`**. See
  `56-dsh-fidelity-gap-closure-errata.md` §3 (56-D6) for the convention and
  worked examples.
- Work targets **one component at a time**.
- **Commit when a coherent change is done** — build green, full suite green,
  warnings-as-errors clean. Keep commits atomic and scoped to one logical change,
  with a concise message in the repo's style. Never commit secrets, and never
  commit `requirements_draft.txt` (untracked by design).

## Testing

Strategy is §44: unit tests, integration tests (fake LLM / fake FS / fake shell),
golden TUI render tests, and replay tests. A deterministic **Fake LLM (§45)**
backs the hermetic suite. **Live tests** (real DeepSeek) are opt-in via
`YMH_LIVE_LLM=1`; they drive the real binary under a PTY.

## Immediate next steps

Milestone 2 is complete and green (supervisor + per-workspace daemons, registry,
Unix-socket JSON-RPC). **Remote SSH/TCP transport is out of scope for this
project** (DECISION, user, 2026-09-17): it is not deferred and not a candidate.
Remaining candidates: multi-workspace switcher / aggregate-flash polish, and
**LSP tools (§28) at the lowest priority** (not dropped). MCP and PTY (Phase 2)
are implemented. See `HANDOFF.md` §9.
