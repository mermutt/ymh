# 90 - Path-Scoped Write/Delete Permissions and Tool-Mediated Reads: a root-relative `path` rule dimension, in-workspace auto-approval, and a class-scoped Session grant

```
Status: verified (Rev 18)
Revision: 18
Verification status: adversarial reviewer (independent, Rev 18): GATE PASS (0 HIGH / 0 MEDIUM, 5 LOW); Oracle confirm PASS
Revision note: Rev 9 adds the TEMP-ROOT EXEMPTION; Rev 12 simplifies it; Rev 13 makes it write-only; Rev 16 bounds it to direct, non-recursive /tmp file writes; Rev 17 fixes the Rev 16 gate findings; Rev 18 fixes the Rev 17 gate findings. The Rev 9..15 amendments failed their gates; Rev 15 was re-gated (0 HIGH / 1 MEDIUM / 5 LOW); Rev 16 was re-gated (0 HIGH / 1 MEDIUM / 5 LOW); Rev 17 and Rev 18 apply those findings and are not re-gated (see sec 17). The invariant/failure-mode IDs are PS1-PS33 / PS-F1-PS-F67, and the /tmp guard override is part of the verified scope.
Tracker: DESIGN_STATUS.md row 90 matches this header at Rev 18 (verified;
         PS1-PS33 / PS-F1-PS-F67 and the /tmp guard override).
Component: 90 (errata) -- makes the dormant `path` rule dimension live, so a
           `config.jsonc` rule can auto-approve operations whose target is the
           workspace root or below it while outside/ancestor targets fall
           through to Ask, and makes the permission dialog's "Allow for
           session" option cover a meaningful class instead of one exact shell
           string. GUARANTEE SCOPE (Oracle reshape): this spec guarantees
           path-scoped WRITE/DELETE enforcement and TOOL-MEDIATED READS for the
           path tools; shell/terminal VISIBLE operands/redirections are matched
           by the best-effort classifier; shell/terminal HIDDEN reads through
           interpreters/unknown syntax are NOT claimed and are recorded as a
           planned follow-up (working name spec 91 "read isolation & egress",
           OQ-90-9; not a registered spec). It does NOT weaken the
           spec-89 shell safety guard **except** for the Rev 9 TEMP-ROOT
           EXEMPTION, which bypasses L1 only for a direct, non-recursive
           write/delete/create `shell`/`terminal` invocation whose every visible
           target is strictly under canonical `/tmp` (sec 4.8); reads keep the normal read policy
           and every other request keeps the full L1/L2 order.
Depends on: 09-permissions.md (verified; `PermissionPolicy`, `PolicyVerdict`,
           `PermissionRequest`, the rule fold), 46-permissions-ui-errata.md
           (verified; config-gated auto-allow, grant layers, O-H1/O-M1, 46-I5/
           46-I6/46-I33), 52-endpoints-models-and-dsh-agent-presets.md
           (verified; permission presets), 89-shell-command-safety-guard-errata.md
           (verified; L1 guard precedence, SG1-SG17), 07-tools-execution.md
           (verified; `ExecutionEnvironment::resolve`, X3), 25-ui-ux-errata.md
           (verified; `force_ask` and the dialog option set), and
           56-dsh-fidelity-gap-closure-errata.md (the 56-D6 non-mirror rule).
Supersedes: 46 O-H1's no-path disposition for `permissions.rules` (the
           `path` key is no longer a ConfigError), 46 sec 3.2 item 2's
           "the `path` dimension is not part of the config rule shape"
           (`46-permissions-ui-errata.md:429-439`), 46 sec 3.3 / 46-F21's
           path-key failure (`:532-535`), the path-key clause of 46-I1
           (`:3462`), and the exact-command-only Session grant of 46-D2 /
           46-I6 for shell/terminal (`src/policy/permission_policy.cpp:535-657`,
           `46-permissions-ui-errata.md:3467`). It does NOT supersede 46-I5 or
           46-I33 for durable `Always` grants: those remain tool + command only
           and keep rejecting a `path` key in `permissions.jsonc`.
```

## 1. Purpose, user request, and precedence

### 1.1 The three requirements (verbatim intent)

1. **In-workspace auto-approval for writes/deletes and tool-mediated reads;
   outside asks; protected paths excluded.** The exact user-facing promise
   (Oracle reshape) is: auto-approve an operation whose target is the
   workspace root (the directory ymh launched in) or a descendant, and
   explicitly ASK for anything above/outside it, for **WRITES and DELETES**
   and for **path-tool READS** (`read_file`, `grep`, `glob`,
   `git_status`/`git_diff`). For `shell`/`terminal`, **visible**
   operands/redirections and `terminal open`'s declared `cwd` are matched by
   the best-effort classifier (sec 4.4);
   **hidden reads** through interpreters or unknown tool syntax are
   **best-effort/NotClaimed** and are recorded as a planned follow-up
   (working name spec 91 "read isolation & egress", OQ-90-9; not a
   registered spec), not guaranteed here. The one deliberate exception is the
   permission system's own durable state and VCS control surface: `.ymh/**`
   (config, grants, session DB, checkpoints) and `.git/**` (hooks, config,
   object store) are **protected paths** and are hard-denied before any allow
   rule (sec 4.3.1, 90-D13). The behavior is configured in the **global**
   `config.jsonc`, not hard-coded (a workspace `permissions` table is rejected,
   90-D18). See the guarantee matrix (sec 1.7).
2. **"Approve for this session" must work or go away.** The dialog option
   "Allow for session" appears never to work. Either make it a useful
   scope-aware grant or remove it. This errata keeps it and makes it
   class-scoped; it is never a guard bypass.

3. **Shell writes to `/tmp` are always allowed (Rev 9; write-only in Rev 13).**
   Verbatim: "writes to /tmp shall always be allowed regardless of all the
   other permission settings". A **direct, non-recursive, regular-file/new-path write/delete/create**
   `shell`/`terminal` invocation (the declared `terminal open` cwd is the one
   existing-directory exception) whose every visible filesystem target is
   provably strictly under the canonical `/tmp` root is **Allow**, exempt from the outside/above ask
   path, the spec-89 L1 destructive-outside `Forbid`/`Confirm` attributable to
   those targets, `ReadOnly`, `force_ask`, and every global permission setting.
   The exemption is **per target** and shell/terminal only: one `/tmp` operand
   never launders a non-temp/ambiguous operand or an untargeted destructive
   construct. **Reads are not exempt** (Rev 13): `cat /tmp/x`, read-only
   shell/terminal commands, and path tools keep the normal policy
   (shell reads use `permissions.shell`/`default`/the guard; `permissions.read`
   seeds path tools, not shell). Recognized read verbs and
   read-only/mixed invocations are non-exempt; see sec 4.8.2 for the
   read/write classification. **Path tools are NOT exempt** (Rev 12):
   `read_file`/`write_file`/`edit_file`/`grep`/`glob`/`git_*` remain
   workspace-root-confined and a `/tmp` path still throws `PathEscape`; use
   `shell` for `/tmp` writes. `$TMPDIR` and other temp bases are **not**
   admitted; only canonical `/tmp`. The bare `/tmp` root, `/tmp/..`,
   `/tmpfoo`, internal `..` components, globs/braces/unresolved expansions,
   symlinks resolving outside or into the workspace/protected tree, and a
   workspace at/under `/tmp` are never promoted; they keep the normal verdict
   (`Ask`, or a guard `Deny`) (sec 4.8, 90-D23, PS28-PS33). This is an explicit
   user scope decision, not a dsh mirror.

The exact shipped dialog label is `Allow for session`
(`src/ui/ui_render.cpp:1164-1169`). This errata leaves that text unchanged: the
option index stays 1 in the four-option dialog (0 Once, 1 Session, 2 Always,
3 Deny), and a `force_ask` request keeps its two-option dialog
(`{"Allow once", "Deny"}`). What changes is the grant the Session answer
creates, not the label.

### 1.2 Verified current state

| ID | Finding | Anchor (shipped code) |
|---|---|---|
| **90-C1** | `permissions.rules` has no `path` dimension. The settings struct is `{tool, command, effect, id}`; the parser's `reject_unknown` set is `{"tool","command","effect","id"}`; a `path` key is a `ConfigError`. | `include/ymh/config/config.hpp:142-150` (comment at `:142-144`), `src/config/config.cpp:542-545`, `src/cli/wiring.cpp:175-183` |
| **90-C2** | `PermissionRequest::path` is never populated by the agent loop. The loop's request-build block sets `root`, `sandbox`, `kernel_confinement`, `destructive`, and `force_ask` only. Only tests set `path`. | `src/agent/agent_loop.cpp:871-889`; `tests/unit/permissions_test.cpp:24-37`; `tests/unit/permission_broker_test.cpp:110` |
| **90-C3** | The policy already has `PolicyRule::path` and `rule_matches` checks it, but the dimension is unreachable from config or grants, so it is dead code. `path_pattern()` calls `std::filesystem::relative`, which may stat, violating the policy's I/O-free contract. | `include/ymh/policy/permission_policy.hpp:84-108`, `src/policy/permission_policy.cpp:114-131` (`root_relative_target`), `:148-168`; 09 Q1/Q7 at `docs/design/09-permissions.md:944-946` |
| **90-C4** | `evaluate` order is: L1 guard `Forbid -> Deny`; `ReadOnly` mutating `-> Deny`; L1 `Confirm -> Ask`; `force_ask -> Ask`; then the rule fold, where config rules plus `session_grants_[session]` are candidates. Grants are NOT consulted after `default_verdict`. | `src/policy/permission_policy.cpp:416-520` (guard `:428-442`, candidate fold `:530-532`) |
| **90-C5** | The Session grant is keyed by `tool` + `command_pattern(request)` with `literal=true`. For `shell`, `command_pattern` returns the raw command; for `terminal` it returns only `arguments["command"]` or joined `argv`, and it **never reads `arguments["input"]`** (`terminal write` is classified from the input by `shell_guard_text`, not by `command_pattern`). A Session grant therefore matches only the byte-identical shell command, and an outside/unknown `terminal write` has an empty command and cannot be remembered at all (90-D7 fixes the grant path by using `shell_guard_text`). For path tools `command_pattern` returns empty, so a path-tool grant is tool-wide. The guard's `Forbid`/`Confirm` short-circuits precede grants, so Session can never suppress those. The grant is in-memory and lost on daemon restart. | `src/policy/permission_policy.cpp:535-657` (remember), `:133-162` (command_pattern), `:416-520` (evaluate); `src/policy/shell_guard.cpp:2112-2114` (terminal-write classification); `src/permission/permission_broker.cpp:335-345`, `:149-156`; `src/ui/supervisor.cpp:3581`, `:230-240`; 46-I6 at `docs/design/46-permissions-ui-errata.md:3467` |
| **90-C6** | The builtin read rules are seeded from `permissions.read`, whose default is `"allow"`. Therefore `"default": "ask"` plus a path-allow rule does NOT make an outside `read_file`/`grep`/`glob` ask: the one-dimension builtin read rule still matches and returns `Allow` (execution then throws `PathEscape`). To meet the user's "outside asks" target for every path-bearing tool, `permissions.read` must also be set to `"ask"`. | `include/ymh/config/config.hpp:188-194`; `src/cli/wiring.cpp:149-161`; `src/execution/environment.cpp:64-95` |
| **90-C7** | The workspace is a writable trust boundary for the permission system's own inputs. A workspace `.ymh/config.jsonc` may set `default`/`read`/`write`/`shell` (only `presets`/`default_preset` are global-only); `default:"allow"` sets `master=true` and upgrades every builtin. `write_file`/`edit_file` perform no protected-path check, so an in-root path rule allows `.ymh/config.jsonc`, `.ymh/permissions.jsonc`, `.ymh/sessions.db`, and `.git/hooks`. The grants loader accepts `tool:"*"` and an omitted `command`, and `rule_matches` glob-matches `*`, so a hand-written or agent-written durable grant can become policy-wide on restart. | `src/config/config.cpp:479-494`, `:536`; `src/cli/wiring.cpp:146-161`; `src/tools/builtin_tools.cpp:43-56`, call sites `:141`, `:188`, `:231`, `:304`, `:384`; `src/config/grant_store.cpp:84-95`; `src/policy/permission_policy.cpp:148-169` |
| **90-C8** | `grep` does not per-file-resolve symlinks. It calls `require_contained` only on the base, walks with `recursive_directory_iterator`, tests `is_regular_file` (follows symlinks), and opens each candidate with `std::ifstream` (follows symlinks). A symlink inside the root to an outside file is read and returned. `read_file` is different: it uses `O_NOFOLLOW`. | `src/execution/filesystem.cpp:48-50`, `:212-213`, `:236-254`, `:268`; `src/tools/builtin_tools.cpp:284` |
| **90-C9** | `git_status`/`git_diff` call `git_repository_open_ext(..., flags = 0, ...)`, so libgit2 searches ancestor directories for a repository. A workspace that is a subdirectory of a larger repository reads the ancestor `.git` (history/object store) while the lexical `path` is in-root. | `src/execution/git_service.cpp:93` |
| **90-C10** | Shell `git` subcommands can write `.git/config` or delete `.ymh` without naming either path. The L1 catalogue has no `git` branch (`token_destructive`/`classify_verb` return `None` for `git config --local core.hooksPath .evil` and `git clean -fdx`); L2 grants the root read+write+remove (`kRootRights`) and `/` read-only; the scope is `InRoot`, so the recommended path rule would allow. The workspace layer also still accepts `permissions.*` and `agent.sandbox` today. | `src/policy/shell_guard.cpp:791-925`; `src/execution/landlock.cpp:47-54`, `:106`, `:115`; `src/config/config.cpp:479-494`; `src/tools/builtin_tools.cpp:387-424` |
| **90-C11** | The classifier has no rule for curl local-file operand prefixes: `@/etc/passwd`, `name=@/etc/passwd`, `name=</etc/passwd` are joined to the cwd as `root/@/...` and classified `InRoot`. | `src/policy/shell_guard.cpp:588-655`, `:721-740` |

### 1.3 Why O-H1 removed `path`, and why this errata supersedes it

O-H1 was a Rev 5 resolution in 46: durable grants and config rules were
rescoped to **tool + command only**, and a `path` key in a config rule became a
`ConfigError`, because the shipped loop never populated
`PermissionRequest::path`. A rule that loads but can never match is dead code,
and dead configuration is worse than no configuration
(`docs/design/46-permissions-ui-errata.md:4538`; the rule-shape text at
`:429-439`; the code comment that repeats the reason at
`include/ymh/config/config.hpp:142-144`).

O-H1's reasoning was correct for the code as shipped. This errata does not
disagree with the principle; it removes the cause:

1. the agent loop now **populates** `PermissionRequest::path` for the tools that
   have a path argument (sec 4.2), and
2. `config.jsonc` now **accepts and validates** `permissions.rules[].path`
   (sec 4.6).

A path rule is therefore live: it has a producer (`request.path`), a parser
(`apply_permissions`), and a matcher (`rule_matches`). The dead-code objection
no longer applies. This errata supersedes O-H1's disposition for config rules,
but **retains O-H1 for durable `Always` grants**: `permissions.jsonc` still
rejects `path` (46-I33), and an `Always` grant is still tool + command only
(46-I5). The Session grant is the only grant that becomes class-scoped.

### 1.4 What this changes in one sentence

A `config.jsonc` rule such as
`{"tool":"*","path":"**","effect":"allow"}` now auto-approves in-workspace
**writes/deletes and path-tool reads**, and matches **visible** shell/terminal
operands/redirections that are lexically provably in-root; outside/unprovable
visible targets fall through to the configured default (`Ask` in the
recommended config). **Hidden shell reads are best-effort/NotClaimed** (spec
91/OQ-90-9), not part of this rule's guarantee. "Allow for session" now
remembers a class (tool + target class + command prefix for in-workspace
shell, exact command for outside/unknown shell, tool + in-workspace for path
tools) instead of one exact string. **Rev 9 adds one bounded exception
(direct-write-only in Rev 16):** a **direct, non-recursive,
regular-file/new-path write/delete/create** `shell`/`terminal` invocation whose
every visible filesystem target is a concrete strict descendant of canonical
`/tmp` is `Allow`, outside this whole rule/ask/protected
machinery (sec 4.8); the declared `terminal open` cwd is the one existing-directory
exception; reads and path tools are not exempt.

### 1.5 Precedence and non-negotiables

This errata is additive. Except for the explicit Rev 9 temp-root exemption
(sec 4.8, last bullet), it **never** weakens spec 89:

- `classify_shell_command_with_scope(...).risk` is computed first, and
  `Forbid -> Deny` / `Confirm -> Ask` still return before the rule fold
  (`src/policy/permission_policy.cpp:416-520`; 89 SG2/SG13).
- `SandboxMode::ReadOnly` mutating calls still return `Deny` before the fold
  (09 Q7).
- `force_ask` still returns `Ask` before the fold; `exit_plan_mode` keeps its
  two-option dialog and records no grant (25-D4, 46-D2.6).
- No path rule and no Session grant can turn a guard `Deny` into `Allow` or a
  guard `Ask` into a silent `Allow`. The scope computed for a path rule is
  **matching input**, not an authorization bypass.
- `danger-full-access` is not changed. The user's goal is narrower than that
  preset: path scoping is not a global approval-off switch.
- **One deliberate exception (Rev 9; direct-write-only in Rev 16):** a direct,
  non-recursive, regular-file/new-path write/delete/create `shell`/`terminal`
  invocation whose every visible filesystem target is strictly under canonical
  `/tmp` returns `Allow` before
  every branch above (sec 4.8, 90-D23); the declared `terminal open` cwd is the
  one existing-directory exception; reads keep the normal read policy. It is the only construct in this spec that can override a
  guard `Forbid`/`Confirm`, `ReadOnly`, or `force_ask`, and it cannot be
  disabled by any `permissions.*` key or `agent.sandbox`. Path tools are not
  exempt. Bare, wildcard, brace-bearing, unexpanded, untargeted-`Forbid`, and
  mixed-target temp requests are not exempt and keep the existing precedence
  (PS30).

The invariant in one line: **a path rule can only ever make an in-workspace
visible/declared target more likely to be allowed; it can never make an
outside target allowed, a hidden shell read guaranteed, or a guard verdict
weaker -- except the explicit, bounded temp-root exemption of sec 4.8, which
is a user scope decision and not a path rule.**

### 1.6 Non-goals

- No OS sandbox change beyond the Rev 9 temp grant **and the `ReadOnly`
  root-rights tightening** (`workspace_writable`: `kRootRights` vs
  `kReadRights`). `ExecutionEnvironment::resolve()` and spec-89 L2 remain the
  real boundaries (`src/execution/environment.cpp:64-95`); L2 gains only a
  RW+make+remove+refer grant on canonical `/tmp` (90-D26).
- No configurable temp root and no temp-root opt-out. The root is canonical
  `/tmp` **only**; `$TMPDIR`, `$XDG_RUNTIME_DIR`, and every other base are
  ignored (Rev 12). No `permissions.*` key, `agent.sandbox`, preset, rule, or
  grant can add, remove, or disable it (90-D23/PS29). A workspace at/under
  `/tmp` does not receive the exemption, so the `ReadOnly` root-rights
  invariant holds (90-D26). A configurable root is OQ-90-10.
- **No path-tool `/tmp` exemption (Rev 12).** `read_file`, `write_file`,
  `edit_file`, `grep`, `glob`, `git_status`, and `git_diff` remain
  workspace-root-confined; under `Workspace`/`ReadOnly` `resolve()` throws
  `PathEscape`, and under `Unrestricted` `LocalFilesystem::require_contained`
  still throws. The exemption is `shell`/`terminal` writes only. This is a
  stated limitation, not a bug: use `shell` for `/tmp` writes.
- No wire-protocol change. `protocol::PermissionRequest` carries tool,
  arguments, summary, expiry, and `force_ask` only
  (`include/ymh/transport/protocol.hpp:280-289`); the core `path` and the
  computed target class stay daemon-side. The UI continues to send the
  existing `PermissionScope::Session` (`:277-295`).
- No durable path field. `Always`/`permissions.jsonc` keep the tool + command
  file shape; a path-tool grant is loaded as in-root-wide (no new key). A finer
  durable path class is OQ-90-1.
- No new permission preset. The recommended behavior is an ordinary
  `permissions.rules` entry plus the built-in protected deny, not a preset.
- No change to the guard's destructive-command catalogue, and no L2 change
  beyond the bounded `/tmp` grant/root-rights tightening (90-D26/A27). H1 is a
  policy/tool guard, not a new kernel rule.
- No network policy and no shell hidden-read guarantee. A shell command that
  connects to a host or reads an arbitrary file through an interpreter/unknown
  syntax is not a filesystem target the path dimension can bound. **Spec 91
  "read isolation & egress"** is the planned follow-up (kernel read allow-list/
  Landlock read confinement + network policy, OQ-90-9); it is not authored
  here. The guarantee matrix labels these cells NotClaimed (sec 1.7).
- No claim that a shell subprocess cannot read, write, or delete protected
  paths through an unpredicted vector (symlink alias, interpreter payload,
  recursive tool). The explicit-target check and the shell git-control
  classifier cover the named vectors; the residual is recorded (risk 6/
  OQ-90-6). The workspace-config escalation is separately closed by 90-D18.

### 1.7 Guarantee matrix (read / write / delete / exec)

This matrix is the anti-enumeration device. A new shell syntax that reads a
hidden file is covered by the **NotClaimed** hidden-read cell, not a new
defect; a new visible operand is covered by **Matched**; a new tool class must
add a row.

| Tool class / target | Read | Write | Delete | Exec |
|---|---|---|---|---|
| `read_file`, `grep`, `glob`, `git_status`, `git_diff` (tool-mediated reads) | **Matched** | **Enforced** (no write capability) | **Enforced** (no delete capability) | **Enforced** (no exec capability) |
| `write_file`, `edit_file` (tool-mediated writes; `edit_file` reads its target) | **Matched** (`edit_file`) / **Enforced** (`write_file` has no read capability) | **Matched** | **Enforced** (no delete capability) | **Enforced** (no exec capability) |
| `shell`/`terminal` **visible** operands/redirections | **Matched** (best-effort classifier) | **Matched** (policy; L2 is the kernel backstop) | **Matched** (policy; L2 is the kernel backstop) | **NotClaimed** (arbitrary exec; 89 SG-F22) |
| `shell`/`terminal` **hidden** reads via interpreters/unknown syntax | **NotClaimed** (best-effort; planned follow-up, OQ-90-9) | **Enforced\*** (L2 write boundary) | **Enforced\*** (L2 delete boundary) | **NotClaimed** (arbitrary exec; 89 SG-F22) |
| `shell`/`terminal` **visible target(s), every one strictly under canonical `/tmp`**, on a **direct, non-recursive, regular-file/new-path write/delete/create** invocation (no internal `..`, brace/glob/unresolved, recursive/following form, directory/non-regular target other than the terminal-open cwd, or read operation) | **Normal policy** (reads are not exempt) | **Allow** | **Allow** | **Matched** (visible target; L2 `/tmp` grant; hidden exec NotClaimed) |
| MCP/LSP | **NotClaimed** (89-D18/15) | **NotClaimed** (89-D18/15) | **NotClaimed** (89-D18/15) | **NotClaimed** (89-D18/15) |

Cell meanings:

- **Enforced** -- a kernel boundary (L2) or the structural absence of the
  capability in that tool. `Enforced\*` is Enforced only while L2 is
  `Enforced`; when L2 is `Unavailable`/`NotApplicable` it degrades to
  **NotClaimed** (89 SG-F20), and the residual is recorded.
- **Matched** -- policy matches on declared/visible targets (the path rule,
  the L1/L2 guard, the `resolve()`/tool checks). It is not a kernel guarantee
  for a hidden syntax.
- **NotClaimed** -- best-effort only. The residual is stated with its reason
  and anchor; per the Oracle reshape, a `NotClaimed` cell with a stated
  residual is **not** a defect. Shell hidden reads/egress are a planned
  follow-up (working name spec 91, OQ-90-9); MCP/LSP are 89-D18; arbitrary
  shell exec is 89 SG-F22.

**Temp-root row (Rev 9; simplified in Rev 12).** This row is evaluated
**before** the guard, `ReadOnly`, `force_ask`, the protected check, and the
fold; it overrides those cells for the same shell/terminal **write** target set.
A **direct, non-recursive, regular-file/new-path** write/delete/create
`shell`/`terminal` invocation whose every visible filesystem target is a
concrete strict descendant of canonical `/tmp` is `Allow` even under
`default:"deny"`, `shell`/`write`, `ReadOnly`, or `force_ask`; the declared
`terminal open` cwd is the one existing-directory exception; reads are
**not** exempt and keep the normal shell/default/guard policy.
**Path tools are not this row**: they keep the ordinary rows and their
`PathEscape` bound (sec 4.8.2). The bare `/tmp` root, `/tmp/..`, `/tmpfoo`, a
shell glob, a brace-bearing token, an unexpanded `$VAR`, an untargeted
`Forbid`, a bare `terminal open`, and a mixed command are **not** this row
(they keep the normal verdict: `Ask`, or a guard `Deny`; PS30). `Allow` here
means the policy permits the visible target; actual writes are still bounded
by Unix permissions and, under `Workspace` with L2 `Enforced`, by the kernel
`/tmp` grant (sec 4.8.4). A workspace at/under `/tmp` disables the exemption.

Recognized visible operand syntax is best-effort and does not upgrade a
NotClaimed cell: `file:` URLs (N12), curl `@path`/`name=@path`/`name=<path`
(N18), and `terminal open`'s declared `cwd` (HIGH-1) are matched; other
tool-specific syntax is NotClaimed. The
guarantee is therefore: **path-scoped write/delete enforcement plus
tool-mediated reads for the path tools, with best-effort matching for visible
shell operands/redirections.** It is not a claim that a shell subprocess cannot
read an arbitrary file.

## 2. Scope boundaries

| Boundary | Owner | This errata |
|---|---|---|
| Verdict values `Allow/Ask/Deny` | 09 sec 2.1 | unchanged |
| Rule total order (specificity -> layer -> effect) | 09 sec 3.4 | unchanged; `path` participates as an existing specificity dimension |
| L1 guard precedence and L2 confinement | 89 SG1-SG17 | unchanged except the Rev 9/13 exemption bypasses L1 only for a write/delete/create `shell`/`terminal` invocation whose every visible target is a concrete strict `/tmp` descendant; reads keep the normal policy; L2 gains an explicit `/tmp` grant (90-D23/90-D26) |
| `ReadOnly` hard deny | 09 Q7 / 07 sec 6.8 | **amended**: a fully-`/tmp` write/delete/create shell/terminal invocation is `Allow` even under `ReadOnly`; a workspace at/under `/tmp` disables the exemption, so a temp-exempt spawn keeps a read-only workspace root (90-A25/90-D26) |
| `force_ask` forced review | 25-D4 / 25-A11 | **amended**: a fully-`/tmp` write/delete/create shell/terminal invocation is `Allow` before `force_ask` (90-D23/A25) |
| `ExecutionEnvironment::resolve` / path tools | 07 X2/X3 | **not amended by Rev 12**: path tools stay workspace-root-confined and `/tmp` still throws `PathEscape`; the temp exemption is `shell`/`terminal` only (90-A26/PS31) |
| Config rule shape (`permissions.rules`) | 46-D1 | **amended**: `path` is accepted and validated |
| Config `permissions.default` master switch | 46-D1 | semantics unchanged; **global-layer-only** (the whole `permissions` table is, 90-D18); `"allow"` still defeats path scoping in the global layer |
| Durable `Always` grant shape and store | 46-D2 / 46-I5 / 46-I33 | file shape unchanged; loader bounds + path-tool `InRoot` derivation (90-A15) |
| Session grant key and matching | 46-D2 / 46-I6 | **amended**: class-scoped, still in-memory |
| Grant lifetime | 46-I6 | unchanged (`Once` nothing, `Session` daemon-memory, `Always` durable per workspace) |
| Wire protocol `permission.*` | 05 sec 7.6 | unchanged; summary string becomes root-relative (90-A16) |
| TUI dialog option set and labels | 25 / 46 | unchanged (`Allow for session` stays; index 1 stays) |
| Shell target scope | new (this errata) | visible operands/redirections derived from the spec-89 tokenizer; advisory matching input; hidden reads NotClaimed (planned follow-up, OQ-90-9) |
| Protected `.ymh/**`/`.git/**` | new (this errata) | hard deny before the fold; tool-level canonical/traversal checks (90-D13) |
| `grep`/`glob` traversal and symlinks | 07 / `LocalFilesystem` | **amended**: `grep` skips symlinks; both skip protected dirs (90-A13) |
| `git_status`/`git_diff` repository discovery | 07 / `GitService` | **amended**: no ancestor search (90-A14) |
| MCP/LSP subprocess confinement | 15 / 89-D18 | not claimed |
| Shell hidden reads / read isolation & egress | **planned follow-up (working name spec 91; no file/tracker row yet)** | NotClaimed here; kernel read allow-list/Landlock read confinement + network policy (OQ-90-9) |

