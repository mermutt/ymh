# 50 — Skills/MCP & Session Lifecycle Errata: Claude-Code Skill/Command Discovery (`~/.ymh`, `$HOME/.claude`), the Fresh-Launch Session Guarantee, the MCP Child Environment, and the MCP Global-Layer-Only Guard

```
Status: **draft (Rev 2)** — awaiting the independent design gate. This spec
        amends the owning specs (20, 15, 10, 46, 21, 07); it introduces no new
        component and no new subsystem. Like 45 and 46, every "current state"
        claim is reproducible from the shipped tree (HEAD `bbf96aeaa`).

        **Origin.** This spec carries the higher-risk half of the original spec
        48, split out by user decision because 48's nine-item surface made each
        gate round oscillate. The split is recorded in `48-ui-and-config-errata.md`
        §15 (Rev 7). The TUI/rendering slice (the Esc-Esc interrupt, word motion,
        the caret, the styling hierarchy, rich tool lines, reasoning spacing)
        stays in 48. This spec owns: item 1 (skill/command discovery), item 3
        (the fresh-launch session guarantee), the MCP child environment fix
        (was 48-D9.1), and the MCP global-layer-only guard (was 48-D9.7).
        The original 48-D10 (MCP 401/403 hints) was **dropped by user decision**
        (the underlying problem was an expired credential, not a harness defect);
        the durable diagnostic procedure is preserved in §5.5.

        **Verification status: DRAFT — not yet reviewed.** No code may be written
        from this spec until an independent gate marks it `verified`
        (AGENTS.md, the rule).
```

## 1. Purpose, scope, and supersession map

### 1.1 In-scope requirements (verbatim, from `requirements_draft.txt`)

1. "Read skills and commands: from ~/.ymh and $HOME/.claude (config.json is
   always ~/.config/ymh/config.json)"
3. "Bug: Just now I interrupted work by pressing Ctrl+C and reentered ymh again.
   On reentering it put me into the previous workspace"

Plus two harness fixes carried from the item-9 investigation: the MCP child
environment (was 48-D9.1) and the MCP global-layer-only guard (was 48-D9.7).

### 1.2 What this changes, in one sentence

Skill and command discovery gains the Claude-Code roots (`~/.ymh`, then
`$HOME/.claude`) with first-run scaffolding and a pinned precedence; a fresh
launch opens a **new** session in the cwd workspace (reconciled with spec 49's
lazy spawn); MCP stdio children spawn with the ambient environment overlaid by
the configured map (today `src/execution/process.cpp:544-551` does
`::clearenv()`); and MCP server definitions become **global-layer only**, closing
a workspace-config supply-chain vector.

### 1.3 Supersession map

#### 1.3.1 Superseded

| ID | Prior text | Change |
|---|---|---|
| 50-S1 | `20-skills.md` §2.4 (`:228-231`) trust table and §3.1 (`:285-318`) search roots: user tier is `$XDG_CONFIG_HOME/ymh/skills` else `$HOME/.config/ymh/skills`; workspace tier is `<workspace>/.ymh/skills`; "No other roots are searched." | **Superseded by 50-D1.** The user tier gains `~/.ymh/skills` and `$HOME/.claude/skills`; the workspace tier is unchanged; the "no other roots" sentence is superseded for skills. The trust rule (user > workspace; user root must be absolute) is retained. |
| 50-S2 | `20-skills.md` §2.4 and §3.2 trust/precedence ("the user (trusted) skill always wins and the workspace skill is rejected"). | **Extended by 50-D1.4.** Both new roots are user-tier/Trusted; the intra-tier deterministic order becomes `~/.ymh` > `$XDG_CONFIG_HOME/ymh` > `$HOME/.claude`. |
| 50-S3 | `15-mcp-adapter.md` §4.1 (`:432-434`) `env` is `std::vector<std::string>` of `KEY=VALUE`. | **Retained, not superseded.** The loader also accepts the localcode **object** form and converts it (`src/config/config.cpp:658-679`); 50-D3 pins the child-side environment semantics, not the config shape. |
| 50-S4 | `07-tools-execution.md` §6.4 / `include/ymh/execution/process.hpp:26-39`: `ProcessRequest::environment` is an *additive* overlay on an inherited environment. | **Amended by 50-D3** for the `spawn()` path only: `spawn()` currently `clearenv()`s the child (`src/execution/process.cpp:544`), which contradicts the `run()` overlay semantics (`:386-388`) and the PTY overlay semantics (`src/execution/pty.cpp:446-457`). 50-D3 makes `spawn()` inherit-and-overlay; the PTY path is unaffected and already correct. |
| 50-S5 | `21-config-jsonc-errata.md` §7 and the loader: `mcp`/`mcp_servers` are accepted from **both** the global and workspace layers (`src/config/config.cpp:990-991`, `:1017-1018`). | **Superseded by 50-D4 (breaking).** MCP server definitions become **global-layer only**; a workspace layer that defines `mcp`/`mcp_servers` is a `ConfigError`. See §6 for the migration note. |
| 50-S6 | `46-permissions-ui-errata.md` 46-D7.1 (`:1528-1535`): "`if (!live.empty()) -> focus live.front() (retained)`". | **Superseded by 50-D2.** On a fresh launch (no `--resume`/`--new`) the active workspace **always** creates a new session; an existing live session is never auto-focused. `/sessions`, the Ctrl-S switcher, and `--resume` remain the only ways to open an existing session. |

#### 1.3.2 Not superseded (explicitly retained)

- 46-D7's `resume_in_flight_` gate and 22-D5/S4 resume semantics.
- Spec 16's daemon ownership (O1–O22) is unchanged; this spec does not touch it.
- 20-D2 (`Builtin` reserved), 20 §3.1 M2 (absolute-root rule).

### 1.4 Scope boundaries

**In scope.** Skill/command discovery roots + scaffolding + precedence; the
`~/.ymh`/`~/.claude` instruction-file candidates; the fresh-launch session
guarantee; the MCP stdio child environment; the MCP global-layer-only guard.

**Out of scope (recorded, not pinned).**
- Claude-Code plugin/marketplace discovery, `settings.json`, hooks, and
  `~/.claude.json`. Only `skills/`, `commands/`, and `CLAUDE.md` are read.
- Migrating or deleting `$XDG_CONFIG_HOME/ymh/skills`; it remains a root.
- Credential management for MCP servers (rotation, expiry, storage); §5.5 only
  documents how to validate a credential.
- A `/mcp test` connectivity probe.
- The user's JIRA server itself (item 9 resolved: an invalid credential).

### 1.5 Terminology (pinned)

- **Fresh launch** = `ymh` invoked with neither `--resume <id>` nor `--new`
  (and not an explicit `/sessions`/Ctrl-S selection).
- **MCP child** = the subprocess spawned by `StdioMcpTransport::start`
  (`src/mcp/mcp_transport.cpp:77-113`).
- **User tier / workspace tier** = the trusted vs untrusted skill/command roots
  (20 §2.4).
- **The env-asymmetry defect** = `spawn()`'s `::clearenv()` versus `run()`/PTY
  inherit-and-overlay; fixed by 50-D3. It had **no bearing on the jira 401**.

## 2. Amendment register

| Owning spec / file | Section | Amendment |
|---|---|---|
| `20-skills.md` | §2.4, §3.1, §3.2, §5.1 (`SkillSource`) | 50-D1: add `~/.ymh/skills` and `$HOME/.claude/skills` user roots; add `Home`/`Claude` provenance enumerators; pin precedence. |
| `20-skills.md` | §5.2 (`SkillCatalog` ctor), §5.6 | 50-D1: `SkillCatalogConfig` gains the root list; the catalog receives resolved absolute roots. |
| `src/prompt/instructions.cpp` / `include/ymh/prompt/instructions.hpp` | `global_instruction_path`, `InstructionFileConfig` | 50-D1: add `$HOME/.claude/CLAUDE.md` as a global instruction candidate. |
| `src/commands/command_registry.cpp` + new `src/commands/file_commands.cpp` | — | 50-D1: file-based command discovery from the four roots. |
| `46-permissions-ui-errata.md` | 46-D7.1 | 50-D2: remove the `live.front()` auto-focus; cwd-workspace startup guarantee (reconciled with spec 49). |
| `15-mcp-adapter.md` | §4.1/§5.6/§6.7 | 50-D3: MCP child environment policy. |
| `include/ymh/execution/process.hpp` | `ProcessRequest` | 50-D3: `env_mode` (inherit vs minimal, `Minimal` pinned) and `stderr_path` (spawn-only). |
| `21-config-jsonc-errata.md` | §7 (`mcp`) | 50-D3: MCP children always inherit (no `mcp.inherit_env`); `mcp.log_child_stderr` (default false), global-layer only. **50-D4 (breaking): `mcp`/`mcp_servers` are global-layer only; a workspace layer is a `ConfigError`.** |
| `src/config/config.cpp` | `apply_document` pre-scan (`:957-971`) and the `apply_mcp` call site (`:990-991`) | 50-D4: guard once at the existing `global_layer` sites; fail with `'mcp'`/`'mcp_servers' is global-layer only`. No new function parameter. |
| `15-mcp-adapter.md` | §4.1 (MCP child `cwd`) | 50-I21 (pinned): an MCP server's `cwd` is root-confined; an out-of-root `cwd` must surface as `McpError{ConfigInvalid}`, not an unhandled `PathEscape`. Empty `cwd` defaults to the workspace root (`src/mcp/mcp_transport.cpp:98-99`). |

---
## 3. D1 — Claude-Code-compatible skills and commands (`~/.ymh`, `$HOME/.claude`)

### 3.1 Current state (verified)

- **Skills.** `SkillCatalog::discover()` scans exactly two roots
  (`src/skills/skill_catalog.cpp:203-222`):
  - the user root, passed in by the daemon as `default_skills_root()`
    (`src/agent/workspace_runtime.cpp:42-51`), which is
    `default_global_config_path().parent_path() / "skills"`
    (`src/skills/skill_catalog.cpp:190-191`) —
    `$XDG_CONFIG_HOME/ymh/skills` else `$HOME/.config/ymh/skills`;
  - the workspace root `<environment-root>/.ymh/skills`
    (`src/skills/skill_catalog.cpp:221`).
  A relative user root disables the user tier (`:213-219`). Spec 20 §3.1
  (`:317-318`) says "No other roots are searched."
- **Commands.** Slash commands are **compiled-in** only. The UI registry is
  built by `CommandRegistry` and populated from code (`src/ui/command_registry.cpp`,
  `src/ui/supervisor.cpp` registration), not from disk. There is no
  `commands/*.md` discovery anywhere in `src/`.