## 3. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 90-A1 | 46 sec 3.2 item 2 (config rule shape) and O-H1 | `docs/design/46-permissions-ui-errata.md:429-439`, `:4538` | `permissions.rules[]` gains an optional `path` string. A `path` key is no longer an unknown key. The reason O-H1 removed it (no producer) is removed by 90-D2. |
| 90-A2 | 46 sec 3.3 / 46-F21 (path key is `ConfigError`) | `docs/design/46-permissions-ui-errata.md:532-535`; 46-I1 `:3462` | Replaced by 90-D3: `path` is validated (relative, no `..`, no `.` segment except `.`, no `[`/`]`, no leading `/` or `~`, requires `tool`, valid glob) and a malformed `path` is still a `ConfigError`. |
| 90-A3 | 46 sec 1.5 terminology and 46-I5 (grants carry tool + command only) | `docs/design/46-permissions-ui-errata.md:251-257`, `:3466` | Applies to `Always`/`LocalGrant` only. The `Session` grant is class-scoped (90-D7). 46-I5 and 46-I33 are retained for durable grants; `permissions.jsonc` still rejects `path`. |
| 90-A4 | 46-I6 (Session lifetime) | `docs/design/46-permissions-ui-errata.md:3467` | Lifetime unchanged: in-memory, per session, lost on daemon restart, re-ask. Only the key and matching change. |
| 90-A5 | 09 sec 3.3 `PermissionRequest::path` and Q7 | `docs/design/09-permissions.md:300-323`, `:944-946` | `path` is populated by the loop from tool arguments; it is absolute and lexically normalized (not "resolved root-relative" in the old comment). The policy computes the root-relative pattern with `lexically_relative` only; no `std::filesystem::relative`, no stat, no realpath. |
| 90-A6 | 09 sec 3.4 step 2 (match) | `docs/design/09-permissions.md:454-458` | Path matching becomes live. It adds the shell special case (`path:"**"` matches only `ShellTargetScope::InRoot`) and target-class matching for Session grants. The fold order is unchanged. |
| 90-A7 | 09 sec 2.3 / Q13 (grant key) | `docs/design/09-permissions.md:213`, `:512-516` | The Session key becomes `(session, tool, TargetClass, prefix-or-exact-command)`; the Q13 lifetime sentence is unchanged. |
| 90-A8 | 89 SG2/SG13 (guard precedence) and SG11 (rule matching unchanged) | `docs/design/89-shell-command-safety-guard-errata.md:853-875`, `:867` | No weakening. `classify_shell_command` keeps its signature and delegates to `classify_shell_command_with_scope(...).risk`; the scope/protected flag is additive and cannot authorize outside/unknown/protected targets. `Confirm -> Unknown` remains (scope is risk-gated). SG11's "rule matching unchanged" is superseded for this errata by 90-A18 (path/class/prefix matching and `shell_guard_text` exact grants). |
| 90-A9 | 52-D15 (presets) | `include/ymh/config/config.hpp:152-171`; `src/cli/wiring.cpp:135-147` | No new preset and no preset change. The recommended behavior is an ordinary rule plus the built-in protected deny. `danger-full-access` remains the explicit unsafe opt-out and is not the way to express "in-workspace only". |
| 90-A10 | First-run config sample | `src/config/config.cpp:1496-1508` | The generated sample **will** document the optional `permissions.rules[]` and its `path` key with the recommended in-root rule and the `.ymh/**`/`.git/**` deny rules as comments (it does not today, L5). No default behavior changes: the sample remains all comments for rules. |
| 90-A11 | `path_pattern` purity | `src/policy/permission_policy.cpp:114-131` (`root_relative_target`) | Replace `std::filesystem::relative` with `lexically_relative` plus an explicit outside test, so path-rule matching remains I/O-free (the temp predicate's bounded canonicalization is the only exception; PS3). |
| 90-A12 | Session grant id | `src/policy/permission_policy.cpp:180-188`, `:373-380` | `Always` keeps the existing `grant_id(tool, command)` and its durable ids. `Session` uses a new id over `(tool, TargetClass, prefix, command)` so two class grants for the same tool do not collide. |
| 90-A13 | 07 / `LocalFilesystem` traversal (H2) | `src/execution/filesystem.cpp:212-268` | `grep` skips symlink entries and opens candidates without following symlinks; `grep`/`glob` canonicalize the base independently of `resolve()` (so the check works under `Unrestricted`), reject a base for which the shared `path_is_protected` (`execution/path_safety.hpp`) is true, and skip `.ymh/`/`.git/` entries. The base remains `resolve()`-checked. Adds a regression test. |
| 90-A14 | 07 / `GitService` open (M7/N9) | `include/ymh/execution/services.hpp:23-27`; `src/execution/git_service.cpp:93`, `:248-249`, `:288` | Add `GitQuery::root` as the repository-open base and keep `path` as the pathspec/query scope. `status`/`diff` open `query.root` with `GIT_REPOSITORY_OPEN_NO_SEARCH`; `GitStatusTool` sets `root=context.root()` and leaves `path` empty; `GitDiffTool` sets `root=context.root()` and `path=resolve_base`. `git_status` has no path argument (schema); the optional `path` for `git_diff` is a pathspec, not the open base. A workspace subdirectory of a larger repo cannot read the ancestor `.git` through the tools. |
| 90-A15 | 46-D2 grant loading (H1/M9/N16/LOW-9) | `src/config/grant_store.cpp:70-110` | Reject wildcard `tool`, an empty `command` for `shell`/`terminal`, any `shell`/`terminal` command pattern with no literal character (`*`, `?`, `**`, ... -> match-all), and a path-targeting tool grant with a non-empty `command` (dead dimension); load a path-tool grant with no command as `target_class=InRoot`; keep the file keys/schema unchanged. |
| 90-A16 | Permission summary (L6) | `src/permission/permission_broker.cpp:78-120`, `:100-106` | `bounded_summary` uses the root-relative target, not `request.path->generic_string()`, so `permission.*.summary` does not expose the absolute workspace path. |
| 90-A17 | Tool-level protected check (H1/N2) | `src/tools/builtin_tools.cpp:43-56`, call sites `:141`, `:188`, `:231`, `:304`, `:384`; `src/execution/filesystem.cpp` | `read_file`/`write_file`/`edit_file` canonicalize the resolved path independently of `resolve()` (`weakly_canonical`) and call `path_is_protected` before I/O, so the check still works under `SandboxMode::Unrestricted`; `grep`/`glob` canonicalize the base and skip protected traversal. |
| 90-A18 | 89 SG11 (rule matching unchanged) | `docs/design/89-shell-command-safety-guard-errata.md:867` | **Superseded for this errata (N7).** `rule_matches` gains path/shell/target-class/prefix matching, and the exact `literal` branch uses `shell_guard_text` for `shell`/`terminal` (so `terminal write` compares the input). `command_pattern` remains the config-command extractor. 89 SG11's "unchanged" wording no longer describes the 90 fold. |
| 90-A19 | 21 / 46 / 52 config layering (N1/N15) | `src/config/config.cpp:479-494`, `:425-444`, `:1726-1734`; `docs/design/52-endpoints-models-and-dsh-agent-presets.md` sec 2.2 `:206-209`; `docs/design/46-permissions-ui-errata.md` localcode-import note | Every `permissions.*` key **and `agent.sandbox`** are global-layer-only; a workspace `.ymh/config.jsonc` containing either is a `ConfigError` at load. This supersedes the workspace acceptance of `permissions.shell`/`write`/`read`/`default`/`rules`, the replace-on-layer behavior for that table, a workspace sandbox baseline, and 52/46's generic "workspace layer overrides" statement for `permissions.*`. **Migration:** an existing workspace `.ymh/config.jsonc` with a permission key now aborts daemon startup; move the key to the global `config.jsonc` or remove it. The failure is a DoS/startup error, not an escalation. |
| 90-A20 | 89 / shell guard (N1/N13/N14/N20/N22/HIGH-2/LOW-8/NEW-1/NEW-6/NEW-7) | `src/policy/shell_guard.cpp:1445-1680` (git subcommand classifier) | Add shell git control-plane classification (sec 4.4.1/90-D17): writing `config`, `clean`, `submodule` mutations, `worktree` mutations -> `protected_target=true`; `init`/`clone`/unknown -> `Unknown`; `stash -a/-u/--all/--include-untracked` and clustered `-au`/`-ua`/... -> `protected_target=true`; `remote` mutations (except `remote -v/show/get-url` and `remote prune`), `branch --track`/`-t`/`--set-upstream*`/`--unset-upstream`/`--edit-description`, `branch -m/-M/-c/-C` and `--move`/`--copy` (always protected), `branch -d/-D/--delete` (always protected), branch creation `branch <new>` (with or without start), `checkout -b/-B <name> [start]` + attached `-b<name>`/`-B<name>`, `switch -c/-C <name> [start]` + `--create`/`--force-create[=]<name>` + attached `-c<name>`/`-C<name>`, `switch --track`/`-t`/`-t<ref>`, `checkout --track`/`-t`, `fetch --set-upstream`, `pull --set-upstream`, `push -u/--set-upstream` -> `protected_target=true` (they write `branch.*`/`remote.*` config) **unless `--no-track`** for creation/start forms; `-c branch.autoSetupMerge=...` is ignored for the class; plain `stash`/data-plane/read-only subcommands (`remote -v/show/get-url`, `remote prune`, `submodule status/summary`, `worktree list`, `config --get/--get-all/--get-regexp/--list`) keep the normal scope. `--worktree` is a protected write. |
| 90-A21 | 89 / shell scope (N12) | `src/policy/shell_guard.cpp:588-655`, `:721-740` | Add URI parsing: a case-insensitive `file:` scheme is stripped (`file://localhost` local), percent-decoded, and classified by the normal absolute/relative rules; non-local host/ambiguous -> `Unknown`; other schemes stay neutral per OQ-90-4. A decoded `.ymh`/`.git` target sets `protected_target`. |
| 90-A22 | 89 / shell scope (N18) | `src/policy/shell_guard.cpp:588-655`, `:721-740` | Add best-effort curl local-file operand parsing: `@path`, `name=@path`, `name=<path`, `name@path` strip the prefix and classify the remainder; `@-` neutral; ambiguous -> `Unknown`. Not a soundness claim (matrix hidden-read cell NotClaimed; planned follow-up OQ-90-9). |
| 90-A23 | 89 / shell guard (N19/LOW-1/LOW-2) | `src/policy/shell_guard.cpp:588-655`, `:721-740` | Add glob-aware protected detection: a **dot-leading literal-prefix** glob component that matches `.ymh`/`.git` sets `protected_target` (`.ymh*`, `.git*`, `.ym?`, `.*`, `src/.ymh*`); a bare `*`/`**` is not protected; only in-root targets are protected (an outside `.ymh`/`.git` path is `Outside`/Ask). Apply to operands, redirections, `find -delete`, and Session target before `remember`. |
| 90-A24 | 07 / `terminal` cwd (HIGH-1; temp cwd Rev 10) | `src/tools/terminal_tool.cpp:38`, `:150-153`; `src/policy/shell_guard.cpp:2049-2100` | Add `terminal open` effective-cwd classification: read `arguments["cwd"]`, lexically resolve against root, use it for the scope and L1 risk walk; outside/non-string/ambiguous -> `Outside`/`Unknown` (Ask); in-root `.ymh`/`.git` -> protected; a strictly-temp cwd -> temp (bare open `Unknown`). The tool computes `weakly_canonical(root/cwd)` first; for a canonical cwd strictly under `/tmp` (workspace not at/under `/tmp`) it uses that path directly instead of calling `resolve()` (which would throw `PathEscape`), and otherwise keeps the `resolve()` path. Its independent check rejects a canonical path that is neither in-root nor strict temp, and rejects in-root protected, under both sandbox modes. `terminal write` stays `Unknown`. |
| 90-A25 | 09 sec 3.4 step 1 / Q7 (`ReadOnly` hard deny) and 25-D4 (`force_ask`) | `src/policy/permission_policy.cpp:416-520`; `docs/design/09-permissions.md:443-495`; 25-D4 | A **direct, non-recursive, regular-file/new-path** write/delete/create `shell`/`terminal` invocation whose every visible target is a concrete strict `/tmp` descendant (the declared `terminal open` cwd is the one existing-directory exception) returns `Allow` **before** the L1 guard, `ReadOnly`, `Confirm`, `force_ask`, and the protected check; the exemption cannot be disabled by `permissions.*` or `agent.sandbox` (90-D23/PS29). Reads and path tools are not exempt. The existing order is unchanged for every non-exempt request. |
| 90-A26 | 07 X2 (no escape in root-confined modes) and `resolve()` | `src/execution/environment.cpp:64-95`; `src/execution/filesystem.cpp:41-46`; `docs/design/07-tools-execution.md:1313` | **Withdrawn by Rev 12**: `resolve()` and `LocalFilesystem` are **unchanged**; path tools remain workspace-root-confined and `/tmp` still throws `PathEscape`. There is no path-tool temp exemption (90-D23/PS31). |
| 90-A27 | 89 SG2/SG13 (guard precedence) and 89-D17/L2 policy | `src/policy/permission_policy.cpp:416-520`; `src/execution/landlock.cpp:41-115`; `include/ymh/execution/confinement.hpp:30-38` | L1 `Forbid`/`Confirm` is bypassed only for a fully-`/tmp`, direct non-recursive write/delete/create `shell`/`terminal` request (90-D23). L2 gains `ConfinementPolicy::tmp_root` (canonical `/tmp` only) and grants `kTmpRights` (including `FS_REFER`); a `ReadOnly` temp-exempt spawn uses a read-only workspace root, and a workspace at/under `/tmp` disables the grant. The guard catalogue and other L2 rights are unchanged. |
| 90-A28 | 89 / shell guard redirection classifier (LOW-3/LOW-4) | `src/policy/shell_guard.cpp:119-121`, `:588-655`, `:721-740` | Split `is_output_redirect` into an output-only form (`>`, `>>`, `>\|`, `&>`) and a read+write `<>` form; only the output-only form feeds redirection-first. Restrict redirection-first to the harmless producers `printf`, `echo`, `:`, `true`; an unknown producer with or without a `/tmp` redirection is not a write and is not exempt. `<>` contributes a read role and is never output-only. |

## 4. Definitions and target model

### 4.1 The target class (`TargetClass`)

Every request is assigned exactly one target class before rule matching:

```cpp
enum class TargetClass : std::uint8_t {
    Any,       // tool has no path dimension (MCP and other non-path tools)
    InRoot,    // target is the canonical root or a lexical descendant
    Outside,   // target is lexically outside the root
    Unknown    // the target cannot be proved either way
};
```

`TargetClass` is a **matching input**, not a security boundary. It is computed
from `PermissionRequest::path` (path tools) or from the spec-89 shell scope
(shell/terminal). It is not sent on the wire and is not durable.

### 4.2 Path-targeting tools and their argument mapping

The loop populates `request.path` for exactly these tools:

| Tool | Path argument | Absent argument | Target |
|---|---|---|---|
| `read_file` | `path` (required) | `nullopt` / `Unknown` | the file |
| `write_file` | `path` (required) | `nullopt` / `Unknown` | the file |
| `edit_file` | `path` (required) | `nullopt` / `Unknown` | the file |
| `grep` | `path` (optional) | root | the search base directory |
| `glob` | `path` (optional) | root | the search base directory |
| `git_status` | none (the schema has no path; an undocumented `path` is ignored) | root | the workspace repository |
| `git_diff` | `path` (optional) | root | the diff pathspec/base |

Rules:

- For `grep`/`glob`, the tool's separate `glob` filter argument is **not** a
  path and is ignored by this mapping. The base is `path` or the root.
- For a required `path` that is missing, not a string, or the empty string, the
  target is `nullopt` and the class is `Unknown`; a path rule does not match and
  the configured default decides. The tool later fails argument validation or
  `PathEscape`, never a silent allow.
- For an optional `path` that is a non-string, the executor's
  `resolve_optional_base` ignores it and uses the root
  (`src/tools/builtin_tools.cpp:44-51`; `src/tools/git_tools.cpp:24-31`), but
  the permission mapper is deliberately more conservative: it returns
  `nullopt`/`Unknown` and the request asks. An empty optional string is invalid
  and also `Unknown` (the executor's `resolve("")` throws).
- `git_status` has no `path` argument in its schema; the permission mapper uses
  the workspace root and the tool must ignore an undocumented `path`
  (`src/tools/git_tools.cpp:24-31` currently reads it and is amended by
  90-A14). `git_diff`'s optional `path` is a pathspec. Both tools set
  `GitQuery::root = context.root()` and open the repository there with
  `GIT_REPOSITORY_OPEN_NO_SEARCH` (90-A14); `git_status` leaves the query
  `path` empty, `git_diff` uses it as a pathspec. A workspace that is a
  subdirectory of a larger repository therefore cannot read the ancestor
  `.git` from the **git tools**. A shell `git` command still uses the system
  git and can search ancestors; that is a separate shell residual (N4, sec 15).
- Every other tool (`shell`, `terminal`, `mcp.*`, and any future tool not in
  this table) leaves `request.path` empty. `shell`/`terminal` get a class from
  the shell scope (sec 4.4); the rest are `Any`.

### 4.3 Canonicalization and containment (lexical; symlinks stated)

For a path argument `arg` and the canonical root `root`:

```text
base candidate = arg.is_absolute() ? arg : (root / arg)
target          = candidate.lexically_normal()
```

This is **lexical** canonicalization: `.` and `..` are resolved by
`std::filesystem::path::lexically_normal`; no filesystem I/O, no `stat`, no
`canonical`, no `weakly_canonical`, no `realpath`. `request.root` is already
realpath-canonical by contract (07 X3, the stored-root comment at
`include/ymh/execution/environment.hpp:59-60`, and the canonicalization at
`src/execution/environment.cpp:56`).

`path_is_within_root(root, target)` is the single containment predicate:

1. Normalize both operands lexically. Both must be absolute; otherwise false.
2. Walk their components; `root` must be a component-wise prefix of `target`.
3. Equality (the root itself) is inside.
4. No case folding (Linux, byte-wise); no trailing-slash sensitivity.

**Symlink policy (must be stated, not implied).** The policy is lexical and
does not resolve symlinks. A path that is lexically inside the root but whose
parent or final component is a symlink pointing outside is classified
`InRoot` for rule matching. Execution is authoritative only where it actually
resolves the final target:

- `read_file`/`write_file`/`edit_file` call `resolve()` on the target, which
  canonicalizes the parent with `weakly_canonical`, canonicalizes the final
  component when it exists, and throws `ToolErrorCode::PathEscape` if the
  resolved path escapes (`src/execution/environment.cpp:64-95`).
- `grep`/`glob` resolve only the **base**, not each traversed file. The shipped
  `grep` follows symlinked files (`src/execution/filesystem.cpp:236-254`,
  `:268`). This errata therefore requires `grep` to skip symlink entries and to
  open candidates without following symlinks, and requires both `grep` and
  `glob` to skip `.ymh/`/`.git/` during traversal (90-A13). Until that change,
  PS4/PS-F7 are true for read/write/edit and the grep base, but NOT for
  per-file grep traversal; the spec must not claim otherwise.
- For `shell`/`terminal`, spec-89 L2 (Landlock) mediates the resolved path; an
  explicit protected target is hard-denied by the policy check (sec 4.3.1),
  and a shell subprocess that recursively reads `.ymh`/`.git` without naming
  them remains a recorded residual (sec 15).

A `path:"**"` rule therefore does not promise that a symlink cannot reach
outside; it promises only that the lexical argument is in-root. This is the
same honesty boundary as 89-D6 (lexical containment) and 89-D15/L2. The
precise split is: lexical policy class for every path tool; canonical
target check for read/write/edit; base check plus the required grep/glob
traversal exclusions for grep/glob.

For **tool path arguments**, `~` and `$HOME` are literal bytes, not shell
expansions: `resolve()` treats them as ordinary relative paths, so
`read_file "$HOME/x"` targets `<root>/$HOME/x`, while an actual absolute home
path such as `/home/user/x` is `Outside`. For **shell text**, `~` and `$HOME`
are home references and are outside by 89-D6. The two predicates are
deliberately different: `path_is_within_root` (tool paths, no home special
case) and `path_within_root` (shell text, 89-D6 home rules).

### 4.3.1 Protected paths (H1)

The workspace contains the permission system's own durable inputs and the VCS
control surface. An in-root allow rule must not cover them. The following are
**protected paths**:

- `.ymh` and any descendant (workspace `config.jsonc`, `permissions.jsonc`,
  `sessions.db`, `checkpoints/**`, and any future state under `.ymh/`);
- `.git` and any descendant (hooks, config, object store, worktree gitdir).

**Any `.ymh`/`.git` component at any depth is protected (N3).** A nested
`src/.git` (nested repo) or `src/.ymh` is protected too; the predicate walks
all components of the root-relative path, not just the first. `.gitignore`,
`.github/**`, `.ymhfoo`, and any other dot path are **not** protected.

**Intentional read exception.** `git_status`/`git_diff` read the workspace's own
`.git` object store internally; that is the tool's intended read-only
operation and is not a protected-path violation. The protected check applies
to an **explicit** `.ymh`/`.git` path argument, an explicit shell/terminal
target, and every write. A `git --git-dir=.git ...` shell command names the
path explicitly and is denied (unless the guard already returned `Ask`).

**Exact comparison.** The policy check is lexical and component-wise:

```text
path_is_protected(root, target):
  r = root.lexically_normal(); t = target.lexically_normal()
  if !r.is_absolute() or !t.is_absolute(): return false
  rel = t.lexically_relative(r)
  if rel.empty() or rel.is_absolute(): return false
  if rel == ".." or rel starts with "../": return false   // outside is handled elsewhere
  for component in rel:
      if component == ".ymh" or component == ".git": return true
  return false
```

- Any component, any depth: `src/.git/hooks/x` and `src/.ymh/config.jsonc`
  are protected; `.ymhfoo` and `.gitignore` are not. The path-tool predicate
  is literal; for **shell tokens**, an in-root glob component whose literal
  prefix starts with `.` and matches `.ymh`/`.git` (`.ymh*`, `.git*`, `.ym?`,
  `.*`, `src/.ymh*`) is protected by the scope scan (N19/LOW-1/LOW-2/sec 4.4),
  not by this literal predicate. A bare `*`/`**` and an outside `.ymh`/`.git`
  path are not protected.
- Case-sensitive (Linux), no trailing-slash sensitivity.
- Lexical: a symlink alias `.work -> .ymh` is **not** protected by this lexical
  check. The tool-level check below canonicalizes **independently of
  `resolve()`** and re-checks, so a `write_file .work/config.jsonc` is still
  denied even under `SandboxMode::Unrestricted` (where `resolve()` returns the
  lexical path, `src/execution/environment.cpp:72-74`).
- Outside paths are not "protected" by this check; they never match a path
  rule anyway. A `/tmp` target is outside the workspace, so it is not
  root-relative protected; a workspace at/under `/tmp` disables the exemption
  (Rev 12).
- **The check is root-relative (N17).** If the workspace root itself lives
  under a `.ymh`/`.git` directory (for example `/srv/.ymh/project`), the
  relative walk sees no protected component and the root is the trust
  boundary; this is stated explicitly, not silently assumed. A test
  `PathTarget.ProtectedRootRelativeCaveat` pins it.

**Precedence.** The protected check runs after the L1 guard and the existing
hard-denies/`force_ask`, and **before the rule fold and before any grant**:

1. `classification.risk == Forbid -> Deny` (89, unchanged).
2. `ReadOnly` mutating -> `Deny` (09 Q7, unchanged).
3. `classification.risk == Confirm -> Ask` (89, unchanged; a protected
   destructive command is still at least an Ask).
4. `force_ask -> Ask` (25-D4, unchanged).
5. **protected check -> `Deny`**:
   - `request.path` present and `path_is_protected(request.root, *request.path)`;
   - `classification.protected_target` true (an explicit `.ymh`/`.git` target
     in shell/terminal text, including `terminal write` input).
6. target class and the rule fold.

A protected path is therefore never auto-approved by a path rule, a
Session grant, an `Always` grant, `default:"allow"`, or `danger-full-access`
**for the path tools and for explicit shell targets/implicit git control
writes**. The claim is scoped honestly: a shell subprocess that writes a
protected path through a symlink alias, or through an unpredicted interpreter
payload that does not name `.ymh`/`.git`, is a recorded residual (sec 15,
risk 6); the global-layer-only permission rule (90-D18) makes the
config-escalation variant non-escalatory even if such a write succeeds. If the
L1 guard already returned `Confirm` (step 3), that `Ask` stands and the
protected check is not reached; the protected outcome is then force-asked, not
silently allowed. `remember` also returns `{}` when the request is protected,
so no Session or `Always` grant can be created for one. The protected deny's
reason is the fixed, echo-free token `permission:protected-path` (09 Q11); it
never contains the path or the command.

**Tool-level enforcement (symlink aliases; works under `Unrestricted`).**
Because the policy comparison is lexical, the path tools enforce the same set
on a **canonical** path they compute themselves, not merely on the `resolve()`
result. `read_file`, `write_file`, and `edit_file` call `resolve()`, then
`std::filesystem::weakly_canonical(resolved)` (or `canonical` when it exists)
independently of the sandbox mode, then `path_is_protected(root, canonical)`
and fail with `ToolError{ToolErrorCode::PathEscape, "protected path"}` before
any I/O. This closes the `.work -> .ymh` alias under `SandboxMode::Unrestricted`
too, where `resolve()` itself skips canonicalization
(`src/execution/environment.cpp:72-74`; N2). `grep` and `glob` canonicalize
the base the same way, reject a protected base, and skip `.ymh/` and `.git/`
entries during traversal (90-A13); `grep` also skips symlink entries.
`LocalFilesystem` calls the **same shared** `path_is_protected` from
`include/ymh/execution/path_safety.hpp`; there is no execution-side duplicate
(LOW-6).

**Durable-input escalation (the H1 trace, pinned as a regression test).**
Without the protected check, the sequence is:

1. `write_file .ymh/config.jsonc` with
   `{"permissions":{"default":"allow"}}` is allowed by the 2-dimension path
   rule; the next daemon start sets `master=true` and allows outside targets.
2. `write_file .ymh/permissions.jsonc` with a `{"tool":"*","effect":"allow"}`
   entry is allowed; the grants loader accepts it and it matches every tool on
   restart.
3. `write_file .git/hooks/pre-commit` plus `shell "chmod +x
   .git/hooks/pre-commit"` gives code execution on the next git command.
4. `shell "git config --local core.hooksPath .evil"` writes `.git/config`
   without an explicit `.git` operand; the shell guard returns `None` and the
   path rule would allow it. The shell git-control classifier (sec 4.4.1)
   marks it `protected_target=true` -> `Deny`.
5. `shell "git clean -fdx"` deletes the untracked `.ymh` directory; the
   classifier marks `clean` protected -> `Deny`.

The protected check denies all five. Even if a future shell vector writes
`.ymh/config.jsonc`, the workspace `permissions` table is **global-layer-only**
(90-D18), so the next daemon load rejects it instead of setting `master=true`;
the write is at worst a startup error, never a policy escalation.
Belt-and-braces, the grants loader rejects a wildcard `tool` and an empty
`command` for `shell`/`terminal` (sec 4.6.1). The recommended config also
shows explicit deny rules, but the policy check is the hard guarantee and is
not config-dependent.

### 4.4 Shell target scope (`ShellTargetScope`)

`shell` and `terminal` have no path argument, so their target is derived from
the same tokenizer/segmenter that spec 89 already uses. The classifier is
extended additively:

```cpp
enum class ShellTargetScope : std::uint8_t {
    NotApplicable,  // empty text, non-shell tool
    InRoot,         // every provable filesystem target is in-root
    Outside,        // at least one provable target is outside
    Unknown         // at least one target cannot be proved, and none is Outside
};
```

`classify_shell_command_with_scope(text, root, cwd, kernel_confinement,
canonical_tmp_root)` returns `{risk, scope, protected_target, tmp_exempt}`. The
existing `classify_shell_command` delegates to `.risk`, so every existing
caller and test keeps its behavior; the new argument is the cached canonical
`/tmp` path (empty disables the check). `tmp_exempt` is the Rev 12 lexical,
I/O-free flag: the target walk must see at least one visible target, every
target must be a concrete strict `/tmp` descendant, and any `Forbid`/`Confirm` the guard returns
must be attributable to those targets. It may therefore be true while
`risk == Forbid` for a destructive `/tmp` operand (`rm /tmp/x`), but false
for an unrelated `Forbid` (fork bomb) or an unconditional-`Forbid` class
(`mkfs /tmp/y`). Path tools never set it.

**Scope depends on `kernel_confinement` through the risk gate.** This is
deliberate, not a bug: `map_operand` (`src/policy/shell_guard.cpp:774-787`)
promotes an in-root bounded destructive operand to `Confirm` when
`!kernel_confinement` (`src/policy/shell_guard.cpp:778-788`), and rule 3
below maps `Confirm` to `Unknown`. Therefore:

- `rm -rf build` with L2 on -> `risk=None`, `scope=InRoot` -> an InRoot prefix
  grant is possible;
- `rm -rf build` with L2 off -> `risk=Confirm`, `scope=Unknown` -> only an
  exact Unknown grant is possible, and the guard `Ask` is returned before the
  fold anyway.

A guard `Ask` (Confirm) can never seed an InRoot prefix grant. The spec must
not claim that scope is independent of the flag.

Scope derivation is a pure, I/O-free pass over the same tokens:

1. Empty text -> `NotApplicable` for a non-shell tool, `Unknown` for
   `shell`/`terminal`, except a `terminal open` with a declared cwd (its cwd
   class is returned; NEW-3/NEW-8).
2. `risk == Forbid` -> `Outside` (the forbidden cases are outside, device, or
   catastrophe targets; the guard has already denied before any rule).
3. `risk == Confirm` -> `Unknown` (unbounded, broad, expanded, or truncated;
   fail-safe).
4. `terminal write` -> `Unknown` for the path-rule scope (the risk gate in
   items 2/3 still runs first). A PTY keeps its own
   cwd across writes (`src/tools/terminal_tool.cpp:150-153`), the policy keeps
   no per-PTY cwd, and the shipped `shell_text_of` `cwd=root` assumption
   (`src/policy/shell_guard.cpp:1853-1855`) has no PTY state to use. A
   `cd /tmp` typed into a PTY followed by
   `terminal write "cat secret"` would otherwise be classified `InRoot`. The
   guard still classifies the write for `Forbid`/`Confirm`; only the path-rule
   scope is forced to `Unknown`, so a `path:"**"` rule never auto-approves a
   terminal write. An explicit `.ymh`/`.git` target still sets
   `protected_target=true` and is denied.
5. `risk == None` -> walk the segments and classify every provable filesystem
   target. Combine with `Outside > Unknown > InRoot` over the whole command.
   If no target evidence exists, the result is `InRoot` (a fresh shell process
   starts at the root, or at the declared in-root `terminal open` cwd). An
   in-root protected `.ymh`/`.git` target sets
   `protected_target=true` even when the scope is `Unknown`.

**The temp-target pass (Rev 9; write-only in Rev 13).** Steps 2/3
short-circuit the *scope* and the guard risk, but the same target walk also
records the visible targets and whether the invocation is a **write operation**.
`ShellClassification::tmp_exempt` is the classifier's **I/O-free lexical
precursor**; it is true iff **all** of the following hold:

1. the invocation is a **write/delete/create**, not a read: at least one
   recognized write operation and **zero** recognized read/unknown segments
   (see the verb table below). A mixed read+write command is not exempt;
   `cat /tmp/x`, `head`, `grep`, `ls`, `stat`, `wc`, `diff`, `sed` without
   `-i`, and similar reads keep the normal read policy;
2. at least one visible filesystem target was found; every visible target
   is a **concrete** strict descendant of canonical `/tmp`: operands,
   redirections, `cd`-resolved relative targets, recursively nested command
   targets, a `terminal open` declared `cwd`, and the filesystem targets in a
   `terminal write` input. The bare `/tmp` root, `/tmpfoo`, `/tmp2/x`, **any
   `..` component before normalization** (`/tmp/..`, `/tmp/a/../b`), any
   `{`/`}` (brace expansion), any glob metacharacter (`*`, `?`, `[`), any
   unexpanded `$VAR`/`${VAR}`/`$(...)`/backtick, a truncated parse, and any
   interpreter/here-doc/`-exec`/`xargs` payload are **not** concrete and make
   the flag false;
3. no target is protected **lexically** (`path_is_protected`; I/O-free);
4. every `Forbid`/`Confirm` the guard returns is attributable to a `/tmp`
   target: an untargeted `Forbid` construct (fork bomb `:(){ :|:& };:`), an
   unconditional-`Forbid` class (`mkfs`/`mke2fs`/`newfs`/`wipefs`), or a
   non-temp/ambiguous cause blocks the exemption even when another segment
   writes `/tmp/x`.

The **resolve-time** checks are not part of the classifier flag (LOW-2):
`request_is_tmp_exempt` canonicalizes every `tmp_targets` entry, rejects a
resolved path outside `/tmp`, inside the workspace/protected tree, or a
dangling/unresolvable symlink (the policy-level check in sec 4.8.3; this
closes the `/tmp/link -> <workspace>/.git` alias), and enforces the
regular-file/new-path type rule. A caller must not treat the lexical
`tmp_exempt` flag as the final exemption without that predicate.

The classifier also records `tmp_targets`: **every visible target**, both read
and write operands plus a `terminal open` cwd, absolute and lexically
normalized, so the final-verdict check can canonicalize them without re-parsing.
`cd`/`pushd` is a cwd update, not a target; options and `NAME=value`
assignments are neutral.

**Write/read classification (pinned).** A segment can carry **both** roles; the
invocation is a write-only candidate only when it has at least one write and
**zero reads**. A read operand means `tmp_exempt` is false and the normal
shell/default/guard policy applies (`permissions.read` seeds path tools, not
shell).

- **Direct write/delete/create (eligible):** `rm` **without** `-r`/`-R`/
  `--recursive` (unlink), `mkdir`, `touch`, `truncate`, `mv` (direct rename),
  the **destination** of `dd of=`, `tee` file arguments, and an output
  redirection (`>`, `>>`, `>|`, `&>`). The named target must be a regular file or a
  new path; the canonical check rejects an existing directory, FIFO, socket,
  or device node, and a symlink into the workspace/outside (LOW-5). `<>` is
  **not** a write-only form: it opens for read+write, so it is non-exempt
  (LOW-3).
- **Read sources:** `cat`/`head`/`tail`/`less`/`more`/`grep`/`rg`/`sed`
  without `-i`/`awk`/`ls`/`stat`/`file`/`du`/`wc`/`diff`/`cmp`/`find`
  without a destructive action/`git status`/`diff`/`log`/`show`/`config
  --get*`, an input redirection (`<`) or a read+write redirection (`<>`), and
  the **source operands** of `cp`, `install`, `ln`, and `dd if=`. These verbs are mixed read+write and
  are therefore **not exempt**; their sources still appear in `tmp_targets`.
- **Recursive/following forms (never exempt):** `rm -r`/`-R`/`--recursive`,
  `cp -r`/`-R`/`-a`/`--recursive`/`--archive`, all `chmod`/`chown`
  (including `-R`; metadata is not Landlock-mediated), `find -delete`/`-L`/
  `-H`/`-exec`, `rsync`, `install -D`, and any `--dereference`/`--follow`.
  A directory target of `rm`/`mv` is also not exempt. The normal policy
  applies: the guard-recognized destructive forms are the L1 `Forbid`
  -> `Deny` (`rm -rf /tmp/build`, `cp -a`, `chmod -R`, `find -L -delete`),
  while `rsync` and `install -D` remain `Ask` (not guard-recognized)
  (MEDIUM-1).
- **Redirection-first:** a segment with an output-only redirection (`>`, `>>`,
  `>|`, `&>`) is a write only when its producer is one of the known harmless
  producers `printf`, `echo`, `:`, or `true` (`printf x > /tmp/x`,
  `echo hi > /tmp/x`); an unknown producer is **not** a write even with an
  output redirection (`git checkout main > /tmp/x` is not exempt), so the
  producer's hidden effects are not laundered into `Allow` by the exemption;
  an unknown producer with no output redirection is likewise not a write.
  `<>` is read+write and is never this case. Implementation anchor:
  `src/policy/shell_guard.cpp:119-121`'s `is_output_redirect` currently
  returns true for `<>`; split it into an output-only form (`>`, `>>`, `>|`,
  `&>`) and a read+write `<>` form, and use only the output-only form for
  this rule (LOW-3/LOW-4).

`mv /tmp/a /tmp/b`, `touch /tmp/x`, `rm /tmp/x`, `mkdir /tmp/d`, `tee /tmp/x`,
`dd of=/tmp/x`, and `printf x > /tmp/x` are direct write-only candidates and
can be exempt. `cp /tmp/a /tmp/b`, `install /tmp/a /tmp/b`, and
`dd if=/tmp/a of=/tmp/b` are mixed read+write and are **not** exempt.
`rm -rf /tmp/d`, `chmod -R /tmp/d`, `chown -R /tmp/d`, `find -L /tmp/d
-delete`, and `rsync /tmp/a /tmp/b` are not exempt. Anything else (unknown
command, interpreter without a recognized write, a bare shell/interpreter, a
here-doc, a truncated/ambiguous parse) is not a write and makes `tmp_exempt`
false, with or without an output redirection. This is deliberately
conservative: an unrecognized write verb is not exempt, so the residual is
"more prompts", never a silent recursive write.
`terminal write` is exempt only when its input text is a recognized **direct
write-only** operation with at least one `/tmp` target; a read input
(`cat /tmp/x`), mixed input, or recursive form is not exempt.
`terminal open` requires a **non-empty command** and its cwd is a mandatory
target; an absent cwd defaults to the workspace root and fails; a bare
`terminal open` is never exempt. `terminal read`/`resize`/`close`/`list` have
no filesystem target and are not exempt.

**Target-type rule (LOW-2/LOW-5).** For every `tmp_targets` entry **except the
declared `terminal open` cwd**, an existing target must be an existing
**regular file** (`symlink_status`/`is_regular_file`); an existing directory,
FIFO, socket, or device node is not exempt, while a new path is allowed. The
declared `terminal open` cwd is the one directory exception: it must be an
existing directory (a missing/non-directory cwd is not exempt), and it is
still subject to strict-`/tmp`, not-workspace/protected, and no-symlink-escape
checks. This keeps `terminal open {cwd:"/tmp/sub", ...}` implementable while
direct write/delete/create targets remain regular-file/new-path only.

**The protected-target scan is independent of the risk gate.** Steps 2/3
short-circuit the *scope* (Forbid -> Outside, Confirm -> Unknown), but the
lexical scan for an explicit `.ymh`/`.git` operand/redirection target and for a
protected git subcommand (sec 4.4.1) still runs and sets
`protected_target=true`. A destructive protected command with L2 off is still
`Ask` (step 3), but `remember` sees the protected flag and returns `{}`; a
destructive protected command with L2 on reaches step 5 and is `Deny`.

Target-evidence rules (the conservative part):

| Token / construct | Scope effect |
|---|---|
| `~`, `~/...`, `$HOME`, `${HOME}` (unquoted or double-quoted) | `Outside` (89-D6) |
| Absolute path | `Outside` if not within root, else in-root |
| Relative path containing `/`, `.`/`..`, a glob, or a `~`/`$` expansion | resolve against the tracked cwd; outside -> `Outside`; inside -> in-root; unresolved expansion -> `Unknown` |
| Any token containing `{` or `}` (brace expansion, quoted or unquoted) | `Unknown` (conservative; HIGH-3). Brace expansion is not enumerated by the tokenizer, so `/tmp/{x,/etc/passwd}` cannot be treated as one lexical temp target; it is never exempt |
| In-root `.ymh`/`.git` (exact or descendant component, after normalization), or an in-root glob component whose **literal prefix starts with `.`** and matches `.ymh`/`.git` (`.ymh*`, `.git*`, `.ym?`, `.*`, `src/.ymh*`), including in a quoted interpreter/payload token (not a bare substring). A bare `*`/`**` is **not** protected; an outside `.ymh`/`.git` path is `Outside` (Ask), not protected. | `protected_target=true` for the in-root cases; the hard deny in sec 4.3.1 returns before the scope or any rule matters (N19/LOW-1/LOW-2) |
| Bare word with no path punctuation (branch name, commit message, hostname) | neutral (no evidence) |
| Option (`-x`, `--x`) | neutral, except an option's following value is still a token and is classified by the same rules |
| `--opt=value` | split at the first `=`; classify `value` by the same rules |
| Attached short option (`-tVALUE`, `-CVALUE`, `-fVALUE`, `-bNAME`, `-BNAME`, `-cNAME`, `-CNAME`, `-uREF`, `-tREF`, `-mOLD`, `-MOLD`) | split after the option letter for the known path-taking letters (`t`, `C`, `f`) and, for git, for the config-writing letters (`b`, `B`, `c`, `C`, `u`, `t`, `m`, `M`); classify/split the value; if ambiguous -> `Unknown` (NEW-7; the shipped operand scan is `src/policy/shell_guard.cpp:588-655`, `:721-740`) |
| Any option token containing `/`, `~`, `$`, or `..` but not split by the rules above | `Unknown` (conservative) |
| Redirection target (`>`, `>>`, `<`, `<>`, >\|, `&>`) | classify as a path |
| `NAME=value` assignment | if `value` starts with `@` or `<`, strip that prefix and classify the remainder as a local file path; else if `value` contains `/`, `~`, `$`, or starts with `..`, classify it; otherwise neutral |
| Curl-style `@path` local file (`curl -d @file`, `--data-binary @file`, `--data-urlencode @file`) | strip the leading `@` and classify the remainder by the normal absolute/relative/protected rules; `@-` -> neutral (stdin), not a file (N18, best-effort) |
| Curl-form `name=@path` / `name=<path` (`-F name=@file`, `-F name=<file`) | strip the `name=@`/`name=<` prefix and classify the rest; malformed/ambiguous -> `Unknown` (N18, best-effort) |
| Curl `name@path` (`--data-urlencode name@file`) | strip the `name@` prefix and classify the rest; ambiguous -> `Unknown` (N18, best-effort) |
| `cd` with one provable operand | update cwd; an outside cwd makes the command `Outside`; an unknown cwd or no/multiple operands makes it `Unknown` |
| `cd` after an outside segment | the command is already `Outside`; a later `cd` back inside does not erase it |
| `sh -c` / `bash -c` / `zsh -c` / `dash -c` / `ksh -c` | recurse to depth 2 (89-D5); combine |
| `python -c`, `python3 -c`, `perl -e`, `ruby -e`, `node -e`, `node --eval`, `php -r`, `lua -e`, `Rscript -e` | `Unknown` (the payload is code, not a path) |
| `awk`, `sed`, `find ... -exec`/`-execdir`/`-ok`, `xargs`, and any quoted script/payload argument to an interpreter | `Unknown` (the payload can read/write arbitrary paths; the shipped default is `None`, `src/policy/shell_guard.cpp:878-925`) |
| Here-document (`<<`, `<<-`) | `Unknown` (the body is not parsed) |
| `$(...)`, backticks, unquoted `$VAR` in a target position | `Unknown` unless the substitution is fully resolvable by the L1 recursion |
| `file:` URL (case-insensitive; `file:/abs`, `file:///abs`, `file://localhost/abs`, percent-encoded) | strip the scheme/host, percent-decode the path, and classify the decoded path as a normal absolute/relative target; outside -> `Outside`, inside -> in-root, non-local host or ambiguous -> `Unknown` (N12) |
| `host:path` (scp/rsync-style) | classify the `path` part; other URI schemes (`http:`, `https:`, `ssh:`, `git:`, ...) are neutral (network, not a local filesystem target) per OQ-90-4 |
| Truncated scan (`s.truncated`) | `Unknown` |

**URI parsing (N12).** Each word/operand is checked for a URI scheme (a
case-insensitive `^[A-Za-z][A-Za-z0-9+.-]*:` prefix). A token is treated as a
URI only when the scheme is a **known URI scheme** (`file`, `http`, `https`,
`ssh`, `git`, `ftp`, `ftps`, `sftp`, `ws`, `wss`) or is followed by `//`;
otherwise a `host:path`/`user@host:path` scp-style token classifies the part
after the first `:` by the normal absolute/relative rules (preserving the
existing `scp host:/etc/passwd` behavior). For a `file:` scheme:

1. Strip the scheme. If the remainder starts with `//`, parse the authority:
   an empty host or `localhost` is local; any other host -> `Unknown` (a
   remote file authority is not a local target); then the path starts at the
   next `/`.
2. Percent-decode the path (`%2F` -> `/`, `%2E` -> `.`, `%25` -> `%`; `+` is a
   literal plus, not a space). A malformed `%` escape -> `Unknown`.
3. Classify the decoded path with the normal absolute/relative rules and the
   tracked cwd; apply the protected-target scan to the decoded path too. A
   `file:` path that resolves outside -> `Outside`; inside -> in-root;
   ambiguous/empty/non-local host -> `Unknown`.

Other schemes (`http:`, `https:`, `ssh:`, `git:`, `ftp:`, ...) remain neutral
per OQ-90-4: the path dimension is about local filesystem targets, and ymh has
no network policy. A `file:` URL is local and is **not** neutral. The same URI
parsing applies to an option value after `--opt=value`/attached-option
splitting (for example `curl --url=file:///etc/passwd`).

**Curl-style local-file operands are best-effort (N18).** The table recognizes
the documented curl forms (`@path`, `name=@path`, `name=<path`, `name@path`) by
stripping the prefix and classifying the remainder. This is **defense in
depth, not soundness**: a new tool-specific syntax, a program that opens a
file internally, or an interpreter payload is **NotClaimed** by the guarantee
matrix (sec 1.7) and recorded as a planned follow-up (working name spec 91,
OQ-90-9). Recognizing more visible
syntaxes does not change the guarantee; it only reduces the most common
best-effort gap. An ambiguous prefix -> `Unknown` (asks).

The scope is **advisory matching input**. It does not execute, stat, or resolve
symlinks, and it does not replace L1/L2. A build tool such as `make` has no
path-like token and is `InRoot`; whatever the Makefile does is bounded by L2,
not by this lexical classifier (sec 15, risk 1).

`classify_shell_request(request, canonical_tmp_root())` is the request-level wrapper used by
`evaluate` and `remember`. It runs in this order (NEW-3):

1. compute the **effective cwd** for `terminal open` (below);
2. take `shell_guard_text(request)` and classify the text against that cwd;
3. if the text is empty, return `NotApplicable` for a non-shell tool and
   `Unknown` for `shell`/`terminal` -- **except** a `terminal open` with a
   declared cwd, whose cwd class (Outside/InRoot/protected) is already known
   and is returned even with empty text; a cwd strictly under `/tmp` is
   `Outside` for the path-rule scope (it is outside the workspace root), never
   `InRoot`, so `path:"**"` cannot match it;
4. force `scope = Unknown` for a `terminal write` while preserving `risk`,
   `protected_target`, and `tmp_exempt` (the input text is classified).

So a bare `terminal open {cwd:".ymh"}` (no command) is still `protected_target`/
`Deny`; `{cwd:"/tmp"}` and `{cwd:"/tmp/sub"}` are `Outside`/`Ask` and are not
`tmp_exempt` because there is no command; the empty-text early return does not
run before the cwd classification. A non-empty `terminal open {cwd:"/tmp/sub",
command:"touch x"}` uses the temp pass and can be exempt.

**`terminal open`'s explicit `cwd` is a classified target (HIGH-1).** The
shipped `terminal` tool accepts `arguments["cwd"]`
(`src/tools/terminal_tool.cpp:38`, `:150-153`), but `shell_text_of` always
uses `request.root` (`src/policy/shell_guard.cpp:1853-1855`). The wrapper
therefore computes an **effective cwd** for `terminal open`:

1. Read `arguments["cwd"]`; absent -> `root`; non-string/empty -> `Unknown`.
2. Lexically resolve `(root / cwd).lexically_normal()` (absolute `cwd` kept).
3. If outside root -> the request is `Outside` (Ask), including `/tmp/...`; the
   cwd is still a temp target for the temp pass. If inside -> use it as
   the cwd for both the scope walk and the L1 risk classifier; if ambiguous ->
   `Unknown`.
4. A `cwd` under `.ymh`/`.git` (in-root) sets `protected_target=true`.

The same effective cwd is passed to `classify_shell_command_with_scope` so
relative destructive operands are classified against the declared terminal
cwd, not always the root. `terminal write` remains `Unknown` (no per-PTY cwd);
only `terminal open` has a declared `cwd`. This is tested under both
`SandboxMode::Workspace` and `SandboxMode::Unrestricted`: an outside `cwd` is
`Outside`/Ask, never auto-approved; under Workspace the later `resolve()`
throws `PathEscape` for a non-temp outside cwd, but the user is asked first.
**Tool change pinned (LOW-2).** The shipped tool's first step is
`context.resolve(cwd)`, which throws for `/tmp` under `Workspace`/`ReadOnly`.
For a declared cwd the tool therefore computes
`weakly_canonical(root / cwd)` first; when the workspace is not at/under `/tmp`
and the canonical result is strictly under canonical `/tmp`, it sets
`request.cwd` directly (no `context.resolve` call). Otherwise it uses the
existing `context.resolve(cwd)` path. The independent check then rejects every
other outside/protected cwd. This is the only temp exception at the tool layer
(MEDIUM-1/LOW-2); the integration test must assert the PTY actually opens at
`/tmp`.

### 4.4.1 Shell git subcommand protection (N1, H1)

A shell `git` subprocess can write `.git/config`, delete `.ymh`, or install a
hook without naming `.ymh`/`.git` as an operand. The scope pass therefore
classifies the `git` subcommand:

1. After 89-D4 wrapper/env stripping, if the executable word is `git`, skip
   global git options and their values (`-C`, `-c`, `--git-dir`,
   `--work-tree`, `--namespace`, `--exec-path`, `--config-env`, `-p`) and take
   the first remaining word as the subcommand.
2. Apply the table below **only when the effective cwd is `InRoot`**. If the
   command scope is `Outside`, the command is `Outside` (asks); if `Unknown`,
   it is `Unknown` (asks). A `-C /tmp` or `--git-dir=/tmp/x` makes the
   effective target outside, so the table does not turn it into a protected
   deny.
3. `protected_target=true` wins over `InRoot` and returns the hard deny in
   step 5 of sec 4.3.1. `UnknownControl` forces `Unknown` (asks).

| git subcommand (or form) | Effect | Reason |
|---|---|---|
| `status`, `diff`, `log`, `show`, `rev-parse`, `ls-files`, `ls-tree`, `cat-file`, `blame`, `grep`, `describe`, `shortlog`, `whatchanged` | normal scope (read-only) | no persistent write |
| `branch`/`tag` list, `remote -v`/`show`/`get-url`, `submodule status`/`summary`, `worktree list` | normal scope (read-only) | no persistent write (LOW-8) |
| `config --get`, `--get-all`, `--get-regexp`, `--list` (even with `--local`) | normal scope (read-only) | no write; the classifier checks these read forms before the write forms |
| `add`, `commit`, `checkout`/`switch` **without** `-b/-B/-c/-C` and without `--track`/`-t` (switching to an existing branch/worktree), `restore`, `reset`, `merge`, `rebase`, `cherry-pick`, `revert`, `gc`, `prune`, `repack`, `fetch`/`pull` **without `--set-upstream`**, plain `push`, `remote prune` (refs only), `tag` mutations | normal scope (data plane) | writes index/objects/refs/worktree; **not** `.git/config` for existing-branch switching. **Branch creation/rename/copy and any `branch.*`/`remote.*` write are in the protected row**, including no-start creation under an invisible global `branch.autoSetupMerge=always` (NEW-1). No persistent code execution. Direct path writes to `.git/**` remain denied. |
| `stash` with `-a`/`--all`/`-u`/`--include-untracked`, including clustered short options `-au`/`-ua`/`-uab`/... (any short-option cluster containing `a` or `u`) | `protected_target=true` -> `Deny` | removes untracked/ignored `.ymh` from the worktree (N13/N20); the same deletion class as `clean` |
| plain `stash`, `stash pop`/`apply`/`drop`/`clear` | normal scope (data plane) | tracked-change stash/restore or stash-ref deletion; does not remove worktree `.ymh` |
| **Branch creation/copy/rename and config-writing forms:** `branch <new>` (creation, no start), `branch <name> <start>`, `branch -m/-M/-c/-C` and `--move`/`--copy` (rename/copy rewrites `[branch]`), `branch -d/-D/--delete` (removes `[branch]`), `checkout -b/-B <name> [start]` and attached `-b<name>`/`-B<name>`, `switch -c/-C <name> [start]`, `--create`/`--force-create[=]<name>`, attached `-c<name>`/`-C<name>`, `switch --track`/`-t`/`-t<ref>`, `checkout --track`/`-t`, `branch --track`/`-t`/`-t<ref>`, `branch --set-upstream-to`/`-u`/`-u<ref>`, `branch --set-upstream-to`/`-u`, `branch --unset-upstream`, `branch --edit-description`, `fetch --set-upstream`, `pull --set-upstream`, `push -u`/`--set-upstream`; `remote` mutations (`add`/`remove`/`rename`/`set-url`/`set-branches`); generally any `branch`/`remote`/`checkout`/`switch`/`push`/`fetch`/`pull` form that writes/deletes a `branch.*`/`remote.*` config key (NEW-6/NEW-7). `fetch -u` is **not** `--set-upstream` (it is `--update-head-ok`) and is data plane; `pull -u` is likewise not treated as set-upstream. **Creation/start forms are protected unless `--no-track`; `branch -m/-M/-c/-C`/`--move`/`--copy` and `branch -d/-D/--delete` are always protected.** Attached short values for the config-writing letters `b`/`B`/`c`/`C`/`u`/`t`/`m`/`M` are split (`-bfoo`, `-Bfoo`, `-cfoo`, `-Cfoo`, `-uorigin/main`, `-torigin/x`). A `-c branch.autoSetupMerge=...` prefix is ignored for the class; the creation form decides. | `protected_target=true` -> `Deny` | writes `.git/config` subsections (remote/branch config keys, not `core.*`, but still the protected control file; N14/N22/HIGH-2/NEW-1). **Branch creation with no start is still protected because a pre-existing global `branch.autoSetupMerge=always` can write tracking config; the classifier is I/O-free and cannot see it, so this is deliberately conservative.** `--no-track` is the opt-out for creation/start forms. |
| `config` writing (`--local`, no scope, `--worktree`, or `--file` under `.git`/`.ymh`) | `protected_target=true` -> `Deny` | writes `.git/config`/`.git/config.worktree` (`core.hooksPath`, `core.fsmonitor`, aliases) -> persistent code execution (the prior H1 vector) |
| `config --global`, `config --system`, or `--file <outside>` | `Outside` -> `Ask` (not auto-allow) | writes outside the root (`$HOME/.gitconfig`, `/etc/gitconfig`, arbitrary file) |
| `clean` | `protected_target=true` -> `Deny` | deletes untracked/ignored `.ymh` (session DB/config/checkpoints); `-fdx` is the concrete vector |
| `submodule` mutations (`add`/`update`/`sync`/`foreach`/...) | `protected_target=true` -> `Deny` | writes `.gitmodules`/`.git/modules`; a `!command` update hook is persistent execution. `submodule status`/`summary` are read-only. |
| `worktree` mutations (`add`/`remove`/`move`/`prune`/`lock`/`unlock`) | `protected_target=true` -> `Deny` | writes `.git/worktrees` (control surface). `worktree list` is read-only. |
| `init`, `clone` | `UnknownControl` -> `Ask` | creates a new `.git` control surface; not auto-approved, but a legitimate setup operation |
| `filter-branch`, `filter-repo`, any unknown/unparseable subcommand | `UnknownControl` -> `Ask` | can run scripts/history rewrites; fail-safe |

**Branch creation/copy/rename is protected (HIGH-2/NEW-1).** Git's default
`branch.autoSetupMerge=true` writes `branch.<name>.remote`/`.merge` into
`.git/config` when a new branch is created from a remote-tracking start point,
even without explicit `--track` (`git checkout -b feat origin/main`,
`git branch feat origin/main`, `git switch -c feat origin/main`,
`git switch --track origin/main`). The classifier is I/O-free and cannot see a
**pre-existing global `branch.autoSetupMerge=always`**, so a creation with no
start point can also write tracking config (`git checkout -b z1`,
`branch z2`, `switch -c z3`). It also cannot resolve a start point, so a `/`
operand is treated as remote-tracking. Conservatively:

- every branch **creation** form (`checkout -b/-B`, `switch -c/-C`,
  `branch <new>` with or without a start) is protected **unless
  `--no-track` is present**;
- `switch -t`/`checkout -t`/`branch -t` (`--track`) are protected;
- `branch -m/-M/-c/-C` (rename/copy rewrites `[branch]`) are **always**
  protected; `--no-track` does not apply;
- a `-c branch.autoSetupMerge=...` prefix is ignored for the class; the
  creation form decides;
- a `checkout`/`switch` with a `/`-containing start is protected/Ask unless
  `--no-track`; switching to an existing local branch (no `-b/-B/-c/-C`, no
  `--track`, no `/` start) is data plane.

`--no-track` is the data-plane opt-out for creation/start forms. This is
deliberately conservative because of the invisible global config.

`git` aliases are covered by the same rule: an alias defined in `.git/config`
is already protected; a `-c alias.x=...` is per-command and does not persist.
The table is a **shell-only** control-plane rule; it does not change the
`git_status`/`git_diff` tools (90-D15) or the direct path-tool protected check.
The data-plane exemption for worktree-writing forms (`checkout`, `restore`,
`reset`, `merge`, `pull`, plain `stash`, ...) assumes `.ymh` is untracked/
ignored (LOW-10): if `.ymh` is tracked, those forms can write it. Do not track
`.ymh`.

The exact regression vectors are `shell "git config --local core.hooksPath
.evil"` and `shell "git clean -fdx"`: both are `protected_target=true` and
`Deny` when the cwd is the root, and both are covered by tests in sec 14.

### 4.5 How a `path` rule matches

A config rule with a non-empty `path` matches a request as follows:

- **Path-targeting tool** (`tool_has_path_dimension(request.tool)`): the request
  must have `request.path`, its target class must be `InRoot`, and
  `path_glob_match(rule.path, root_relative_target(request))` must be true.
  `root_relative_target` is `request.path->lexically_relative(request.root)`
  normalized with `/` separators; the root itself is `"."`. If the relative
  result is absolute, empty, `".."`, or starts with `"../"`, the rule does not
  match (outside is never matched by a path rule).
- **`shell` / `terminal`**: the request's target class must be `InRoot`, and the
  pattern must be exactly `"**"`. The shell target path is not statically known,
  so a narrower pattern such as `src/**` cannot match a shell request. A shell
  rule that needs to narrow the command must use the `command` dimension
  (for example `{"tool":"shell","path":"**","command":"git *","effect":"allow"}`).
- **Any other tool**: no match. A path rule never matches a tool that has no
  path dimension (MCP, etc.).

Consequences, pinned:

- `path:"**"` is the **universal in-root pattern**. It matches the root, every
  in-root path, and in-root shell commands. It does **not** match an outside
  path or an unknown shell scope.
- Protected `.ymh/**` and `.git/**` targets are **hard-denied before matching**
  (sec 4.3.1). A path rule is never even consulted for them. This is the one
  exception to "in-root is auto-approved".
- A path rule is a **positive in-root matcher**. It cannot express "outside".
  Outside/unknown targets simply fail to match and fall through to the other
  rules and the configured default. With the recommended `default:"ask"` they
  ask; with `default:"allow"` they can be allowed (the documented residual,
  sec 15).
- `terminal write` is always `Unknown` scope (sec 4.4), so a path rule never
  auto-approves a PTY write; the Session option can remember an exact input
  chunk, but the path rule cannot.
- The path pattern does not let `**` cross into `..`: the outside test is
  performed before glob matching, and `..` segments are rejected in config
  patterns (sec 4.6).

### 4.6 `path` pattern syntax and validation

`permissions.rules[].path` is a **root-relative** pattern over
`path_glob_match` (`src/execution/glob.cpp:79-85`):

- `*` matches within one segment; `**` matches zero or more whole segments;
  `?` matches one character in a segment. The matcher is `flat_match`
  (`src/execution/glob.cpp:10-34`), which has **no character-class syntax**:
  `[` and `]` are literal bytes. A path pattern containing `[` or `]` is a
  `ConfigError` so a rule cannot load and silently never match (M1).
- No leading `/`, no leading `~`, no `..` segment, no `.` segment other than
  the single pattern `"."` (so `src/./x` is a `ConfigError` too; the normalized
  target never contains `.` segments), and no leading `./` (write `src/**`,
  not `./src/**`). A present empty string is invalid. The single pattern `.`
  is allowed and matches the root itself.
- A `path` rule **must also set `tool`**. Rationale is the same as the existing
  command-requires-tool rule (46-D1.2/F-08): a one-dimension `{path:...}` rule
  ties the one-dimension builtin `{tool:...}` rules and then loses the `tool`
  comparison, so the operator's rule would silently not win. Requiring `tool`
  makes the path rule at least as specific as the builtin it is meant to
  override. The canonical broad form is `{"tool":"*","path":"**",...}`.
- `command` still requires `tool` (unchanged). A rule may use `tool` + `path` +
  `command` together; the dimensions all must match (AND).
- Unknown keys remain fatal (46-I1). Only `tool`, `path`, `command`, `effect`,
  and `id` are accepted in a rule object.
- A malformed glob (`validate_glob`) is still `PolicyConfigError` ->
  `ConfigError` (46-I1/O-M4).

The grants file (`permissions.jsonc`) does not gain a `path` key; the loader
still rejects unknown keys and fail-safe-skips the file (46-I33). Only the
layered config file accepts `path`.

**Layer scope: the whole `permissions` table is global-layer-only (N1, H1).**
The structural fix for the workspace-config escalation is to make every
`permissions.*` key global-layer-only: `permissions.default`,
`permissions.shell`, `permissions.write`, `permissions.read`,
`permissions.rules`, `permissions.presets`, and `permissions.default_preset`.
The deployment baseline `agent.sandbox` is also global-layer-only, because a
workspace `unrestricted` value would widen `effective_sandbox_mode`
(`src/config/config.cpp:1726-1734`, `:1753-1760`). A workspace
`.ymh/config.jsonc` that contains a `permissions` table or `agent.sandbox` is a
`ConfigError` at load, exactly like a workspace `presets` key today
(`src/config/config.cpp:479-494`). The agent may still write `.ymh/config.jsonc`
(path tools and the shell git-control classifier deny/try to deny it, sec
4.3.1), but even a successful write cannot broaden the policy on the next
daemon start: the workspace permission layer is rejected before it can set
`default`/`read`/`write`/`shell`/`rules`/`master`. This makes the write a
denial-of-service/error at worst, not a privilege escalation. The recommended
fragment (sec 4.7) belongs in the **global** layer.

Consequences:

- The old behavior (workspace `permissions.rules` replaces the global array,
  `src/config/config.cpp:537`; workspace `shell`/`write`/`read`/`default` were
  accepted) is superseded for `permissions.*` (90-A19). The merge/clear code
  remains for any future non-permission section, but a workspace
  `permissions` table never reaches `to_permission_config`.
- A workspace cannot **narrow** permissions either; that is the price of a
  simple, fail-safe rule. Per-workspace narrowing would need a narrow-only
  comparison and is deferred (OQ-90-8).
- Non-permission workspace keys (logging, workspace, prompt, etc.) are
  unaffected; `agent.sandbox` is the one exception and is global-only because
  it feeds the sandbox baseline. The protected guard still applies to the
  file.
- This is defense in depth, not a replacement for the protected-path check:
  the check stops the direct write; the global-only rule stops the escalation
  even if the write succeeds through an unpredicted shell vector.
- **Migration (N15).** An existing workspace `.ymh/config.jsonc` that carries
  a `permissions` key or `agent.sandbox` now aborts daemon startup
  (`ConfigError`). Move those keys to the global `config.jsonc` or delete
  them. This is a startup error/DoS, not an escalation. It also supersedes
  52 sec 2.2 (`:206-209`) and the 46 localcode-import note where they say the workspace
  layer overrides a permission key; that statement is now false for
  `permissions.*` (90-A19).

### 4.6.1 Durable grant loading (H1/M9)

The grants loader (`src/config/grant_store.cpp:70-110`) is tightened without a
schema change:

- A `tool` value containing `*`, `?`, or `[` is rejected (no wildcard durable
  grant). The loader loads `{}` and re-asks fail-safe.
- A grant for `shell` or `terminal` with no `command` (or an empty command) is
  rejected: a tool-wide shell grant is unbounded.
- A `shell`/`terminal` grant whose `command` pattern has **no literal
  character** (for example `"*"`, `"?"`, `"**"`, or any combination of only
  glob metacharacters) is rejected as match-all (N16). A pattern such as
  `"git *"` is bounded and allowed. The loader loads `{}`/re-asks fail-safe.
  A hand-authored file cannot turn the persistent grant layer into allow-all
  for shell/terminal.
- A grant for a path-targeting tool with no command is accepted (the shipped
  `Always` shape) but loads as `target_class = InRoot`, not `Any`. It therefore
  cannot match an outside path, including under `SandboxMode::Unrestricted`.
- A grant for a path-targeting tool **with a non-empty `command`** is rejected
  (LOW-9): path tools have no command dimension and `command_pattern` returns
  empty, so such a grant would be dead configuration (O-H1 principle). The
  loader loads `{}`/re-asks fail-safe.
- A grant for a non-path tool with no command remains tool-wide (`Any`), which
  is the shipped MCP behavior.
- The file shape (`tool`, optional `command`, `match`, `effect`, `id`) is
  unchanged; the new behavior is a load-time validation/class derivation.
- We do **not** reject every empty-command grant. A path-tool grant with no
  command is the shipped `Always` shape and is bounded to `InRoot`; a non-path
  (MCP) grant with no command is tool-wide by design. The dangerous cases are
  rejected: a wildcard `tool` and an empty command for `shell`/`terminal`.
- The `RulePermissionPolicy` constructor applies the same normalization to
  every `GrantStore::load()` result (defense in depth), so a non-file test
  store cannot inject a wildcard `tool` or an empty shell/terminal command.
- `remember` follows the same rules for the in-memory `Always` grant: a
  path-targeting tool requires `request.path` and sets `InRoot`; a
  shell/terminal `Always` uses `shell_guard_text` (so `terminal write` is not
  empty) and rejects an empty raw; protected targets create no grant.

### 4.7 The user's configuration, and why `read:"ask"` is required

The exact target behavior is expressed by this `config.jsonc` fragment:

```jsonc
{
  "permissions": {
    // Ask by default, and keep the tool-verdict seeds at ask so an outside
    // target is not auto-allowed by a one-dimension builtin rule.
    "default": "ask",
    "read": "ask",
    "write": "ask",
    "shell": "ask",

    // TEMP-ROOT EXEMPTION (Rev 9; direct-write-only in Rev 16): a direct,
    // non-recursive, regular-file/new-path write/delete/create shell/terminal
    // invocation whose every visible target is strictly under /tmp is allowed before any of these settings (the declared terminal-open cwd is the one existing-directory exception). No key here
    // (or `agent.sandbox`) can deny it; reads are not exempt. Path tools are
    // not exempt; $TMPDIR is ignored. The bare root, glob/brace/internal-../
    // ambiguous targets, mixed commands, and untargeted Forbid constructs
    // keep the normal verdict (Ask, or a guard Deny) (sec 4.8).

    // Auto-approve in-workspace targets only. `tool:"*"` plus `path:"**"`
    // is two dimensions, so it outranks the one-dimension builtin rules.
    "rules": [
      {
        "tool": "*",
        "path": "**",
        "effect": "allow",
        "id": "workspace.in-root"
      },
      // Belt-and-braces only: the policy already hard-denies these before
      // the fold (sec 4.3.1). The denies are 2-dimension rules whose path
      // has more literal characters than "**", so they would win anyway.
      {
        "tool": "*",
        "path": ".ymh/**",
        "effect": "deny",
        "id": "protected.ymh"
      },
      {
        "tool": "*",
        "path": ".git/**",
        "effect": "deny",
        "id": "protected.git"
      }
    ]
  }
}
```

Put this fragment in the **global** layer (`$XDG_CONFIG_HOME/ymh/config.jsonc`).
A workspace `.ymh/config.jsonc` cannot carry any `permissions` key at all
(90-D18/90-A19): the whole table is global-layer-only, so a workspace
`default:"allow"`/`read`/`write`/`shell`/`rules` is a `ConfigError` at load.
The protected-path guard and the shell git-control classifier are mandatory
and independent of this file.

**Documented effect (matches the guarantee matrix, sec 1.7).** With this
fragment: in-root **path-tool reads** (`read_file`/`grep`/`glob`/
`git_status`/`git_diff`) and **path-tool writes** (`write_file`/`edit_file`)
are auto-approved; in-root **visible** shell/terminal writes/deletes and
recognized read operands are matched and auto-approved unless the guard or the
protected check asks/denies; outside visible targets ask; `.ymh/**`/`.git/**`
are denied; **hidden shell reads/egress are not guaranteed** (NotClaimed, spec
91/OQ-90-9). The config is not a claim that a shell subprocess cannot read an
arbitrary file.

Why each part is needed:

- `"default": "ask"` is already the shipped fallback, but it is not sufficient
  by itself because the builtin tool rules are consulted in the fold.
- `"read": "ask"` is the part the naive snippet misses. `permissions.read`
  defaults to `"allow"` (`include/ymh/config/config.hpp:188-194`), and the
  builtin `read_file`/`grep`/`glob` rules inherit it
  (`src/cli/wiring.cpp:156-158`). Without `"read": "ask"`, an outside read
  returns `Allow` from the builtin rule, then fails `PathEscape` at execution;
  it never asks. Setting `"read": "ask"` makes the outside read fall through to
  `Ask` (the path rule does not match outside, and the builtin read rule is now
  ask).
- `"write": "ask"` and `"shell": "ask"` are already the defaults; they are
  restated so the fragment is self-contained and survives a deployment that
  changed them.
- The allow rule uses `tool:"*"` (not a bare `path`) because of the specificity
  rule in sec 4.6.
- The `.ymh/**`/`.git/**` denies are **not** the protection mechanism for
  shell/terminal: a shell path rule only matches the exact pattern `"**"`
  (sec 4.5), so these deny rules match path tools only. The policy hard deny in
  sec 4.3.1 covers both path tools and shell/terminal explicit targets, and
  beats `default:"allow"` and `danger-full-access`.
- Do **not** use `"default": "allow"`, `"shell": "allow"`, or the
  `danger-full-access` preset for this goal: `"allow"` is a global opt-out that
  also allows outside targets (except where the guard denies), which is the
  opposite of path scoping. The path rule plus `default:"ask"` is the narrow
  construct.

**Temp-root documented effect (Rev 9; scoped in Rev 10).** This fragment's
effect is: **in-root targets allow, direct non-recursive temp-root
write/delete/create targets allow, outside/ambiguous/recursive targets keep the
normal verdict, protected in-root targets deny.** The temp-root allow is not produced by the
`{"tool":"*","path":"**"}` rule; it happens before the fold and therefore also
holds if the operator changes `default` to `"deny"`, `write`/`read`/`shell` to
`"deny"`, removes the allow rule, or sets `agent.sandbox` to `read-only`. It
applies to `shell`/`terminal` only; path tools remain root-confined and use
`shell` for `/tmp` work. A `/tmp` target is outside the workspace, so the
root-relative `.ymh/**`/`.git/**` deny does not apply; a workspace at/under
`/tmp` disables the exemption, so the workspace's own `.ymh`/`.git` is never
reachable through it (PS-F57). The generated sample comment
(90-A10/90-D28) states this; there is no `permissions.*` or `agent.sandbox` key
that disables it.

### 4.8 TEMP-ROOT EXEMPTION (Rev 9; simplified in Rev 12; write-only in Rev 13)

**User requirement (verbatim).** "writes to /tmp shall always be allowed
regardless of all the other permission settings".

### 4.8.1 Canonical `/tmp` root and the strict predicate

**The temp root is canonical `/tmp` only.** There is no `$TMPDIR`, no
`$XDG_RUNTIME_DIR`, no per-user temp base, no protected-root set, and no
`--config`-path interaction. `canonical_tmp_root()` (`include/ymh/execution/
path_safety.hpp`) computes `weakly_canonical("/tmp")` once (function-local
cache); if `/tmp` is missing or not a directory it returns an empty path and the
exemption is disabled. The policy caches it at construction; `evaluate` reads
the cache, and its only filesystem I/O is the bounded per-target
`weakly_canonical`/dangling-symlink check in `request_is_tmp_exempt` (PS3). This
is the user's literal requirement (Rev 13).

`path_is_under_tmp_root(canonical_tmp_root, candidate)` is pure, lexical,
component-wise, and I/O-free:

```text
path_is_under_tmp_root(tmp, target):
  if tmp.empty(): return false              // unavailable /tmp: NOT exempt
  for component in target (before normalization):
      if component == "..": return false    // any `..`, internal or escaping
  if not target.is_absolute(): return false  // caller resolves relative first
  t = target.lexically_normal()
  r = tmp.lexically_normal()
  if t == r: return false                   // bare root: NOT exempt
  return r is a component-wise prefix of t  // at least one more component
```

`/tmpfoo`, `/tmp2/x`, `/tmp/..`, `/tmp/a/../b`, and `/tmp/../etc/passwd` are
false (an internal `..` is rejected before normalization). If `/tmp` is itself
a symlink, a literal `/tmp/x` is not lexically under the canonical root and the
exemption is **disabled** (fail-closed, intentional; use the canonical path or
accept Ask) (LOW-5). a relative target
is resolved against the tracked cwd/root first and is never exempt on its own.
The lexical predicate feeds the classifier; `request_is_tmp_exempt` then
probes/canonicalizes every `tmp_targets` entry and rejects a resolved path
outside `/tmp`, inside the workspace/protected tree, or a dangling/unresolvable
symlink (sec 4.8.3). Under
`Unrestricted`/L2 `Unavailable` the canonical check is still policy-level; only
the kernel backstop is absent (PS-F56/PS-F59).

**Workspace safety (single check).** If the canonical workspace root is equal
to `/tmp` (`root == tmp || path_is_under_tmp_root(tmp, root)`) or lies under
it, the exemption is disabled for that workspace: a
`/tmp` kernel grant would cover the workspace's own `.ymh`/`.git`, so a
`ReadOnly` spawn could hidden-write it. This is not a protected-root set; it is
one workspace-overlap check. Such a workspace keeps normal policy for every
target (recorded limitation, risk 21/PS-F57).

### 4.8.2 Scope: `shell`/`terminal` visible targets only

**Path tools are explicitly NOT exempt (Rev 12).** `read_file`, `write_file`,
`edit_file`, `grep`, `glob`, `git_status`, and `git_diff` remain
workspace-root-confined: `resolve()` and `LocalFilesystem::require_contained()`
still throw `PathEscape` for `/tmp`, exactly as before this errata. There is no
path-tool `TempRootSet`, accessor, or canonical exemption. If an agent needs to
write `/tmp`, it must use `shell` (for example `touch /tmp/x`). This limitation
is stated in sec 1.6 and the guarantee matrix.

A request is **tmp-exempt** iff it is a **write/delete/create** `shell`,
command-bearing `terminal open`, or `terminal write` (sec 4.4's write/read
classification: at least one recognized write, zero reads/unknown segments),
and **every visible filesystem target token** in it is provably a concrete
strict descendant of canonical `/tmp`:

- `shell`: operands, redirections, `cd`-updated relative targets, and
  recursively nested command targets;
- `terminal open`: the declared `cwd` **is a mandatory target** plus every
  command target; an absent cwd defaults to the workspace root and therefore
  fails; a bare `terminal open` (no command) is not exempt;
- `terminal write`: the filesystem targets in `arguments["input"]`; the
  invocation has no target of its own, so an input with no filesystem target is
  not exempt. `terminal read`/`resize`/`close`/`list` have no filesystem target
  and are not exempt.

At least one target must exist. A target is **not** concrete (and the whole
invocation is not exempt) if it is:

- the bare `/tmp` root, `/tmpfoo`, `/tmp2/x`, or any target outside `/tmp`;
- any `..` component **before normalization** (`/tmp/..`, `/tmp/a/../b`);
- any `{`/`}` (brace expansion, quoted or unquoted), any glob metacharacter
  (`*`, `?`, `[`), any unexpanded `$VAR`/`${VAR}`/`$(...)`/backtick, or a
  truncated parse;
- an interpreter/here-doc/`-exec`/`xargs` payload whose targets cannot be
  enumerated;
- a protected target, or a mixed command with any non-temp visible target.

`cd`/`pushd` is a cwd update, not a target; options and `NAME=value` assignments
are neutral; any unknown/interpreter segment blocks the flag (fail-safe). A
shell **read** whose only visible target is under `/tmp` is **not** exempt:
`cat /tmp/x`, read-only commands, and mixed read+write commands keep the
normal shell/default/guard policy; `permissions.read` seeds path tools, not
shell.

**Guard attribution.** The exemption may override a `Forbid`/`Confirm` only
when that verdict is attributable to the `/tmp` targets. An untargeted
`Forbid` construct (fork bomb `:(){ :|:& };:`), an unconditional-`Forbid`
command class (`mkfs`/`mke2fs`/`newfs`/`wipefs`), or a non-temp/ambiguous cause
blocks the exemption even when another segment writes `/tmp/x`.

### 4.8.3 Precedence: a final-verdict exemption

`evaluate` computes the pure classification first, then:

```text
0. request_is_tmp_exempt(request, classification, canonical_tmp_root()) -> Allow
1. classification.risk == Forbid                  -> Deny   (89, unchanged)
2. request.sandbox == ReadOnly && tool_is_mutating(request) -> Deny   (09 Q7, unchanged)
3. classification.risk == Confirm                 -> Ask    (89, unchanged)
4. request.force_ask                              -> Ask    (25-D4, unchanged)
5. protected path                                 -> Deny   (sec 4.3.1, unchanged)
6. target class and the rule fold                 (sec 6, unchanged)
```

Step 0 is the only branch before the guard. It is expressed as a final-verdict
exemption, not as a guard flag: there is no `mutating`/`temp_write_only` flag
threaded through `classify_impl`. `request_is_tmp_exempt` performs the bounded
canonical check for each `tmp_targets` entry: every path prefix is inspected
with `symlink_status`; a symlink that is dangling/unresolvable (`is_symlink`
but `exists` false) or whose `weakly_canonical` errors is **ambiguous** and
makes the predicate false (MEDIUM-2); otherwise the canonical result must be
strict under canonical `/tmp`, must not be the workspace, inside the
workspace, or a protected path, and must not be an existing directory, FIFO,
socket, or device node (only regular files/new paths for direct operations).
The **declared `terminal open` cwd is the one directory exception**: it must
be an existing directory (missing/non-directory is not exempt) and is checked
separately from the regular-file/new-path rule. The predicate identifies that
entry from the request (`terminal`, `action:"open"`, and `arguments["cwd"]`
resolved as in PS27); if it cannot identify the cwd unambiguously, no directory
is exempt (fail-closed). A `/tmp/link -> <workspace>/.git` alias, an
outside alias, or a dangling alias into a missing outside/workspace target is
**not exempt** (MEDIUM-3). This is the only I/O in `evaluate` (PS3 is amended
for this bounded per-target check + dangling-symlink probe). The `ReadOnly` hard deny still uses the
shipped `tool_is_mutating(request)` for every non-exempt request, including
`write_file`/`edit_file` (whose classification is `NotApplicable`).

An exempt invocation is `Allow` even under `default:"deny"`,
`shell`/`write`, `ReadOnly`, `force_ask`, or a guard `Forbid`/`Confirm`
attributable to the `/tmp` targets. Reads keep the normal shell/default/guard
policy (`permissions.read` seeds path tools, not shell). It creates no dialog and
no grant. The protected-path rules are unchanged: a protected target is not
`tmp_exempt`, and a workspace at/under `/tmp` disables the exemption
(sec 4.8.1).

`remember` must apply the same guard **after** classification:

```cpp
ShellClassification cls{};
if (request.tool == "shell" || request.tool == "terminal") {
    cls = classify_shell_request(request, canonical_tmp_root());
    if (request_is_tmp_exempt(request, cls, canonical_tmp_root())) {
        return {};                       // no session/durable grant
    }
    if (cls.protected_target) {
        return {};
    }
}
```

This is executable code in the sketch, uses the current two-argument
classifier, and sits after `cls` is computed (sec 7.3).

### 4.8.4 `SandboxMode` and L2

Policy-only exemption is not sufficient under `Workspace` with L2 `Enforced`:
the shipped ruleset does not grant RW on `/tmp`. `ConfinementPolicy` gains
`std::optional<std::filesystem::path> tmp_root` (`nullopt` when
`canonical_tmp_root()` is empty **or** `root == tmp ||
path_is_under_tmp_root(tmp, root)`; otherwise canonical `/tmp`) and `bool workspace_writable`.
`apply_or_abort` adds `kTmpRights`
(read+exec+write+make+remove+**refer**) for `tmp_root`; `FS_REFER` is required
for a cross-directory rename/link inside `/tmp`. `Workspace` keeps
`kRootRights`; `ReadOnly` grants only `kReadRights`; no admitted `/tmp` grant
covers the workspace. The shipped call sites become

```cpp
SpawnConfinement confinement(context.root(), context.execution().mode());
```

and `SpawnConfinement` calls `canonical_tmp_root()` and omits the rule when the
workspace is at/under `/tmp`. Under `Unrestricted` or L2 `Unavailable` the
exemption is policy-only; a shell symlink alias under `/tmp` can escape the
temp scope and is the recorded residual (risk 21/PS-F59). Path tools are not
affected: they remain daemon-side, root-confined, and never receive a `/tmp`
resolution.

## 5. Decision register (90-D#)

**90-D1 - Re-introduce `path` as a config rule dimension.**
`PermissionRuleSettings` gains `std::optional<std::string> path`
(`include/ymh/config/config.hpp:145-150`); `apply_permissions` accepts it
(`reject_unknown` set `{"tool","path","command","effect","id"}`,
`src/config/config.cpp:542-545`); `to_permission_config` copies it to
`PolicyRule::path` (`src/cli/wiring.cpp:175-183`). This supersedes O-H1 for
config rules (90-A1). Caller: `apply_permissions` at
`src/config/config.cpp:541-560`; `to_permission_config` at
`src/cli/wiring.cpp:175-183`.

**90-D2 - Populate `PermissionRequest::path` in the agent loop from tool
arguments, lexically.** The request-build block calls
`request_path_target(call.name, call.arguments, request.root)` (new symbol,
sec 8) and assigns the result to `request.path`
(`src/agent/agent_loop.cpp:871-889`). The helper is pure and total over
malformed JSON: a missing/non-string/empty required `path` yields `nullopt`; a
missing optional `path` yields the root; an absolute `path` is normalized as
given; a relative `path` is normalized as `(root / path)`. No `resolve()` and
no filesystem access happen in the permission path. `request.path` is
**absolute, lexically normalized**, not the old "root-relative" comment; the
header comment is amended (90-A5). Symlink policy is sec 4.3.

**90-D3 - Validate `path` strictly, and require `tool` with it.**
`validate_rule(tool, path, command)` (a new 3-argument overload; the existing
2-argument form delegates with an empty path) rejects: all dimensions empty, a
`path` without `tool`, a `command` without `tool`, a leading `/`, a leading
`~`, any `..` segment, any `.` segment other than the single pattern `"."`, a
`[` or `]` (the matcher has no character classes; M1), and a malformed glob.
The config parser additionally rejects a `path` key that is present but empty
(a JSON presence check, before the validator is called; the validator treats
an empty path as absent). A malformed rule is a `ConfigError` at load (fatal),
never a silent default. The
`PolicyRule` overload gains the same path checks so a programmatically built
rule cannot bypass them. Caller: `apply_permissions`
(`src/config/config.cpp:562-563`) and the `RulePermissionPolicy` constructor.

**90-D4 - A path rule matches in-root targets only.**
`rule_matches` (i) requires `InRoot` and a matching root-relative pattern for
path tools, (ii) requires `InRoot` and the exact pattern `"**"` for
shell/terminal, and (iii) never matches a tool with no path dimension. Outside
and unknown targets never match a path rule; they fall through to the fold and
the default. This is the user's "outside asks" behavior under
`default:"ask"`. The rule is a positive in-root matcher, not an outside
detector (sec 4.5).

**90-D5 - Derive a shell target scope from the spec-89 tokenizer.**
`classify_shell_command_with_scope` returns
`{ShellCommandRisk, ShellTargetScope, protected_target}`;
`classify_shell_command` delegates to `.risk`, preserving every existing
caller. Scope is computed as sec 4.4 (Forbid -> Outside, Confirm -> Unknown,
`terminal write` -> Unknown, None -> lexical target walk). Scope **does depend
on `kernel_confinement` through the risk gate** (Confirm -> Unknown), so a
guard `Ask` can never seed an InRoot prefix grant (M2). The walk splits
`--opt=value` and known attached short options, treats ambiguous path-bearing
options as Unknown, and marks interpreter/`awk`/`sed`/`-exec`/`xargs` payloads
Unknown (M8/M11). `classify_shell_request(request, canonical_tmp_root())` is the request-level
wrapper. The scope never authorizes a command by itself.

**90-D6 - Keep the precedence exactly as shipped, then add the protected deny.**
`Forbid -> Deny`, `ReadOnly` mutating -> `Deny`, `Confirm -> Ask`,
`force_ask -> Ask`, **protected path -> Deny**, then the fold (90-D13). The
target class and path rules are consulted only inside the fold. No path rule or
grant is consulted before the guard or the protected check. The reason strings
for the guard are unchanged (`shell-guard:outside-root`,
`shell-guard:unbounded-in-root`); a protected deny uses a bounded
`permission:protected-path` reason.

**90-D7 - Make the Session grant class-scoped.**
A `Session` answer creates a grant whose key is `(session, tool, TargetClass,
match)`, where `match` is one of:
- `InRoot` path tool: tool + `InRoot`, command empty -> covers every in-root
  path for that tool in this session;
- `InRoot` simple `shell` / `terminal open` command: tool + `InRoot` +
  executable prefix (`command_prefix`) -> covers the same **single simple
  command** in-root in this session (for example `git` after approving
  `git status`); the prefix is an exact string equality, not a glob. A compound
  command (`;`, `&&`, `||`, `|`, `&`, newline) has no prefix: `command_prefix`
  returns empty and the grant is exact (M10). After approving `git status`,
  `git status && rm -rf src` does **not** match;
- `terminal write`: exact input chunk (`shell_guard_text(request)`, i.e.
  `arguments["input"]`), target class `Unknown`; never a prefix, because a PTY
  stream has no simple-command/cwd guarantee (M3/M4);
- `Outside` shell/terminal: tool + `Outside` + the exact raw
  `shell_guard_text(request)` (`literal=true`) -> covers only that identical
  outside command; outside path tools create no Session grant (the answer
  applies once);
- `Unknown` shell/terminal: tool + `Unknown` + the exact raw
  `shell_guard_text(request)` (`literal=true`) -> covers only that identical
  unprovable command;
- non-path tool (MCP, etc.): tool + `Any` -> tool-wide within the session
  (unchanged behavior).

A **protected** request (`.ymh`/`.git` explicit target) creates no grant:
`remember` returns `{}` before building the key, so Session and Always cannot
persist a protected-path answer.

The Session grant is still `Layer::SessionGrant`, still stored in
`session_grants_[session]`, still in-memory, still lost on daemon restart
(46-I6). It never overrides the guard (90-D6). The label stays
`Allow for session`; the dialog option set and indices are unchanged.

`command_prefix(text)` returns empty unless the text is one simple command
(one segment, no unquoted `;`, `&&`, `||`, `|`, `&`, or newline). The
`rule_matches` prefix branch therefore cannot match a compound command whose
first word happens to equal the approved executable.

Session is a narrowing layer in the existing fold, not a precedence override:
a more specific config rule (for example `{tool:"write_file",path:"src/**"}`
with two dimensions) can still beat a one-dimension path-tool Session grant.
This is the 09 sec 3.4 order, unchanged. A user who wants Session to win over a
specific config ask must remove or relax that config rule; the spec does not
reorder specificity to favor Session.

**90-D8 - Unknown/outside shell grants stay exact.**
Prefix matching is limited to the provable in-root class. An outside or
unprovable shell command is remembered by its exact raw text, so the user's
"outside asks" default is not widened by a prefix. A path-tool outside request
creates no Session grant at all: `ExecutionEnvironment::resolve()` rejects the
outside path with `PathEscape`, so such a grant would be dead code, and the
requirement is that outside targets ask. This is the safety asymmetry in
90-D7.

**90-D9 - `Always` keeps its file shape but becomes in-root for path tools.**
`Always` remains tool + command only on disk, with no `path` key
(46-I5/46-I33). It is **exact for command-bearing tools** (`shell`/`terminal`,
using `shell_guard_text` so `terminal write` is not empty), **in-root-wide for
path tools** (the loader and `remember` set `target_class=InRoot` for a
path-tool grant with no command), and **tool-wide only for non-path tools**
(MCP). It is not uniformly "the precise durable answer": for path tools it is
coarser than Session's path class, and before this errata it could match
outside paths under `SandboxMode::Unrestricted`, where `resolve()` skips
containment (`src/execution/environment.cpp:72-74`); setting `InRoot` removes
that outside match (M9). The grants loader additionally rejects wildcard `tool` and
empty `command` for `shell`/`terminal` (sec 4.6.1). A future errata may store a
finer path class durably; that needs a grants-file schema bump (OQ-90-1).

**90-D10 - Document the recommended config, the `read` gotcha, and the layer.**
The first-run sample (`src/config/config.cpp:1496-1508`) **will** document the
optional rule (90-A10; it does not today, L5). The user-facing fragment is sec
4.7 with `read:"ask"`, the protected deny rules, and the **global** layer.
The spec states loudly that `default:"allow"`, `shell:"allow"`, and
`danger-full-access` do not provide path scoping, and that a workspace
`permissions` table is rejected entirely (90-D18); a workspace can no longer
broaden the policy (M5/N1).

**90-D11 - No new cache or generation state.**
The target class and root-relative pattern are per-call values. The only new
persistent state is the `path` string in the parsed `Config` and the
`target_class`/`prefix` fields on a `PolicyRule`; `target_class`/`prefix` are
not specificity dimensions and do not affect rule ordering. `evaluate` remains
deterministic and I/O-free.

**90-D12 - Preserve the wire and UI contract.**
`protocol::PermissionRequest` does not gain `path` or a scope field. The UI
sends the existing `PermissionScope::Session`, and the daemon's `remember`
computes the class from the core request. This avoids a 05 sec 7.6 wire change
and keeps the dialog label/option set unchanged.

**90-D13 - Protected paths are hard-denied before the fold.**
Any `.ymh`/`.git` component at any depth is protected (sec 4.3.1; N3). The
check is lexical, component-wise, case-sensitive, and runs after the 89
guard/`ReadOnly`/`Confirm`/`force_ask` and before the rule fold and every
grant. If the 89 guard already returned `Ask`/`Deny`, that verdict stands;
otherwise the protected check returns `Deny`. Path tools canonicalize
independently of `resolve()` and re-check (`read`/`write`/`edit`), so the check
works under `Unrestricted` (N2); `grep`/`glob` canonicalize/reject a protected
base and skip protected traversal. Shell explicit targets and git control-plane
subcommands (sec 4.4.1/90-D17) set `protected_target`; `remember` returns `{}`
for a protected request, so no Session/Always grant can launder it. Along with
the global-only `permissions` table (90-D18), this is the closure of H1: the
in-root auto-approve promise excludes the permission system's own state and VCS
control surface, and a written workspace config cannot broaden the policy.

**90-D14 - `grep`/`glob` traversal exclusions (H2).**
`LocalFilesystem::grep` must skip symlink entries and open candidates without
following symlinks; both `grep` and `glob` must canonicalize the base
independently of `resolve()` (so the check works under `Unrestricted`), reject
a protected base, and skip `.ymh/` and `.git/` entries during traversal. The
base is still `resolve()`-checked. This closes the shipped symlink read leak
(`src/execution/filesystem.cpp:236-254`, `:268`) and the recursive
protected-read hole for the tool paths, including a `.work -> .ymh` base
alias. A shell subprocess that recursively reads these paths without naming
them remains a recorded residual (sec 15).

**90-D15 - `git_status`/`git_diff` tools must not search ancestor repositories.**
`GitQuery` gains `root` (the repository-open base); `LibGit2GitService::status`
and `::diff` open `query.root` with `GIT_REPOSITORY_OPEN_NO_SEARCH` (or an
equivalent no-search probe) and surface "not a repository" as a tool error.
`GitStatusTool` sets `root=context.root()` and leaves `path` empty;
`GitDiffTool` sets `root=context.root()` and uses `path` as a pathspec, not the
open base. A workspace that is a subdirectory of a larger repository therefore
cannot read the ancestor `.git` **through the git tools** (M7/N9). A nested
in-root repository is not discovered implicitly. A shell `git` command is the
system executable and still searches ancestors; that is a recorded shell
residual (N4, sec 15), not a claim of this tool fix.

**90-D16 - Permission summaries stay root-relative (L6).**
`bounded_summary` currently prints `request.path->generic_string()`
(`src/permission/permission_broker.cpp:100-106`). Once `request.path` is
absolute, the summary would expose the absolute workspace path. Pin: the
summary uses the root-relative target (or the tool name for the root itself),
not the absolute path. This keeps `permission.*.summary` stable and avoids
leaking host paths to the UI/event log.

**90-D17 - Shell git control-plane subcommands are protected (N1/N13/N14/N20/N22/HIGH-2/LOW-8/NEW-1/NEW-6/NEW-7).**
The scope pass classifies the `git` subcommand as sec 4.4.1: writing `config`,
`clean`, `submodule` mutations, `worktree` mutations, `stash
-a/-u/--all/--include-untracked` and clustered `-au`/`-ua`/..., and any
`remote`/`branch`/`checkout`/`switch`/`push`/`fetch`/`pull` form that writes
`branch.*`/`remote.*` config (`remote` mutations, `branch --track`/`-t`/
`--set-upstream*`/`--unset-upstream`/`--edit-description`, `branch
-m/-M/-c/-C`/`--move`/`--copy`, `branch -d/-D/--delete`, branch creation `branch <new>` with or without start,
`checkout -b/-B <name> [start]` + attached `-b<name>`/`-B<name>`, `switch -c/-C <name> [start]` + `--create`/`--force-create` + attached `-c<name>`/`-C<name>`,
`checkout/switch --track`/`-t`/`-t<ref>`, `branch -u<ref>`/`-t<ref>`, `fetch --set-upstream`, `pull --set-upstream`, `push -u/--set-upstream`) set
`protected_target=true` **unless `--no-track`** for creation/start forms
(hard deny when the effective cwd is in-root); `branch -m/-M/-c/-C`/
`--move`/`--copy` and `branch -d/-D/--delete` are always protected; attached
short values for `b/B/c/C/u/t/m/M` are split; `-c branch.autoSetupMerge=...` is ignored for the class;
`init`/`clone`/`filter-branch`/unknown subcommands force `Unknown` (ask);
plain `stash`, read-only forms (`config --get/--get-all/--get-regexp/--list`,
`remote -v/show/get-url`/`remote prune`, `submodule status/summary`,
`worktree list`, `branch` list), and data-plane subcommands keep the normal
scope with the stated exemption. Branch creation is protected even with no
start because an invisible global `branch.autoSetupMerge=always` can write
tracking config; `--no-track` is the opt-out. `fetch -u`/`pull -u` are not
`--set-upstream` (they are `--update-head-ok`) and remain data plane. This is the shell-side
protected check the direct path-tool check cannot provide.

**90-D18 - All `permissions.*` keys and `agent.sandbox` are global-layer-only (N1).**
The structural fix for workspace-config escalation: a workspace
`.ymh/config.jsonc` containing a `permissions` table or `agent.sandbox` is a
`ConfigError` at load. The workspace layer can no longer set
`default`/`shell`/`write`/`read`/`rules`/`presets`/`default_preset`/`master`
or the sandbox baseline, no matter how the file was written. The global layer
remains the one place to express the user's path-scoped policy. Workspace
narrowing is deferred (OQ-90-8). This is defense in depth with the
protected-path check and the shell git-control classifier.

**90-D19 - `file:` URLs are local targets, not neutral (N12).**
The scope pass parses URI schemes per sec 4.4: a case-insensitive `file:`
scheme is stripped (including `file://localhost`), percent-decoded, and
classified by the normal absolute/relative rules; outside -> `Outside`, inside
-> in-root, non-local host/ambiguous -> `Unknown`. Other schemes remain
neutral (OQ-90-4). A `file:` URL whose decoded path is `.ymh`/`.git` also sets
`protected_target`. This closes `curl file:///etc/passwd` and
`git remote add evil file:///...` from the in-root auto-approve.

**90-D20 - Curl-style local-file operands are recognized best-effort (N18).**
The scope pass recognizes `@path`, `name=@path`, `name=<path`, and
`name@path` (curl data/form/urlencode local-file syntax), strips the prefix,
and classifies the remainder by the normal absolute/relative/protected rules;
`@-` is stdin (neutral), ambiguous -> `Unknown`. This is **not** a soundness
claim: hidden interpreter/unknown-tool reads remain NotClaimed (sec 1.7,
planned follow-up OQ-90-9). The vector is recognized for defense in depth, not as a
guarantee.

**90-D21 - Protected glob tokens are protected (N19/LOW-1/LOW-2).**
A path-bearing token whose component contains a glob is protected only when
the component's **literal prefix starts with `.`** and the component
glob-matches `.ymh`/`.git` (`.ymh*`, `.git*`, `.ym?`, `.*`, `src/.ymh*`). A
bare `*`/`**` is **not** protected (it respects the in-root read promise; a
destructive broad glob is already the guard's `Confirm`). The token must
resolve **in-root**: an outside `.ymh`/`.git` path is `Outside` (Ask), not
protected. The same test applies to `find ... -delete` and to a Session
grant's target before `remember`. This is glob-aware, not a bare substring:
`.ymhfoo` remains unprotected.

**90-D22 - `terminal open`'s declared `cwd` is a classified target (HIGH-1).**
`classify_shell_request` reads `arguments["cwd"]` for `terminal open`, resolves
it lexically against the root, and uses it as the effective cwd for the scope
and L1 risk classification; outside/non-string/ambiguous -> `Outside`/
`Unknown` (Ask), in-root `.ymh`/`.git` -> `protected_target`, a strictly-temp
cwd -> temp (bare open `Unknown`, non-empty command judged by the temp pass),
otherwise the normal scope. `terminal write` stays `Unknown` (no per-PTY cwd).
This closes the outside-`cwd` auto-approve under both `Workspace` and
`Unrestricted`. The `terminal` tool also canonicalizes the resolved `cwd`
independently (`weakly_canonical`) and rejects a canonical path that is neither
in-root nor strict temp, and rejects an in-root protected path, so a symlink
alias cannot escape under `Unrestricted` (same pattern as A17; MEDIUM-1).

**90-D23 - TEMP-ROOT EXEMPTION: shell/terminal write invocations wholly under
canonical `/tmp` are Allow (Rev 9; write-only in Rev 13).**
The temp root is canonical `/tmp` only; `$TMPDIR`, `$XDG_RUNTIME_DIR`, other
temp bases, and protected-root sets are not part of the exemption. A
recognized **direct, non-recursive, regular-file/new-path** write/delete/create
`shell`/`terminal` invocation (the declared `terminal open` cwd is the one
existing-directory exception) is exempt iff all its **write** targets (destinations, redirections, `terminal open` cwd,
`terminal write` input targets) and any canonicalized visible target are
concrete strict descendants of `/tmp`, with no internal `..` before
normalization, no brace/glob/unresolved expansion, no interpreter payload, no
protected target, no read/unknown role (a read source such as `cp`/
`install`/`ln`/`dd if=` makes the request non-exempt), no read+write `<>`,
no existing directory/FIFO/socket/device target **other than the declared
`terminal open` cwd** (which must be an existing directory), no recursive/following form (`rm -r`, `cp -r/-a`, `chmod`/`chown`,
`find -L/-delete`, `rsync`, `install -D`), and any guard `Forbid`/`Confirm`
attributable to those targets (sec 4.8.2). The final
check canonicalizes `tmp_targets`; a resolved target outside `/tmp`, inside the
workspace/protected tree, or a dangling/unresolvable symlink is not exempt. Path tools are **not** exempt and
stay root-confined. Reads are **not** exempt. An exempt request returns `Allow` before
the L1 guard, `ReadOnly`, `force_ask`, the protected check, and the fold; it
cannot be disabled or reordered by any config key, preset, rule, or grant
(PS29). The bare root, wildcard/brace/unexpanded targets, mixed commands, bare
opens, and a workspace at/under `/tmp` keep the normal verdict (`Ask`, or a
guard `Deny`; PS30). Callers: `RulePermissionPolicy::evaluate` (sec 6) and
`remember` (sec 7).

**90-D24 - `canonical_tmp_root` and `path_is_under_tmp_root`.**
`canonical_tmp_root()` (new, `include/ymh/execution/path_safety.hpp`) computes
`weakly_canonical("/tmp")` once and returns an empty path if unavailable.
`path_is_under_tmp_root(canonical_tmp_root, candidate)` returns false for an
empty root and for any `..` component before normalization; otherwise it is
component-wise, lexical, I/O-free, and false for equality. `RulePermissionPolicy`
warms the cache in its constructor so `evaluate` never performs the first I/O.
The final predicate canonicalizes `tmp_targets` (bounded I/O); there is no
`TempRootSet`, no
alias list, no `$TMPDIR` reader, and no protected-root computation. Callers:
`RulePermissionPolicy::evaluate`/`remember`, `classify_shell_request`,
`SpawnConfinement`, and the terminal cwd check
(`src/policy/permission_policy.cpp:416-520`,
`src/tools/builtin_tools.cpp:436-448`,
`src/tools/terminal_tool.cpp:135-176`,
`src/execution/landlock.cpp:41-115`).

**90-D25 - Evaluate the exemption as a final verdict; no grant and no config
switch.**
The evaluate order is `tmp-exempt -> Allow` for shell/terminal, then the
unchanged `Forbid`/`ReadOnly`/`Confirm`/`force_ask`/protected/fold order. The
flag is `ShellClassification::tmp_exempt` (write-only); there is no
`mutating`/`temp_write_only` guard flag. `remember` classifies first and returns
`{}` for an exempt request (active guard, sec 7.3). Reads never set it. `SandboxMode::ReadOnly` for non-exempt requests
still uses `tool_is_mutating(request)` (NEW MEDIUM-2). The only way to make an
exempt request ask is to make a target fail the strict/concrete/write-only
bound (read, read+write `<>`, recursive/following form, existing directory/
non-regular target other than the terminal-open cwd, bare root, internal
`..`, glob, brace, unexpanded, interpreter, mixed, protected, empty canonical
`/tmp`, resolved workspace/protected path, dangling/unresolvable symlink, or a
workspace at/under `/tmp`; PS30).

**90-D26 - L2 grants canonical `/tmp`; `ReadOnly` keeps a read-only workspace
root.**
`ConfinementPolicy` gains `std::optional<std::filesystem::path> tmp_root`
(`nullopt` when the canonical root is empty or the workspace is at/under
`/tmp`; otherwise canonical `/tmp`) and `workspace_writable`. `landlock.cpp::apply_or_abort` adds `kTmpRights`
(including `FS_REFER`) for `tmp_root` and uses `kRootRights` only when
`workspace_writable`, otherwise `kReadRights`. Because the workspace-overlap
check disables the grant for a workspace at/under `/tmp`, the additive `/tmp`
grant cannot cover the workspace root; a temp-exempt spawn cannot hidden-write
the workspace under `ReadOnly` (PS32). Under `Unrestricted`/L2 `Unavailable`
the exemption is policy-only and the shell symlink residual is recorded
(risk 21/PS-F59).

**90-D27 - The temp safety bound is syntactic and deliberately conservative.**
A target is not exempt when it is a read/unknown invocation, a recursive/
following form, an existing directory/non-regular target other than the
terminal-open cwd, the bare `/tmp` root, contains
any `..` component before normalization, a shell glob or `{`/`}`, is
`/tmpfoo`, is an unexpanded/truncated expansion, is an
interpreter/here-doc/`-exec`/`xargs` payload, belongs to a command with any
non-temp visible target, is a bare `terminal open`, is a `terminal open` whose
declared cwd is not strictly under `/tmp`, is a `terminal write` whose input
has no write/`/tmp` target, is a path tool, or resolves (symlink) outside
`/tmp`, into the workspace/protected tree, or through a dangling/unresolvable
symlink. An untargeted `Forbid`/`Confirm` construct or an
unconditional-`Forbid` class blocks the exemption (sec 4.8.2/PS30).

**90-D28 - Document the temp-root effect in the recommended config and the
first-run sample.**
The sec 4.7 fragment gains the comment and the "Temp-root documented effect"
paragraph; the generated sample (90-A10, `src/config/config.cpp:1496-1508`)
will state that a **direct, non-recursive, regular-file/new-path**
write/delete/create `shell`/`terminal` invocation wholly under canonical `/tmp`
is allowed (the declared `terminal open` cwd is the one existing-directory
exception), that recursive/following forms (`rm -rf`, `chmod -R`,
`find -L -delete`) ask, that reads and path tools are not exempt (use `shell`
for direct `/tmp` writes), that `$TMPDIR` is ignored, and that bare/glob/brace/
internal-`..`/ambiguous targets keep the normal verdict (`Ask`, or a guard
`Deny`). It will also state that a workspace at/under `/tmp` is not exempt. No
default behavior changes and no new config key is accepted.

## 6. Evaluation algorithm (pinned order)

```text
evaluate(request):
  classification = classify_shell_request(request, canonical_tmp_root())
  // empty text -> NotApplicable/Unknown, except a terminal open with a
  // declared cwd returns its cwd class (InRoot/Outside/protected);
  // terminal write -> scope Unknown, tmp_exempt from its input;
  // otherwise classify_shell_command_with_scope(shell_guard_text(request),
  //   request.root, effective_cwd(request), request.kernel_confinement,
  //   canonical_tmp_root()),
  // where effective_cwd is the declared `terminal open` cwd, else root.
  // The classifier also sets `tmp_exempt` (a write/delete/create, every
  // visible target concrete strict under canonical /tmp, no internal ../
  // brace/glob/unresolved, guard verdict attributable; reads never set it).

  0. TEMP-ROOT EXEMPTION (Rev 9; simplified in Rev 12, sec 4.8) -> Allow:
       if request_is_tmp_exempt(request, classification, canonical_tmp_root()):
           return Allow
       // This is the ONLY branch before the guard. It applies to shell/terminal
       // only; it creates no grant and consults no rule. Path tools never reach
       // it (they stay root-confined). A bare root, glob/brace/../unresolved
       // target, mixed command, path tool, or workspace at/under /tmp falls
       // through.

  1. classification.risk == Forbid                  -> Deny   (89, unchanged)
  2. request.sandbox == ReadOnly && tool_is_mutating(request) -> Deny   (09 Q7, unchanged)
  3. classification.risk == Confirm                  -> Ask    (89, unchanged)
  4. request.force_ask                               -> Ask    (25-D4, unchanged)

  5. PROTECTED PATH (H1, sec 4.3.1) -> Deny:
       if request.path.has_value()
          && path_is_protected(request.root, *request.path): Deny
       if classification.protected_target: Deny

  6. target_class = classify_target(request, classification.scope):
       if request.path.has_value():
           path_is_within_root(request.root, *request.path) ? InRoot : Outside
       else if tool_has_path_dimension(request.tool):
           Unknown
       else if request.tool == "shell" || request.tool == "terminal":
           map(classification.scope)   // NotApplicable -> Unknown
       else:
           Any

  7. candidates = base_rules_ + session_grants_[request.session.value]
     (the shipped reorder() order is unchanged)

  8. for each candidate rule:
       tool:  rule.tool empty or glob_match(rule.tool, request.tool)
       path:
         if rule.path is empty: pass
         else if tool_has_path_dimension(request.tool):
             target_class == InRoot
             && request.path.has_value()
             && path_glob_match(rule.path, root_relative_target(request))
         else if request.tool is shell/terminal:
             target_class == InRoot && rule.path == "**"
         else:
             fail
       target_class:
         if rule.target_class != Any:
             rule.target_class == target_class
       command:
         if rule.command is empty: pass
         else if rule.prefix:
             command_prefix(shell_guard_text(request)) == rule.command
         else if rule.literal:
             exact_text = (request.tool == shell/terminal)
                              ? shell_guard_text(request)
                              : command_pattern(request)
             exact_text == rule.command
         else:
             glob_match(rule.command, command_pattern(request))

  9. winner = greatest by specificity -> layer -> effect (09 sec 3.4, unchanged)
 10. deny-veto for narrowing Allow (46-D2.13, unchanged)
 11. winner ? winner.effect
            : (tool_defaults match ? that verdict : config.default_verdict)
```

Notes:

- **Step 0 is the temp-root exemption (Rev 9; simplified in Rev 12).** It is
  evaluated before the guard (and therefore before
  `ReadOnly`/`force_ask`/protected/the fold). It performs the bounded
  canonical/symlink probe (sec 4.8.3) and returns `Allow` only for a
  **write/delete/create** `shell`/`terminal` invocation whose every
  visible target is a concrete strict descendant of canonical `/tmp` and whose
  guard verdict is attributable to those targets; reads keep the normal read
  policy. Path tools, bare roots, globs, braces,
  `..`/unresolved targets, mixed commands, untargeted `Forbid` constructs, bare
  opens, and a workspace at/under `/tmp` fall through to the normal order.
- **`classify_shell_request` and `classify_shell_command_with_scope` take the
  canonical `/tmp` path**; `canonical_tmp_root()` is cached and returns empty
  when `/tmp` is unavailable. Empty disables the exemption.
- `classify_target` and the protected check are the new steps; both are pure
  and use the per-call classification. The protected check is a hard deny, not
  a rule. The protected check is unreachable for a temp-exempt request because
  step 0 already returned; a protected target is never `tmp_exempt`.
- `root_relative_target` is: `request.path->lexically_relative(request.root)`;
  if the result is empty, use `"."`; if it is absolute, is `".."`, or starts
  with `"../"`, return `nullopt` (no match).
- `map(ShellTargetScope)` is `InRoot -> InRoot`, `Outside -> Outside`,
  `Unknown/NotApplicable -> Unknown`.
- A path rule on a path tool with `request.path == nullopt` fails (the
  `Unknown` class does not satisfy `InRoot`).
- The `literal` branch uses `shell_guard_text` for `shell`/`terminal`, so an
  exact `Always`/Session grant on `terminal write` compares the input chunk,
  not the empty `command_pattern` (M3).
- The `deny-veto` and `tool_defaults` logic are unchanged; a path rule simply
  joins the candidate set.

## 7. `remember()` and the Session grant

### 7.1 Why the shipped Session option appears broken

The UI and broker wiring are correct: the supervisor maps option index 1 to
`GrantScope::Session` (`src/ui/supervisor.cpp:3581`), `wire_scope` maps it to
`protocol::PermissionScope::Session` (`:230-240`), the broker maps it back
(`src/permission/permission_broker.cpp:149-156`), and `onDecision` calls
`policy.remember` (`src/permission/permission_broker.cpp:335-345`). The problem is the grant's key:

1. For `shell`/`terminal`, `remember` stores
   `command = command_pattern(request)` with `literal=true`
   (`src/policy/permission_policy.cpp:535-657`, `:133-162`). For `shell` that
   is the raw command; for `terminal` it is `command` or joined `argv`, and it
   **never reads `arguments["input"]`**. Any different command, even `git diff`
   after `git status`, does not match and asks again. An outside/unknown
   `terminal write` has an empty command and cannot be remembered at all. This
   is the main reason a session decision "does nothing" for shell users.
2. For path tools, `command_pattern` returns empty, so the grant is tool-wide
   and does work across paths. Users who mostly answer shell prompts do not see
   that.
3. The guard short-circuits before grants: `Forbid -> Deny` and
   `Confirm -> Ask` (`:416-520`). A destructive in-root command with L2
   unavailable is `Confirm` on every call, so Session can never suppress it.
   This is by design (89-D1/89-D15), not a wiring bug.
4. The grant is in-memory and lost on daemon restart (46-I6); after a restart
   the next call asks again.
5. A `force_ask` request (plan mode) returns `Ask` before the fold and records
   no grant (46-D2.6).
6. A protected `.ymh`/`.git` request must never create a grant (sec 4.3.1),
   so even a successful `Always` selection on it applies once.

The fix is therefore semantic: broaden the key to a class for the provable
cases, use `shell_guard_text` for the exact fallback (so terminal writes are
covered), keep the exact fallback for unprovable/outside cases, refuse a
prefix for compound commands, and keep the guard/protected precedence
untouched. The Rev 9/12 temp exemption is evaluated before `remember` and creates no
grant; `remember` classifies first and returns `{}` for a `tmp_exempt` request
(active guard, sec 4.8.3/Ps-F60).

### 7.2 The class-scoped key and matching

`remember(request, kind, scope)` keeps its signature. For `scope == Session`
it creates:

| Request target | Session grant | Matching |
|---|---|---|
| path tool, `InRoot` | `tool` + `target_class=InRoot`, `command=""`, `prefix=false` | same tool, any in-root path |
| path tool, `Outside` or `path==nullopt` | no grant (return `{}`) | the answer applies once; outside always asks |
| `shell`, `InRoot`, single simple command, non-empty prefix | `tool` + `InRoot`, `command=prefix`, `prefix=true`, `literal=false` | same tool, `InRoot`, same executable prefix, and the request is one simple command |
| `terminal open`, `InRoot`, single simple command, non-empty prefix | tool + `InRoot`, prefix grant | same as shell |
| `terminal write` (always `Unknown`) | tool + `Unknown`, `command=shell_guard_text(request)` (the input chunk), `literal=true`, `prefix=false` | same tool, `Unknown`, byte-identical input chunk; no prefix |
| `shell`/`terminal`, `InRoot`, compound command | tool + `InRoot`, `command=shell_guard_text(request)`, `literal=true`, `prefix=false` | same tool, `InRoot`, byte-identical compound command (M10) |
| `shell`/`terminal`, `Outside`, non-empty raw | tool + `Outside`, `command=shell_guard_text(request)`, `prefix=false`, `literal=true` | same tool, `Outside`, byte-identical raw command |
| `shell`/`terminal`, `Unknown`, non-empty raw | tool + `Unknown`, `command=shell_guard_text(request)`, `prefix=false`, `literal=true` | same tool, `Unknown`, byte-identical raw command |
| `shell`/`terminal`, no text/empty raw | no grant (return `{}`) | the answer applies once |
| non-path tool | tool + `Any`, `command=""`, `prefix=false` | tool-wide in the session (unchanged) |
| protected `.ymh`/`.git` request | no grant (return `{}`) | hard-denied before the fold; the answer applies once |
| `tmp_exempt` **write/delete/create** request (Rev 9; Rev 13) | no grant (return `{}`) | the exemption is evaluated first and auto-`Allow`s only a recognized write whose every target is concrete strict `/tmp`; no dialog is shown. `remember` classifies first and returns `{}` so nothing durable or session-scoped is stored. Reads follow the normal read policy and also create no grant through this row |

`command_prefix(text)` is pure and uses the spec-89 tokenizer/wrapper stripping
(`src/policy/shell_guard.cpp:657-719`; 89-D4): it returns the first executable
word only when `text` is **one simple command** (one segment, no unquoted `;`,
`&&`, `||`, `|`, `&`, or newline), so
`sudo git status`, `FOO=1 git status`, and `git status` all have prefix `git`.
It returns an empty string for empty/unparseable/compound text. Prefix
matching is exact byte equality, not a glob. A compound command therefore
falls back to the exact grant and cannot inherit the `git` prefix (M10).

The grant id for Session is
`grant:<tool>:<class>:<p|e>:<16 hex of fnv1a64(command)>`, where `class` is
`i`/`o`/`u`/`a` for `InRoot`/`Outside`/`Unknown`/`Any` and `p`/`e` marks
prefix vs exact. `Always` keeps the existing `grant:<tool>:<16 hex>` id and its
durable file entries (90-A12).

### 7.3 Lifetime, guard, and label

- `Session` remains in-memory, keyed by `request.session.value`, owned by
  `RulePermissionPolicy`, dropped when the daemon exits (46-I6). It survives
  session switches within the same daemon only for its own session key; it is
  not durable and not written to `permissions.jsonc`.
- `force_ask` still returns `{}` before any grant is built; `Deny` and `Once`
  still return `{}`; a protected request also returns `{}`; a `tmp_exempt`
  shell/terminal request returns `{}` after classification (it is auto-allowed
  before the dialog, sec 4.8.3).
- `Always` keeps the file shape but path-tool grants load as `InRoot` and
  shell/terminal grants use `shell_guard_text`; an empty shell/terminal raw is
  rejected (90-D9/sec 4.6.1).
- The dialog label is `Allow for session` (unchanged), index 1, with the same
  four options and the same two-option `force_ask` dialog. The spec defines the
  class the label grants; no wire or render change is required. A protected
  request that reaches step 5 is hard-denied, so no dialog is shown for it; a
  protected destructive command with L2 off is a guard `Confirm` (`Ask`) and
  **does** show the dialog (LOW-3).
- The guard can never be suppressed: a `Session` grant is a candidate in the
  fold, and the fold is never reached for `Forbid`/`Confirm`/`ReadOnly`/
  `force_ask`/protected.

## 8. New symbols and their callers

Every new symbol is normative: signature, contract, and at least one concrete
caller in the shipped tree it lands in.

| Symbol | Signature | Contract | Concrete caller |
|---|---|---|---|
| `ymh::TargetClass` | `enum class TargetClass : uint8_t { Any, InRoot, Outside, Unknown }` | request target class; matching input only | `PolicyRule::target_class`; `classify_target` in `evaluate` |
| `ymh::tool_has_path_dimension` | `bool tool_has_path_dimension(std::string_view tool) noexcept` | true for exactly the seven tools in sec 4.2; no allocation | `request_path_target`, `evaluate`, `remember` |
| `ymh::request_path_target` | `std::optional<std::filesystem::path> request_path_target(std::string_view tool, const nlohmann::json& arguments, const std::filesystem::path& canonical_root)` | pure, total over malformed JSON; sec 4.2 mapping; absolute lexically normalized result | `AgentLoop` request build, `src/agent/agent_loop.cpp:885` |
| `ymh::path_is_within_root` | `bool path_is_within_root(const std::filesystem::path& root, const std::filesystem::path& candidate)` | lexical containment; root itself true; no I/O; no home special case | `classify_target` in `evaluate`, `remember` |
| `ymh::path_is_protected` | `bool path_is_protected(const std::filesystem::path& root, const std::filesystem::path& candidate)` (`include/ymh/execution/path_safety.hpp`) | lexical component-wise `.ymh`/`.git` check at **any depth** (sec 4.3.1); no I/O; single shared implementation (LOW-6) | `evaluate` (step 5), `remember`, `read_file`/`write_file`/`edit_file` after independent canonicalization, `LocalFilesystem::grep`/`glob` |
| `ymh::canonical_tmp_root` | `const std::filesystem::path& canonical_tmp_root()` (`include/ymh/execution/path_safety.hpp`) | cached `weakly_canonical("/tmp")`; empty if `/tmp` is unavailable and then `path_is_under_tmp_root`/`request_is_tmp_exempt` are false; no `$TMPDIR`/XDG/other base | `RulePermissionPolicy::evaluate`/`remember`, `SpawnConfinement`, terminal cwd check |
| `ymh::path_is_under_tmp_root` | `bool path_is_under_tmp_root(const std::filesystem::path& canonical_tmp_root, const std::filesystem::path& candidate)` | false for an empty root, a relative candidate (the caller resolves relative targets first), or any `..` component before normalization; otherwise lexical, component-wise, absolute, strict (equality false); no I/O; `/tmpfoo`/`/tmp/..`/`/tmp/a/../b` false | `classify_shell_request`, `request_is_tmp_exempt`, terminal cwd check |
| internal `request_is_tmp_exempt` | `bool request_is_tmp_exempt(const PermissionRequest&, const ShellClassification&, const std::filesystem::path& canonical_tmp_root)` | resolve-time predicate: true iff `shell`/`terminal`, `classification.tmp_exempt` (the lexical precursor), non-empty canonical root, workspace not `/tmp`/under it, and every `tmp_targets` has no dangling/unresolvable symlink component, canonicalizes strict under `/tmp`, and is outside the workspace/protected tree; every target **except the declared `terminal open` cwd** (identified from the request's `cwd`, PS27) must be a regular file or a new path (existing directory/FIFO/socket/device rejected); the terminal cwd must be an existing directory (LOW-2/LOW-5); no read roles, no recursive/following forms; path tools always false; this is where all filesystem I/O for the exemption lives | `evaluate` step 0, `remember` after classification |
| `ymh::ShellClassification::tmp_exempt` | `bool tmp_exempt = false` | **I/O-free lexical precursor** (not the final verdict): true iff a recognized **direct non-recursive** write/delete/create, **zero read roles** (`<>` is read+write and blocks it), at least one visible target, every target a concrete strict `/tmp` descendant (no internal `..`, brace/glob/unresolved/interpreter), no lexically protected target, no recursive/following form (`rm -r`, `cp -r/-a`, `chmod`/`chown`, `find -L/-delete`, `rsync`, `install -D`), and guard verdict attributable; `request_is_tmp_exempt` adds the canonical/symlink/workspace/type checks (LOW-2/LOW-3/LOW-4/LOW-5) | set by `classify_shell_command_with_scope`/`classify_shell_request`; read by `request_is_tmp_exempt` |
| `ymh::ShellClassification::tmp_targets` | `std::vector<std::filesystem::path> tmp_targets` | absolute lexically-normalized **all** visible targets (read sources + write destinations + `terminal open` cwd); canonicalized by `request_is_tmp_exempt`; empty for non-temp/unknown. `tmp_exempt` additionally requires zero read roles | set by `classify_shell_command_with_scope`/`classify_shell_request`; read by `request_is_tmp_exempt` |
| `ymh::ConfinementPolicy::tmp_root` / `workspace_writable` | `std::optional<std::filesystem::path> tmp_root; bool workspace_writable = true;` | canonical `/tmp` only; `tmp_root` is `nullopt` when `canonical_tmp_root()` is empty or the workspace is at/under `/tmp`; L2 grant `kTmpRights` (includes `FS_REFER`) and `ReadOnly` root-rights choice (90-D26) | `SpawnConfinement`, `landlock.cpp::apply_or_abort` |
| `ymh::ShellTargetScope` | `enum class ShellTargetScope : uint8_t { NotApplicable, InRoot, Outside, Unknown }` | shell filesystem target class | `ShellClassification`, `remember`, `classify_target` |
| `ymh::ShellClassification` | `struct ShellClassification { ShellCommandRisk risk; ShellTargetScope scope; bool protected_target = false; bool tmp_exempt = false; std::vector<std::filesystem::path> tmp_targets; }` | returned by the additive classifier; `tmp_exempt` is the Rev 12 shell/terminal final-verdict flag | `RulePermissionPolicy::evaluate`/`remember` |

## 9. C++ interface sketches

```cpp
// include/ymh/policy/path_target.hpp (new)
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ymh {

enum class TargetClass : std::uint8_t { Any, InRoot, Outside, Unknown };

[[nodiscard]] bool tool_has_path_dimension(std::string_view tool) noexcept;

[[nodiscard]] std::optional<std::filesystem::path> request_path_target(
    std::string_view tool,
    const nlohmann::json& arguments,
    const std::filesystem::path& canonical_root);

[[nodiscard]] bool path_is_within_root(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& candidate);

} // namespace ymh
```

```cpp
// include/ymh/execution/path_safety.hpp (new)
#pragma once

#include <filesystem>

namespace ymh {

// 90-D24 (Rev 9; simplified Rev 12): canonical /tmp only. Cached
// weakly_canonical("/tmp"); empty when /tmp is unavailable.
[[nodiscard]] const std::filesystem::path& canonical_tmp_root();

// 90-D23 (Rev 9/12): lexical, component-wise, absolute, strict.
[[nodiscard]] bool path_is_under_tmp_root(
    const std::filesystem::path& canonical_tmp_root,
    const std::filesystem::path& candidate);

// 90-D13: lexical component-wise .ymh/.git protection (unchanged).
[[nodiscard]] bool path_is_protected(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& candidate);

} // namespace ymh
```

```cpp
// include/ymh/policy/shell_guard.hpp (additive)
// Rev 9/12 adds: #include "ymh/execution/path_safety.hpp".
namespace ymh {

enum class ShellTargetScope : std::uint8_t {
    NotApplicable,
    InRoot,
    Outside,
    Unknown
};

struct ShellClassification {
    ShellCommandRisk risk  = ShellCommandRisk::None;
    ShellTargetScope scope = ShellTargetScope::NotApplicable;
    bool              protected_target = false;   // explicit .ymh/.git target
    bool              tmp_exempt = false;         // Rev 13: write-only, every
                                                   // visible target strict /tmp
    std::vector<std::filesystem::path> tmp_targets; // canonicalized by the
                                                   // final-verdict predicate
};

[[nodiscard]] ShellClassification classify_shell_command_with_scope(
    std::string_view text,
    const std::filesystem::path& root,
    const std::filesystem::path& cwd,
    bool kernel_confinement,
    const std::filesystem::path& canonical_tmp_root);

[[nodiscard]] ShellClassification classify_shell_request(
    const PermissionRequest& request,
    const std::filesystem::path& canonical_tmp_root);

[[nodiscard]] bool is_simple_shell_command(std::string_view text);
[[nodiscard]] std::string command_prefix(std::string_view text);

} // namespace ymh
```

```cpp
// include/ymh/policy/permission_policy.hpp (delta)
#include "ymh/execution/path_safety.hpp"
#include "ymh/policy/path_target.hpp"

struct PolicyRule {
    // ... existing fields unchanged ...
    TargetClass target_class = TargetClass::Any;  // 90-D7
    bool        prefix = false;                   // 90-D7
};

void validate_rule(std::string_view tool, std::string_view path,
                   std::string_view command);

// Rev 9/12: caches canonical /tmp via canonical_tmp_root() at construction.
explicit RulePermissionPolicy(PermissionConfig config, GrantStore* store = nullptr);
```

```cpp
// include/ymh/config/config.hpp (delta)
struct PermissionRuleSettings {
    std::optional<std::string> tool;
    std::optional<std::string> path;      // 90-D1
    std::optional<std::string> command;
    std::string                effect;
    std::string                id;
};
```

```cpp
// include/ymh/execution/confinement.hpp (delta; Rev 12 single root)
struct ConfinementPolicy {
    std::filesystem::path                root;
    bool                                 workspace_writable = true; // ReadOnly: false
    std::optional<std::filesystem::path> tmp_root;                // canonical /tmp only
    std::optional<std::filesystem::path> home;
    std::optional<std::filesystem::path> tmpdir;
    std::vector<std::filesystem::path>   ro_paths;
};
// SpawnConfinement(path workspace_root, SandboxMode mode);
//                 if (tmp_root) add_path_rule(*tmp_root, kTmpRights);
// kTmpRights includes FS_REFER.
```

```cpp
// src/policy/permission_policy.cpp (delta; remember)
// Rev 12: executable guard after classification, before any grant.
if (request.tool == "shell" || request.tool == "terminal") {
    const ShellClassification cls =
        classify_shell_request(request, canonical_tmp_root());
    if (request_is_tmp_exempt(request, cls, canonical_tmp_root())) {
        return {};
    }
    if (cls.protected_target) {
        return {};
    }
}
```

## 10. Invariants (PS1-PS33)

| ID | Invariant |
|---|---|
| **PS1** | **The path dimension is live.** `permissions.rules[].path` parses, and `PermissionRequest::path` is populated for the seven tools in sec 4.2. A path rule with no producer or no parser is a regression of O-H1 and blocks `verified`. |
| **PS2** | **In-root only.** A path rule matches a path tool only when `path_is_within_root(request.root, *request.path)` is true and the root-relative target matches the pattern. Outside (`..`, absolute outside) never matches. |
| **PS3** | **Lexical purity for path rules; bounded canonicalization for the temp predicate.** Path-rule matching uses `lexically_normal`/`lexically_relative` only (no `canonical`/`realpath`/`relative`/`stat`). The one exception is `request_is_tmp_exempt`, which `symlink_status`-probes each path prefix and `weakly_canonical`-izes each `tmp_targets` entry to enforce the `/tmp`, dangling/unresolvable, and workspace/protected bounds before the final verdict (Rev 13/14). |
| **PS4** | **Symlink/enforcement honesty (H2).** A lexically in-root symlink is `InRoot` for the policy. `read_file`/`write_file`/`edit_file` resolve the final target and enforce containment; `grep`/`glob` resolve only the base, and this errata requires `grep` to skip symlink entries and both to skip `.ymh/`/`.git/` during traversal (90-A13/90-D14). The spec does not claim lexical matching detects symlink escapes, and does not claim the shipped `grep` is safe before that change. |
| **PS5** | **Shell scope is fail-safe and risk-gated (M2/M4/M8/M11).** `Forbid -> Outside`, `Confirm -> Unknown`, truncated/unresolved/ambiguous -> `Unknown`; `terminal write` -> `Unknown`; `InRoot` requires every provable target to be in-root. Because `Confirm -> Unknown`, scope depends on `kernel_confinement` through the risk gate; a guard `Ask` can never seed an InRoot prefix grant. `path:"**"` matches only `InRoot`. |
| **PS6** | **No shell narrowing by a path pattern.** Only `path:"**"` matches a shell/terminal request. `src/**` does not match a shell request; shell narrowing uses the `command` dimension. |
| **PS7** | **Guard and protected precedence.** `Forbid -> Deny`, `ReadOnly` `tool_is_mutating(request)` -> `Deny`, `Confirm -> Ask`, `force_ask -> Ask`, then the protected-path `Deny`, all precede the fold. No path rule or grant changes them. |
| **PS8** | **Outside asks, protected denies (or guard-asks), under the recommended config.** With `default:"ask"` and the sec 4.7 fragment (including `read:"ask"`), an outside/unknown target that matches no path rule returns `Ask` (unless the guard returns `Deny`/`Ask` first); an explicit **in-root** `.ymh`/`.git` target returns `Deny` when the guard did not already return `Ask`/`Deny` (a guard `Confirm` is `Ask`, sec 4.3.1), never `Allow`; an **outside** `.ymh`/`.git` path is `Outside` and asks, not protected (N5/LOW-2). |
| **PS9** | **Session is class-scoped for the provable in-root case.** `InRoot` simple-command shell grants match by executable prefix; `InRoot` path-tool grants match every in-root path for the tool; `Any` non-path grants are tool-wide. A compound command never inherits a prefix grant (M10). |
| **PS10** | **Outside/unknown/terminal-write Session stays narrow.** `Outside`/`Unknown` shell grants match only the byte-identical `shell_guard_text` (including the `terminal write` input chunk); outside path targets create no Session grant; `terminal write` never gets a prefix (M3/M4). |
| **PS11** | **Session lifetime is unchanged.** In-memory, per session, lost on daemon restart, never durable, never written to `permissions.jsonc` (46-I6). |
| **PS12** | **`Always` keeps its file shape but is in-root for path tools (M9).** Durable grants remain tool + command only on disk, exact for command-bearing tools (using `shell_guard_text`), in-root-wide for path tools (`target_class=InRoot` on load/remember), and tool-wide only for non-path tools; the grants file still rejects `path` (46-I5/46-I33). |
| **PS13** | **Config validation is total and fatal.** Unknown keys, `path` without `tool`, `command` without `tool`, no dimension, a leading `/` or `~`, a `..` segment, any `.` segment except the single `"."`, `[`/`]` in a path pattern, an empty `path`, and a malformed glob are `ConfigError` at load. |
| **PS14** | **Specificity is unchanged.** `path` remains a specificity dimension; `target_class` and `prefix` are matching predicates only and do not enter `specificity_key`. The fold order is specificity -> layer -> effect. |
| **PS15** | **Verdict determinism (L9).** The same request, policy generation, and configuration produce the same verdict. Scope and target class have no clock, randomness, or I/O input. Two grants that differ only in `target_class`/`prefix` may tie under `rule_sort_less`/`rule_greater`, but all such grants are `Allow` and their target classes are mutually exclusive, so no verdict change is possible; strict ordering among those ties is not claimed. |
| **PS16** | **Wire and UI unchanged.** No `path` or target-class field is added to `protocol::PermissionRequest`; the Session choice uses the existing `PermissionScope::Session`; the dialog label and option indices are unchanged. The summary is made root-relative (90-D16). |
| **PS17** | **No path rule for non-path tools.** MCP and other tools without a declared path dimension never match a path rule; they fall through to their tool default or the configured default. |
| **PS18** | **No config weakening of the guard or protected set.** No `path`, `target_class`, `prefix`, session grant, `Always` grant, or `default` value can enable/disable/downgrade L1/L2 or allow a protected `.ymh`/`.git` target (89 SG10/SG13, 90-D13). |
| **PS19** | **Protected paths are hard-denied before the fold (H1/N1/N2/N3/N5).** `path_is_protected` is lexical, component-wise at **any depth**, and case-sensitive; the check runs after the 89 guard/`ReadOnly`/`Confirm`/`force_ask` (so a guard `Confirm` is `Ask`) and before any rule and any grant. Path tools canonicalize independently of `resolve()` and re-check, including under `Unrestricted`; `grep`/`glob` canonicalize/skip protected traversal; shell explicit targets and git control-plane subcommands (sec 4.4.1) set `protected_target`; `remember` returns `{}` for protected requests. |
| **PS20** | **Durable grant loading is bounded (H1/M9/N16/LOW-9).** The loader rejects wildcard `tool`, empty `command` for `shell`/`terminal`, a match-all `shell`/`terminal` command pattern with no literal character, and a path-tool grant with a non-empty `command` (dead dimension); a path-tool grant with no command loads as `InRoot`, never `Any`; the file schema is unchanged. |
| **PS21** | **The workspace cannot carry a permission table or sandbox baseline (N1).** Every `permissions.*` key and `agent.sandbox` are global-layer-only; a workspace `.ymh/config.jsonc` containing either is a `ConfigError` at load, so a written workspace config cannot set `default`/`read`/`write`/`shell`/`rules`/`master` or widen the sandbox. |
| **PS22** | **Shell git control-plane protection (N1/N13/N14/N20/N22/HIGH-2/LOW-8/NEW-1/NEW-6/NEW-7).** The scope pass classifies `git` subcommands per sec 4.4.1: writing `config`/`clean`/`submodule` mutations/`worktree` mutations, `stash -a/-u/--all/--include-untracked` and clustered `-au`/`-ua`/..., and any `remote`/`branch`/`checkout`/`switch`/`push`/`fetch`/`pull` form writing `branch.*`/`remote.*` config (including `fetch --set-upstream`/`pull --set-upstream`; `fetch -u`/`pull -u` are data plane) -> `protected_target=true`; branch creation (`checkout -b/-B`, attached `-b/-B`, `switch -c/-C`, `--create`/`--force-create`, attached `-c/-C`, `branch <new>`) is protected **unless `--no-track`** because an invisible global `branch.autoSetupMerge=always` can write tracking config; `branch -m/-M/-c/-C`/`--move`/`--copy` and `branch -d/-D/--delete` are always protected; `init`/`clone`/unknown -> `Unknown`; plain `stash`, read-only forms (`remote -v/show/get-url`, `remote prune`, `submodule status/summary`, `worktree list`), and data-plane subcommands keep normal scope. This is shell-only and does not change the git tools. |
| **PS23** | **`file:` URLs are local (N12).** The scope pass strips/percent-decodes a case-insensitive `file:` URL (including `file://localhost`) and classifies the decoded path by the normal rules; non-local host/ambiguous -> `Unknown`; a decoded `.ymh`/`.git` target sets `protected_target`. Other URI schemes stay neutral per OQ-90-4. |
| **PS24** | **GitQuery root/pathspec separation (N9).** `GitQuery::root` is the repository-open base (`context.root()`); `path` is the pathspec/query scope (empty for status). The service opens `root` with `GIT_REPOSITORY_OPEN_NO_SEARCH`; the pathspec cannot reintroduce ancestor discovery. |
| **PS25** | **Curl local-file operands are best-effort (N18).** The scope pass recognizes `@path`, `name=@path`, `name=<path`, `name@path`, strips the prefix, and classifies the remainder; `@-` is stdin; ambiguous -> `Unknown`. This reduces a known gap; hidden reads remain NotClaimed (planned follow-up OQ-90-9). |
| **PS26** | **Protected globs are protected (N19/LOW-1/LOW-2).** A dot-leading literal-prefix glob component that matches `.ymh`/`.git` (`.ymh*`, `.git*`, `.ym?`, `.*`, `src/.ymh*`) sets `protected_target`; a bare `*`/`**` is not protected; only in-root targets are protected (outside `.ymh`/`.git` -> `Outside`/Ask). Applies to operands, redirections, `find -delete`, and Session grant targets. `.ymhfoo` remains unprotected. |
| **PS27** | **`terminal open` cwd is classified and is a mandatory temp target (HIGH-1; Rev 12).** `classify_shell_request` reads `arguments["cwd"]`, lexically resolves it against root, and uses it for scope/risk; outside/non-string/ambiguous -> `Outside`/`Unknown` (Ask); in-root `.ymh`/`.git` -> protected; a cwd under `/tmp` is `Outside` for the path-rule scope but is a temp target for `tmp_exempt`. `terminal open` requires a non-empty command; an absent cwd defaults to the workspace root and therefore cannot be exempt. `terminal write` forces scope `Unknown` but classifies its input. The tool computes `weakly_canonical(root/cwd)` and, for a canonical cwd strictly under `/tmp` (workspace not at/under `/tmp`), assigns it directly **without** `context.resolve` (which would throw `PathEscape`); every other outside/protected cwd is rejected. |
| **PS28** | **Temp-root definition (Rev 9; simplified in Rev 12).** The temp root is canonical `/tmp` only. `path_is_under_tmp_root(canonical_tmp_root, target)` returns false for an empty `canonical_tmp_root()` and for any `..` component before normalization, then is absolute, lexical, component-wise, and strict: equality is false; `/tmpfoo`, `/tmp2/x`, `/tmp/..`, `/tmp/a/../b`, and `/tmp/../etc/x` are not under `/tmp`. `$TMPDIR`, `$XDG_RUNTIME_DIR`, other bases, and protected-root sets are not used. A relative target is never exempt. An empty `canonical_tmp_root()` disables the exemption. If `/tmp` is a symlink, literal `/tmp` paths are not under the canonical root and the exemption is disabled (fail-closed, intentional). |
| **PS29** | **Temp-root exemption is shell/terminal direct-write-only, per-target, and first.** A recognized **direct, non-recursive** write/delete/create `shell`/`terminal` invocation whose every visible target (+ canonical `tmp_targets`) is a concrete strict `/tmp` regular-file/new path outside the workspace/protected tree, **with the declared `terminal open` cwd allowed to be an existing directory**, returns `Allow` before the L1 guard, `ReadOnly`, `Confirm`, `force_ask`, the protected check, and the rule fold. It is exempt from the outside/above ask path, the spec-89 destructive-outside `Forbid`/`Confirm` attributable to those targets, and every `permissions.*`/`agent.sandbox` setting (including a `deny` default and `read-only`). Path tools are **not** exempt. It is not exempt for reads (including read+write `<>`), recursive/following forms (`rm -r`, `cp -r/-a`, `chmod`/`chown`, `find -L/-delete`, `rsync`, `install -D`), directory/FIFO/socket/device targets other than the terminal-open cwd, a bare open, a bare root, path tools, a workspace at/under `/tmp`, an empty canonical `/tmp`, a resolved workspace/protected path, a dangling/unresolvable symlink, or any guard verdict attributable to an untargeted/non-temp cause. No config key, preset, rule, Session grant, or `Always` grant can disable, narrow, or reorder it. It produces no grant and no dialog. |
| **PS30** | **Temp-root safety bound.** Not exempt and therefore keeping the normal verdict (`Ask`, or a guard `Deny`): any read (read-only or mixed read+write, including `cp`/`install`/`ln`/`dd if=` source operands and `<>`), the bare `/tmp` root (equality), `/tmpfoo`, any `..` component before normalization (`/tmp/..`, `/tmp/a/../b`), an empty canonical `/tmp`, any shell/terminal target with a glob metacharacter, any brace-bearing target (`{`/`}`), any unexpanded `$VAR`/`${VAR}`/`$(...)`/backtick or truncated parse, a command with any visible non-temp target, a mixed command, a path tool, a bare `terminal open`, a `terminal open` whose declared cwd is not under `/tmp`, a `terminal write` whose input has no direct `/tmp` target, a recursive/following form (`rm -r`, `cp -r/-a`, `chmod`/`chown`, `find -L/-delete`, `rsync`, `install -D`), an existing directory/FIFO/socket/device target other than the terminal-open cwd, an interpreter/here-doc/`-exec`/`xargs` payload, an untargeted `Forbid`/`Confirm` construct or unconditional-`Forbid` class, a workspace at/under `/tmp`, a canonical target inside the workspace/protected tree (`/tmp/link -> <workspace>/.git`), a dangling/unresolvable symlink target (`/tmp/link -> <missing>`), and a symlink-resolved escape. For path tools a `*` in a filename is literal, not shell-expanded. |
| **PS31** | **Path tools are not exempt (Rev 12/13).** `LocalFilesystem::require_contained()` is unchanged and always root-only. Under `Workspace`/`ReadOnly`, `resolve()` throws `PathEscape` for `/tmp`; under `Unrestricted`, `resolve()` returns the lexical path but `LocalFilesystem` still throws. `read_file`/`write_file`/`edit_file`/`grep`/`glob`/`git_*` stay workspace-root-confined. Use `shell` for `/tmp` writes. |
| **PS32** | **L2 makes the exemption real where it can.** `ConfinementPolicy` carries `tmp_root` (canonical `/tmp` only; `nullopt` for an empty root or a workspace at/under `/tmp`) and `workspace_writable`; `kTmpRights` includes `FS_REFER`. `Workspace` keeps the root RW, `ReadOnly` grants only `kReadRights`, and the `/tmp` grant cannot cover the workspace because of the workspace-overlap check, so a direct non-recursive temp-exempt spawn cannot hidden-write the workspace under `ReadOnly`; `chmod`/`chown` are not exempt because Landlock does not mediate metadata. Under `Unrestricted` or L2 `Unavailable` the exemption is policy-only; the shell symlink escape is the recorded residual (risk 21). |
| **PS33** | **No new durable state; bounded I/O only for the temp predicate.** The only new state is the cached canonical `/tmp` path (function-local static), the per-call `tmp_exempt` flag, and `tmp_targets`; nothing is persisted, written to the event log, or sent on the wire. `classify_shell_request` and `path_is_under_tmp_root` are I/O-free; `request_is_tmp_exempt` performs a `symlink_status` probe per path prefix plus one `weakly_canonical` per `tmp_targets` entry (PS3); `canonical_tmp_root()` is computed once. |

## 11. Failure modes (PS-F#, F# tagged per 00 sec 54)

F# is the architecture review-finding class (`docs/design/00-architecture.md:4832-4842`):
F1 path/process isolation, F2 background permission, F6 input/config surface,
F10 resume-suspended. Path-scope and containment failures are F1; grant
fail-to-ASK/lifetime failures are F2/F10; config-shape failures are F6.

| PS-F# | Failure | Detection | Required behavior | F# |
|---|---|---|---|---|
| **PS-F1** | `write_file` with `path="../x"` | lexical normalization escapes root | path rule does not match; `default:"ask"` -> `Ask`; execution later `PathEscape` (no silent allow) | F1 |
| **PS-F2** | `shell "cat /etc/passwd"` with `path:"**"` allow | absolute outside operand -> `Outside` | no path match; builtin shell `ask`/default -> `Ask` | F1 |
| **PS-F3** | `shell "rm -rf ~"` | L1 `Forbid` | `Deny` before the fold; path rule irrelevant | F1 |
| **PS-F4** | `shell "rm -rf build"` with L2 unavailable | L1 `Confirm` | `Ask` before the fold; Session cannot suppress | F1/F2 |
| **PS-F5** | `shell "python3 -c '...'"` with `path:"**"` allow | interpreter payload -> `Unknown` | no path match -> `Ask` (fail-safe) | F1 |
| **PS-F6** | `shell "make"` runs a script that writes outside | no path token -> `InRoot`; L2 is the boundary | path rule may `Allow`; outside write is blocked by L2 when enforced; without L2 this is the 89 SG-F22 residual (sec 15) | F1 |
| **PS-F7** | symlink in root points outside, `read_file` through it | lexical `InRoot`; execution resolves the final component | policy may `Allow`; `resolve()` throws `PathEscape`; L2 mediates shell | F1 |
| **PS-F8** | `permissions.rules` entry with `path` but no `tool` | config load | `ConfigError`; never a silently ineffective rule | F6 |
| **PS-F9** | `permissions.rules` entry with `path:"../x"` or `"/abs"` | config load | `ConfigError` (root-relative only) | F6 |
| **PS-F10** | outside `read_file` with only `"default":"ask"` (no `"read":"ask"`) | builtin read rule still `Allow` | spec documents the gotcha; the recommended config sets `read:"ask"`; without it the verdict is `Allow` then `PathEscape` | F6/F1 |
| **PS-F11** | `"default":"allow"` or `danger-full-access` used for "in-root only" | master switch makes builtins/fallback `Allow` | outside can be allowed (except guard); spec says this does not meet the user's goal | F2/F6 |
| **PS-F12** | Session answer on `git status`, then `git diff` | old exact-command grant | new `InRoot` prefix grant for `git` matches; no second prompt | F1 |
| **PS-F13** | Session answer on `cat /etc/passwd`, then `cat /etc/shadow` | `Outside` exact grant | still `Ask` (exact outside only); no broad outside grant | F1 |
| **PS-F14** | Session answer on an unprovable interpreter payload | `Unknown` exact grant | covers only the identical raw command; a different payload asks | F1 |
| **PS-F15** | daemon restart after a Session answer | in-memory store gone | re-ask; do not claim durability (46-I6) | F10 |
| **PS-F16** | workspace `config.jsonc` defines `rules` | shipped `apply_permissions` clears the array per layer | a global auto-approve rule is replaced by the workspace array; the effective default (ask) applies; documented as OQ-90-2, not changed by this errata | F6 |
| **PS-F17** | `terminal` open with no command | empty shell text; no declared cwd | class `Unknown`; path rule does not match; default `Ask`. With a declared `cwd`, the cwd class wins: `{cwd:".ymh"}` -> protected/`Deny`; `{cwd:"/tmp"}` -> `Outside`/`Ask` (NEW-3/NEW-8). | F1 |
| **PS-F18** | `terminal write` with `input="rm -rf ~"` | `shell_guard_text` returns the input; L1 `Forbid` | `Deny`; no path or Session bypass | F1 |
| **PS-F19** | `write_file .ymh/config.jsonc` with `{"permissions":{"default":"allow"}}` (H1) | protected path; canonical re-check | `Deny` before the fold; no file write; no next-start master switch | F1 |
| **PS-F20** | `.ymh/permissions.jsonc` contains a `tool:"*"` grant (H1) | direct write denied; loader rejects wildcard `tool` and empty shell/terminal `command` | `Deny` on the write; file loads `{}`/re-asks on restart; no policy-wide grant | F1/F6 |
| **PS-F21** | `write_file .git/hooks/pre-commit` then `shell "chmod +x .git/hooks/pre-commit"` (H1) | protected path (path tool and shell explicit target) | `Deny` for both; no code execution on the next git command | F1 |
| **PS-F22** | cloned repo has `secret -> /home/user/.ssh/id_rsa`; `grep {"pattern":".","path":"."}` (H2) | shipped `grep` follows symlinks | required: `grep` skips symlink entries and opens candidates without following symlinks; no outside content in the result/event log; regression test | F1 |
| **PS-F23** | terminal PTY `cd /tmp`, then `terminal write "cat secret"` (M4) | no per-PTY cwd; `terminal write` scope is forced `Unknown` | path rule does not match; `Ask` (Session may remember the exact input chunk only) | F1 |
| **PS-F24** | workspace is a subdirectory of a larger repo; `git_status` (M7) | libgit2 ancestor discovery | required: `GIT_REPOSITORY_OPEN_NO_SEARCH`; return "not a repository", never read the ancestor `.git` | F1 |
| **PS-F25** | `git --git-dir=/tmp/repo log`, `cp -t/tmp x`, `tar --directory=/tmp ...` (M8) | attached/inline option values | split `--opt=value`/known attached short options; classify the value; ambiguous -> `Unknown` (asks) | F1 |
| **PS-F26** | Session-approved `git status`, then `git status && rm -rf src` (M10) | compound command; no simple-command prefix | `command_prefix` empty; exact fallback only; no `git` prefix match; guard still applies to `rm` | F1 |
| **PS-F27** | `awk 'BEGIN{while((getline l < "/etc/passwd")>0) print l}'` (M11) | quoted interpreter/`-exec`/`xargs` payload | `Unknown` (asks), never `InRoot` auto-allow | F1 |
| **PS-F28** | `path:"src/[ab].cpp"` or `path:"src/./x"` (M1/L7) | no character classes; no `.` segment | `ConfigError` at load; no silently ineffective rule | F6 |
| **PS-F29** | `Always` on a path tool under `SandboxMode::Unrestricted` (M9) | loaded path-tool grant would be `Any` | loads/remembers as `InRoot`; cannot match an outside path; no outside allow | F1 |
| **PS-F30** | outside/unknown `terminal write` approved for Session (M3) | `command_pattern` does not read `input` | exact `shell_guard_text` (input chunk) grant with `target_class=Unknown`; no Once-only hole, no prefix | F1 |
| **PS-F31** | permission dialog summary after `request.path` becomes absolute (L6) | `bounded_summary` prints `request.path` | summary is root-relative (or the tool name at root); no absolute host path in the UI/event log | F6 |
| **PS-F32** | a cloned workspace `.ymh/config.jsonc` sets `default:"allow"` (M5/N1) | workspace `permissions` table global-layer-only | `ConfigError` at load (90-D18); a written file cannot set `master=true`; recommended fragment is global-layer | F6 |
| **PS-F33** | `grep`/`glob` over the root would traverse `.ymh`/`.git` (M12) | no protected traversal exclusion in the shipped tools | skip `.ymh/`/`.git/` entries; base `resolve()` remains | F1 |
| **PS-F34** | Session-approved `git status`, then `sudo git status` (L8) | wrapper stripping in `command_prefix` | prefix is shared (documented); target class and guard still apply | F1 |
| **PS-F35** | `shell "git config --local core.hooksPath .evil"` (N1) | shell git-control classifier (sec 4.4.1); no explicit `.git` operand | `protected_target=true` -> `Deny`; no `.git/config` write | F1 |
| **PS-F36** | `shell "git clean -fdx"` (N1) | shell git-control classifier | `protected_target=true` -> `Deny`; `.ymh` state is not deleted | F1 |
| **PS-F37** | `shell "git config --global core.hooksPath .evil"` (N1) | `--global` writes outside root | `Outside` -> `Ask`; never auto-allowed; `--local`/no-scope is `Deny` | F1 |
| **PS-F38** | workspace `.ymh/config.jsonc` contains `permissions.default:"allow"` (N1) | workspace `permissions` table global-layer-only | `ConfigError` at load; no `master=true` on the next start, even if the file was written | F6 |
| **PS-F39** | nested `src/.git/hooks/x` or `src/.ymh/config.jsonc` (N3) | any-component protected check | `Deny`; nested repos/state are not auto-approved | F1 |
| **PS-F40** | `.work -> .ymh` symlink, `write_file .work/config.jsonc` under `SandboxMode::Unrestricted` (N2) | tool-level independent canonicalization | `Deny`; the `resolve()` early return at `environment.cpp:72-74` does not skip the tool check | F1 |
| **PS-F41** | shell `git` in a workspace subdirectory of a larger repo (N4) | system git searches ancestors | recorded residual: ancestor read is allowed by L2's `/` read-only; ancestor write is L2-blocked under Workspace, not if L2 unavailable; not claimed closed | F1 |
| **PS-F42** | protected destructive command with L2 off (`rm -rf .ymh`) under the recommended config (N5) | guard `Confirm` before the protected check | `Ask` (force-asked), not `Allow`; `remember` returns `{}` | F1/F2 |
| **PS-F43** | `shell "curl file:///etc/passwd"` / `file://localhost/...` / `%70`-encoded (N12) | URI parser in the scope pass | `Outside` -> `Ask` under the recommended rule; never `InRoot`/`Allow`; `https://` stays neutral; `file://otherhost` -> `Unknown` | F1 |
| **PS-F44** | `shell "git stash -a"` / `git stash -u` (N13) | git-control classifier: stash-with-untracked/ignored is protected | `Deny`; plain `git stash`/`pop`/`apply`/`drop`/`clear` remain data plane | F1 |
| **PS-F45** | `shell "git remote add evil file:///tmp/x"`, `git remote set-url`, `git branch --set-upstream-to`/`--track`/`--unset-upstream`/`--edit-description`, `git checkout --track`/`-b --track`, `git switch -c --track`, `git push -u` (N14/N22) | git-control classifier: these write `branch.*`/`remote.*` config | `Deny`; `git remote -v`/`show`/`get-url` and `git branch` list are read-only | F1 |
| **PS-F46** | a durable grant `{"tool":"shell","command":"*","match":"glob"}` (N16) | grants loader match-all check | loader rejects (`{}`/re-ask); no persistent allow-all shell grant | F6 |
| **PS-F47** | workspace root itself under `.ymh`/`.git` (N17) | root-relative `path_is_protected` | stated caveat: the root is the trust boundary and is not protected by the relative walk; test `PathTarget.ProtectedRootRelativeCaveat` | F1 |
| **PS-F48** | `shell "curl -d @/etc/passwd"`, `--data=@/etc/passwd`, `-F f=@/etc/passwd`, `-F f=</etc/passwd`, `@../../etc/passwd` (N18) | best-effort curl operand parser | outside -> `Outside`/`Ask`; `.ymh`/`.git` -> `protected`; `@-` neutral; hidden syntax remains NotClaimed (planned follow-up OQ-90-9) | F1 |
| **PS-F49** | `shell "rm -rf .ymh* .git*"`, `.ym?`, `src/.ymh*` (N19) | glob-aware protected scan | `protected_target` -> `Deny` (or guard `Ask` first); a bare `*` is not protected; outside `.ymh`/`.git` -> `Outside`/Ask; `.ymhfoo` literal remains unprotected | F1 |
| **PS-F50** | `terminal open {"action":"open","cwd":"/tmp","command":"cat x"}` under Workspace and Unrestricted (HIGH-1) | `classify_shell_request` effective-cwd classification | `Outside` -> `Ask` (never auto-approved); under Workspace the later `resolve()` may also `PathEscape`; no outside read under Unrestricted without a prompt. **Rev 9:** `cwd:"/tmp"` is the bare root, not exempt; `cat x` resolves to `/tmp/x` (temp), so the command target is temp but the outside `/tmp` cwd keeps the request `Ask` (PS30). | F1 |
| **PS-F51** | `shell "touch /tmp/x"` / `rm /tmp/x` with L2 off (Rev 9; direct-write-only Rev 16) | `tmp_exempt`: every visible target a concrete strict `/tmp` descendant and guard verdict attributable; direct non-recursive | `Allow` before L1; the destructive-outside `Forbid` attributable to `/tmp/x` is overridden (PS29). `rm -rf /tmp/x` is recursive -> not exempt, so the outside-root `Forbid` stands -> `Deny` (PS-F66) | F1 |
| **PS-F52** | `shell "rm -rf /tmp"` / `rm -rf /tmp/` / `/tmp/..` | bare root or `..` escape after normalization; `rm` with an outside-root operand | not exempt; the L1 `Forbid` -> `Deny` (outside-root/bare-root destructive form), never an unconditional `Allow` (PS30) | F1 |
| **PS-F53** | `shell "rm -rf /tmp/*"`, `/tmp/x*`, `/tmp/?.txt`, `/tmp/{x,/etc/passwd}`, `$TMPDIR/x` | glob, brace, or unexpanded target | not exempt; `rm -rf /tmp/*` is an outside-root `Forbid` -> `Deny`; the non-`rm` forms keep the normal guard/policy verdict (`Ask` unless the guard denies); no expansion-based allow (PS30) | F1 |
| **PS-F54** | `shell "cp /etc/passwd /tmp/x"`, `rm -rf /tmp/x ~/y`, or `sh -c 'touch /tmp/x; rm src/y'` | mixed visible target set | not exempt; `cp`/`rm` with an outside destination/operand hit the L1 `Forbid` -> `Deny`; a mixed command with no guard `Forbid` keeps normal policy (`Ask` under the recommended config) (PS30) | F1 |
| **PS-F55** | `shell "touch /tmp/x"` with `default:"deny"`, `shell:"deny"`, `ReadOnly`, or `force_ask=true` | final-verdict exemption | `Allow`; config/sandbox/force-ask cannot disable it (PS29) | F2/F6 |
| **PS-F56** | `shell "touch /tmp/link/passwd"` where `/tmp/link -> /etc`, `/tmp/link -> <workspace>/.git/hooks`, or `/tmp/link -> <missing>` | lexical `/tmp`, canonical escape/protected/workspace, or dangling/unresolvable | `request_is_tmp_exempt` inspects canonical/symlink status: outside `/tmp`, inside the workspace/protected tree, or dangling/unresolvable -> **not exempt**; the policy rejection is required because under `Workspace` the L2 root grant would allow a resolved workspace target (`ReadOnly` denies it) (PS29/PS32/risk 21) | F1 |
| **PS-F57** | workspace root `/tmp/ws`, `shell "write /tmp/ws/.ymh/config.jsonc"` or any `/tmp` write | workspace at/under `/tmp` disables the exemption (workspace-overlap check) | normal policy (protected `Deny` for `.ymh`), never `Allow` by the exemption (PS28/PS32) | F1/F6 |
| **PS-F58** | `write_file /tmp/x` or `read_file /tmp/x` | path tools are not exempt (Rev 12) | normal policy; under `Workspace`/`ReadOnly` `resolve()` throws, under `Unrestricted` `LocalFilesystem` throws; use `shell` (PS31) | F1 |
| **PS-F59** | L2 `Unavailable` or `Unrestricted` (`NotApplicable`) | no kernel `/tmp` grant | the policy still `Allow`s a fully-`/tmp` write; with no kernel temp grant (or with the `Workspace` root RW grant covering a workspace target), a symlink swapped in after `request_is_tmp_exempt` canonicalized `tmp_targets` (TOCTOU) is the recorded residual (PS32/risk 21) | F1 |
| **PS-F60** | `terminal write "touch /tmp/x\n"` / `terminal write "cat /tmp/x\n"` / `terminal write "ls\n"` | input classified for `tmp_exempt`; write-only; no target for `ls` | `touch /tmp/x` is `Allow`; `cat /tmp/x` is a read and `ls` has no filesystem target: not exempt (PS29/PS30) | F1 |
| **PS-F61** | `$TMPDIR` set to `/var/tmp` or `/etc`; `shell "touch $TMPDIR/x"` | `$TMPDIR` is not admitted; unexpanded variable target | `/var/tmp` is not a temp root; the literal `$TMPDIR/x` is unexpanded -> not exempt (PS28/PS30) | F1/F6 |
| **PS-F62** | `shell "touch /tmp/x; :(){ :\|:& };:"` or `touch /tmp/x; mkfs /tmp/y` | one `/tmp` target, but guard `Forbid` is untargeted/unconditional | not exempt; `Deny` (the unrelated `Forbid` is never laundered by the `/tmp` operand) (PS30) | F1 |
| **PS-F63** | `terminal open {cwd:"/etc", command:"touch /tmp/x"}` or `{cwd:<root>, ...}` | cwd is a mandatory temp target | not exempt; normal policy/`Ask`; the tool also rejects the non-temp cwd (PS27/PS30) | F1 |
| **PS-F64** | `shell "cat /tmp/x"`, `cp /tmp/a /tmp/b`, `dd if=/tmp/a of=/tmp/b`, `install /tmp/a /tmp/b`, or `cat /tmp/x > /tmp/y` | recognized read or mixed read+write (source operands are reads); `tmp_exempt` requires zero reads | not exempt; normal shell/default/guard policy (`permissions.read` gates path tools, not shell) (PS29/PS30/MEDIUM-1/LOW-3) | F1 |
| **PS-F65** | `touch /tmp/x 2>/dev/null` | `/dev/null` is a non-temp visible target | not exempt; normal verdict (conservative; device whitelist is L1-only) (PS30/LOW-8) | F1 |
| **PS-F66** | `rm -rf /tmp/d`, `cp -a /tmp/a /tmp/b`, `chmod -R /tmp/d`, `find -L /tmp/d -delete`, `rsync /tmp/a /tmp/b`, `install -D /tmp/a /tmp/b` (Rev 16) | recursive/following form; target is a directory tree, not a direct regular-file operand | not exempt; `rm -rf`, `cp -a`, `chmod -R`, and `find -L -delete` are guard-recognized outside-root destructive forms -> L1 `Forbid` -> `Deny`; `rsync` and `install -D` are not guard-recognized -> normal `Ask`/policy; a child symlink into the workspace/protected tree is never followed by the exemption (MEDIUM-1/PS30) | F1 |
| **PS-F67** | `rm /tmp/d` or `mv /tmp/d /tmp/e` where `/tmp/d` is an existing directory | canonical target is an existing directory; the `rm`/`mv` operand/destination is outside the workspace | not exempt; the L1 `Forbid` -> `Deny` (direct regular-file/new-path only) (PS29/PS30) | F1 |

## 12. dsh (DeepSeek Harness) mapping

| dsh surface | ymh mirror | Justification / anchor |
|---|---|---|
| Per-call approval decision (allow/ask/deny) | `PermissionPolicy::evaluate` with the spec-89 guard as its first branch | Mirror refinement: the same approval decision, one level deeper per command and now per target class. Anchor: `docs/design/09-permissions.md:443-495`, `src/policy/permission_policy.cpp:416-520`. |
| Sandbox scope (host-plane confinement) | `SandboxMode` + `ExecutionEnvironment::resolve()`; the path rule is a **policy matching dimension**, not a second sandbox | Mirror: the sandbox remains 07/89; path matching only decides whether to ask. Anchor: `src/execution/environment.cpp:64-95`, `docs/design/89-shell-command-safety-guard-errata.md:131-157`. |
| Protected daemon/VCS state (`.ymh/**`, `.git/**`, any depth) | **ymh-specific policy hard deny** plus tool-level independent canonical re-check, `grep`/`glob` traversal exclusions, shell git control-plane classification, and global-only workspace `permissions` (non-mirror) | Deliberate divergence: dsh's approval model does not store the agent's own policy/state in the workspace, while ymh's workspace config, grants, sessions, checkpoints, and `.git` are in-root. Auto-approving in-root must not include them. The deny is before the fold and cannot be weakened by a grant; the workspace config cannot carry a permission table, so a written config is not an escalation. Anchor: `src/config/grant_store.cpp:84-95`, `src/execution/filesystem.cpp:236-268`, `src/policy/shell_guard.cpp:1445-1680`, 90-D13/90-D14/90-D17/90-D18. |
| Approval request / outcome / scope | Core `PermissionRequest` / `PermissionDecisionKind` / `GrantScope{Once,Session,Always}` | Mirror: unchanged. Anchor: `docs/design/09-permissions.md:1034-1056`, `include/ymh/transport/protocol.hpp:277-295`. |
| Root-relative path rule in the approval policy | **ymh-only extension** (non-mirror): dsh's approval policy is tool/command-oriented; ymh adds `permissions.rules[].path` | Deliberate divergence: the user requirement is path-scoped, and the 09 fold already defines a path dimension (`docs/design/09-permissions.md:454-458`); this errata makes it reachable. The extension is config-only and cannot weaken the guard. Anchor: `docs/design/46-permissions-ui-errata.md:429-439` (the O-H1 shape this supersedes), 89 SG10. |
| Class-scoped Session approval | **ymh refinement** (non-mirror): dsh approvals are per-call/policy; ymh's Session is an in-memory class grant (`tool` + `TargetClass` + prefix/exact) | Deliberate divergence: the shipped exact-string Session was unusable for shell (90-C5); the class is still in-memory, per-session, and below the guard. Anchor: `src/policy/permission_policy.cpp:535-657` (the exact grant this replaces), `docs/design/46-permissions-ui-errata.md:3467` (46-I6). |
| Shell command classification before approval | `classify_shell_command` (89 L1) plus the additive `ShellTargetScope` | Mirror plus a matching-only scope: dsh has no path-scope classifier; ymh reuses the existing tokenizer and keeps L1/L2 authoritative. Anchor: `docs/design/89-shell-command-safety-guard-errata.md:853-875` (SG1-SG17), `src/policy/shell_guard.cpp:1945`/`:2036`. |
| Temp-root shell/terminal exemption (Rev 9; direct-write-only in Rev 16) | **ymh-only non-mirror**: a **direct, non-recursive, regular-file/new-path write/delete/create** `shell`/`terminal` invocation whose every visible filesystem target (read and write operands) is a concrete strict descendant of canonical `/tmp` is `Allow` | Deliberate user scope decision: "writes to /tmp shall always be allowed regardless of all the other permission settings". dsh's per-call approval has no `/tmp` dimension, so there is nothing to mirror. The divergence is bounded to **direct, non-recursive** write/delete/create shell/terminal invocations with concrete strict `/tmp` regular-file/new-path targets; reads, recursive/following forms, directory targets other than the declared terminal-open cwd, and path tools stay normal/root-confined, and bare roots, internal `..`, globs, braces, unexpanded targets, mixed/read commands, untargeted `Forbid` constructs, and a workspace at/under `/tmp` keep the normal verdict (90-D23/PS30). `$TMPDIR` is not admitted. Anchor: decision 90-D23; `docs/design/09-permissions.md:443-495`; `src/policy/shell_guard.cpp:2036-2116`. |
| MCP tool approval | No path dimension; MCP tools fall through to their configured tool default | **Architectural absence, not a gap**: MCP tool schemas are external and the request has no path producer, so a path rule cannot match. The seam is `request.path == nullopt`; the missing consumer is any MCP path argument. Anchor: `src/agent/agent_loop.cpp:871-889`, `src/cli/wiring.cpp:163-173`. |
| Durable always-allow | Tool + command only; no path | Retained non-mirror of a finer durable path grant: the durable store is a workspace-scoped JSONC file with a frozen schema; extending it needs a schema bump, and the user's ask is met by config rules plus Session. Anchor: `docs/design/46-permissions-ui-errata.md:3466` (46-I5), `:3494` (46-I33), OQ-90-1. |

All non-mirror cells carry a concrete anchor per 56-D6
(`docs/design/56-dsh-fidelity-gap-closure-errata.md` sec 3;
`AGENTS.md` "Fidelity divergences carry a reason").

## 13. State-lifetime table

| State | Type / where | Created | Destroyed / cleared | Owner | Survives process restart? |
|---|---|---|---|---|---|
| `PermissionRuleSettings::path` | `std::optional<std::string>` in `Config` | config parse (`apply_permissions`) | with `Config` destruction / next load | `Config` owner (CLI/daemon startup) | no (re-read from `config.jsonc`) |
| `PolicyRule::path` | `std::string` in `RulePermissionPolicy::base_rules_` | `to_permission_config` | with the policy / daemon | `RulePermissionPolicy` | no (rebuilt from `Config`) |
| `PolicyRule::target_class` / `prefix` | enum/bool on a rule | config rules `Any/false`; Session grants at `remember`; `Always` path tools `InRoot`; `Always` shell/terminal `Any` | with the rule (base or session vector) | `RulePermissionPolicy` | `Always` survives via the grants file; a path-tool `Always` reloads as `InRoot`; Session no |
| `PermissionRequest::path` | `std::optional<std::filesystem::path>` | per call, `AgentLoop` request build (`src/agent/agent_loop.cpp:885`) | at call end | call frame -> `evaluate` / `broker` | no |
| target class | `TargetClass`, local in `evaluate` | per call | at call return | call frame | no |
| root-relative pattern | `std::string`, local in `rule_matches` | per call | at call return | call frame | no |
| `ShellClassification` / `ShellTargetScope` / `protected_target` / `tmp_exempt` | value, local in `evaluate` and `remember` | per call (one classifier call) | at call return | call frame | no |
| shell git subcommand class | per-call classification (sec 4.4.1) | per call | at call return | call frame | no |
| protected-path decision | `bool`, local from `path_is_protected`/`protected_target` | per call | at call return | call frame | no |
| tool-level canonical path | `std::filesystem::path`, local in read/write/edit/grep/glob | per call | at call end | tool call frame | no |
| global `permissions` table | `PermissionDefaults` in `Config` | global `config.jsonc` parse | with `Config` | CLI/daemon config owner | no (re-read) |
| `Config::agent.sandbox` | `std::string` in `AgentSettings` | global `config.jsonc` parse | with `Config` | CLI/daemon config owner | no (re-read) |
| workspace `permissions` table / `agent.sandbox` | none: rejected by `apply_permissions`/`apply_agent` | n/a | n/a | n/a | n/a (ConfigError) |
| Session grant | `PolicyRule` in `session_grants_[session]` | human `Session` decision -> `remember` | daemon exit; per-session map lifetime | `RulePermissionPolicy` (daemon) | no (46-I6) |
| Always grant | `PolicyRule` in `local_grants_` + `permissions.jsonc` | human `Always` decision -> `remember` -> `GrantStore::append` | workspace deletion / explicit file edit | `RulePermissionPolicy` + `GrantStore` | yes, per workspace; file shape unchanged, load-time `InRoot` derivation applies |
| Session grant id | `std::string` on the grant | `remember` | with the grant | `RulePermissionPolicy` | no |
| config sample comment | static text in `src/config/config.cpp` | build | n/a | n/a | n/a |
| canonical `/tmp` path | `std::filesystem::path`, function-local static in `canonical_tmp_root()` | first call (startup/policy construction) | process exit | process-wide cache | no (recomputed) |
| `ConfinementPolicy::tmp_root` / `SpawnConfinement` | `std::optional<std::filesystem::path>` (`nullopt` for empty/overlap), copied into the per-spawn policy | each `SpawnConfinement` construction (shell/terminal call) | at spawn end / `SpawnConfinement` destruction | the tool call frame | no |
| `ShellClassification::tmp_exempt` / `tmp_targets` | `bool` + `std::vector<std::filesystem::path>`, per-call classification | per call | at call return | `evaluate`/`remember` call frame | no |
| temp-root exemption verdict | `bool`/`Allow` branch, local in `evaluate` | per call | at call return | call frame | no |

No new cache, timer, background thread, or durable artifact is introduced. The
only new state is the process-wide cached canonical `/tmp` path, the per-call
`tmp_exempt` flag, and `tmp_targets`; no `$TMPDIR`/XDG/protected-root/base state exists.
A daemon restart recomputes `/tmp`. The only durable state remains the existing
`Always` grants file; its **file shape** is unchanged, but its loader now
rejects wildcard/empty-command grants and derives `InRoot` for path-tool grants
(sec 4.6.1). No temp-root setting is durable or configurable. Path tools and
`LocalFilesystem` hold no temp state (Rev 12).

## 14. Test plan (hermetic)

All tests are hermetic; no live LLM. Use the existing `TempWorkspace`
(`tests/support/test_env.hpp`), `AgentTestEnv` (`tests/support/agent_test_env.hpp`),
`FakeLLM` (spec 45), and the fake permission transport
(`tests/unit/permission_broker_test.cpp:26`). The spec-89 tests must stay green
unchanged.

### 14.1 New unit tests

**`tests/unit/errata90_path_target_test.cpp`**

- `PathTarget.AllSevenToolsAreMapped`: `read_file`, `write_file`, `edit_file`,
  `grep`, `glob`, `git_status`, `git_diff` return the expected absolute lexical
  target; a non-path tool returns `nullopt`.
- `PathTarget.RelativeIsJoinedAndNormalized`: `src/../src/x` -> `root/src/x`.
- `PathTarget.DotDotEscapesToAbsolute`: `../x` -> an absolute path outside the
  root (not nullopt; the class is `Outside`).
- `PathTarget.AbsoluteOutsideIsKept`: `/etc/passwd` -> `/etc/passwd`.
- `PathTarget.MissingRequiredIsNullopt`: `read_file` with no `path`, a
  non-string, and an empty string.
- `PathTarget.OptionalDefaultsToRoot`: `grep`/`glob`/`git_diff` with no
  `path` -> the root; `git_status` always -> the root, including when an
  undocumented `path` is present (N9).
- `PathTarget.EmptyOptionalIsNullopt`: `grep` with `path:""`; a non-string
  optional `path` -> `Unknown`/`Ask` (N6).
- `PathTarget.TildeIsLiteralForToolPaths`: `read_file "~"` -> `root/~`
  (in-root), not `$HOME`.
- `PathTarget.NoIO`: a non-existent path returns a normalized path and does not
  throw or create anything.
- `PathTarget.ProtectedDetection`: `.ymh`, `.ymh/config.jsonc`, `.git`,
  `.git/hooks/x`, `src/.git/hooks/x`, `src/.ymh/config.jsonc` are protected;
  `.ymhfoo` and `.gitignore` are not (N3: any component, any depth).
- `PathTarget.ProtectedOutsideNotProtected`: an outside path is not protected
  by the lexical check (it is `Outside`, handled by the path rule/default).
- `PathTarget.ProtectedRootRelativeCaveat` (N17): with root
  `/srv/.ymh/project`, the target `/srv/.ymh/project/x` has relative `x` and
  is not protected by the root-relative walk; the root is the trust boundary.

**`tests/unit/errata90_protected_path_test.cpp`** (H1)

- `Protected.ConfigWriteDenied`: `write_file .ymh/config.jsonc` -> `Deny` with
  the recommended allow rule; `remember` returns `{}`.
- `Protected.GrantWriteDenied`: `write_file .ymh/permissions.jsonc` -> `Deny`;
  a pre-existing file with `tool:"*"` is rejected by the loader.
- `Protected.GitHookDenied`: `write_file .git/hooks/pre-commit` -> `Deny`;
  `shell "chmod +x .git/hooks/pre-commit"` -> `Deny`.
- `Protected.SessionWriteDenied`: `read_file .ymh/sessions.db` -> `Deny`
  (including `default:"allow"` and a matching Session/Always grant).
- `Protected.CanonicalSymlinkAlias`: a `.work -> .ymh` symlink; `write_file
  .work/config.jsonc` is denied by the tool-level independent canonical check
  under both `SandboxMode::Workspace` and `SandboxMode::Unrestricted` (N2).
- `Protected.PolicyBeatsDefaultAllow`: `default:"allow"` + `danger-full-access`
  still denies `.ymh`/`.git` for the path tools and explicit shell targets.
- `Protected.GrepGlobSkip`: `grep`/`glob` over the root do not traverse
  `.ymh/`/`.git/` (with 90-A13).
- `Protected.NestedGitDenied`: `src/.git/hooks/x`, `src/.ymh/config.jsonc` ->
  `Deny` (N3: any component, any depth).
- `Protected.GitWildcardDenied` (N19): `shell "rm -rf .ymh* .git*"`,
  `rm -rf .ym?`, `rm -rf src/.ymh*` -> `Deny`; a bare `rm -rf *` is guard
  `Confirm`/`Ask`, not protected; an outside `.git` path -> `Outside`/Ask;
  `rm -rf .ymhfoo` literal is not protected.
- `Protected.TerminalOpenCwdProtected` (HIGH-1): `terminal open {cwd:".ymh"}`
  / `{cwd:".git"}` -> `Deny`; outside `cwd` -> `Outside`/Ask under both
  sandbox modes; a `cwd` symlink alias to outside is denied by the terminal
  tool's independent canonical check under `Unrestricted` too.
- `Protected.TerminalOpenCwdToolRejectsOutside` (HIGH-1/NEW-5): with a policy
  forced to `Allow` (for example `default:"allow"` or a `path:"**"` allow
  rule) and `SandboxMode::Unrestricted`, calling the `terminal` tool open with
  `cwd:"/tmp"` still throws `ToolError{PathEscape}` from the tool-level
  canonical check; the plain outside cwd is tested directly, not only through
  the symlink alias.
- `Protected.GitConfigLocalDenied`: `shell "git config --local core.hooksPath
  .evil"` -> `Deny` (shell git-control classifier, sec 4.4.1).
- `Protected.GitCleanDenied`: `shell "git clean -fdx"` -> `Deny` (it can delete
  `.ymh`).
- `Protected.GitGlobalConfigAsks`: `shell "git config --global core.hooksPath
  .evil"` -> `Ask`/`Outside`, never auto-approved.
- `Protected.GitDataPlaneAllowed`: `shell "git add ."` / `git commit` with an
  in-root command are normal scope (not `protected_target`); the documented
  exemption applies.
- `Protected.GitStashAllDenied`: `shell "git stash -a"` and `git stash -u`
  -> `Deny` (N13; they remove untracked/ignored `.ymh`).
- `Protected.GitStashClusteredDenied` (N20): `git stash -au`, `-ua`, `-uab`,
  and any short-option cluster containing `a`/`u` -> `Deny`.
- `Protected.GitStashPlainAllowed`: `shell "git stash"` (tracked changes only)
  / `git stash pop` -> normal scope, not `protected_target`.
- `Protected.GitRemoteAddDenied`: `shell "git remote add evil file:///tmp/x"`,
  `git remote set-url origin ...` -> `Deny` (writes `.git/config`; N14).
- `Protected.GitBranchUpstreamDenied`: `shell "git branch --set-upstream-to=origin/main"`,
  `git branch --track br2 origin/master`, `git checkout --track origin/master`,
  `git checkout -b x --track`, `git switch -c sw --track origin/master`,
  `git branch --unset-upstream br2`, `git branch --edit-description`,
  `git push -u origin main` -> `Deny` (all write `branch.*`/`remote.*`
  config; N14/N22).
- `Protected.GitAutoTrackDenied` (HIGH-2/NEW-1): `git checkout -b feat
  origin/main`, `git checkout -B feat2 origin/main`, `git branch feat3
  origin/main`, `git switch -c feat4 origin/main`, `git switch -C feat5
  origin/main`, `git switch -t origin/main`, `git switch --track origin/main`,
  `git branch -m old new`, `git branch -M old new`, `git branch -c main copyb`,
  `git branch -C main copyc`, and no-start creation `git checkout -b z1` /
  `git branch z2` / `git switch -c z3` -> `Deny` (default or invisible global
  `branch.autoSetupMerge` writes `.git/config`); `git -c
  branch.autoSetupMerge=always checkout -b z4` is also `Deny` (the `-c` prefix
  is ignored for the class); creation/start `--no-track` variants -> data
  plane; `branch -m/-M/-c/-C` remain protected even with `--no-track`.
- `Protected.GitReadOnlyFormsAllowed` (LOW-8): `git remote -v`, `git remote
  prune`, `git submodule status`, `git worktree list` -> normal scope, not
  `protected_target`.
- `Protected.GitFetchSetUpstreamDenied` (NEW-6): `git fetch --set-upstream
  origin main`, `git pull --set-upstream origin main` -> `Deny` (writes
  `branch.*` config); `git fetch -u origin main` / `git pull -u` -> data plane
  (`-u` is `--update-head-ok`, not `--set-upstream`).
- `Protected.GitLongSynonymsDenied` (NEW-7): `git branch --move old new`,
  `git branch --copy main copyb`, `git switch --create=bar origin/main`,
  `git switch --force-create=baz origin/main`, attached `git checkout -bfoo`,
  `-Bfoo`, `git switch -cfoo`, `-Cfoo`, `git branch -uorigin/main`,
  `-torigin/x`, and `git branch -d/-D/--delete feat` -> `Deny`.
- `Protected.WorkspacePermissionsRejected`: a workspace
  `.ymh/config.jsonc` with `permissions.default`/`read`/`write`/`shell`/
  `rules` or `agent.sandbox` -> `ConfigError` at load (90-D18/90-A19), so a
  written file cannot set `master=true` or widen the sandbox on the next
  start.
- `Protected.WorkspaceConfigPersistenceEscalation`: write a workspace
  `.ymh/config.jsonc` containing `permissions.default:"allow"` directly to
  disk (simulating a successful shell vector), then `load_config` -> throw
  `ConfigError`; assert no `master=true`. This is the concrete H1 persistence
  regression even when the direct write guard is bypassed.

**`tests/unit/errata90_grant_loader_test.cpp`** (H1/M9/N16)

- `GrantLoader.RejectsWildcardTool`: `{"tool":"*"}` -> `{}` (fail-safe skip).
- `GrantLoader.RejectsEmptyShellCommand`: `{"tool":"shell"}` and
  `{"tool":"terminal"}` -> `{}`.
- `GrantLoader.RejectsUnboundedShellCommand` (N16):
  `{"tool":"shell","command":"*","match":"glob"}`,
  `{"tool":"terminal","command":"?*"}` -> `{}`; `{"tool":"shell","command":
  "git *","match":"glob"}` is accepted (has literal characters).
- `GrantLoader.RejectsPathToolCommand` (LOW-9):
  `{"tool":"write_file","command":"src/*","match":"glob"}` -> `{}` (path tools
  have no command dimension; the grant would be dead).
- `GrantLoader.PathToolLoadsInRoot`: `{"tool":"write_file"}` loads with
  `target_class=InRoot`; an outside path does not match.
- `GrantLoader.NonPathToolRemainsToolWide`: an MCP tool grant with no command
  remains `Any`.
- `GrantLoader.FileShapeUnchanged`: the JSON keys are still
  `tool`/`command`/`match`/`effect`/`id`; no `path` key.

**`tests/unit/errata90_path_rules_test.cpp`**

- `PathRule.InRootMatches`: `write_file` at `root/src/x` with
  `{tool:"write_file",path:"src/**",effect:"allow"}` -> `Allow`.
- `PathRule.OutsideDotDotDoesNotMatch`: `write_file` at `root/../x` -> `Ask`
  (with `default:"ask"` and `read/write/shell:"ask"`).
- `PathRule.AbsoluteOutsideDoesNotMatch`: `read_file /etc/passwd` -> `Ask`
  (with `read:"ask"`).
- `PathRule.RootMatchesDoubleStar`: `grep` default root with `path:"**"` ->
  `Allow`; `grep` at `src` -> `Allow`.
- `PathRule.NarrowPatternDoesNotMatchShell`: `shell "ls"` with
  `{tool:"shell",path:"src/**",effect:"allow"}` -> `Ask`.
- `PathRule.NonPathToolDoesNotMatch`: an MCP tool with the `**` allow rule ->
  `Ask`; the rule is not dead, it is conditional on the path dimension.
- `PathRule.MissingPathDoesNotMatch`: `write_file` with no `path` and the `**`
  allow rule -> `Ask`; a path-less request is `Unknown`, not `InRoot`.
- `PathRule.DoubleStarMatchesInRootShell`: `shell "git status"` with
  `{tool:"*",path:"**",effect:"allow"}` -> `Allow` (L2 on).
- `PathRule.OutsideShellDoesNotMatch`: `shell "cat /etc/passwd"` -> `Ask`.
- `PathRule.UnknownShellDoesNotMatch`: `shell "python3 -c 'print(1)'"` ->
  `Ask`.
- `PathRule.GuardForbidStillDeny`: `shell "rm -rf ~"` -> `Deny`.
- `PathRule.GuardConfirmStillAsk`: `shell "rm -rf build"` with
  `kernel_confinement=false` -> `Ask`, even with the path allow rule.
- `PathRule.ForceAskStillAsk`: `force_ask=true` -> `Ask`.
- `PathRule.PathRequiresToolConfigError`: `{path:"**",effect:"allow"}` ->
  `ConfigError`.
- `PathRule.PathDotDotConfigError`: `{tool:"*",path:"../**",...}` ->
  `ConfigError`.
- `PathRule.PathAbsoluteConfigError`: `{tool:"*",path:"/etc/**",...}` ->
  `ConfigError`.
- `PathRule.EmptyPathConfigError`: `path:""` -> `ConfigError`.
- `PathRule.BracketPatternConfigError`: `path:"src/[ab].cpp"` -> `ConfigError`
  (the matcher has no character classes; M1).
- `PathRule.MiddleDotConfigError`: `path:"src/./x"` -> `ConfigError` (L7).
- `PathRule.ProtectedConfigDenied`: with the recommended allow + deny rules,
  `.ymh/config.jsonc` and `.git/hooks/x` -> `Deny` (path tools).
- `PathRule.ProtectedShellDenied` (L2 on): `shell "cat .git/config"`,
  `shell "chmod +x .git/hooks/pre-commit"`, `shell "rm -rf .ymh"` -> `Deny`
  (explicit protected target). With L2 off, a guard-`Confirm` protected
  command is `Ask` (force-asked), not auto-allowed.
- `PathRule.ProtectedBeatsDefaultAllow`: `default:"allow"` +
  `danger-full-access` still denies `.ymh`/`.git`.
- `PathRule.ProtectedBeatsSessionAndAlways`: a matching Session/Always grant
  cannot allow a protected path (`remember` returns `{}`).
- `PathRule.UnknownKeyStillFatal`: `{tool:"*",path:"**",nope:1}` ->
  `ConfigError`.
- `PathRule.UserRequestFragment`: the exact sec 4.7 fragment: in-root
  `write_file`/`shell`/`read_file` -> `Allow`; outside `write_file`/`shell`/
  `read_file` -> `Ask`; `.ymh/config.jsonc` and `.git/config` -> `Deny`;
  `terminal open` **with no command** -> `Ask`; `terminal open` with an
  in-root command (`ls`) -> `Allow`; `terminal write` -> `Ask`; MCP tool ->
  `Ask` (N8).
- `PathRule.ReadDefaultGotcha`: with only `"default":"ask"` plus the rule and
  `read` left at its default, outside `read_file` is `Allow` (documents
  90-C6); with `read:"ask"` it is `Ask`.

**`tests/unit/errata90_shell_scope_test.cpp`**

- `ShellScope.InRootCommand`: `ls`, `git status`, `cargo build` -> `InRoot`.
- `ShellScope.OutsideAbsolute`: `cat /etc/passwd` -> `Outside`.
- `ShellScope.OutsideDotDot`: `cat ../x`, `rm -rf ../x` -> `Outside`.
- `ShellScope.OutsideHome`: `cat ~/.ssh/id_rsa`, `cat $HOME/x` -> `Outside`.
- `ShellScope.NestedShellInRoot`: `bash -c 'ls'` -> `InRoot`.
- `ShellScope.NestedShellOutside`: `bash -c 'cat /etc/passwd'` -> `Outside`.
- `ShellScope.InterpreterPayloadUnknown`: `python3 -c 'print(1)'`,
  `node -e '...'`, `awk 'BEGIN{...}'`, `sed 's/x/y/'`, `find . -exec ...`,
  `xargs ...` -> `Unknown` (M11).
- `ShellScope.DotDotOutside`: a bare `cd ..` -> `Outside` (L2 rename).
- `ShellScope.CdOutsideThenInside`: `cd /tmp && cd "$PWD" && ls` -> `Outside`.
- `ShellScope.TruncatedUnknown`: an unterminated quote / deep nesting ->
  `Unknown`.
- `ShellScope.HostPath`: `scp host:/etc/passwd .` -> `Outside`; `curl
  https://example.com` -> `InRoot` (other URI schemes neutral, OQ-90-4).
- `ShellScope.FileUrlOutside` (N12): `curl file:///etc/passwd`,
  `curl file://localhost/etc/passwd`, `curl file:///etc/%70asswd` ->
  `Outside`/`Ask`; `curl file:///root/ws/x` -> in-root; `file://otherhost/x`
  -> `Unknown`; a decoded `.ymh`/`.git` path sets `protected_target`.
- `ShellScope.HttpsUrlNeutral` (N12): `curl https://example.com`,
  `wget https://host/file` -> neutral/`InRoot` (no local file target);
  `file:` is the only scheme classified as local. (`git remote add ...
  https://...` is separately protected by N14 because it writes
  `.git/config`.)
- `ShellScope.CurlDataFileOutside` (N18): `curl -d @/etc/passwd`,
  `curl --data=@/etc/passwd`, `curl -d @../../etc/passwd` -> `Outside`/`Ask`;
  `curl -d @.ymh/config.jsonc` -> `protected_target`.
- `ShellScope.CurlFormFileOutside` (N18): `curl -F f=@/etc/passwd`,
  `curl -F f=</etc/passwd`, `curl --data-urlencode name@/etc/passwd` ->
  `Outside`; `@-` -> neutral (stdin); malformed -> `Unknown`.
- `ShellScope.ProtectedGlobDenied` (N19/LOW-1/LOW-2): `rm -rf .ymh*`,
  `.git*`, `.ym?`, `.*`, `src/.ymh*` -> `protected_target`/`Deny`; a bare
  `ls *` / `cat *` / `**` is **not** protected (in-root read promise);
  `shell "cat /tmp/repo/.git/config"` -> `Outside`/`Ask` (outside `.git` is
  not protected); `.ymhfoo` literal -> not protected.
- `ShellScope.GitStashAllProtected` (N13/N20): `git stash -a`, `git stash -u`,
  `git stash --include-untracked`, and clustered `-au`/`-ua`/`-uab` ->
  `protected_target`/`Deny`; plain `git stash` -> normal.
- `ShellScope.GitRemoteMutationProtected` (N14/N22/HIGH-2): `git remote add/set-url`,
  `git branch --set-upstream-to`, `git branch --track`, `git branch
  --unset-upstream`, `git branch --edit-description`, `git checkout --track`,
  `git checkout -b --track`, `git switch -c --track`, `git push -u`, and the
  default auto-tracking `git checkout -b feat origin/main` / `git branch feat
  origin/main` / `git switch -c feat origin/main` / `git switch --track
  origin/main` -> `protected_target`/`Deny`; `--no-track` variants -> data
  plane; `git remote -v`/`git branch` list/`remote prune`/`submodule
  status`/`worktree list` -> read-only (LOW-8).
- `ShellScope.GitAutoTrackProtected` (HIGH-2/NEW-1): `git checkout -b/-B <name>
  [start]`, `git branch <new> [start]`, `git switch -c/-C <name> [start]`,
  `git switch -t`/`--track`, `git branch -m/-M/-c/-C`, and `git -c
  branch.autoSetupMerge=always checkout -b z1` -> `protected_target`/`Deny`
  (default/invisible global `branch.autoSetupMerge`); creation/start forms
  with `--no-track` -> data plane; `branch -m/-M/-c/-C` always protected.
  A bare `git checkout main` (existing local branch) -> data plane.
- `ShellScope.GitReadOnlyFormsAllowed` (LOW-8): `git remote -v`, `git remote
  prune`, `git submodule status`, `git worktree list` -> normal scope, not
  `protected_target`.
- `ShellScope.GitFetchSetUpstreamProtected` (NEW-6): `git fetch
  --set-upstream origin main`, `git pull --set-upstream origin main` ->
  `protected_target`/`Deny`; `git fetch -u`/`git pull -u` -> data plane.
- `ShellScope.GitAttachedBranchOptionProtected` (NEW-7): `git branch --move`,
  `--copy`, `git switch --create=...`, `--force-create=...`, `git checkout
  -bfoo`/`-Bfoo`, `git switch -cfoo`/`-Cfoo`, `git branch -uorigin/main`/
  `-torigin/x`, and `git branch -d/-D/--delete` -> `protected_target`/`Deny`.
- `ShellScope.GitConfigReadFormsReadOnly` (N14): `git config --get`,
  `--get-all`, `--get-regexp`, `--list` -> read-only; `git config --worktree`
  -> protected write.
- `ShellScope.EmptyNotApplicable`: empty text -> `NotApplicable`.
- `ShellScope.ConfirmScopeDependsOnConfinement`: `rm -rf build` is
  `None/InRoot` with `kernel_confinement=true` and `Confirm/Unknown` with
  `false` (M2).
- `ShellScope.TerminalWriteAlwaysUnknown`: `terminal write "ls\n"` and
  `terminal write "cd /tmp\n"` -> `Unknown`; `terminal open` with `ls` ->
  `InRoot` (M4).
- `ShellScope.TerminalOpenCwdOutside` (HIGH-1): `terminal open {cwd:"/tmp",
  command:"cat x"}` -> `Outside`/`Ask` under both `Workspace` and
  `Unrestricted`; never auto-approved. `cwd:"../../x"` -> Outside; `cwd` under
  the root -> InRoot and relative operands resolve against that cwd (e.g.
  `cat ../../x` -> Outside).
- `ShellScope.TerminalOpenCwdProtected` (HIGH-1): `cwd:".ymh"` / `cwd:".git"`
  -> `protected_target`/`Deny`; a non-string/empty `cwd` -> `Unknown`/Ask.
- `ShellScope.AttachedOptionOutside`: `git --git-dir=/tmp/repo log`,
  `cp -t/tmp x`, `tar --directory=/tmp ...` -> `Outside`/`Unknown`, never
  `InRoot` (M8).
- `ShellScope.ProtectedTarget`: `cat .git/config`, `chmod +x
  .git/hooks/pre-commit`, `rm -rf .ymh` -> `protected_target=true` and
  `Deny` in `evaluate` (H1).
- `ShellScope.GitControlProtected`: `git config --local core.hooksPath .evil`,
  `git clean -fdx`, `git submodule update`, `git worktree add` ->
  `protected_target=true`/`Deny` (sec 4.4.1).
- `ShellScope.GitGlobalConfigOutside`: `git config --global ...` ->
  `Outside`/`Ask`; `git -C /tmp config --local ...` -> `Outside`/`Ask`.
- `ShellScope.GitDataPlaneNormal`: `git add .`, `git commit -m x`, `git log`,
  `git status` -> normal `InRoot` (not `protected_target`), with the documented
  data-plane exemption.
- `ShellScope.GitInitAsk`: `git init`, `git clone ...`, `git filter-branch`
  -> `Unknown`/`Ask`, not auto-allowed.
- `ShellScope.RiskDelegation`: `classify_shell_command(t) ==
  classify_shell_command_with_scope(t).risk` for the spec-89 corpus.

**`tests/unit/errata90_session_grant_test.cpp`**

- `Session.PathToolCoversInRootClass`: approve `write_file src/x` with Session;
  `write_file docs/y` -> `Allow`; `write_file ../x` -> `Ask`.
- `Session.PathToolOutsideCreatesNoGrant`: approve an outside path tool with
  Session; the next outside request still asks.
- `Session.ShellPrefixCoversGitSubcommands`: approve `git status` with Session;
  `git diff` in-root -> `Allow`; `ls` -> `Ask` (different prefix).
- `Session.ShellOutsideStaysExact`: approve `cat /etc/passwd` with Session;
  `cat /etc/shadow` -> `Ask`; `cat /etc/passwd` again -> `Allow`.
- `Session.ShellUnknownStaysExact`: approve `python3 -c 'x'`; a different
  payload asks.
- `Session.GuardConfirmNotSuppressed`: approve `rm -rf build` with Session and
  L2 off; `rm -rf build` still `Ask` (guard Confirm).
- `Session.GuardForbidNotSuppressed`: approve `rm -rf ~` with Session; the next
  call is still `Deny` (guard).
- `Session.LostOnRestart`: a new `RulePermissionPolicy` with the same config
  and no store does not see the previous Session grant.
- `Session.AlwaysUnchangedExact`: an Always grant on `git status` still matches
  only the exact command (46-I5).
- `Session.GrantIdsDifferByClass`: `InRoot` and `Outside` grants for the same
  tool/command do not share an id.
- `Session.NonPathToolIsToolWide`: an MCP tool Session grant applies to every
  call of that tool (unchanged).
- `Session.TerminalWriteExactInput`: approve `terminal write "ls\n"` with
  Session; the same input -> `Allow`; `terminal write "ls -la\n"` -> `Ask`
  (M3).
- `Session.TerminalWriteNoPrefix`: a `terminal write` grant has
  `prefix=false` and `target_class=Unknown`; it never becomes an `ls` prefix
  grant (M3/M4).
- `Session.CompoundCommandNoPrefix`: approve `git status` then
  `git status && rm -rf src` -> not matched by the `git` prefix; exact
  fallback/guard decides (M10).
- `Session.CompoundCommandExact`: an approval of a compound command creates an
  exact `InRoot` grant, not a prefix (M10).
- `Session.ProtectedCreatesNoGrant`: approve `.ymh/config.jsonc` or `.git/config`
  with Session/Always; `remember` returns `{}` and the next call is `Deny`
  (H1).
- `Session.AlwaysShellExact`: an Always grant on `git status` still matches
  only the exact command (46-I5).
- `Session.AlwaysPathToolInRoot`: an Always grant on `write_file` matches
  in-root paths but not an outside path (M9; with `SandboxMode::Unrestricted`
  too).
- `Session.WrapperPrefixShared`: approving `git status` also covers
  `sudo git status`, and the reverse (L8; documented).

**`tests/unit/errata90_temp_root_test.cpp`** (Rev 9; simplified in Rev 12)

- `TempRoot.PredicateStrict`: `/tmp/x` and `/tmp/a/b` are under; `/tmp`,
  `/tmp/`, `/tmp/.`, `/tmpfoo`, `/tmp2/x`, `/tmp/..`, `/tmp/a/../b`,
  `/tmp/../etc/passwd`, and a relative `tmp/x` are not (PS28/MEDIUM-4).
- `TempRoot.EmptyRoot`: `path_is_under_tmp_root({}, "/tmp/x")` is false;
  `request_is_tmp_exempt` is false when `canonical_tmp_root()` is empty
  (MEDIUM-2).
- `TempRoot.CanonicalRoot`: `canonical_tmp_root()` is canonical `/tmp`; an
  unavailable `/tmp` returns empty and `path_is_under_tmp_root({}, absolute)`
  and `request_is_tmp_exempt` are false (MEDIUM-2).
- `TempRoot.TmpdirIgnored`: `TMPDIR=/var/tmp`, `TMPDIR=/etc`, and
  `XDG_RUNTIME_DIR=/etc` do not change the temp root; `shell "touch
  $TMPDIR/x"` is unexpanded and not exempt (PS-F61).
- `TempRoot.WorkspaceUnderTmpDisabled`: a workspace at/under `/tmp` makes
  `request_is_tmp_exempt` false even for `/tmp/x` (PS-F57).
- `TempRoot.ShellWriteAllow`: `shell "touch /tmp/x"` -> `Allow` under
  `default:"deny"`, `shell:"deny"`, `write:"deny"`, `ReadOnly`, and
  `force_ask=true`; `shell "rm /tmp/x"` -> `Allow` even though L1 classifies
  the outside operand `Forbid`; `rm -rf /tmp/x` is recursive and not exempt,
  so the outside-root `Forbid` stands -> `Deny` (PS29/PS-F51/PS-F55/PS-F66).
- `TempRoot.RedirectProducerAllow`: `printf x > /tmp/x`, `echo hi > /tmp/x`,
  `: > /tmp/x`, and `true > /tmp/x` -> `Allow` (redirection-first is limited
  to the harmless producers `printf`/`echo`/`:`/`true`; LOW-2/LOW-3).
- `TempRoot.DirectFileWriteAllow`: `rm /tmp/x`, `mv /tmp/a /tmp/b`,
  `mkdir /tmp/d`, `touch /tmp/x`, `truncate /tmp/x`, `tee /tmp/x`,
  `dd of=/tmp/x`, and `printf x > /tmp/x` -> `Allow` (direct non-recursive
  regular-file/new-path writes).
- `TempRoot.RecursiveWriteNotExempt`: `rm -rf /tmp/d`, `rm -r /tmp/d`,
  `cp -a /tmp/a /tmp/b`, `chmod -R /tmp/d`, `chown -R /tmp/d`, and
  `find -L /tmp/d -delete` -> not exempt and `Deny` (guard `Forbid`,
  outside-root); `rsync /tmp/a /tmp/b` and `install -D /tmp/a /tmp/b` ->
  not exempt and `Ask` under the recommended config (not guard-recognized)
  (MEDIUM-1/PS-F66).
- `TempRoot.DirectoryTargetNotExempt`: `rm /tmp/d` and `mv /tmp/d /tmp/e`
  where `/tmp/d` is an existing directory -> not exempt and `Deny` (the
  `rm`/`mv` outside operand/destination is L1 `Forbid`; regular-file/new-path
  only; MEDIUM-1). The declared `terminal open` cwd is the one
  exception: `{cwd:"/tmp/<existing-dir>", command:"touch x"}` -> `Allow`
  (LOW-2); `{cwd:"/tmp/missing", ...}` -> not exempt (cwd must be an existing
  directory).
- `TempRoot.ReadWriteRedirectNotExempt`: `printf x <> /tmp/x` and
  `dd of=/tmp/y <> /tmp/x` -> not exempt; `<>` is read+write, not a
  write-only form (LOW-3).
- `TempRoot.NonRegularTargetNotExempt`: with a pre-existing FIFO
  `/tmp/<unique>/fifo` (created with `mkfifo`), an `AF_UNIX` socket
  `/tmp/<unique>/sock` (created with `bind()`), and a character device
  `/tmp/<unique>/dev` (created with `mknod` when the test has the privilege;
  use `GTEST_SKIP()` only for that device subcase on `EPERM`, never for the
  whole test), `touch` and `tee` on each -> not exempt; existing targets must
  be regular files/new paths. The socket/device cases prove `is_regular_file`
  is type-agnostic; do not drop the FIFO case (LOW-5/LOW-4).
- `TempRoot.ShellReadNotExempt`: `shell "cat /tmp/x"` and `head /tmp/x` under
  `shell:"deny"`/`default:"deny"` -> `Deny`; `cat /tmp/x > /tmp/y` (mixed
  read+write) is not exempt. `permissions.read` does not gate shell reads
  (LOW-3/PS-F64).
- `TempRoot.ShellBareRoot`: `rm -rf /tmp`, `rm -rf /tmp/`,
  `touch /tmp/../etc/passwd`, and `touch /tmp/a/../b` -> not exempt; the `rm`
  forms -> `Deny` (guard `Forbid`), never an unconditional `Allow`
  (PS-F52/MEDIUM-4).
- `TempRoot.ShellGlobBrace`: `rm -rf /tmp/*`, `/tmp/x*`, `touch /tmp/?.txt`,
  and `rm -rf /tmp/{x,/etc/passwd}` -> not exempt (PS-F53).
- `TempRoot.SymlinkWorkspaceAliasNotExempt`: with `/tmp/link ->
  <root>/.git/hooks`, `shell "touch /tmp/link/pre-commit"` is not exempt: the
  canonical target is inside the workspace/protected tree (PS-F56/MEDIUM-3).
- `TempRoot.DanglingSymlinkNotExempt`: with `/tmp/link ->
  <root>/.git/hooks/missing` (target absent), `touch /tmp/link` is **not**
  exempt because the symlink is unresolvable/ambiguous (MEDIUM-2/PS-F56).
- `TempRoot.SiblingWorkspaceUnderTmp`: workspace A outside `/tmp` and workspace
  B at `/tmp/B`; a shell write to `/tmp/B/.ymh/x` is `Allow` (documented
  sibling-workspace reachability; risk 21/LOW-3).
- `TempRoot.DeviceRedirectNotExempt`: `touch /tmp/x 2>/dev/null` is not exempt
  because `/dev/null` is a non-temp target (conservative; PS-F65/LOW-8).
- `TempRoot.ShellUnexpanded`: `$TMPDIR/x`, `"$TMPDIR"/x`, `$(mktemp)`, and a
  here-document target -> not exempt (PS30/PS-F53/PS-F61).
- `TempRoot.ShellMixed`: `cp /etc/passwd /tmp/x`, `rm -rf /tmp/x ~/y`,
  `rm -rf /tmp/x ~root`, `rm -rf /tmp/x ~+`, `sh -c 'touch /tmp/x; rm src/y'`, and
  `cat /etc/passwd > /tmp/x` -> not exempt (PS-F54/LOW-5).
- `TempRoot.SourceOperandReadDenied`: `cp /tmp/a /tmp/b` under
  `shell:"deny"`/`default:"deny"` -> `Deny`; with `shell:"allow"` it is
  still not `tmp_exempt` (source is a read role); `dd if=/tmp/a of=/tmp/b`,
  `dd if=/etc/passwd of=/tmp/b`, and `install /tmp/a /tmp/b` likewise.
  `permissions.read` is not the shell gate (MEDIUM-1/LOW-3).
- `TempRoot.SourceSymlinkNotExempt`: with `/tmp/link -> <root>/.git/config`
  and `/tmp/link2 -> /etc/passwd`, `cp /tmp/link /tmp/out`,
  `dd if=/tmp/link2 of=/tmp/out`, and `install /tmp/link /tmp/out` are not
  exempt (source read + canonical/symlink check; MEDIUM-1/PS-F56).
- `TempRoot.MvTempAllow`: `mv /tmp/a /tmp/b` -> `Allow` (rename/delete source,
  write destination; no content read).
- `TempRoot.TeeAllow`: `tee /tmp/x` -> `Allow` (file target write; stdin
  hidden).
- `TempRoot.DdOfAllow`: `dd of=/tmp/x` -> `Allow` (write-only; stdin hidden);
  `dd if=/tmp/a of=/tmp/b` is not (source read).
- `TempRoot.UnknownProducerNoRedirect`: `echo hi` -> not exempt (no output
  redirection, so no write role; the producer alone is not a write).
- `TempRoot.UnknownProducerWithTempRedirectNotExempt`: `git checkout main >
  /tmp/x` and `curl -o /tmp/y https://example.invalid/x > /tmp/z` -> not
  exempt; an unknown producer with a `/tmp` output redirection is not a write
  (redirection-first is limited to the harmless producers; LOW-3).
- `TempRoot.InputRedirectNotExempt`: `tee /tmp/x < ~/.ssh/id_rsa` -> not
  exempt (input redirection is a read source plus a write).
- `TempRoot.UnknownWriterNotExempt`: `rsync /tmp/a /tmp/b` and
  `curl -o /tmp/x https://example.invalid/y` -> not exempt (unknown
  producer/writer without an output redirection; LOW-3/LOW-4).
- `TempRoot.ShellUntargetedForbid`: `touch /tmp/x; :(){ :|:& };:` and
  `touch /tmp/x; mkfs /tmp/y` -> not exempt; `Deny` from the guard
  (PS-F62).
- `TempRoot.ShellInterpreter`: `python3 -c 'open("/tmp/x","w")'`,
  `find /tmp -exec rm {} +`, and `xargs rm` -> not exempt (PS30).
- `TempRoot.PathToolNotExempt`: `write_file`/`read_file`/`grep`/`glob` at
  `/tmp/x` keep normal policy and are never `Allow` by the exemption; the tool
  then throws `PathEscape` (PS31/PS-F58). Path tools are the reason `shell` is
  required for `/tmp`.
- `TempRoot.TerminalOpenTemp`: `terminal open {cwd:"/tmp/sub",
  command:"touch x"}` -> `Allow`; `{cwd:"/tmp/sub", command:"touch
  /tmp/x"}` -> `Allow` (PS27/PS-F63).
- `TempRoot.TerminalOpenCwdRequired`: `terminal open {cwd:<root>,
  command:"touch /tmp/x"}` and `terminal open {command:"touch /tmp/x"}` -> not
  exempt because the cwd is a mandatory target and is not `/tmp`;
  `{cwd:"/etc", command:"touch /tmp/x"}` -> not exempt (PS27/PS-F63).
- `TempRoot.TerminalOpenBare`: a bare `terminal open` (no command) -> not
  exempt, even with `{cwd:"/tmp/sub"}` (PS30).
- `TempRoot.BareInterpreterNotExempt`: `terminal open {cwd:"/tmp/sub",
  command:"bash"}` is not a recognized write -> not exempt (LOW-7).
- `TempRoot.TerminalWriteTemp`: `terminal write "touch /tmp/x\n"` -> `Allow`;
  `terminal write "ls\n"` -> not exempt (no filesystem target) (PS-F60).
- `TempRoot.NoGrant`: a Session/Always decision path defensively called for a
  `tmp_exempt` request returns `{}`; no `permissions.jsonc` entry is appended
  (90-D25/PS29).
- `TempRoot.Determinism`: same request + same `/tmp` root yields the same
  verdict; no clock/randomness/order input (PS33).

### 14.2 New integration tests (real seams)

Production-set temp tests create the workspace outside `/tmp` (for example
under the build tree) so the `/tmp` exemption is available; tests that
intentionally use a workspace under `/tmp` assert the workspace-overlap
disable instead. The root is always `canonical_tmp_root()`/`/tmp`; there is no
fixture temp set. **Pinned harness mechanism (LOW-1):** `AgentTestEnv` owns a
`TempWorkspace` built on `std::filesystem::temp_directory_path()`
(`tests/support/test_env.hpp:29-36`), which is normally `/tmp`; a test that
needs the exemption must set `TMPDIR` to a test-owned directory outside `/tmp`
(for example `<build>/test-tmp`, created before construction) and restore the
previous value with an RAII guard at test end, or pass an explicit non-`/tmp`
workspace base if the harness grows one. Before an `Allow` case runs, assert
`env.workspace.path()` is not under `canonical_tmp_root()`; a workspace under
`/tmp` silently disables the exemption and must fail the test, not skip it.
`TMPDIR` here only relocates the test workspace; the exemption root remains
canonical `/tmp` (PS28/`TempRoot.TmpdirIgnored`).

**`tests/integration/errata90_agent_loop_path_test.cpp`**

- Drives `AgentTestEnv` with a `FakeLLM` script whose tool call is
  `write_file {"path":"src/x","content":"..."}` and captures the
  `PermissionRequest` in `gate->set_attention_hook` (the pattern at
  `tests/unit/agent_loop_test.cpp:993-1034`). Asserts
  `request.path == root/src/x` and that the path allow rule yields `Allow`.
- A second script uses `path:"../escape"`; asserts `request.path` is the
  normalized absolute outside path, the class is `Outside`, and the verdict is
  `Ask`.
- A third script uses `shell {"command":"git status"}`; asserts the path rule
  allows it in-root and that `shell {"command":"cat /etc/passwd"}` asks.
- A fourth script uses `write_file {"path":".ymh/config.jsonc"}` and
  `shell {"command":"chmod +x .git/hooks/pre-commit"}`; asserts `Deny` before
  the tool runs (H1).

**`tests/integration/errata90_grep_symlink_test.cpp`** (H2)

- Creates a temp workspace with `secret -> <outside temp file>`; calls
  `LocalFilesystem::grep` with `path=root` and a pattern that matches the
  outside content; asserts the result does not contain it and the symlink is
  not followed.
- Creates `.ymh/secret` and `.git/config`; asserts `grep`/`glob` over the root
  do not traverse them (90-A13/90-D14).
- A non-symlink in-root file still matches (no over-skip).

**`tests/integration/errata90_git_no_search_test.cpp`** (M7/N9)

- Creates a parent temp directory with a `.git` and a child workspace that is
  not itself a repository; `git_status`/`git_diff` in the child must not read
  the parent repository (tool error "not a repository"), never an ancestor
  history.
- `GitStatusTool` sets `GitQuery::root=context.root()` and leaves `path` empty;
  `GitDiffTool` sets `root=context.root()` and `path=resolve_base`; the service
  opens `query.root`, so the pathspec cannot reintroduce ancestor discovery.
- A real workspace repository still works, including a `git_diff path=src/x`
  pathspec.

**`tests/integration/errata90_path_broker_test.cpp`**

- Uses the shipped `PermissionBroker` with `FakeTransport`
  (`tests/unit/permission_broker_test.cpp:26`) and a real
  `RulePermissionPolicy` built from `to_permission_config`.
- Asserts a `Session` decision on `git status` creates a Session grant and a
  subsequent `git diff` request resolves `Allow` without a new pending entry
  (the broker -> policy seam at
  `src/permission/permission_broker.cpp:335-345`).
- Asserts a `Session` decision on `cat /etc/passwd` does not allow
  `cat /etc/shadow`.
- Asserts a `Session` decision on `rm -rf ~` still denies/asks on the next
  call (guard).
- Asserts `make_wire`'s `summary` is root-relative (for example
  `write_file src/x`, not `write_file /home/user/ws/src/x`); L6/90-D16.

**`tests/integration/errata90_temp_root_l2_test.cpp`** (Rev 9; simplified Rev 12)

- With L2 `Enforced` and `SandboxMode::Workspace`, a real shell
  `touch /tmp/<unique>/tmp-file` succeeds (the `/tmp` grant is present); the
  same spawn with `ConfinementPolicy::tmp_root` empty fails with `EACCES` (the
  control proves the grant is what made it work). Use a unique `/tmp`
  subdirectory and remove it afterwards; the workspace must be outside `/tmp`
  (construct the environment after setting `TMPDIR` to a test-owned non-`/tmp`
  base with RAII restore, as pinned in the 14.2 preamble; assert the workspace
  path is not under `canonical_tmp_root()` before the `Allow` case).
- `SandboxMode::ReadOnly`: a `tmp_exempt` shell (`printf x > /tmp/...`) is
  allowed by the policy and the write succeeds, while a hidden write to the
  workspace root (`printf x > <root>/hidden`) is kernel-denied because the root
  is granted only `kReadRights` (PS32/90-D26). The negative side is run with the
  policy verdict forced to `Allow`, so the kernel, not the policy, denies it.
- `ReadOnly` workspace **under** `/tmp`: `tmp_root` is empty (workspace-overlap
  check), so the `.ymh` write keeps the protected `Deny`, the policy verdict is
  not `Allow` by the exemption, and a hidden workspace write is kernel-denied
  (PS-F57).
- Cross-directory rename/link inside `/tmp` (`mv /tmp/<unique>/a
  /tmp/<unique>/b`, `ln /tmp/<unique>/a /tmp/<unique>/sub/b`) succeeds under
  L2 `Enforced` (`FS_REFER`); a link whose source is outside `/tmp` is not
  granted (PS32). A symlink `/tmp/<unique>/link -> /etc` plus `touch
  /tmp/<unique>/link/x` is not `tmp_exempt` (canonical target outside `/tmp`)
  and L2 also denies it under `Workspace` (PS-F56). A symlink `/tmp/<unique>/link
  -> <workspace>/.git/hooks` plus `touch .../link/pre-commit` is not exempt
  (canonical target inside the workspace/protected tree) under both sandbox
  modes, so the lexical protected deny is not bypassed (MEDIUM-3/PS-F56). A
  dangling `/tmp/<unique>/link -> <workspace>/.git/hooks/missing` plus `touch
  .../link` is not exempt (unresolvable symlink; MEDIUM-2). A read source
  symlink (`/tmp/<unique>/src -> <root>/.git/config`, then `cp
  /tmp/<unique>/src /tmp/<unique>/out` or `dd if=... of=...`) is not exempt
  and does not read the protected file under the exemption (MEDIUM-1).
- Recursive/following forms (`chmod -RL /tmp/<unique>/d`,
  `find -L /tmp/<unique>/d -delete`) with a child symlink to `<root>` are not
  exempt and `Deny` (guard `Forbid`, outside-root); under `ReadOnly` `chmod`/`chown` are not
  exempt at all, so Landlock's lack of metadata mediation is not reachable
  through the exemption (MEDIUM-1/PS-F66).
- `SandboxMode::Unrestricted` (L2 `NotApplicable`): a shell `/tmp` write is
  policy-`Allow`; no kernel guarantee is asserted (PS-F59).
- L2 `Unavailable`: the policy verdict is unchanged (`Allow`); the test records
  the policy-only mode (PS32/PS-F59).
- Empty canonical `/tmp` (simulated missing/unavailable): `SpawnConfinement::
  policy().tmp_root` is `std::nullopt` and no `/tmp` rule is added; shell
  spawns are not confined into an empty path (LOW-3).

**`tests/integration/errata90_temp_root_agent_test.cpp`** (Rev 9; simplified
Rev 12)

- Before constructing `AgentTestEnv`, sets `TMPDIR` to a test-owned directory
  outside `/tmp` (RAII restore) and asserts `env.workspace.path()` is not under
  `canonical_tmp_root()`; otherwise every `Allow` case in this file would
  silently test normal policy instead of the exemption (LOW-1).
- Drives `AgentTestEnv` with a `FakeLLM` `shell {"command":"touch
  /tmp/<unique>/x"}` under `default:"deny"`/`shell:"deny"` and `ReadOnly`;
  asserts the gate returns `Allow` without a pending request, the shell runs,
  and the file exists. Repeat under `SandboxMode::Unrestricted`.
- A second script uses `shell {"command":"rm -rf /tmp/<unique>"}`; asserts
  **`Deny`** (the recursive form is never exempt, PS29/PS-F66; `rm` outside the
  workspace is L1 `Forbid` -> `Deny`; the target is also an existing
  directory) and the directory is **untouched**. Do not assert `Ask`, `Allow`,
  or removal (MEDIUM-1).
- A third script uses `shell {"command":"rm -rf /tmp"}` and a fourth uses
  `rm -rf /tmp/..`; the guard verdict stands and nothing is removed.
- A fifth script uses `write_file {"path":"/tmp/<unique>/x"}`; asserts normal
  policy then a tool `PathEscape`, no file exists (path tools are not exempt;
  PS-F58).
- A sixth script uses `shell {"command":"touch /tmp/<unique>/x; :(){ :|:& };:"}`
  and `touch /tmp/<unique>/x; mkfs /tmp/y`; the guard `Deny` stands (PS-F62).
- A seventh script uses `rm -rf /tmp/{x,/etc/passwd}`; the brace token is not
  exempt (PS-F53).
- An eighth script uses `terminal {"action":"open","cwd":"/tmp/<unique>",
  "command":"touch x"}` under `Workspace` and `Unrestricted`; asserts `Allow`,
  the PTY opens, and `x` exists. `{cwd:<root>, command:"touch /tmp/<unique>/y"}`
  is **not** exempt because cwd is a mandatory target (PS-F63), and the bare
  open at `/tmp/<unique>` is not exempt.

### 14.3 Existing tests to update

- **Rev 12 classifier signature spill:** every direct call to
  `classify_shell_command_with_scope`/`classify_shell_request`
  (`tests/unit/errata90_shell_scope_test.cpp:16`, `:246`, `:263-300`, `:368`)
  now passes the canonical `/tmp` path (`canonical_tmp_root()`) as the last
  argument, not a `TempRootSet`. The semantics are unchanged for non-temp
  requests; an empty path disables the temp check.
- **Rev 12 workspace-under-`/tmp`:** a workspace at/under `/tmp` disables the
  exemption (`request_is_tmp_exempt` false, `ConfinementPolicy::tmp_root`
  empty), so `root/../x` is not temp-exempt. `PathRule.OutsideDotDotDoesNotMatch`,
  `PathRule.OutsideShellDoesNotMatch`, `errata90_agent_loop_path_test`'s
  `path:"../escape"`, and the `ReadOnly`/Q7 fixtures keep their outside
  assertions; add `TempRoot.WorkspaceUnderTmpDisabled` and the L2 workspace-
  under-`/tmp` case above. No test passes a synthetic temp set any more; the
  root is `/tmp`.
- `tests/unit/errata90_path_target_test.cpp` and
  `tests/unit/errata90_grant_loader_test.cpp` are unaffected: they do not
  consult the temp predicate.
- `tests/unit/errata46_permissions_test.cpp`: `UI46_D1_PathKeyRejected`
  (`:187-195`) becomes `UI46_D1_PathKeyAccepted` (the key parses), and a new
  `UI46_D1_PathKeyWithoutToolRejected` keeps the strictness. The
  `UI46_D1_CommandOnlyRuleRequiresTool` test stays unchanged.
- `tests/unit/errata46_permissions_test.cpp` **fixture (M6)**: `request_for()`
  (`:30-50`) currently never sets `path`; add an optional path and use
  `root / "src/x"` for path-tool tests. `UI46_D2_PathToolGrantIsToolWide`
  (`:476-485`) must pass an absolute in-root path; rename it to
  `UI46_D2_PathToolGrantIsInRootWide` and keep the two in-root `Allow`
  assertions, adding an outside `Ask` assertion.
  `UI46_D2_ConfigDenyOverridesDurableGrant` (`:598-621`) must pass a request
  with an absolute in-root `path` so the `Always`/Session path-tool grant (now
  `target_class=InRoot`) matches. With that fixture, the first assertion stays
  `Deny` (Project deny vetoes the LocalGrant), and the second stays `Allow`
  (Session is out of scope of the deny-veto). Without the fixture, the first
  `Deny` assertion still passes (the Project `deny` rule matches a path-less
  request); the second `Allow` assertion fails because the request is
  `Unknown` and `remember` returns `{}` (LOW-4).
- `tests/unit/config_test.cpp`: add parse coverage for
  `permissions.rules[].path`; keep the unknown-key and malformed-glob fatal
  cases; add `[`/`]` and `.`-segment rejection. Add
  `WorkspacePermissionsRejected` (N1/90-D18): a workspace
  `.ymh/config.jsonc` with `permissions.default`/`read`/`write`/`shell`/
  `rules`/`presets`/`default_preset` or `agent.sandbox` -> `ConfigError`; the
  same keys in the global layer load.
- `tests/unit/permissions_test.cpp`: `SessionGrantIsToolWide` (`:169-185`)
  becomes `SessionGrantIsInRootClassWide`; add an outside-not-covered
  assertion. The existing `Policy` path tests (`:73-80`) now go through the
  live path dimension rather than a hand-set test path.
- `tests/unit/permission_broker_test.cpp`: add the broker->Session class
  assertions above; keep the existing `force_ask`/`Once`/`Session` no-store
  tests. **Fixture (M6):** `ask_request()` (`:103-114`) sets
  `path = "src/x"` (relative) and `root = "/tmp/ws"`; change it to
  `path = root / "src/x"` (absolute). `SessionScopeRecordsGrant` (`:342-352`)
  otherwise sees `Outside`, creates no grant, and fails.
- `tests/unit/host_runtime_test.cpp` **fixture (L10):** `:195-204` uses the
  same relative `path="src/x"`; change it to the absolute contract even though
  only `Once` is exercised today.
- `tests/unit/ui_render_golden_test.cpp`: no change; the label stays
  `Allow for session` (`:2251`).
- `tests/unit/supervisor_harness_test.cpp`: no change; the Session selection is
  still index 1 (`:536-559`).
- `CMakeLists.txt` (L3/LOW-6): add `src/policy/path_target.cpp` to
  `ymh_policy` (`CMakeLists.txt:291-294`) and `src/execution/path_safety.cpp`
  to `ymh_execution` (`CMakeLists.txt:257-269`); the shared
  `path_is_protected` lives in `include/ymh/execution/path_safety.hpp`. `tests/CMakeLists.txt`: add
  `errata90_path_target_test.cpp`, `errata90_protected_path_test.cpp`,
  `errata90_grant_loader_test.cpp`, `errata90_path_rules_test.cpp`,
  `errata90_shell_scope_test.cpp`, `errata90_session_grant_test.cpp`,
  `errata90_temp_root_test.cpp`,
  `integration/errata90_agent_loop_path_test.cpp`,
  `integration/errata90_grep_symlink_test.cpp`,
  `integration/errata90_git_no_search_test.cpp`,
  `integration/errata90_path_broker_test.cpp`,
  `integration/errata90_temp_root_l2_test.cpp`, and
  `integration/errata90_temp_root_agent_test.cpp` to the explicit `ymh_tests`
  list (`tests/CMakeLists.txt:1-170`; integration entries near `:170-174`).
  The spec names the files, so a missing CMake entry is a build failure, not a
  silent skip.
- `tests/fixtures/spec_symbol_catalog.json` (L4): add the new spec-90 symbols
  (`TargetClass`, `tool_has_path_dimension`, `request_path_target`,
  `path_is_within_root`, `path_is_protected`, `canonical_tmp_root`,
  `path_is_under_tmp_root`,
  `ShellTargetScope`, `ShellClassification`,
  `classify_shell_command_with_scope`, `classify_shell_request`,
  `is_simple_shell_command`, `command_prefix`) if
  the catalog check requires them (`tests/unit/spec_catalog_test.cpp:23-26`
  enumerates only a few headers today); this is a fixture update, not a
  behavior change.

### 14.4 Acceptance trace

1. `permissions.rules[].path` loads and is rejected when invalid, including
   `[`/`]` and `.` segments (sec 4.6, PS13).
2. The loop populates `request.path` for all seven tools; outside paths are
   `Outside` (sec 4.2/4.3, PS1/PS2).
3. The shell scope is `InRoot`/`Outside`/`Unknown` as pinned, including the
   `kernel_confinement` coupling, `terminal write=Unknown`, attached options,
   interpreter payloads, and protected targets (sec 4.4, PS5).
3a. `terminal open`'s declared `cwd` is classified: outside/ambiguous ->
    `Outside`/`Unknown` Ask, in-root `.ymh`/`.git` -> protected/`Deny`, a
    strictly-temp cwd -> temp (a bare open there is `Unknown`/`Ask`); the tool
    computes `weakly_canonical(root/cwd)` first and bypasses `context.resolve`
    for a canonical temp cwd (which would throw `PathEscape`), while rejecting
    every non-temp outside/protected cwd under both sandbox modes
    (PS27/HIGH-1/MEDIUM-1/LOW-2/LOW-5).
4. The sec 4.7 fragment auto-approves in-root targets, asks outside for every
   path-bearing tool, and denies `.ymh/**`/`.git/**`, including nested
   components and shell git control-plane writes (PS8/PS19/PS22).
5. The guard still denies/asks before any path rule or grant; a protected
   `Confirm` is `Ask`, not `Allow` (PS7/PS8/N5).
6. A Session answer covers the in-root class and survives the next `evaluate`
   within the same daemon, while outside/unknown/terminal-write stay exact or
   one-shot; compound commands do not inherit a prefix (PS9/PS10).
7. `Always` keeps its file shape; path-tool grants load in-root; wildcard/empty
   shell grants are rejected (PS12/PS20).
8. `grep`/`glob` skip symlinks/protected traversal; the git **tools** do not
   search ancestors; the shell git ancestor search is a recorded residual
   (PS4, 90-D14/90-D15/N4).
9. A workspace `.ymh/config.jsonc` with a `permissions` table or
   `agent.sandbox` is rejected at load; the workspace cannot broaden the policy
   (PS21/90-D18).
10. `file:` URLs are parsed/decoded and outside ones ask; other URI schemes
    stay neutral (PS23/90-D19/N12).
11. `git stash -a/-u` and `remote`/upstream config mutations are protected;
    plain `stash` and read-only git forms remain data plane (PS22/N13/N14).
12. The durable grants loader rejects match-all shell/terminal commands
    (PS20/N16).
13. `GitQuery::root` is the open base and `path` is the pathspec; the git
    tools cannot rediscover an ancestor repo (PS24/N9).
14. The wire/dialog label are unchanged and the summary is root-relative
    (PS16).
15. **Rev 16 (Rev 18 bounds):** a **direct, non-recursive, regular-file/new-path
    write/delete/create** `shell`/`terminal` invocation whose every visible
    target is a concrete strict `/tmp` descendant (the declared terminal-open
    cwd is the one existing-directory exception; `<>` is read+write and
    non-exempt; every other existing target must be a regular file; and
    redirection-first is limited to `printf`/`echo`/`:`/`true`, so an unknown
    producer with a `/tmp` redirection is not exempt; 90-A28) is `Allow`
    before the guard, `ReadOnly`, `force_ask`, the protected check, and the
    fold; a `deny` default, `deny` shell/write, a config deny rule, and
    `agent.sandbox=read-only` cannot change it (PS29/90-D23). Reads keep
    normal shell/default/guard policy; `$TMPDIR` and other bases are ignored; path
    tools are not exempt.
16. **Rev 12 safety bound:** the bare `/tmp` root, `/tmp/..`, `/tmpfoo`,
    glob/brace/unexpanded targets, mixed non-temp commands, path tools,
    untargeted `Forbid`/unconditional-`Forbid` constructs, a bare
    `terminal open`, a `terminal open` whose cwd is not `/tmp`, a
    `terminal write` with no `/tmp` target, and an interpreter payload keep
    the normal verdict (`Ask`, or a guard `Deny`); they are not auto-allowed
    (PS30/PS-F51..PS-F63).
17. **Rev 12 path tools:** `resolve()` and `LocalFilesystem` are unchanged;
    `read_file`/`write_file`/`edit_file`/`grep`/`glob`/`git_*` at `/tmp` throw
    `PathEscape`. Use `shell` for `/tmp` work (PS31/PS-F58).
18. **Rev 12 L2:** under `Workspace` with L2 `Enforced`, canonical `/tmp` is
    granted `kTmpRights` (including `FS_REFER`); a `ReadOnly` temp-exempt spawn
    grants the workspace root only read rights. A workspace at/under `/tmp`
    disables the grant, so the temp write succeeds without reopening
    workspace writes (PS32/90-D26).

## 15. Recorded risks and limitations

1. **Path scoping is not a sandbox.** The lexical target class is a policy
   matching input. `shell` commands such as `make` can execute arbitrary code;
   L2 Landlock is the real write boundary, and without L2 the spec-89 SG-F22
   residual applies (an unparsed interpreter payload can escape). A path rule
   does not change that. The honesty statement of spec 89 sec 1.2/1.4 remains
   in force.
2. **Symlinks and `grep` (H2).** A symlink inside root is `InRoot` to the
   lexical policy; read/write/edit resolve the final target, and `grep` must
   skip symlinks (90-A13). Before that change the shipped `grep` follows them
   and leaks outside content; the spec now states this and requires the fix.
   `glob` reports names but reads no content.
3. **Outside under `default:"allow"`.** A path rule cannot deny outside; it can
   only fail to match. With `default:"allow"` (or `danger-full-access`) outside
   targets can be allowed except where the guard denies. The recommended
   configuration is `default:"ask"` plus `read/write/shell:"ask"`.
4. **The `read` default.** `permissions.read` defaults to `"allow"`; without
   `"read":"ask"`, an outside read returns `Allow` and then fails `PathEscape`.
   This is documented (90-C6/PS-F10) and corrected in the recommended config.
5. **Shell scope is conservative but lexical.** Bare words without path
   punctuation are neutral; `ssh host`, `git commit -m msg`, and `make` are
   `InRoot` even though they are not filesystem targets. Network egress is out
   of scope for a path operand; `git config --global`/`--system` is classified
   `Outside` and asks (sec 4.4.1). `file:` URLs are local and are decoded and
   classified (N12); curl `@path`/`name=@path`/`name=<path` are recognized
   best-effort (N18); other URI schemes are neutral. Interpreters with
   `-c`/`-e`, `awk`/`sed` scripts, `find -exec`, and `xargs` are `Unknown`
   (they ask), but a shell wrapper that hides a payload in a file can still be
   `InRoot`. **Hidden shell reads are NotClaimed**: the guarantee matrix
   (sec 1.7); the read-isolation/egress follow-up is a planned item (working
   name spec 91, OQ-90-9), not a registered spec;
   recognizing more visible syntaxes does not make the classifier sound.
6. **Protected paths are a policy/tool/shell-control guard, not a kernel rule
   (H1/N1/N2).** The policy check is lexical; the tool-level check
   canonicalizes independently and works under `Unrestricted` for path tools.
   Shell **git control-plane** writes (`config`, `clean`, `submodule`,
   `worktree`) are denied by sec 4.4.1, but a shell subprocess that recursively
   reads/writes/deletes `.ymh`/`.git` through an unpredicted vector (for
   example `grep -r .`, a symlink alias, or `python -c` opening the file) is a
   recorded residual; L2 does not deny reads under an allowed `/`, and under
   `Unrestricted`/no-L2 it may not deny the write. The structural fix is
   90-D18: a workspace `.ymh/config.jsonc` cannot carry a `permissions` table,
   so even a successful write of the config is a startup `ConfigError`, not a
   `master=true` escalation. That non-escalation argument covers the
   `.ymh/config.jsonc` variant only; a `python -c`/`perl -e` payload that
   **computes** a `.git/config` path (so the lexical scan sees no literal
   `.git` segment) can still write a hook/config and is a code-execution
   residual needing an OS-level deny (OQ-90-6). The tool `grep`/`glob` skip protected directories,
   direct `read_file`/`write_file` protected paths are denied, and
   `git_status`/`git_diff` keep their internal read-only `.git` exception.
   A shell `git` command in a workspace subdirectory can still read an
   ancestor `.git` (N4); see risk 13. `git stash -a/-u` (including clustered
   `-au`/`-ua`) and `remote`/`branch`/upstream config mutations are covered by
   the git-control classifier (N13/N14/N20/N22); a plain `stash` is exempt
   because it touches tracked changes, not the untracked/ignored `.ymh` tree.
   Glob tokens that could expand into `.ymh`/`.git` (`.ymh*`, `.git*`,
   `.ym?`) set `protected_target` (N19). Hidden reads/egress remain
   NotClaimed and are recorded as a planned follow-up (working name spec 91,
   OQ-90-9).
7. **Session breadth.** An in-root shell Session grant covers the executable
   prefix: `git` covers all `git` subcommands in-root, and `rm` after approving
   `rm -rf build` covers other in-root `rm` commands. A compound command does
   not inherit the prefix (M10). The guard still catches outside/broad cases,
   but it does not turn every in-root destructive variant into an ask (that is
   the `None`-with-L2 case of 89-D8/SG7). An InRoot path-tool Session grant is
   tool-wide in-root, but protected `.ymh`/`.git` paths are hard-denied, so the
   earlier `.ymh/config.jsonc` example is closed by H1; other in-root files are
   still covered. Wrapper stripping means `git status` and `sudo git status`
   share a prefix (L8). A user who wants precision should use `Once` or a
   narrower config rule. `Always` is exact for commands and in-root-wide for
   path tools (M9).
8. **Session is not durable.** A daemon restart loses every Session grant
   (46-I6); the next call asks. This is fail-safe, not a bug.
9. **Workspace `permissions` is rejected (M5/N1).** The whole `permissions`
   table is global-layer-only (90-D18); a workspace `.ymh/config.jsonc` with
   `default`/`read`/`write`/`shell`/`rules`/`presets`/`default_preset` is a
   `ConfigError` at load. This closes the workspace-config broadening even if
   the agent writes the file. The old replace-on-layer behavior
   (`src/config/config.cpp:537`) is therefore unreachable for `permissions.*`.
   A written invalid workspace config is at worst a startup error/DoS, not an
   escalation. The recommended fragment is global-layer.
10. **Path rules do not apply to MCP.** External tool schemas have no path
    dimension; MCP tools fall through to their configured default. No path
    confinement is claimed for MCP subprocesses (89-D18).
11. **`Always` asymmetry (M9).** Durable grants are exact for commands and
    in-root-wide for path tools; Session is class-scoped for in-root shell.
    This may surprise a user who expects `Always` to be broader than `Session`.
    The load-time `InRoot` derivation prevents the previous outside-path match
    under `SandboxMode::Unrestricted` (`src/execution/environment.cpp:72-74`);
    a finer durable path class would require a grants-file schema change
    (OQ-90-1).
12. **PTY cwd is not modeled (M4).** `terminal write` is always `Unknown` for
    path-rule purposes; a `cd /tmp` in the PTY followed by a bare relative
    target cannot be auto-approved by `path:"**"`. Session can remember the
    exact input chunk, but not a class.
13. **Git ancestor search (M7/N4).** The no-search fix applies to the
    `git_status`/`git_diff` **tools** and changes a workspace that is a
    subdirectory of a larger repo from "status of the parent repo" to "not a
    repository"; this is the safe behavior for the root-scoped promise. A
    shell `git` command is the system executable and still searches ancestors:
    under Workspace, L2's `/` is read-only and an ancestor `.git` **write** is
    blocked by L2; an ancestor **read** is allowed, and an ancestor write is
    not blocked when L2 is unavailable. This shell residual is not claimed as
    closed; the shell git-control classifier covers the in-workspace
    `config`/`clean`/`submodule`/`worktree` vectors.
14. **Permission summary (L6).** The summary is made root-relative so the UI
    and event log do not expose the absolute workspace path; a client that
    expected the old absolute summary text sees a different string.
15. **Workspace narrowing is lost (N1).** Rejecting the whole workspace
    `permissions` table means a repository cannot set a *narrower* policy for
    itself (for example `read:"deny"`); that is the price of the simple
    fail-safe rule. Per-workspace narrow-only policies are deferred (OQ-90-8).
    A deliberately malformed workspace `permissions` table is a startup error,
    not an escalation.
16. **`file:` URL and curl operand parsing is lexical and best-effort
    (N12/N18).** The parser strips/percent-decodes `file:` and recognizes
    `@path`/`name=@path`/`name=<path`; a URL that redirects through a network
    service, a dynamic interpreter payload, or another tool-specific syntax
    remains a residual (the payload is `Unknown`/NotClaimed; planned follow-up
    working name spec 91, OQ-90-9).
    `http:`/`https:`/`ssh:`/`git:` operands are neutral per OQ-90-4; network
    policy is out of scope.
17. **Stash/remote exemptions are subcommand-level (N13/N14/N20/N22).** Plain
    `git stash` is data plane; `-a/-u/--all/--include-untracked` and clustered
    `-au`/`-ua`/... are protected. `remote`/branch/upstream mutations are
    protected because they write `branch.*`/`remote.*` config; a
    hand-authored `git config` is denied. The **global** data-plane exemption
    (plain `stash`, `checkout`, `restore`, `reset`, `merge`, `pull`, etc.)
    assumes `.ymh` is untracked/ignored (LOW-10); a repo that tracks `.ymh` can
    have it overwritten/removed by those worktree-writing forms, so **do not
    track `.ymh`**. The classifier does not stat the index (I/O-free), so this
    is a documented deployment assumption. A future git subcommand not in the
    table is `Unknown`/`Ask` (fail-safe).
18. **Grant globs (N16).** A match-all shell/terminal grant is rejected; a
    bounded glob (for example `git *`) is still an operator-intentional
    allow-list and can cover more than one concrete command.
19. **Protected-path root-relative caveat (N17).** If the workspace root itself
    lives under `.ymh`/`.git`, the relative predicate does not protect the root
    or its descendants; the root is the trust boundary. The recommended
    deployment does not use such a root.
20. **The anti-enumeration boundary (Oracle reshape).** A pure string
    classifier cannot enumerate every shell read vector. The guarantee matrix
    (sec 1.7) is the boundary: hidden reads/egress are NotClaimed and tracked
    by a planned follow-up (working name spec 91, OQ-90-9). Adding `file:`/curl
    `@` recognition is best-effort
    defense in depth, not a soundness claim; future gate findings about a new
    hidden-read syntax belong to that follow-up, not a new spec-90 defect.
21. **The temp-root exemption is a deliberate hole in the workspace boundary
    (Rev 9; direct-write-only in Rev 16, PS29/PS30).** `/tmp` is shared,
    world-writable scratch space. Allowing a fully-`/tmp` **direct,
    non-recursive, regular-file/new-path write/delete/create** shell/terminal
    invocation means the agent can create, overwrite, and delete any direct
    file it can reach under `/tmp`, bounded by "strictly under" (not the root
    itself), the concrete/no-internal-`..`/no-glob/no-brace/no-unresolved
    bound, the per-target guard-attribution rule, and other users' Unix
    permissions (a
    `0700` directory is still denied by the kernel/OS; a world-readable file is
    readable, and `kTmpRights` includes read/exec as well as write/delete/make).
    **Reads are not exempt**: `cat /tmp/x` keeps normal shell/default/guard policy (`permissions.read` gates path tools, not shell).
    **Path tools are not exempt**: `write_file /tmp/x` still throws `PathEscape`;
    use `shell` for `/tmp` writes. `$TMPDIR`/`$XDG_RUNTIME_DIR` are ignored, so
    a deployment cannot widen the root. If `/tmp` is itself a symlink, a literal
    `/tmp/x` target is not lexically under the canonical root and the exemption
    is intentionally disabled (fail-closed, more prompts, never unsafe). A workspace at/under `/tmp` disables the
    exemption, keeping `ReadOnly` sound and preventing writes to its `.ymh`/
    `.git`. Sibling workspaces under `/tmp` are still reachable by a shell with
    another outer workspace (recorded and tested; the per-workspace warning is
    the only mitigation - do not keep durable workspace state under `/tmp`).
    A `/tmp` symlink is canonicalized by `request_is_tmp_exempt`: a target
    resolving outside `/tmp` or into the workspace/`.ymh`/`.git`, or a
    dangling/unresolvable symlink, is not exempt (MEDIUM-2/MEDIUM-3). Under `Unrestricted`/L2 `Unavailable` a symlink swapped in after
    that check (TOCTOU) is the recorded residual. Under `Workspace`+L2 the
    kernel root grant is RW, so it **allows** a resolved workspace target; the
    policy canonical check is the guard, not L2. Under `ReadOnly` the root is
    read-only and the kernel also denies data writes. Exempt operations are
    direct/non-recursive regular-file/new-path writes, so a child symlink in a
    traversed directory cannot reach the workspace; recursive/following forms
    (`rm -r`, `cp -r/-a`, `chmod`/`chown`, `find -L/-delete`, `rsync`,
    `install -D`) are not exempt. `chmod`/`chown` are excluded because Landlock
    does not mediate metadata, so `ReadOnly` alone cannot bound them. `FS_REFER` in `kTmpRights`
    makes cross-directory rename/link inside `/tmp` work; a link whose other
    side is outside `/tmp` has no matching grant. `/dev/null`/`/dev/fd/*`
    redirections are non-temp targets, so `touch /tmp/x 2>/dev/null` keeps the
    normal verdict (conservative LOW-8). Hardlink aliases are **not** detected
    by canonicalization: a pre-existing `/tmp/x` hardlink to a workspace
    `.git`/`.ymh` file is a regular `/tmp` file, so a later `touch /tmp/x` is
    temp-exempt; the enabler is a hidden/privileged write (risk 6 residual),
    recorded as out of scope for this lexical/canonical predicate (LOW-8). An unrelated `Forbid`/
    unconditional-`Forbid` construct is not laundered by a `/tmp` operand, and
    brace expansion is ambiguous. The user chose this explicitly; the strict
    descendant, write-only, and shell-only bounds are the accepted compromise.

## 16. Open questions

- **OQ-90-1 - Durable path-class grants.** Should `Always` also store
  `tool + TargetClass` (or a path prefix) in `permissions.jsonc`? Deferred: it
  needs a `ymh.permissions/2` schema and a migration, and the user's ask is met
  by config rules plus Session. The existing `Always` behavior is retained.
- **OQ-90-2 - Config rule layer merge. [resolved for permissions.]** The
  replace-on-layer behavior (`src/config/config.cpp:537`) is unreachable for
  `permissions.*` because the whole table is global-layer-only (90-D18). A
  future non-permission section that wants layered rules would need its own
  merge/provenance design; not this errata.
- **OQ-90-3 - Workspace-layer path allow rules. [rejected.]** A workspace
  `.ymh/config.jsonc` cannot add `permissions.rules` at all (90-D18); only the
  global layer can. This is the structural fix for N1, not a deferred
  hardening.
- **OQ-90-4 - Network targets.** Should `ssh`/`curl`/`scp` network targets be
  `Unknown` rather than neutral/`InRoot`? Deferred: the path dimension is about
  local filesystem targets, and ymh has no network policy. The pin is:
  `file:` URLs are **local** and are classified by the normal path rules
  (N12/90-D19); `http:`/`https:`/`ssh:`/`git:`/... remain neutral;
  `host:path` operands still classify the path part.
- **OQ-90-5 - UI scope hint.** Should the dialog show the exact Session class
  (for example `session: shell | in-workspace | prefix "git"`)? This would
  need a wire field or a supervisor-side recomputation. The label stays
  `Allow for session` and the spec documents the class; deferred to a UI
  errata if users need the hint.
- **OQ-90-6 - Protected recursive shell reads/writes/deletes.** Should L2 (or
  a shell-tool wrapper) deny reads, writes, and deletes of `.ymh`/`.git` to
  close `grep -r .`, symlink aliases, and unpredicted interpreter payloads?
  Landlock is allow-only and the current policy grants `/` read-only and the
  root read+write+remove; a robust deny needs a narrower allow-list, which is
  a spec-89 change. Deferred and recorded in risk 6. The **config** variant is
  non-escalatory regardless because 90-D18 rejects workspace `permissions`.
- **OQ-90-7 - PTY cwd tracking.** Should the daemon track a per-PTY cwd so
  `terminal write` can be classified instead of always `Unknown`? Deferred;
  it needs terminal-instance state that the permission request does not
  currently carry.
- **OQ-90-8 - Workspace narrow-only policy.** Rejecting the whole workspace
  `permissions` table also rejects a workspace that only wants to *narrow*
  (`read:"deny"`). A future design could compare the workspace value to the
  global value and accept only narrowing; deferred because the fail-safe
  "global-only" rule is simpler and the user's policy is global.
- **OQ-90-9 - Planned follow-up "read isolation & egress" (working name spec 91).** This
  spec does **not** guarantee shell hidden reads or network egress; the
  guarantee matrix marks those cells NotClaimed. The planned work item
  (working name spec 91) is a kernel read allow-list/Landlock read confinement
  plus a network policy, including `curl @path`/interpreter/unknown-tool reads
  and MCP/LSP read confinement. There is **no `91-*.md` and no tracker row**
  yet; it is an open question/planned follow-up, not a registered spec. It is
  **not authored here**; this errata records the boundary and the residual
  (matrix sec 1.7, risk 5/6).
- **OQ-90-10 - Configurable or narrower temp roots.** Should additional temp
  roots, a per-profile `$TMPDIR`, or an opt-out be configurable? Deferred: the
  user's requirement is unconditional ("always ... regardless of all the other
  permission settings"), and a `permissions.*`/`agent.sandbox` switch that
  could disable the exemption would contradict it. A jailed deployment that
  needs a narrower scope requires an explicit future spec; this errata pins the
  fixed root (canonical `/tmp` only; `$TMPDIR` is ignored).

## 17. Revision log

- **Rev 18 (applies the Rev 17 gate findings; not re-gated).**
  MEDIUM-1: the non-exempt `rm -rf /tmp/<unique>` verdict is `Deny`
  (outside-root L1 `Forbid`), not `Ask`; also fixed the `rm -rf /tmp/build`
  prose, PS-F52/PS-F66/PS-F67, and the recursive/following test prose;
  `rsync` and `install -D` remain `Ask` (not guard-recognized). LOW-1: 14.2
  pins the non-`/tmp` harness workspace (`TMPDIR` set before `AgentTestEnv`
  construction with RAII restore, plus an assertion that
  `env.workspace.path()` is not under `canonical_tmp_root()`). LOW-2:
  `tmp_exempt` is an I/O-free lexical precursor; the canonical/symlink/type
  checks are resolve-time in `request_is_tmp_exempt`. LOW-3: redirection-first
  is limited to the harmless producers `printf`/`echo`/`:`/`true`; an unknown
  producer with a `/tmp` redirection is not a write; test added. LOW-4:
  socket/device coverage added (device subcase skips only on `EPERM`);
  `is_output_redirect` is named as the helper to split output-only
  (`>`, `>>`, `>|`, `&>`) from `<>` (90-A28). Bumped to Revision 18.

- **Rev 17 (applies the Rev 16 gate findings; not re-gated).**
  MEDIUM-1: the integration temp-root test now expects `rm -rf
  /tmp/<unique>` -> `Ask` (superseded by Rev 18: the verdict is `Deny`;
  recursive and existing-directory; never `Allow`) and asserts the directory
  is untouched. LOW-1: the header records that
  tracker row 90 is stale at the promoted Rev 8 baseline; the spec does not
  edit the tracker. LOW-2: the `terminal open` declared cwd is the one
  existing-directory exception; all other targets must be regular-file/new
  paths. LOW-3: `<>` is read+write and non-exempt. LOW-4: the `tmp_exempt`
  contract now names direct/non-recursive, zero-read, existing-directory and
  non-regular exclusions. LOW-5: existing non-directory targets must be
  regular files (FIFO/socket/device rejected); new paths allowed. Tests
  `TempRoot.ReadWriteRedirectNotExempt`, `TempRoot.NonRegularTargetNotExempt`,
  and the terminal-cwd directory case added. Bumped to Revision 17.

- **Rev 16 (applies the Rev 15 re-gate findings; not re-gated).** MEDIUM-1:
  bounds the exemption to **direct, non-recursive, regular-file/new-path**
  writes; recursive/following forms (`rm -r/-rf`, `cp -r/-a`, `chmod`/
  `chown`, `find -L/-delete`, `rsync`, `install -D`) and existing directory
  targets are not exempt (normal `Ask`, or a guard `Forbid` `Deny`; Rev 18
  pins the verdicts); `chmod`/`chown` are excluded because
  Landlock does not mediate metadata, preserving the `ReadOnly` claim.
  LOW-1: tracker not edited. LOW-2: pinned the terminal tool's temp-cwd path
  (`weakly_canonical` first, bypass `context.resolve`) so the PTY can open at
  `/tmp`. LOW-3: clarified that `permissions.read` gates path tools, not shell
  reads; tests use `shell:"deny"`/`default:"deny"`. LOW-4: added input-
  redirection-from-outside and unknown-writer tests. LOW-5: recorded the
  symlinked-`/tmp` fail-closed behavior as intentional. Adds PS-F66/PS-F67.
  Bumped to Revision 16.

- **Rev 15 (applies the Rev 14 re-gate findings; not re-gated).** MEDIUM-1:
  source operands of `cp`/`install`/`ln`/`dd if=` are read roles and make the
  invocation non-exempt (zero-read rule); `tmp_targets` now records every
  visible target (read sources + write destinations + cwd), so a source
  symlink is canonicalized; tests for `cp /tmp/a /tmp/b` under
  `read:"deny"`, source symlinks, `mv`, `tee`, `dd`, `~root`/`~+`, and
  unknown producers. MEDIUM-2: dsh mirror cell now says write/delete/create.
  LOW-1: PS-F56/risk 21 no longer claim L2 denies a resolved workspace target
  under `Workspace` (the root grant allows it; policy rejects; `ReadOnly`
  denies). LOW-2: removed "pure" and added the per-prefix `symlink_status`
  I/O to PS3/PS33. LOW-3: tracker not edited per instruction. LOW-4: dangling
  added to the 4.8.1/D23/D25/D27 summaries. LOW-5: source-operand and verb
  tests added. Bumped to Revision 15.

- **Rev 14 (applies the Rev 13 re-gate findings; not re-gated).** MEDIUM-1:
  removed every read-inclusive summary of the temp exemption; the matrix,
  1.4/1.5, scope boundaries, A25/A27, recommended config, precedence, and
  acceptance trace now say write/delete/create, reads keep `read:"deny"`.
  MEDIUM-2: a dangling/unresolvable symlink (including a missing
  workspace/protected target) is ambiguous and not exempt; `symlink_status`
  prefix checks added to `request_is_tmp_exempt` with tests. LOW-1: remember
  broker anchor `:335-345`. LOW-2: output redirection makes a segment a write
  even for `printf`/`echo`. LOW-3: `ConfinementPolicy::tmp_root` is `nullopt`
  when `/tmp` is unavailable or overlaps the workspace, with an L2 test.
  LOW-4: 4.8.1 no longer says `evaluate` is I/O-free except the bounded temp
  check. LOW-5: tracker row not edited per instruction. LOW-6: L2 non-goal
  reworded for the bounded grant. LOW-7: predicate contract pins relative
  candidates -> false. LOW-8: hardlink aliases recorded as out of scope.
  Bumped to Revision 14.

- **Rev 13 (applies the Rev 12 re-gate findings; not re-gated).** MEDIUM-1:
  the exemption is restricted to recognized write/delete/create shell/terminal
  invocations; reads keep the normal `read:"deny"`/`read:"ask"` policy, with a
  pinned read/write verb classification and an honest residual for unknown
  verbs. MEDIUM-2: `path_is_under_tmp_root` returns false for an empty
  `canonical_tmp_root()`; tests added. MEDIUM-3: `request_is_tmp_exempt`
  canonicalizes `tmp_targets`; a resolved target outside `/tmp` or inside the
  workspace/protected tree (including `/tmp/link -> <workspace>/.git`) is not
  exempt. MEDIUM-4: any `..` component is rejected before normalization
  (`/tmp/a/../b`). LOW-1: stale anchors corrected; LOW-2: remember table
  write-only; LOW-3: sibling workspace under `/tmp` recorded and tested;
  LOW-4: dsh/matrix read and `..` wording; LOW-6: workspace equality pinned;
  LOW-7: bare interpreter is not a write; LOW-8: device redirects are a
  recorded conservative limitation; LOW-9: path-tool `Unrestricted` wording.
  Adds PS-F64/PS-F65. Bumped to Revision 13.

- **Rev 12 (design simplification; not re-gated).** Replaces the over-scoped
  temp exemption with canonical `/tmp` only: `$TMPDIR`, `$XDG_RUNTIME_DIR`,
  protected-root sets, aliases, and `--config` interactions are removed.
  Scope is `shell`/`terminal` visible targets only; path tools
  (`read_file`/`write_file`/`edit_file`/`grep`/`glob`/`git_*`) remain
  workspace-root-confined and `/tmp` throws `PathEscape` - use `shell`. The
  predicate is per target: every visible target (operands, redirections,
  `terminal open` cwd, `terminal write` input) must be a concrete strict
  `/tmp` descendant with no brace/glob/`..`/unresolved expansion. The effect
  is a final-verdict `Allow` overriding default/shell/write/`ReadOnly`/guard
  `Forbid|Confirm`; the old `mutating`/`temp_write_only` flag is gone and
  `ReadOnly` still uses `tool_is_mutating`. `remember` classifies first and
  returns `{}` for `tmp_exempt` (active guard). Tests, matrix, invariants,
  decisions, state table, dsh row, and risk 21 are updated. Bumped to
  Revision 12.

- **Rev 11 (superseded by Rev 12).** NEW
  MEDIUM-1: `canonical_protected_roots(workspace)` defines the protected set
  (workspace, `.ymh`, `.git`, `$HOME`, global config/state dirs) and
  `canonical_temp_roots(workspace, protected_roots)` rejects any candidate that
  overlaps it; `$TMPDIR` must also be under a known temp base
  (`/tmp`, `/var/tmp`, `/dev/shm`, `$XDG_RUNTIME_DIR`). NEW MEDIUM-2: the
  temp-pass flag is renamed `temp_write_only`; the `ReadOnly` step is pinned to
  `tool_is_mutating(request)`. NEW MEDIUM-3: a declared `terminal open` cwd is
  a resolution base, not a temp target; an in-root cwd with an absolute
  `/tmp/...` write is exempt. NEW MEDIUM-4: the `remember` sketch and prose
  pass `temp_roots_` to `classify_shell_request`. LOW-1: `90-A24`/`90-C5`
  anchors corrected to `shell_guard.cpp:2049-2100`/`:2085-2100` and
  `terminal_tool.cpp:38`. LOW-2: `TempRootSet{canonical, aliases}`; L2 uses
  `canonical` only. LOW-3: the `SpawnConfinement(root, mode, tempRoots)` call
  expression is pinned. LOW-4: Rev 10 wording corrected for the `ReadOnly`
  root-rights tightening and `$TMPDIR` environment input. LOW-5: the `:|:`
  pipe in PS-F62 is escaped. Bumped to Revision 11.

- **Rev 10 (superseded by Rev 12).** HIGH-1: pins the
  end-to-end execution seam - `LocalFilesystem` gains the same `TempRootSet`
  and `require_contained()` accepts a strict temp descendant,
  `ExecutionEnvironment::tempRoots()`/`ToolContext::tempRoots()` expose the
  set, and the independent
  canonical tool checks are stated; integration tests write `/tmp/...` under
  `Workspace` and `Unrestricted`. HIGH-2: the exemption is per target and
  write-only, and a guard `Forbid`/`Confirm` is bypassed only when attributable
  to the temp targets; fork bombs, `mkfs /tmp/y`, and mixed targets keep the
  guard verdict. HIGH-3: brace-bearing tokens are ambiguous and never exempt.
  MEDIUM-1: `terminal open` accepts a canonical temp cwd (PS27/acceptance 3a).
  MEDIUM-2/3: `canonical_temp_roots(workspace, protected_roots)` rejects a candidate equal to,
  inside, or an ancestor of the workspace; the `ReadOnly` workspace invariant is
  restated as conditional on that exclusion. MEDIUM-4: reads are not exempt
  (path tools and shell), so the protected read deny remains. LOW-1: stale
  anchors corrected (config sample `:1496-1508`, path call `:885`,
  `command_pattern`/`remember`/`evaluate`, git classifier `:1445-1680`, dsh
  evaluation anchor). LOW-2: `kTmpRights` includes `FS_REFER`. LOW-3: the
  guarantee matrix splits path-tool and shell/terminal temp rows (path-tool
  Exec is `Enforced`, not `Matched`). LOW-4: the bare-root headline says
  normal verdict (`Ask`, or guard `Deny`). LOW-5: a bare `terminal open` at a
  temp cwd is `Unknown`, never `InRoot`. Adds PS-F61..PS-F63 and updates
  tests/state. Bumped to Revision 10.

- **Rev 9 (superseded by Rev 12).** Adds the user's TEMP-ROOT EXEMPTION (verbatim:
  "writes to /tmp shall always be allowed regardless of all the other
  permission settings"). A target strictly under canonical `/tmp` or an
  absolute, existing, distinct `$TMPDIR` is `Allow` before the L1 guard,
  `ReadOnly`, `force_ask`, the protected check, and the fold, and cannot be
  disabled by any `permissions.*`/`agent.sandbox` setting (sec 4.8,
  90-D23..D28, A25-A27, PS28-PS33, PS-F51..PS-F60). Pins `TempRootSet`,
  `canonical_temp_roots`, the component-wise strict predicate, the bounded
  `resolve()`/path-tool `PathEscape` exemption, the L2 canonical-temp grant,
  and the `ReadOnly` read-only-root spawn; updates the guarantee matrix,
  decisions, state table, recommended-config effect, sample requirement,
  tests, and the shared-`/tmp` risk. Bumped to Revision 9; not re-gated.

- **Rev 8 (draft).** Applies the seventh independent gate-90 findings. NEW-6
  (MEDIUM): `fetch --set-upstream`/`pull --set-upstream` move to the protected
  row/90-D17/A20/PS22 (data-plane `fetch`/`pull` is qualified "without
  `--set-upstream`"); `fetch -u`/`pull -u` remain data plane
  (`--update-head-ok`, not `--set-upstream`); tests
  `Protected.GitFetchSetUpstreamDenied`/`ShellScope.GitFetchSetUpstreamProtected`.
  NEW-7: documented long/attached synonyms are pinned: `branch
  --move`/`--copy`, `switch --create`/`--force-create[=]`, attached
  `-b/-B/-c/-C/-u/-t/-m/-M` values, and `branch -d/-D/--delete` (removes
  `[branch]`); target-evidence table/4.4.1/90-D17/A20/PS22/tests updated.
  NEW-8: the three stale empty-text comments (sec 6, sec 9 sketch, PS-F17) now
  carry the declared-cwd exception. Bumped to Revision 8; not re-gated.

- **Rev 7 (draft).** Applies the sixth independent gate-90 findings. NEW-1
  (MEDIUM): 4.4.1/90-D17/A20/PS22 and tests extend protected branch/config
  writes to `checkout -B`, `switch -C`, `switch -t`, `branch -m/-M/-c/-C`,
  no-start branch creation, and `git -c branch.autoSetupMerge=...`; branch
  creation is protected unless `--no-track` because an invisible global
  `branch.autoSetupMerge=always` can write tracking config (deliberately
  conservative; the data-plane row is aligned). NEW-2: the evaluation
  pseudocode comment uses `effective_cwd`, not `request.root`. NEW-3:
  `classify_shell_request` pins the order: terminal-open cwd first, then
  empty-text return, so a bare `terminal open {cwd:".ymh"}` is still
  protected. NEW-4: acceptance trace item 3a covers `terminal open`'s cwd.
  NEW-5: direct tool-level forced-`Allow` outside-cwd rejection test under
  `Unrestricted` (`Protected.TerminalOpenCwdToolRejectsOutside`). Bumped to
  Revision 7; not re-gated.

- **Rev 6 (draft).** Applies the fifth independent gate-90 findings. HIGH-1:
  `terminal open`'s declared `cwd` is classified by `classify_shell_request`
  (effective cwd for scope and L1 risk; outside/ambiguous -> `Outside`/
  `Unknown` Ask; in-root `.ymh`/`.git` protected); both sandbox modes tested
  (90-D22/A24/PS27/PS-F50). HIGH-2: git default auto-tracking
  (`branch.autoSetupMerge=true`) forms (`checkout -b <name> <start>`,
  `branch <name> <start>`, `switch -c <name> <start>`, `switch --track`) are
  protected unless `--no-track`; regression tests. LOW-1: protected globs
  require a dot-leading literal prefix; bare `*`/`**` are not protected.
  LOW-2: outside `.ymh`/`.git` shell paths are `Outside`/Ask, not protected.
  LOW-3: the dialog sentence is scoped to step-5 denials (a guard `Confirm`
  is Ask and shows the dialog). LOW-4: 14.3 says the first assertion passes
  and only the second fails without the fixture. LOW-5: A19 anchors 52 sec
  2.2 (`:206-209`), not sec 3.4. LOW-6: one shared `path_is_protected` in
  `execution/path_safety.hpp` used by policy and `LocalFilesystem` (no
  duplicate). LOW-7: "spec 91" is a planned/open follow-up, not a registered
  spec (no file/tracker row). LOW-8: `remote prune`, `submodule
  status/summary`, `worktree list` are read-only, not protected mutations.
  LOW-9: a path-tool grant with a `command` is rejected as dead. LOW-10: the
  `.ymh`-must-not-be-tracked assumption is global to all worktree-writing git
  forms, not only stash. Bumped to Revision 6; not re-gated.

- **Rev 5 (draft) - Oracle reshape.** Retitles/scopes the spec to
  **path-scoped write/delete permissions + tool-mediated reads**; adds the
  guarantee matrix (sec 1.7) as the anti-enumeration device with
  Enforced/Matched/NotClaimed cells, and states that shell hidden reads are
  best-effort/NotClaimed. Splits the **planned follow-up "read isolation &
  egress" (working name spec 91)** (OQ-90-9; not a registered spec; not
  authored here) for kernel read
  confinement/Landlock + network policy. N18: recognizes curl
  `@path`/`name=@path`/`name=<path` as best-effort visible operands
  (90-D20/A22/PS25; tests). N19: glob-aware protected detection (`.ymh*`,
  `.git*`, `.ym?`, `src/.ymh*`) (90-D21/A23/PS26; tests). N20: clustered
  `git stash -au`/`-ua`/... protected. N21: `PolicyRule::target_class` row
  reconciled with the `Always` loader (`Always` path tools = `InRoot`). N22:
  `branch --track`/`--unset-upstream`/`--edit-description`,
  `checkout/switch --track` and the general `branch.*`/`remote.*` config-write
  rule protected. Bumped to Revision 5; not re-gated.

- **Rev 4 (draft).** Applies the third independent gate-90 findings. N12
  (HIGH): `file:` URLs are parsed (scheme/host strip, percent-decode) and
  classified as local paths; outside -> `Outside`/`Ask`; other schemes stay
  neutral (sec 4.4/90-D19/90-A21, PS23, tests `ShellScope.FileUrlOutside`/
  `ShellScope.HttpsUrlNeutral`). N13: `git stash -a/-u/--all/
  --include-untracked` is protected/`Deny`; plain stash stays data plane
  (sec 4.4.1, PS22, tests). N14: `remote`/upstream/branch-config mutations
  are protected because they write `.git/config`; read-only config forms
  (`--get`/`--get-all`/`--get-regexp`/`--list`) and `remote -v` are
  enumerated; `--worktree` is a write. N15: A19 names 52/46 and adds the
  workspace-config migration note (startup `ConfigError`, move keys to
  global). N16: the grants loader rejects match-all `shell`/`terminal`
  command patterns (no literal character); `git *` remains allowed. N17:
  the state table gains `agent.sandbox`; the root-relative
  `path_is_protected` caveat is stated and tested. Also closes prior N9 by
  adding `GitQuery::root` (open base) with `path` as the pathspec/scope and
  `GitStatusTool`/`GitDiffTool` callers. Bumped to Revision 4; not re-gated.

- **Rev 3 (draft).** Applies the second independent gate-90 findings. N1 (HIGH):
  makes the whole `permissions` table and `agent.sandbox` global-layer-only (90-D18/90-A19), adds
  shell git control-plane classification (sec 4.4.1/90-D17/90-A20) for
  `config`/`clean`/`submodule`/`worktree`, and adds the H1 regression tests
  (`git config --local`, `git clean`, workspace permissions rejection). N2:
  tool-level canonicalization is independent of `resolve()`, so the symlink
  alias check works under `Unrestricted`; the "never by danger-full-access"
  claim is scoped. N3: protected is any `.ymh`/`.git` component at any depth.
  N4: git no-search is scoped to the git tools; shell git ancestor search is a
  recorded residual. N5: PS8/4.3.1 say a protected `Confirm` is `Ask`, not
  `Deny`. N6: optional non-string `path` is `Unknown`/`Ask` (executor defaults
  to root). N7: 90-A18 records that 89 SG11 is superseded for the 90 fold. N8:
  `terminal open` with no command is `Ask`, with an in-root command is `Allow`.
  N9: `git_status` has no path argument; `git_diff`'s optional path is a
  pathspec; A14 pins the tool seam. N10: `Unrestricted` anchor fixed to
  `environment.cpp:72-74`. N11: the tracker note says promotion adds the
  `DESIGN_STATUS.md` row; this draft does not edit it. Bumped to Revision 3;
  not re-gated.

- **Rev 2 (draft).** Applies the independent gate-90 findings. H1: adds the
  protected-path hard deny (sec 4.3.1/90-D13), tool-level canonical re-check,
  `grep`/`glob` protected traversal, and grant-loader bounds (sec 4.6.1). H2:
  requires `grep` to skip symlinks and documents the exact enforcement split
  (90-D14). M1 removes the false `[abc]` class and rejects brackets; M2 removes
  the "scope independent of `kernel_confinement`" claim and pins the
  `Confirm -> Unknown` coupling; M3 makes the exact grant use
  `shell_guard_text` (including terminal write); M4 forces `terminal write`
  scope `Unknown`; M5 replaces the false workspace-layer rationale and
  recommends the global layer; M6/L10 name the exact fixture changes in
  `permission_broker_test`, `errata46_permissions_test`, and
  `host_runtime_test`; M7 pins `GIT_REPOSITORY_OPEN_NO_SEARCH`; M8 pins
  `--opt=value`/attached-option splitting; M9 rewords `Always` and loads
  path-tool grants as `InRoot`; M10 forbids compound prefix grants; M11 expands
  interpreter/`awk`/`sed`/`-exec`/`xargs` detection; M12 is covered by H1.
  L1 fixes the canonical-root anchor; L2 renames the dot-dot test; L3 adds the
  `path_target.cpp`/CMake/test-list wiring; L4 adds `ShellClassification` to
  the catalog list; L5 changes the sample claim to future tense; L6 pins a
  root-relative permission summary; L7 rejects middle `.` segments; L8 records
  wrapper-prefix breadth; L9 scopes determinism to the verdict. Bumped to
  Revision 2; not re-gated.

- **Rev 1 (draft).** Initial authoring for the team spec. Re-derives the six
  current-state findings (90-C1..90-C6), re-introduces `path` as a config rule
  dimension, populates `PermissionRequest::path` from tool arguments, derives
  the shell target scope from the spec-89 tokenizer, pins the in-root-only
  matching rule and the recommended `config.jsonc` fragment (including the
  `read:"ask"` gotcha), and replaces the exact-command Session grant with a
  class-scoped grant that keeps the guard's precedence, the `Always` store, the
  wire protocol, and the dialog label unchanged. Not yet reviewed.