- **Instructions.** `InstructionLoader` reads `AGENTS.md` and `CLAUDE.md`
  (`include/ymh/prompt/instructions.hpp:22`) from the project root down to the
  workspace root, plus **one** global file:
  `global_instruction_path(global_name)` where `global_name` is
  `candidates.front()` = `AGENTS.md` (`src/prompt/instructions.cpp:107-108`),
  resolved to `$XDG_CONFIG_HOME/ymh/AGENTS.md` else
  `$HOME/.config/ymh/AGENTS.md` (`:52-60`). There is **no** `$HOME/.claude`
  awareness and no `~/.ymh` awareness (repo-wide grep for `.claude` in `src/`
  and `include/` is empty; the only hit is `requirements_draft.txt`).
- **Scaffolding.** `scaffold_for_invocation` creates the conventional **global
  config** path only (`scaffold_for_invocation`, `src/cli/cli.cpp:183`; spec 21 §6). It never creates
  `~/.ymh` or any skills/commands directory.
- **Environment on this machine.** `~/.ymh` does not exist; `~/.claude` contains
  only `transcripts/` (verified `ls -la`). So item 1 cannot be satisfied by
  reading files that are already there.

### 3.2 Decision (50-D1) — USER REQUIREMENT

1. **Config-path correction (documentation, no behaviour change).** The spec,
   README, and CLI help must say `~/.config/ymh/config.jsonc`; ymh has **no**
   `config.json` awareness. This is already true in code
   (`default_global_config_path`, `src/config/config.cpp:1242-1250`) and is
   restated only to correct the user's belief. No loader change.

2. **Skill roots (pinned order).** `SkillCatalog` searches, in order:

   | # | Root | Provenance | Trust |
   |---|---|---|---|
   | 1 | `$HOME/.ymh/skills` | `SkillSource::Home` (new) | Trusted |
   | 2 | `$XDG_CONFIG_HOME/ymh/skills` else `$HOME/.config/ymh/skills` | `SkillSource::User` (retained) | Trusted |
   | 3 | `$HOME/.claude/skills` | `SkillSource::Claude` (new) | Trusted |
   | 4 | `<workspace>/.ymh/skills` | `SkillSource::Workspace` (retained) | Untrusted |

   All three user roots MUST be absolute; a relative or empty root disables that
   root only and records one `SkillLoadWarning` (retains 20 §3.1 M2/SK-F13).
   `$HOME` unset disables roots 1 and 3. The layout is unchanged:
   `<root>/<skill-name>/SKILL.md` (20 §2.1).

3. **Command roots (new).** A **file command** is a single markdown file
   `<root>/<command-name>.md` whose body is a prompt template; its
   slash-command name is the file's basename (kebab-case, grammar
   `[a-z][a-z0-9]*(-[a-z0-9]+)*`, 1–64). Roots, in order:

   | # | Root | Scope | Trust |
   |---|---|---|---|
   | 1 | `$HOME/.ymh/commands` | user | Trusted |
   | 2 | `$XDG_CONFIG_HOME/ymh/commands` else `$HOME/.config/ymh/commands` | user | Trusted |
   | 3 | `$HOME/.claude/commands` | user | Trusted |
   | 4 | `<workspace>/.ymh/commands` | workspace | Untrusted |

   A file command may carry optional frontmatter (`description:`, `argument-hint:`,
   `allowed-tools:` advisory) parsed by the existing 20 §4.2 hand-rolled YAML
   subset; a missing/invalid fence degrades to "no description" rather than a
   fatal error (a command file is a convenience, not a safety artifact). The
   body supports one substitution: `$ARGUMENTS` → the raw text after the command
   token (empty when absent).

4. **Precedence (pinned).** Collisions resolve as:
   - **Across trust tiers:** a user (Trusted) skill/command always wins over a
     workspace (Untrusted) one; the workspace entry is rejected and a warning is
     recorded. (Retains 20 §3.2.)
   - **Within the user tier:** first root in the table above wins
     (`~/.ymh` > `$XDG_CONFIG_HOME/ymh` > `$HOME/.claude`).
   - **A file command shadows a compiled-in command of the same name** only if
     the compiled-in command is **not** `reserved`. This requires a real flag: the
     `Command` struct (`include/ymh/ui/command_registry.hpp:51-56`) is currently
     `{name, description, handler, aliases}` with **no** reserved marker, so
     50-D1.4 adds `bool reserved = false;` and marks `/exit`, `/quit`, `/help`,
     `/sessions`, `/skills`, `/mcp`, `/status`, `/context` as `reserved = true` at
     their `CommandRegistry::add` sites (`src/ui/command_registry.cpp:67`,
     `:154-300`). A file command whose name collides with a **reserved**
     compiled-in command is rejected with a warning; a collision with a
     **non-reserved** compiled-in command shadows it (file command wins) with a
     warning. The reserved set is passed to discovery as `reserved_names`
     (`discover_file_commands`, §7).

5. **Instruction roots.** `InstructionLoader` gains
   `$HOME/.claude/CLAUDE.md` as a global instruction candidate, read **after**
   the existing `$XDG_CONFIG_HOME/ymh/AGENTS.md` (broader-to-specific ordering
   is unchanged; the global block is appended last today,
   `src/prompt/instructions.cpp:179-189`). Workspace `CLAUDE.md` candidates are
   already read (`:169-171`). `~/.ymh/AGENTS.md` and `~/.ymh/CLAUDE.md` are also
   added to the global candidate set, ordered after the config-root file.

6. **First-run scaffolding.** On the conventional invocation
   (`ymh`, `ymh run`, `ymh list`, `ymh show`, `ymh replay`, `ymh fork`), ymh
   creates `$HOME/.ymh/skills` and `$HOME/.ymh/commands` as **empty**
   directories, idempotently, `0700`. It never writes a file and never creates
   these dirs when `HOME` is unset or when `--config <path>` is explicit (spec
   21's "explicit path is never scaffolded" rule, extended). Failure to create
   is a warning, never fatal. `$HOME/.claude` is **never** created or written.

7. **Discovery timing.** Skill/command discovery is a **daemon-startup** action,
   like skills today (`src/agent/workspace_runtime.cpp:66-79`), because the
   roots are absolute and workspace-relative. `/skills`, `/skill <name>`, and the
   `skill` tool are unchanged (20 §6/§7). File commands appear in the `/` list
   and in `/help`; their description is the frontmatter `description` or the
   first non-empty body line, truncated to 80 columns.

### 3.3 Invariants

- **50-I1.** A user root is used only if it is absolute; a relative user root
  disables that root and records exactly one warning (retains 20 §3.1 M2).
- **50-I2.** Discovery performs **no** writes and never follows a symlink out of
  a root (retains 20 §3.3). `~/.ymh` scaffolding is the only write and happens
  before discovery, on the conventional path only.
- **50-I3.** A workspace (untrusted) skill/command never shadows a user
  (trusted) one; the rejection is deterministic (root order, then byte-wise
  name ascending).
- **50-I4.** A file command never shadows a **reserved** compiled-in command;
  the reserved flag is a real member of `Command`
  (`include/ymh/ui/command_registry.hpp`, `reserved`), not an assumed one. A
  non-reserved compiled-in command may be shadowed with a warning.
- **50-I5.** `$ARGUMENTS` substitution happens exactly once, on the raw body,
  before it enters the composer; the composer text is what history stores.
- **50-I6.** Skill identity and trust semantics are otherwise unchanged; a file
  command is not a `Tool` and cannot be invoked by the model.

### 3.4 Failure modes

- **50-F1** (`F1`, path isolation). A command/skill file whose realpath escapes
  its root is skipped with a warning; the workspace root is never allowed to
  reach the home roots.
- **50-F2.** `$HOME` unset: roots 1/3 (skills) and 1/3 (commands) and
  scaffolding are disabled; roots 2/4 remain. One warning per disabled root.
- **50-F3.** A malformed command frontmatter degrades to "no description"; the
  command is still listed and runnable. (Contrast 20's fatal skill policy: a
  skill's frontmatter is safety-adjacent, a command's is cosmetic.)
- **50-F4.** `~/.ymh` scaffolding fails (read-only home): one warning; the run
  proceeds with discovery of whatever exists.
- **50-F5** (`F6`). A file command whose name collides with a reserved command
  is rejected with a warning; `/help` and completion show the compiled-in row.

### 3.5 Test plan

- **Unit.** Root-order resolution (table-driven over a fake `HOME`/`XDG_CONFIG_HOME`);
  relative-root disable; precedence (user beats workspace; `~/.ymh` beats
  `$XDG_CONFIG_HOME` beats `~/.claude`); reserved-command shadow rejection;
  `$ARGUMENTS` substitution; malformed frontmatter degrades; `$HOME` unset.
- **Integration.** A temp home with `~/.ymh/skills/demo/SKILL.md`,
  `~/.claude/commands/fix.md`, and a workspace `.ymh/commands/fix.md`; assert
  `/skills` lists `demo` and the `/` list shows `fix` with the user body winning.
- **Golden TUI.** `/skills` output and the `/` completion row for a file
  command (description column).

---

## 4. D2 — The fresh-launch session guarantee and the Ctrl+C re-entry bug

### 4.1 Hypothesis (NOT verified) — and the discriminating question

The user reported: "interrupted work by pressing Ctrl+C and reentered ymh again.
On reentering it put me into the previous workspace."

**Rev 1 titled this section "Root cause (verified)". That was wrong.** The gate
established that the mechanism Rev 1 named cannot produce the reported symptom:
`live.front()` restores a *session within the cwd workspace*, not a different
workspace; `workspaces.front()` is *alphabetical*, not "the previous
workspace"; no last/focused workspace is persisted; and Ctrl+C does not exit
ymh at all. Rev 1 also contained no reproduction. Rev 2 demotes this to a
**HYPOTHESIS** with a discriminating question (50-OQ-5) and covers both
branches.

**The discriminating question (must be answered by the user or a live repro).**
Did the user return to the **same workspace but a different (previous)
session**, or genuinely to a **different workspace**? The code paths that exist
support only the former; the latter is not explained by any path found.

**Branch A — same workspace, previous session (supported by code).**

The startup path, end to end:

1. `ymh` with no args → `run_cli` → `CliInvocation::Command::Tui`
   (`src/cli/cli.cpp:1194-1204`).
2. `resolve_workspace` returns `--workspace` or **`current_path()`**
   (`src/cli/cli.cpp:149-153`). `--resume` overrides it with the session's own
   workspace (`src/cli/cli.cpp:466-488`). There is **no** persisted
   last/focused workspace: the `workspaces` table has no focus/active column
   (`src/registry/registry.cpp:44-59`).
3. `run_supervisor_entry` canonicalizes the root, registers the row, calls
   `lifecycle.ensureRunning(row->id, …)` (spawn-or-attach, waits for the claim:
   `src/host/workspace_host.cpp:1136-1172`), then takes **one** snapshot of all
   live daemons via `scanner.scanOnce()` (`src/cli/cli.cpp:521-522`), and sets
   `options.initial_workspace = *canonical` (`src/cli/cli.cpp:538`).
4. `SupervisorApp::run()` attaches every scanned workspace, then sets
   `model_.activeWorkspaceId = options_.workspaces.front().id` and overrides it
   only if a spec's `cwd` string-equals `initial_workspace`
   (`src/ui/supervisor.cpp:402-417`). **`workspaces.front()` is alphabetical**:
   the scanner is fed by `WorkspaceRegistry::listWorkspaces`, which is
   `ORDER BY canonical_path ASC` (`src/registry/registry.cpp:813-822`;
   `include/ymh/registry/registry.hpp:216`). It is *not* "the previous
   workspace". The `front()` fallback can only win when the cwd workspace is
   **absent** from the one-shot scan (see Branch B).
5. On `Attached`, `refresh_sessions`'s reply branch decides focus/create for the
   **active** workspace only (`src/ui/supervisor.cpp:1415-1428`). `live` is
   built from the `session.list` reply in daemon order
   (`src/ui/supervisor.cpp:1378-1390`), so `live.front()` is *a* live session —
   with a single live session it is the one just interrupted:

   ```cpp
   if (it->second.activeSessionId().value.empty() &&
       model_.activeWorkspaceId == workspace &&
       resume_in_flight_.count(workspace) == 0) {
       if (!live.empty()) {
           activate_session(workspace, live.front().first);   // <-- focuses a live session
       } else {
           create_session(workspace, std::string{});
       }
   }
   ```

`Ctrl+C` does **not** quit ymh and does **not** close the session: it is
`cancelActive()` → `agent.cancel` (`src/ui/supervisor.cpp:2809-2811`,
`:459-467`). So an interrupted session stays **live/open** in the daemon.
"Re-entering" therefore means starting a **second** `ymh` process. **A clean
exit does NOT leave the daemon alive**: spec 16 voids 04 H8 / 00 D23 and tears
the daemon down when the last supervisor exits cleanly
(`docs/design/16-daemon-ownership.md:99-100`; `src/ui/supervisor.cpp:655-672`
`confirm_exit` → `teardown_daemons`). So for a live daemon to survive to the
second launch, the first supervisor must still be running, or must have been
killed ungracefully (the daemon-side owner watchdog then backstops it). When
that daemon is still alive, step 5 focuses its live session **in the same
workspace** — "the previous conversation", not a different workspace. 46-D7 only
guarantees a clean session when there is **no** live session; its `live.front()`
branch is the defect.

**Branch B — a different workspace (unexplained by any path found).** For
`workspaces.front()` to win, the cwd workspace must be **absent** from the
one-shot scan at step 3–4 (`src/ui/supervisor.cpp:409`), because
`initial_workspace` otherwise overrides it. `ensureRunning` returns only after
the claim (`src/cli/cli.cpp:514-519`), so the cwd daemon is normally live and
present in the scan; `scanOnce` also has a single-workspace backfill
(`src/cli/cli.cpp:523-534`). A scan miss is possible only if the cwd daemon died
between the claim and the scan, or if the cwd row/spec is absent. **No path was
found that selects the previously *used* workspace, because none is persisted.**
Branch B therefore remains a hypothesis; 50-D2.3 adds the cwd-wins guarantee
that removes the only way a *different* workspace can become active.

**Why this is not fixed by 46-D7 alone.** 46-D7 removed the
`resume stored.front()` branch, but deliberately kept the live-session focus
(`docs/design/46-permissions-ui-errata.md:1528-1535`). A live session is exactly
what an interrupted (Ctrl+C) turn leaves behind.

### 4.2 Decision (50-D2) — BUG FIX

**Branch coverage.** 50-D2 fixes **both** branches of §4.1: D2.1–D2.2 and D2.4
fix Branch A (a live session is focused on a fresh launch); D2.3 fixes Branch B
(the `workspaces.front()` fallback). D2.3 is a guarantee worth having on its own
merits (the cwd is the user's stated workspace), independent of which branch the
user actually hit. Neither branch is asserted as the user's root cause until
50-OQ-5 is answered.

1. **Fresh launch always creates.** Replace the `!live.empty()` auto-focus with
   `create_session(workspace, std::string{})`. The condition that reaches this
   branch (active workspace, no active session, no resume in flight) is
   precisely the fresh-launch/reconnect case; a reconnect has a non-empty
   `activeSessionId` and never reaches it, so no spurious sessions appear on
   reconnect. **Reconciled with spec 49:** on a bare launch with no workspace,
   49-D1's lazy creation runs on the first prompt and this rule applies then
   (50-OQ-6).
2. **Opening an existing session is explicit only.** `/sessions`, the Ctrl-S
   switcher, and `--resume <id>` remain the ways to open a stored/live session.
   Selecting a live session still goes through `resume_after_attach`
   (`src/ui/supervisor.cpp:1032-1040`), which sets the `resume_in_flight_`
   marker and is unaffected by 50-D2.1.
3. **The cwd workspace wins — once it exists; no notice on a bare launch.** In
   `run()`, when `initial_workspace` is non-empty **and the supervisor already has
   (or is explicitly activating) that workspace**, do **not** fall back to
   `workspaces.front()` — which is alphabetical (`src/registry/registry.cpp:813-822`),
   not the previous workspace. Either (a) append a spec built from the registry row
   for `initial_workspace` (`workspace_spec_from_registry`,
   `src/ui/supervisor.cpp:1216-1232`), or (b) fail loudly with a notice. Pin (a),
   plus a status-bar notice only when the row is genuinely missing **for an
   explicit activation**. This removes the one-shot-scan race that is the **only**
   way a different workspace can become active (Branch B).
   **Reconciled with spec 49 (Rev 2).** On a bare `ymh` there is **no cwd row and
   no workspace** at startup: 49-D1 starts with zero workspaces and creates the
   row+daemon lazily on the first prompt. Therefore, on a bare launch:
   - `initial_workspace` is a **path hint only**; it must **not** trigger (a)
     appending a spec from a nonexistent row, nor (b) a notice. There is nothing
     to win yet, and emitting "cwd registry row missing" on every bare launch
     would be a **spurious notice**.
   - The cwd-wins guarantee (a)/(b) applies only when the row exists: an explicit
     activation (`--resume`/`--new`, which register eagerly, 49-D3) or after
     49-D1's lazy creation has modeled the workspace.
   The lazy-creation timing is owned by 49-D1; the fresh-launch session guarantee
   (D2.1) applies when the workspace is created (50-OQ-6).
4. **`--new` is explicit.** `--new` (already parsed, `src/cli/cli.cpp:1198-1200`)
   keeps its precedence over `--resume` and now also forces
   `create_session` on attach, so a scripted fresh session is unambiguous.
5. **Ctrl+C semantics are unchanged.** Ctrl+C stays cancel-only (48-D2.3 uses
   the same path). The bug is the startup focus, not Ctrl+C.
6. **Test seam.** Add `SupervisorHarness` methods `seed_live_session` and
   `last_focus_decision` so the fresh-launch rule is testable without a daemon.

### 4.3 Invariants

- **50-I7.** A fresh launch (`no --resume`, `no --new`) creates a new session
  in the cwd workspace and never focuses an existing live session.
- **50-I8.** The cwd workspace is always the initial active workspace; if it
  is missing from the scan it is added from the registry, never replaced by
  another workspace.
- **50-I9.** An explicit resume (`--resume`, `/sessions`, Ctrl-S) still opens
  the requested session; 50-D2 does not regress 22-D5/S4 or 46-D7's
  `resume_in_flight_` gate.
- **50-I10.** A reconnect to a workspace with a non-empty `activeSessionId`
  never creates a session.

### 4.4 Failure modes

- **50-F6** (`F10`, resume-suspended). A missing cwd registry row **during an
  explicit activation** (`--resume`/`--new`): a notice is shown and the supervisor
  still starts; it never silently focuses a different workspace. A **bare launch**
  has no cwd row by design (49-D1) and is **not** this failure — no notice is
  emitted.
- **50-F7.** A resume in flight when the `session.list` reply lands: the
  `resume_in_flight_.count(workspace) == 0` guard is retained, so the create
  branch does not fire (no double session).
- **50-F8** (`F3`). A late `session.list` reply after the user selected a
  session: the branch only fires when `activeSessionId().value.empty()`.

### 4.5 Test plan

- **Unit (harness).** Fresh launch with a live session present → `create_session`
  called, no `activate_session`; explicit `--resume` → `resume_after_attach`;
  reconnect (non-empty active) → neither; `initial_workspace` absent from scan →
  the cwd spec is appended and becomes active.
- **Integration.** Two workspaces A and B both live; launch with cwd = B;
  assert the active workspace is B and a new session is created.
- **PTY/live.** Ctrl+C a turn, start a second `ymh` in the same workspace, assert
  the transcript is empty (new session), not the previous conversation.

**Deterministic discriminating reproduction (for 50-OQ-5).** This is the manual
procedure that decides Branch A vs Branch B. Run it once, record which branch
fires.

Setup: workspace `W1` = cwd, with a live daemon and one previously interrupted
session `S_old` (send a prompt, Ctrl+C it). Register a second workspace `W2`
whose `canonical_path` sorts **before** `W1` (so it is
`workspaces.front()`), and make `W2`'s daemon live. `S_old` must be the only
live session in `W1`.

1. Keep `W1`'s daemon alive while starting the second `ymh`. **A clean exit does
   not do this**: spec 16 voids 04 H8 / 00 D23, and the last supervisor's clean
   exit tears the daemons down (`docs/design/16-daemon-ownership.md:99-100`;
   `src/ui/supervisor.cpp:655-672` `confirm_exit` → `teardown_daemons`). Achieve
   it either by (a) leaving the **first supervisor running** and launching the
   second `ymh` in a second terminal, or (b) killing the first supervisor
   ungracefully (`SIGKILL`) so the daemon survives on its owner watchdog until
   the new supervisor attaches.
2. `cd W1 && ymh` (the second process, or the relaunch after the ungraceful
   kill).
3. Observe the **header**: does it show `W1`'s title/cwd, or `W2`'s?
   - **Branch A fires** (same workspace, previous session): header shows `W1`;
     the status bar's session id equals `S_old`; the transcript shows `S_old`'s
     prior conversation. This is the branch the code supports.
   - **Branch B fires** (different workspace): header shows `W2` and `W2`'s
     session. For this to happen, `W1` must have been absent from the one-shot
     scan; reproduce the miss deliberately by making `W1`'s daemon die between
     the claim and `scanner.scanOnce()` (or by deleting `W1`'s registry row
     before step 2). If Branch B fires **without** an induced scan miss, that is
     a new defect and must be reported.
4. The branch is recorded as the answer to 50-OQ-5; 50-D2 fixes both, so the
   user-visible outcome after the fix is identical: a **new** session in `W1`.

- **PTY/live (automated).** The step-2 sequence is scripted under a PTY with
  `W2` live and sorting first, asserting the active workspace is `W1` and the
  transcript is empty.

---

## 5. D3 — The MCP child environment (was 48-D9)

### 5.1 Current state (verified)

**Config.** The live file is `~/.config/ymh/config.jsonc` (not `config.json`;
`src/config/config.cpp:1242-1254`). It contains a top-level `mcp_servers` object
(the localcode-import shape) with `sj-jira`:

```jsonc
"sj-jira": {
  "args": ["/tools/mcp-hub/src/sj-jira.py"],
  "command": "python3",
  "env": { "JIRA_TOKEN": "…", "JIRA_URL": "http://sj-jira.ibn.com:8080" },
  "required": false,
  "type": "stdio"
}
```

The loader supports `mcp_servers` as an alternative to `mcp.server`
(`src/config/config.cpp:943`, `:957-969`, `:1017-1018`, `:1186-1232`), accepts
`type` as a synonym of `transport` (`:690-703`), and parses the **object** `env`
into `KEY=VALUE` strings (`:733-735`, `:658-679`). The mapping into
`McpServerConfig` copies `env` (`src/cli/wiring.cpp:169`).

**Env forwarding is correct (proven).**

1. `StdioMcpTransport::start` sets
   `request.environment = resolve_mcp_env(config_.env)`
   (`src/mcp/mcp_transport.cpp:100`).
2. `resolve_mcp_env` (`:21-60`) splits `KEY=VALUE` at the first `=`, expands
   `${VAR}` from the **daemon** environment (throwing `ConfigInvalid` if the
   variable is missing), and passes literal values through.
3. `LocalProcessService::spawn` (`src/execution/process.cpp:509-555`) forks,
   `clearenv()`s the child, sets `PATH=/usr/local/bin:/usr/bin:/bin`, applies
   `request.environment` with `setenv(..., 1)`, then `execvp`s (`:544-551`).

**Empirical proof (run for this spec, HEAD `bbf96aeaa`).** A fake MCP stdio
server configured via `mcp_servers.fake` with
`env {JIRA_TOKEN: TOKEN123, JIRA_URL: …, FOO: bar}` was spawned by
`build/ymh --host …`. The server dumped its environment:

```json
{ "JIRA_TOKEN": "TOKEN123", "JIRA_URL": "http://example.invalid:8080",
  "FOO": "bar", "HOME": null,
  "PATH": "/usr/local/bin:/usr/bin:/bin",
  "env_keys": ["FOO","JIRA_TOKEN","JIRA_URL","LC_CTYPE","PATH"] }
```

So the configured `env` **is** delivered to the MCP child. The `LC_CTYPE` key is
**child-injected** (PEP 538 locale coercion), not seeded by ymh
(`src/execution/process.cpp:544-548`) — see 50-I15.

**The env-consistency defect (PROVEN, independent of jira).** `spawn()` calls
`::clearenv()` and seeds **only** `PATH` + the configured env
(`src/execution/process.cpp:544-548`), whereas the sibling `run()` path (shell
tools) inherits the daemon environment and only overlays `request.environment`
(`:386-388`). The PTY path is a **third** variant: it inherits `environ` and
overlays (`src/execution/pty.cpp:446-457`). So the same daemon hands three
different environments to three child classes. That asymmetry is a real defect
worth fixing on its own merits: it is deterministic, observable, and pinned by
50-I14. It had **no bearing on the jira 401**.

### 5.2 Decision (50-D3)

1. **`spawn()` inherits and overlays, like `run()`.** Remove the unconditional
   `::clearenv()` from `LocalProcessService::spawn`
   (`src/execution/process.cpp:544`). The child inherits the daemon environment
   and `request.environment` is applied with `setenv(..., 1)` (overlay wins).
   This makes `spawn()` consistent with `run()` and with the reference MCP
   transports.
2. **`env_mode` (pinned semantics).** `ProcessRequest` gains

   ```cpp
   enum class ProcessEnvMode : std::uint8_t { Inherit, Minimal };
   ProcessEnvMode env_mode = ProcessEnvMode::Inherit;
   ```

   `Inherit` is the default and is the mode **MCP stdio children always use**
   (50-D3.3). `Minimal` reproduces **exactly** today's `spawn()` behaviour and
   is pinned as: `::clearenv()`, then `PATH=/usr/local/bin:/usr/bin:/bin`
   (`src/execution/process.cpp:545`, byte-identical), then `request.environment`
   overlaid with `setenv(..., 1)`. **No other key** is seeded — in particular no
   `HOME`, `SHELL`, `USER`, `TMPDIR`, `LANG`, proxy, or CA-bundle variable.
   `Minimal` is retained as a **seam capability only** (a future tool-isolation
   spec may use it); **no production caller selects it** at HEAD and it is not
   configurable for MCP.

   **Caller inventory (exhaustive at HEAD `bbf96aeaa`).**
   `ProcessService::spawn` has exactly **one** production caller:
   `StdioMcpTransport::start` (`src/mcp/mcp_transport.cpp:105`) — it is pinned
   to `Inherit`. `ProcessService::run` has exactly **one** production caller:
   the shell tool (`src/tools/builtin_tools.cpp:387-403`) — it leaves `env_mode`
   at the default `Inherit`, which is byte-identical to today's `run()`
   behaviour (it already inherits). No other production code constructs a
   `ProcessRequest` (`grep -rn ProcessRequest src include` → `builtin_tools.cpp:387`,
   `mcp_transport.cpp:92` only). **The PTY path is not affected**: it is a
   separate seam (`PtyRequest`, `include/ymh/execution/pty.hpp:370-381`) and
   already inherits `environ` then overlays (`src/execution/pty.cpp:446-457`),
   which is the desired end state; `PtyRequest` gains no `env_mode`. The
   host-daemon launcher (`HostLauncher::spawn`, `src/host/workspace_host.cpp:1139`)
   is likewise unrelated.
3. **MCP always inherits; no `Minimal` opt-out.** `McpSettings` gains **no**
   `inherit_env` key: `StdioMcpTransport::start` sets
   `request.env_mode = ProcessEnvMode::Inherit` unconditionally. This is a
   **correctness** decision (§5.1), not a preference: **libraries read the
   environment**, and a curated set silently changes their behavior. It is
   independent of the jira 401 (resolved as an invalid credential). The security
   trade-off is real — an inherited environment is a broader surface than a
   curated one — and is handled by MCP server definitions being **global-layer
   only** (50-D4) plus the recorded per-server `env_denylist` / `env_allowlist`
   follow-up (50-OQ-4). A layer that sets the retired `mcp.inherit_env` key is a
   `ConfigError` (`reject_unknown`, `src/config/config.cpp:579`).
4. **Redaction unchanged.** The child environment is never logged
   (`src/llm/redaction.cpp` is not in this path; the MCP logger already redacts
   notifications, `src/mcp/mcp_client.cpp:436`). `env` values are still never
   logged.
5. **`stderr` visibility (pinned, 50-D3.5; corrected Rev 2).** MCP `stderr`
   continues to `/dev/null` by default. `McpSettings` gains
   `bool log_child_stderr = false;` (global-layer only; a workspace layer that
   sets it is a `ConfigError`) in `include/ymh/config/config.hpp:174-192` and the
   `mcp` `reject_unknown` allowlist (`src/config/config.cpp:579`). When `true`,
   `StdioMcpTransport::start` sets
   `request.stderr_path = environment_.root() / ".ymh" / "mcp" /
   (sanitized_server_id + ".stderr.log")`, and `ProcessRequest` gains
   `std::optional<std::filesystem::path> stderr_path;`. In `spawn()`, when
   `stderr_path` is set the child's `STDERR_FILENO` is opened
   `O_WRONLY|O_CREAT|O_APPEND` with mode `0600` and `dup2`-ed onto it; otherwise
   the existing `redirect_to_devnull(STDERR_FILENO)` is retained
   (`src/execution/process.cpp:543`). `run()` is unaffected (`stderr_path` is a
   `spawn()`-only field; `run()` keeps `capture_stderr`), and `spawn()` does not
   consult `capture_stderr` — its value stays `false` in the transport
   (`src/mcp/mcp_transport.cpp:102`).
   **Why a dedicated file, not `host.log` (Rev 2).** The daemon's log sink
   (`config_.log_sink`) **is** `<workspace>/.ymh/host.log` (`src/cli/cli.cpp:302`;
   defaulted at `src/host/workspace_host.cpp:455-456`) and is opened `O_TRUNC` at
   daemon start (`:1064`). Routing raw child stderr there would (i) write a
   child's self-report — which may embed environment values — into ymh's **own**
   log, falsifying 50-I13, and (ii) race the daemon's `O_TRUNC` against other
   children's `O_APPEND` on one shared file. A per-server `.stderr.log` under
   `.ymh/mcp/` is a distinct diagnostic artifact: created under the daemon's
   `umask(0077)` (`src/host/workspace_host.cpp:1061`) with mode `0600`, off by
   default, and **not** ymh's logging path — so 50-I13 (which governs ymh's own
   logging, including `host.log`) is not weakened. If the `.ymh/mcp/` directory
   or file cannot be created, fall back to `/dev/null` (50-F12).
6. **Config documentation correction.** The `mcp_servers` object form and the
   `env` object form are documented as first-class (they are already
   implemented); the user's `config.json` belief is corrected to `config.jsonc`.

### 5.3 Invariants

- **50-I11.** The MCP child **always** receives the configured `env` overlaid on
  the inherited environment (`env_mode = Inherit`); the configured values always
  win.
- **50-I12.** MCP stdio children never use `Minimal`; `Minimal` is a reserved
  seam capability with no production caller, and no config key can select it for
  MCP (the retired `mcp.inherit_env` is a `ConfigError`).
- **50-I13.** Env values are never written to ymh's **own** logging, at any level
  — including the daemon log sink `host.log` (`config_.log_sink`,
  `src/cli/cli.cpp:302`; `src/host/workspace_host.cpp:455-456`). The opt-in
  **per-server** raw child-stderr file of 50-D3.5 is a separate diagnostic
  artifact, not ymh's logging; it is off by default and `0600`.
- **50-I14.** `spawn()` and `run()` have the same environment semantics under
  `Inherit`; the only difference is the long-lived bidirectional pipe.
- **50-I15.** Under `Minimal`, the keys **ymh seeds** are exactly
  `PATH=/usr/local/bin:/usr/bin:/bin` plus `request.environment`; no other key is
  seeded. The child may still add keys of its own — in particular the runtime may
  inject `LC_CTYPE` (PEP 538 locale coercion; the §5.1 dump shows it), which is
  **not** seeded by `src/execution/process.cpp:544-548` and is outside this
  invariant.
- **50-I16.** `stderr_path` is honored only by `spawn()`; when unset the child's
  `stderr` goes to `/dev/null` as today. `run()` ignores `stderr_path`.
- **50-I17.** `mcp.log_child_stderr` is global-layer only; a workspace layer
  that sets it is a `ConfigError`.
- **50-I18.** The PTY path is unaffected by `env_mode`; it always inherits
  `environ` and overlays `request.environment`.
- **50-I19.** The MCP child's cwd defaults to the workspace root when the
  server's `cwd` is empty (`src/mcp/mcp_transport.cpp:98-99`).
- **50-I20.** The MCP child's inherited environment is not filtered by ymh;
  variables `httpx`/`requests` consult (proxy, `SSL_CERT_FILE`/`SSL_CERT_DIR`,
  `NETRC`, `HOME`) reach the child unless the user's own environment lacks them.
- **50-I21.** An MCP server's `cwd` must **resolve inside the workspace root**
  (`src/execution/environment.cpp:74-76`). An out-of-root `cwd` is rejected as
  `McpError{ConfigInvalid}` — it must **never** escape as an unhandled
  `ToolError{PathEscape}`. When `cwd` is empty it defaults to the workspace root
  (`src/mcp/mcp_transport.cpp:98-99`). The resolve call must move inside the
  existing `try`/catch at `src/mcp/mcp_transport.cpp:104`.

### 5.4 Failure modes

- **50-F9** (`F1`). Inheriting the daemon environment could leak a secret to a
  malicious MCP server. Mitigation: MCP server definitions are **global-layer
  only** (50-I22, a hard rule — a workspace layer is a `ConfigError`); `Inherit`
  is mandatory for MCP (50-D3.3) because a curated set silently breaks
  environment-reading clients; the per-server `env_denylist` / `env_allowlist`
  follow-up is 50-OQ-4. Recorded, not solved.
- **50-F10.** A `${VAR}` reference whose variable is absent: `resolve_mcp_env`
  throws `ConfigInvalid` (`src/mcp/mcp_transport.cpp:46-49`); the server is
  skipped/failed with a warning (retains 15 §5.2). Inherit mode makes this rarer
  but does not change the error.
- **50-F11** (`F8`). A child that inherits a huge environment: bounded by the OS
  `ARG_MAX`/env limits; no ymh cap (recorded).
- **50-F12** (`F1`). `stderr_path` cannot be opened (permission/disk): log once
  and fall back to `/dev/null`; never fail the MCP server start for a diagnostic.
- **50-F13.** The raw child-stderr log contains a secret the child printed: the
  flag is off by default, the file is `0600` and per-server under `.ymh/mcp/`,
  and the invariant 50-I13 (ymh's own logging, incl. `host.log`) is not weakened;
  recorded.
- **50-F14.** A helper's `load_dotenv()` is cwd-relative: from the workspace-root
  cwd (50-I19) it can read a `.env` in the workspace tree (supplying a variable
  ymh does not pass; `override=False` keeps the passed value) and cannot reach a
  `.env` beside the helper. **Investigated for item 9 and cleared**: the two
  variables ymh passes come from the `env` map, so `load_dotenv()` was never
  needed. Recorded, not filtered; no cwd-default change is pinned.
- **50-F15** (`F4`). An MCP server's `cwd` is an absolute path outside the
  workspace root: `environment_.resolve()` throws `ToolError{PathEscape}`
  (`src/execution/environment.cpp:74-76`). Today the call is **outside** the
  try/catch (`src/mcp/mcp_transport.cpp:98-99` vs `:104`), so it escapes
  uncaught. Handling (50-I21): move the resolve inside the `try` and rethrow as
  `McpError{ConfigInvalid, "mcp cwd escapes workspace root: <path>"}`.
### 5.5 The item-9 401 was an invalid credential (RESOLVED) — and the credential-validation procedure

**RESOLVED — root cause: an invalid (expired/revoked) Jira credential, NOT a
harness defect.** The user regenerated the Jira token and **both localcode and
ymh now access jira successfully**. localcode failed identically with the same
old token, which is what proved the harness was never at fault. **ymh was never
at fault.** The token was delivered correctly throughout (50-I11; the empirical
dump in §5.1), and the helper's own `raise RuntimeError` on an empty
`JIRA_TOKEN` means an empty token would have crashed at startup, not produced a
401. Every harness mechanism was ruled out with evidence: token missing; token
mangled (byte-identical to localcode's; `JIRA_TOKEN` len=48, sha256
`aeb8cc97ac79f71d`); `resolve_mcp_env` substitution (no `$`/`{` in any value);
proxy (none exists); TLS/CA (plain `http://`); `~/.netrc`/`HOME`; `load_dotenv()`
defeated by cwd; and Python module resolution under a cleared `HOME` (refuted by
measurement: `env -i PATH=… python3 -c "import httpx"` resolves the identical
`httpx 0.28.1` as a normal shell, because Python falls back to the password
database for the home directory when `HOME` is unset). The earlier `env -i`/cwd
diagnostics are **superseded and removed**; the out-of-root `cwd` experiment was
unexecutable anyway (50-I21).

**Verification procedure (pinned — the documented way to validate any MCP
credential).** Never echo a token; extract it into the shell without printing.

```sh
# 1. Extract values from the ymh config without echoing them.
JIRA_URL=$(jq -r '.mcp_servers."sj-jira".env.JIRA_URL' \
    ~/.config/ymh/config.jsonc)
JIRA_TOKEN=$(jq -r '.mcp_servers."sj-jira".env.JIRA_TOKEN' \
    ~/.config/ymh/config.jsonc)

# 2. Reachability without auth (does the host answer at all?).
curl -sS -o /dev/null -w '%{http_code}\n' "$JIRA_URL/rest/api/2/serverInfo"

# 3. Test the credential: Bearer first, then Basic.
curl -sS -o /dev/null -w '%{http_code}\n' \
    -H "Authorization: Bearer $JIRA_TOKEN" "$JIRA_URL/rest/api/2/myself"
curl -sS -o /dev/null -w '%{http_code}\n' \
    -u "$USER:$JIRA_TOKEN" "$JIRA_URL/rest/api/2/myself"

# 4. Read the error body (headers + first lines) when a status is non-2xx.
curl -sS -i -H "Authorization: Bearer $JIRA_TOKEN" \
    "$JIRA_URL/rest/api/2/myself" | head -20
```

| HTTP status | Meaning | Action |
|---|---|---|
| `401` | Credential rejected (invalid/expired/revoked) | Regenerate the token; re-run step 3. |
| `403` | Credential valid, not authorized for this resource | Check the account's permissions; do not regenerate blindly. |
| `000` | Unreachable (DNS/TLS/connection) | Check the URL/host/network; not a credential problem. |
| `200` | Credential fine | The fault is in the caller (headers/URL path), not the token. |

This is the procedure that would have resolved item 9 in minutes; it is pinned so
the next MCP credential question is answered without a code investigation.

### 5.6 Test plan

- **Unit.** `ProcessEnvMode::Inherit` overlay order (a fake env var is visible
  and a configured value overrides it); `Minimal` seeds **exactly** the keys ymh
  seeds — `PATH` + the configured map (assert that set, 50-I15); a retired
  `mcp.inherit_env` key is a `ConfigError`; `mcp.log_child_stderr` parse +
  workspace-layer rejection; `spawn()` honors `stderr_path` and `run()` ignores
  it. The test asserts **ymh-seeded** keys only and tolerates child-injected
  `LC_CTYPE` (50-I15).
- **Integration.** The fake MCP server of §5.1 asserts that under the pinned MCP
  mode (`Inherit`) `HOME`/proxy/`NETRC` variables are visible and configured
  values always win, and that under `Minimal` they are gone; a second run with
  `log_child_stderr = true` asserts the child's stderr lands in
  `<workspace>/.ymh/mcp/<server>.stderr.log` (and **not** in `host.log`, 50-I13).
- **Integration (cwd policy, 50-I21).** A fake stdio server reports its
  `getcwd()`: with no configured `cwd` it reports the workspace root (50-I19);
  with an in-root `cwd` it reports that dir; with an **out-of-root absolute**
  `cwd` the spawn fails with `McpError{ConfigInvalid}` (never an unhandled
  `PathEscape`).
- **Hermetic env-difference test (the env-fix shape, not a 401 cure).** A fake
  stdio server reports the proxy/`SSL_CERT_*`/`NETRC`/`HOME` variables it
  observes; the test runs it through `spawn()` under `Minimal` versus `Inherit`
  and asserts the observable difference is **exactly** those keys.
- **PTY regression.** `PtyRequest` has no `env_mode` and the PTY child still
  inherits `environ` (50-I18).
- **Live (opt-in).** With a real jira MCP server and a valid token, the pinned
  `Inherit` path returns non-401 (guarded by `YMH_LIVE_LLM=1` and a token env;
  never logged).

---

## 6. D4 — MCP server definitions are global-layer only (was 48-D9.7; breaking)

### 6.1 Current state (verified)

`load_config` applies the workspace layer (`src/config/config.cpp:1396-1400`),
and `apply_document` handles `mcp`/`mcp_servers` for **both** layers with no
`global_layer` guard (`:990-991`, `:1017-1018`; the pre-scan at `:957-971`),
unlike the `session` guard at `:997-999` and the `llm.api_key` guard at
`:429-432`. A repo-local `.ymh/config.jsonc` in a cloned repository can therefore
add or override MCP servers that then execute with the daemon's environment — a
supply-chain / arbitrary-execution vector.

### 6.2 Decision (50-D4)

1. **Rule.** `mcp` and `mcp_servers` are **global-layer only**. On a workspace
   layer each is a hard `ConfigError`, raised at the **existing** `global_layer`
   sites in `apply_document`: the `mcp_servers` pre-scan (`src/config/config.cpp:958`)
   and the `apply_mcp` call site (`:990`). **Not** a silent ignore — silent
   drops hide the problem.
2. **No new parameter.** The guard is pinned at the call sites, which already
   have `global_layer` (`apply_document`): the `mcp_servers` pre-scan
   (`src/config/config.cpp:958`) and the `mcp` section (`:990-991`).
   `apply_mcp` (`void apply_mcp(Config&, const Json&, const std::filesystem::path&)`,
   `:578`) and `apply_mcp_servers_object` (`void apply_mcp_servers_object(McpSettings&,
   const Json&, const std::filesystem::path&)`, `:1186`) keep their current
   signatures and are **not** passed `global_layer` — verified at HEAD, so there
   is **no** `-Wunused-parameter` break under `-Werror`. (If the implementer
   instead puts the guard inside either function, the added parameter must be
   used there; it must never be unused.)
3. **Error text (exact, via `fail` at `src/config/config.cpp:49-51`):**
   `config <path>: 'mcp' is global-layer only` and
   `config <path>: 'mcp_servers' is global-layer only`. This mirrors the
   `api_key`/`session` precedent.
4. **Migration (breaking).** A workspace-local `mcp_servers`/`mcp.server` block
   must move to the global config (`$XDG_CONFIG_HOME/ymh/config.jsonc`). On
   startup ymh exits with the `ConfigError` above naming the workspace file; no
   servers from that layer are loaded.

### 6.3 Invariants

- **50-I22.** MCP server definitions are **global-layer only**: a workspace-layer
  `mcp` or `mcp_servers` section is a hard `ConfigError`
  (`config <path>: 'mcp' is global-layer only` /
  `config <path>: 'mcp_servers' is global-layer only`), never a silent drop.

### 6.4 Failure modes

- **50-F16** (`F1`). A cloned repository ships a workspace-local
  `.ymh/config.jsonc` with `mcp_servers`/`mcp.server`: before 50-D4 this added
  servers that executed with the daemon's environment. Handling: the workspace
  layer is rejected with the exact `ConfigError` above; no servers from it load.

### 6.5 Test plan

- **Hermetic config-layer test (50-I22).** A workspace-layer config defining
  `mcp_servers` (and, separately, `mcp.server`) → `ConfigError` with the exact
  text `config <path>: 'mcp_servers' is global-layer only` (resp. `'mcp'`); the
  same block in the global layer loads normally.

---

## 7. C++ interface sketches (pinned)

Only new/changed symbols are shown; unchanged members are elided with `…`.
Every sketch pins the headers it needs. All types live in `namespace ymh` unless
noted.

```cpp
// ── include/ymh/skills/skill_types.hpp ─────────────────────────────────────
#include <cstdint>
namespace ymh {

// 50-D1.2: two new user-tier provenances. `User` and `Workspace` are retained;
// `Builtin` stays reserved (20-D2).
enum class SkillSource : std::uint8_t {
    Home,       // $HOME/.ymh/skills/<name>/SKILL.md          (NEW, Trusted)
    User,       // <config-root>/ymh/skills/<name>/SKILL.md   (retained, Trusted)
    Claude,     // $HOME/.claude/skills/<name>/SKILL.md       (NEW, Trusted)
    Workspace,  // <workspace>/.ymh/skills/<name>/SKILL.md    (retained, Untrusted)
    Builtin,    // reserved; never produced in v1
};

} // namespace ymh
```

```cpp
// ── include/ymh/skills/skill_catalog.hpp ───────────────────────────────────
#include <filesystem>
#include <string>
#include <vector>
namespace ymh {

// 50-D1.2: the catalog receives a resolved, ordered root list instead of a
// single user root. Each entry carries its provenance + trust; a relative or
// empty path disables that root (one SkillLoadWarning).
struct SkillRoot {
    std::filesystem::path path;      // MUST be absolute to be used
    SkillSource           source;
    SkillTrust            trust;
};

struct SkillCatalogConfig {
    bool                     enabled = true;
    bool                     expose_workspace = false;
    std::size_t              max_skills = 256;
    std::size_t              max_skill_bytes = 64u * 1024u;
    std::size_t              max_description_bytes = 512;
    std::size_t              max_index_bytes = 8u * 1024u;
    std::size_t              max_frontmatter_bytes = 4u * 1024u;
    // 50-D1.2: ordered roots. `discover()` uses this list verbatim; the
    // workspace root is appended by the caller (it is environment-relative).
    std::vector<SkillRoot>   roots;                              // NEW
};

class SkillCatalog {
public:
    SkillCatalog(SkillCatalogConfig       config,
                 const ExecutionEnvironment& environment,
                 std::vector<SkillRoot>  roots,                  // CHANGED (was user_root)
                 Logger&                 logger);
    // … discover()/all()/find()/warnings()/index_section() unchanged.
};

} // namespace ymh
```

```cpp
// ── include/ymh/skills/skill_roots.hpp (NEW) ───────────────────────────────
#include <filesystem>
#include <string>
#include <vector>
namespace ymh {

// 50-D1.2. Pure path resolution; no I/O. `home`/`xdg` are injected (never
// getenv in a unit test). Returns the ordered roots; a root is emitted only
// when its base is non-empty and absolute, EXCEPT the workspace root, which the
// caller appends (it is environment-relative).
[[nodiscard]] std::vector<SkillRoot>
skill_roots(const std::filesystem::path& home,
            const std::filesystem::path& xdg_config_home);

// 50-D1.6. Create $HOME/.ymh/skills and $HOME/.ymh/commands, 0700, idempotent.
// Returns false and records a warning on failure; never throws.
[[nodiscard]] bool scaffold_user_ymh(const std::filesystem::path& home,
                                     std::string& error);

} // namespace ymh
```

```cpp
// ── include/ymh/commands/file_command.hpp (NEW) ────────────────────────────
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
namespace ymh {

// 50-D1.3. A discovered markdown command. `name` is the file basename (grammar
// identical to SkillName). `description` is frontmatter `description` or the
// first non-empty body line, truncated to 80 columns. `body` is the raw
// markdown; `$ARGUMENTS` is substituted at invocation.
struct FileCommand {
    std::string           name;
    std::string           description;
    std::string           argument_hint;   // frontmatter, display only
    std::vector<std::string> allowed_tools; // ADVISORY ONLY (never enforced)
    std::string           body;
    std::filesystem::path file;            // canonical path (provenance)
    bool                  trusted = false; // user tier
};

struct FileCommandLoadWarning {
    std::filesystem::path file;
    std::string           reason;
};

// 50-D1.3/D1.4. Pure-of-IO discovery; malformed frontmatter degrades to "no
// description" (50-F3), never fatal. Deterministic order: root order, then
// byte-wise ascending name. A workspace entry that collides with a trusted one
// is rejected with a warning; a name that collides with a reserved compiled-in
// command is rejected (50-I4).
[[nodiscard]] std::vector<FileCommand>
discover_file_commands(const std::vector<SkillRoot>& roots,
                       const std::vector<std::string>& reserved_names);

// 50-D1.3. Exactly one substitution of `$ARGUMENTS`; absent -> empty.
[[nodiscard]] std::string substitute_arguments(std::string_view body,
                                               std::string_view arguments);

} // namespace ymh
```

```cpp
// ── include/ymh/ui/command_registry.hpp (50-D1.4) ──────────────────────────
namespace ymh::ui {
struct Command {
    std::string              name;        // without the leading '/'
    std::string              description;
    std::function<void(CommandContext&, const std::string& args)> handler;
    std::vector<std::string> aliases{};
    // 50-D1.4: NEW. A file command may shadow a compiled-in command only when
    // this is false; reserved commands (/exit, /quit, /help, /sessions, /skills,
    // /mcp, /status, /context) are never shadowed.
    bool                     reserved = false;
};
} // namespace ymh::ui
```

```cpp
// ── include/ymh/prompt/instructions.hpp ────────────────────────────────────
namespace ymh {
struct InstructionFileConfig {
    std::vector<std::string> project_root_markers{".git"};
    std::vector<std::string> candidates{"AGENTS.md", "CLAUDE.md"};
    std::vector<std::string> local_candidates{"AGENTS.local.md", "CLAUDE.local.md"};
    // 50-D1.5: global candidates, in order. `~/.ymh` before the config root;
    // `$HOME/.claude/CLAUDE.md` last. Only the first existing file per base is
    // read (the loader's current single-global-file rule is generalized to an
    // ordered list; the config-root AGENTS.md keeps its present position).
    std::vector<std::filesystem::path> global_candidates;        // NEW
    bool                     load_local = false;
    std::size_t              max_bytes = 0;
    std::size_t              max_source_bytes = 1048576;
};
} // namespace ymh
```
```cpp
// ── include/ymh/execution/process.hpp ──────────────────────────────────────
namespace ymh {

// 50-D3.2
enum class ProcessEnvMode : std::uint8_t { Inherit, Minimal };

struct ProcessRequest {
    std::string               executable;
    std::vector<std::string>  argv;
    std::filesystem::path     cwd;
    std::vector<std::pair<std::string, std::string>> environment;
    ProcessEnvMode            env_mode = ProcessEnvMode::Inherit;   // NEW
    // 50-D3.5 (Rev 2): spawn()-only. When set, the child's stderr is appended
    // to this file (0600) instead of /dev/null. `run()` ignores it.
    std::optional<std::filesystem::path> stderr_path;               // NEW
    std::chrono::milliseconds timeout{0};
    std::optional<std::chrono::milliseconds> deadline;
    bool                      capture_stdout{true};
    bool                      capture_stderr{true};   // spawn() does not read this
    OutputSink*               sink{nullptr};
};

} // namespace ymh
```

```cpp
// ── include/ymh/config/config.hpp ──────────────────────────────────────────
namespace ymh {
struct McpSettings {
    …
    // 50-D3.3 (Rev 3): no `inherit_env` — MCP stdio children always Inherit.
    bool log_child_stderr = false;   // 50-D3.5 (global-layer only)
};
} // namespace ymh
```

```cpp
// ── src/config/config.cpp (internal; 50-D4) ────────────────────────────────
namespace ymh {
// 50-D4: NO new function parameter. The guard is added at the existing
// `global_layer` sites in `apply_document`:
//   * the `mcp_servers` pre-scan   (src/config/config.cpp:958)
//   * the `apply_mcp` call site    (src/config/config.cpp:990)
// `apply_mcp` / `apply_mcp_servers_object` keep their current signatures:
// threading an unused `global_layer` into the latter would be
// -Wunused-parameter under -Werror (it does not need the flag once the guard
// is at the pre-scan). If the implementer instead puts the guard inside the
// function, the parameter must be used there.
// Workspace-layer error text (raised via fail, not ignored):
//   config <path>: 'mcp' is global-layer only
//   config <path>: 'mcp_servers' is global-layer only
} // namespace ymh
```
```cpp
// ── include/ymh/ui/supervisor.hpp ──────────────────────────────────────────
namespace ymh::ui {
struct SupervisorRunOptions {
    …
    // 50-D2.4: `--new` forces create_session on attach; takes precedence over
    // `initial_resume` exactly as the CLI already documents (cli.cpp:1198-1200).
    bool force_new_session = false;                                      // NEW
    // 50-D2.6: the test seam for the fresh-launch rule.
    std::function<void(const WorkspaceId&, const SessionId&)> on_focus_decision; // NEW
};
} // namespace ymh::ui
```

```cpp
// ── include/ymh/ui/supervisor_harness.hpp (additive test seam) ─────────────
namespace ymh::ui {
class SupervisorHarness {
    …
    // 50-D2.6
    virtual void seed_live_session(const WorkspaceId&, const SessionId&) = 0;
    [[nodiscard]] virtual std::vector<std::string> focus_decisions() const = 0;
};
} // namespace ymh::ui
```

## 8. Invariants

Numbered `50-I#`. Each is testable or explicitly labelled a **retained/regression
pin**. The "Gate" column is `Y` (gates this change) or `pin` (retained).

| ID | Gate | Invariant |
|---|---|---|
| 50-I1 | Y | A user skill/command root is used only if absolute; a relative root disables that root and records one warning (retains 20 §3.1 M2). |
| 50-I2 | Y | Discovery performs no writes; `~/.ymh` scaffolding is the only write and only on the conventional path. |
| 50-I3 | Y | A workspace (untrusted) entry never shadows a user (trusted) entry; rejection is deterministic. |
| 50-I4 | Y | A file command never shadows a **reserved** compiled-in command (real `Command::reserved` flag, 50-D1.4); a non-reserved compiled-in command may be shadowed with a warning. |
| 50-I5 | Y | `$ARGUMENTS` is substituted exactly once, on the raw body, before the composer. |
| 50-I6 | pin | A file command is not a `Tool` and is never model-invocable. |
| 50-I7 | Y | A fresh launch creates a new session in the cwd workspace and never focuses a live one (reconciled with spec 49's lazy spawn: the session is created when the cwd workspace is lazily created on the first prompt). |
| 50-I8 | Y | Once the cwd workspace exists (explicit activation, or 49-D1's lazy creation), it is the initial active workspace; a missing scan entry is added from the registry, never replaced by another workspace. On a bare launch no workspace exists and 49-D1 owns creation — no spec is appended and no notice is emitted. |
| 50-I9 | Y | `--resume` / `/sessions` / Ctrl-S still open the requested session; the `resume_in_flight_` gate is retained. |
| 50-I10 | Y | A reconnect with a non-empty `activeSessionId` never creates a session. |
| 50-I11 | Y | The MCP child **always** gets the configured `env` overlaid on the inherited env (`Inherit`); configured wins. |
| 50-I12 | Y | MCP stdio children never use `Minimal`; the retired `mcp.inherit_env` key is a `ConfigError`; `Minimal` has no production caller. |
| 50-I13 | Y | Env values are never written to ymh's own logging (incl. the daemon log sink `host.log`); the opt-in **per-server** raw child-stderr file (50-D3.5) is a separate diagnostic artifact, off by default, `0600`. |
| 50-I14 | Y | `spawn()` and `run()` share environment semantics under `Inherit`. |
| 50-I15 | Y | Under `Minimal`, the keys **ymh seeds** are exactly `PATH=/usr/local/bin:/usr/bin:/bin` + `request.environment`; child-injected `LC_CTYPE` (PEP 538) is outside the invariant. |
| 50-I16 | Y | `stderr_path` is honored only by `spawn()`; unset keeps `/dev/null`; `run()` ignores it. |
| 50-I17 | Y | `mcp.log_child_stderr` is global-layer only; a workspace layer setting it is a `ConfigError`. |
| 50-I18 | Y | The PTY path is unaffected by `env_mode`; it inherits `environ` and overlays. |
| 50-I19 | Y | The MCP child's cwd defaults to the workspace root when the server's `cwd` is empty. |
| 50-I20 | Y | ymh does not filter the MCP child's inherited env; `httpx`-relevant vars (proxy, `SSL_CERT_*`, `NETRC`, `HOME`) reach it. |
| 50-I21 | Y | An MCP server's `cwd` is root-confined; an out-of-root `cwd` is `McpError{ConfigInvalid}` (never an unhandled `PathEscape`); absent defaults to the workspace root. |
| 50-I22 | Y | MCP server definitions are global-layer only; a workspace-layer `mcp`/`mcp_servers` is a `ConfigError` with the exact text `'mcp'`/`'mcp_servers' is global-layer only`. |

---

## 9. Failure modes

Component-local tags `50-F#`; each maps to the shared F1–F12 finding where
applicable (`00-architecture.md:4836-4856`).

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 50-F1 | F1 | Command/skill file escapes its root via symlink | Skip + warning; realpath checked against the root. |
| 50-F2 | — | `$HOME` unset | Home/Claude roots + scaffolding disabled; config/workspace roots remain. |
| 50-F3 | — | Malformed command frontmatter | Degrade to "no description"; command still runnable. |
| 50-F4 | — | `~/.ymh` scaffolding fails | Warning; run proceeds. |
| 50-F5 | F6 | File command collides with a reserved command | Reject with warning; compiled-in row wins. |
| 50-F6 | F10 | cwd registry row missing **during an explicit activation** (`--resume`/`--new`) | Notice; supervisor starts; never silently focuses another workspace. A **bare launch** has no row by design (49-D1) and is not a failure. |
| 50-F7 | F10 | Resume in flight when `session.list` lands | `resume_in_flight_` guard; no create. |
| 50-F8 | F3 | Late `session.list` after a selection | Branch requires empty `activeSessionId`. |
| 50-F9 | F1 | Inherited env leaks a secret to an MCP server | `Inherit` mandatory for MCP (50-D3.3); MCP defs global-layer only (50-I22); `env_denylist` follow-up (50-OQ-4). |
| 50-F10 | — | `${VAR}` reference missing | `ConfigInvalid`; server skipped/failed with a warning. |
| 50-F11 | F8 | Huge inherited environment | OS-bounded; no ymh cap (recorded). |
| 50-F12 | F1 | `stderr_path` cannot be opened | Log once, fall back to `/dev/null`; never fail MCP start. |
| 50-F13 | — | Raw child-stderr log contains a secret the child printed | Off by default; per-server file `0600` under `.ymh/mcp/`; 50-I13 (ymh's own logging) unchanged. |
| 50-F14 | — | Helper's `load_dotenv()` is cwd-relative | Investigated for item 9 and cleared; cwd is the workspace root (50-I19); no cwd-default change pinned. |
| 50-F15 | F4 | MCP server `cwd` is an absolute path outside the workspace root | Resolve inside the `try`, rethrow `McpError{ConfigInvalid}` (50-I21); never an unhandled `PathEscape`. |
| 50-F16 | F1 | Workspace-local `.ymh/config.jsonc` defines MCP servers | `ConfigError` (global-layer only, 50-I22); no servers from that layer load. |

---

## 10. dsh (DeepSeek Harness) mapping

| dsh concept | ymh realization (50) |
|---|---|
| Skill/command plugin discovery | Ordered absolute roots (`~/.ymh`, config root, `$HOME/.claude`, workspace) with trust by provenance; file commands are prompt templates, not tools. |
| Session lifecycle | A fresh launch is a new session; resumption is an explicit act. This mirrors dsh's explicit-resume posture and strengthens 46-D7, reconciled with spec 49's lazy spawn. |
| Capability environment | An MCP child is a capability subprocess; its environment is an explicit policy (`Inherit` mandatory for MCP), not an accident of `clearenv()`. |
| Capability configuration | MCP server definitions are trusted configuration (global-layer only), never supplied by a cloned workspace. |

---

## 11. Test plan

### 11.1 Unit (hermetic, no LLM, no daemon)

- **50-D1:** `skill_roots` table over injected `HOME`/`XDG_CONFIG_HOME`; relative
  root disable; `scaffold_user_ymh` idempotence + failure warning;
  `discover_file_commands` precedence/reserved/`$ARGUMENTS`; malformed
  frontmatter; instruction global-candidate order.
- **50-D2:** harness fresh-launch creates; `--resume` resumes; reconnect neither;
  missing cwd spec appended.
- **50-D3:** `ProcessEnvMode` overlay/minimal (assert the **ymh-seeded** key set
  only, tolerating child-injected `LC_CTYPE`, 50-I15); the retired
  `mcp.inherit_env` key is a `ConfigError`; `mcp.log_child_stderr` parse and
  workspace-layer rejection; the hermetic env-difference test
  (proxy/`SSL_CERT_*`/`NETRC`/`HOME` visible under `Inherit`, gone under
  `Minimal`); `spawn()` honors `stderr_path` while `run()` ignores it; PTY has no
  `env_mode`; the MCP child's cwd policy per 50-I21.
- **50-D4:** a workspace-layer config defining `mcp_servers` (and, separately,
  `mcp.server`) → `ConfigError` with the exact text
  `config <path>: 'mcp_servers' is global-layer only` (resp. `'mcp'`); the same
  block in the global layer loads.

### 11.2 Integration (FakeLLM / fake daemon)

- Fake daemon emits `session.list` with a live session; a fresh launch creates a
  new session and emits no `session.resume`.
- File-command discovery + `/` completion + `$ARGUMENTS` dispatch through the
  command registry.
- Fake MCP stdio server asserts the inherited vs minimal environment and, with
  `log_child_stderr = true`, that the child's stderr lands in
  `<workspace>/.ymh/mcp/<server>.stderr.log`; it reports its cwd to assert the cwd
  policy (workspace-root default; in-root configured honored; out-of-root rejected).

### 11.3 PTY / live (opt-in, `YMH_LIVE_LLM=1`)

- Ctrl+C a turn, relaunch in the same workspace under a PTY, assert an empty
  transcript.
- With a real jira MCP server and a valid token, the pinned `Inherit` path
  yields a non-401 result (item 9 is resolved; this is a smoke check, guarded by
  a token env; never logged).

### 11.4 Invariant / failure-mode coverage

Every `50-I#` has at least one unit or integration assertion; every `50-F#` is
exercised by at least one negative test. The matrix is completed in the
implementation plan, not this spec.

---

## 12. Open questions / interpretations

- **50-OQ-1 (item 1: `~/.ymh` vs config root).** The user said "from ~/.ymh and
  $HOME/.claude". This spec keeps `$XDG_CONFIG_HOME/ymh/skills` as a third user
  root (retro-compatibility with spec 20) and puts `~/.ymh` first. If the user
  wants `~/.ymh` to *replace* the config-root skills dir, that is a one-line
  change (drop root 2). **Interpretation chosen: additive.**
- **50-OQ-2 (file-command argument grammar).** Only `$ARGUMENTS` is pinned.
  Claude-Code's `$1`/`$@`/frontmatter `argument-hint` execution is not. A
  follow-up spec can extend it without breaking 50-I5.
- **50-OQ-3 (item 9 — RESOLVED; closed).** The 401 was an **invalid
  (expired/revoked) credential**, not a harness defect. localcode failed
  identically with the same old token, proving the harness was never at fault.
  The credential-validation procedure is pinned in §5.5. **Closed; do not
  re-open without new evidence.**
- **50-OQ-4 (MCP env inheritance and secrets).** Inheriting the daemon env could
  expose a secret to a malicious MCP server. `Inherit` is **mandatory** for MCP
  (a curated set silently changes what environment-reading libraries see), so
  there is no isolation opt-out today. MCP server definitions are **global-layer
  only** (50-I22), so a cloned repository cannot inject a server that would
  receive the daemon's environment; the remaining exposure is a server the user
  themself configured globally. A future spec may add a per-server
  `env_denylist` / `env_allowlist` that strips named secrets while preserving the
  keys libraries need. Recorded.
- **50-OQ-5 (item 3 — the discriminating question; MUST be answered by the user
  or a live repro).** Did the user return to the **same workspace but a
  different (previous) session**, or genuinely to a **different workspace**?
  The code supports only the former: `live.front()`
  (`src/ui/supervisor.cpp:1415-1422`) focuses a session within the cwd
  workspace; `workspaces.front()` (`:409-417`) is alphabetical
  (`src/registry/registry.cpp:813-822`), not the previous workspace; and no
  last/focused workspace is persisted (`src/registry/registry.cpp:44-59`). The
  latter (Branch B) requires the cwd workspace to be absent from the one-shot
  scan and is unexplained by any path found. The deterministic discriminating
  repro is in §4.5. **Design covers both branches:** D2.1–D2.2/D2.4 fix the
  session-focus defect; D2.3 adds the cwd-wins guarantee. Neither is asserted as
  the user's root cause until this question is answered.
- **50-OQ-6 (reconciliation with spec 49).** Spec 49
  (`49-switcher-single-workspace.md`) makes a bare `ymh` start with **zero
  workspaces** and create the cwd workspace lazily on the first prompt (49-D1).
  Rev 1 framed this as a conflict with a "48-D3.3 cwd always wins at startup"
  clause; **that clause does not exist** — spec 48 has no `D3` decision (the old
  48-D3, the Ctrl+C re-entry item, moved to this spec as 50-D2; see §14). The
  real overlap is 46-D7/46-I13 and 23-D58. This spec reconciles: 50-D2.3's
  cwd-wins guarantee applies **once the cwd workspace exists**; on a bare launch
  `initial_workspace` is a path hint only — no spec is appended and **no spurious
  notice** is emitted (50-F6 is scoped to explicit activation). The lazy-creation
  timing is owned by 49-D1; the fresh-launch guarantee (D2.1) applies when the
  workspace is created. **Resolved by scoping** (no contradiction with 49).

---

## 13. Cross-spec reconciliation (48/49/50)

The original spec 48 was split into 48 (TUI/rendering), 49 (lazy
spawn/switcher), and 50 (this spec), and each half was revised in isolation.
Rev 2 reconciles the three; this section is the record.

### 13.1 Removed fabricated reference (48)

Rev 1 of this spec (and Rev 2 of spec 49) cited a "48-D3.3 cwd always wins at
startup" clause. **That clause does not exist.** Spec 48 has **no `D3` decision**:
its kept decisions are D2/D4/D5/D6/D7/D8 (48 §15 records that the old 48-D3 — the
Ctrl+C re-entry item — moved to this spec as **50-D2**; 48's only `D3` token is
that historical note at `48:1007`). No "cwd always wins" text exists anywhere in
48 (a `cwd` grep in 48 returns no matches). The reference was removed from
**49-A12** and replaced with the real overlap (46-D7.1 / 46-I13 / 23-D58 /
50-D2). Spec 48 is not edited (it is being implemented concurrently); if 48 later
needs an amendment it is recorded here, not in 48.

### 13.2 49 ↔ 50 (lazy spawn vs the fresh-launch guarantee)

- 49-D1 starts a bare `ymh` with **zero workspaces** and creates the cwd
  row+daemon lazily on the first prompt; the workspace is modeled/attached only
  after the daemon registers (`ensure_worker_loop`,
  `src/ui/supervisor.cpp:1028-1079`), and the first draft rides `pending_creates_`
  consumed on `on_link_state(Attached)` (`:1334`).
- 50-D2.1's "always create" fresh-launch rule therefore applies **when the cwd
  workspace exists**: an explicit activation (`--resume`/`--new`), or after
  49-D1's lazy creation. On a bare launch there is nothing to focus or create
  yet.
- 50-D2.3's cwd-wins guarantee is scoped accordingly: `initial_workspace` is a
  path hint on a bare launch — no spec is appended from a nonexistent row and
  **no spurious notice** is emitted (50-F6 is scoped to explicit activation;
  50-I8 restated). See 50-OQ-6.
- Both specs route the first prompt through the same attach branch (46-I13,
  `src/ui/supervisor.cpp:1415-1427`) and `create_session`
  (`src/ui/supervisor.cpp:1453-1490`).

### 13.3 48 ↔ 49/50 (the split)

- 48 keeps D2/D4–D8 and is self-contained (48 §15). Its invariants 48-I11/I12
  and the span-truncation code around `48:620-643` are unrelated to
  cwd/workspace selection; 49 no longer references them.
- 48-D10 (MCP 401/403 hints) was dropped by user decision; the diagnostic
  procedure survives here in §5.5.
- No spec 48 decision is contradicted by 49 or 50.

### 13.4 46/23 ↔ 50 (the real session-lifecycle overlap)

- 50-S6 supersedes **46-D7.1**'s `focus live.front()` (`46:1530`) with
  `create_session` ("always create"). **46-I13** (`46:3474`) remains the
  complementary attach-branch description; **23-D58** (`23:375`) owns the refresh
  auto-create. 49-D1's lazy first prompt uses the same branch, so 46/23/49/50
  agree.

---

## 14. Revision log

- **Rev 1 (initial; split from spec 48).** This spec was created by the user's
  decision to split the nine-item spec 48 by risk. It carries the higher-risk
  content with its existing pinned design: item 1 (skill/command discovery,
  was 48-D1), item 3 (the fresh-launch session guarantee, was 48-D3, now 50-D2),
  the MCP child environment fix (was 48-D9.1–D9.6, now 50-D3), and the MCP
  global-layer-only guard (was 48-D9.7, now 50-D4). Decision IDs, invariants
  (`50-I1–I22`), and failure modes (`50-F1–F16`) were rebased contiguously.
  - **Dropped:** the original 48-D10 (MCP 401/403 hints) was **dropped by user
    decision** (the underlying problem was an expired credential, not a harness
    defect); only the credential-validation procedure survives (§5.5).
  - **Global-layer guard corrected:** the guard is pinned once at the existing
    `global_layer` call sites (`src/config/config.cpp:958`, `:990`) rather than
    by threading an unused parameter into `apply_mcp_servers_object` (which
    would break the build under `-Werror`).
  - **Citations verified against HEAD `bbf96aeaa`:** `resolve_mcp_env`
    `:21-60`; `src/mcp/mcp_transport.cpp:104` (the `try`); `src/cli/cli.cpp:183`
    (`scaffold_for_invocation`); `src/agent/workspace_runtime.cpp:42-51`
    (skill-catalog construction/discovery timing); `src/execution/process.cpp:544-548`;
    `src/config/config.cpp:958`/`:990`/`:997-999`/`:429-432`.
  - **Reconciliation with spec 49** recorded in 50-OQ-6.

- **Rev 2 (gate repair; adversarial gate FAIL: 1 HIGH + MEDIUMs).**
  - **(HIGH) 50-D2.3 reconciled with spec 49's lazy spawn.** Rev 1's "append a
    spec from the registry row for `initial_workspace`, else fail loudly" would
    have (a) appended from a **nonexistent** row on a bare launch and (b) emitted
    a **spurious notice** on every bare launch. 50-D2.3 is now scoped to when the
    cwd workspace exists (explicit activation, or after 49-D1's lazy creation);
    on a bare launch `initial_workspace` is a path hint only. 50-I8 restated and
    50-F6 scoped to explicit activation (50-OQ-6; §13.2).
  - **50-I13 carve-out corrected.** Rev 1 routed the opt-in raw MCP child stderr
    to `<workspace>/.ymh/host.log` and declared it "outside" 50-I13 — but that
    path **is** the daemon's own log sink (`config_.log_sink`, `src/cli/cli.cpp:302`;
    `src/host/workspace_host.cpp:455-456`, opened `O_TRUNC` at `:1064`), so raw
    child output could embed env values into ymh's own log. 50-D3.5 now routes to
    a **dedicated per-server** `<workspace>/.ymh/mcp/<server>.stderr.log` (0600,
    under the daemon `umask(0077)`, `:1061`); 50-I13 is restated as "ymh's own
    logging (incl. `host.log`) never contains env values", and 50-F13, the sketch,
    and both test-plan entries were updated. (The daemon's `umask(0077)`/0600 did
    limit exposure, but the invariant as written was false.)
  - **50-D1.4's reserved mechanism made real.** The `Command` struct
    (`include/ymh/ui/command_registry.hpp:51-56`) has **no** `reserved` field, so
    "unless marked reserved" had no mechanism. 50-D1.4/50-I4 now pin a new
    `bool reserved = false;` member and the reserved set, plus a `Command` sketch
    in §7.
  - **MCP global-layer guard re-verified.** `apply_mcp` (`src/config/config.cpp:578`)
    and `apply_mcp_servers_object` (`:1186`) take no `global_layer` parameter, so
    pinning the guard at `:958`/`:990-991` introduces **no** unused parameter and
    no `-Werror` break (50-D4.2).
  - **Cross-spec reconciliation added (§13):** the fabricated "48-D3.3 cwd always
    wins" reference removed (48 has no `D3`), the 49↔50 lazy-spawn/fresh-launch
    boundary pinned, and the real 46-D7.1/46-I13/23-D58 overlap recorded. Spec 48
    is not edited.

  **Verification status: DRAFT — not yet reviewed.** No implementation may begin
  until an independent gate marks this spec `verified` (AGENTS.md, the rule).
