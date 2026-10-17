# 89 - Shell Command Safety Guard: a fail-safe pre-permission classifier plus kernel confinement (L2)

```
Status: verified (Rev 3)
Revision: 3
Verification status: adversarial reviewer (independent, Rev 3): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Component: 89 (errata) -- two layers against the incident (`rm -fr build ~` through
           the `shell` tool). L1 is a pure, I/O-free destructive-command
           classifier inserted as the FIRST branch of
           `RulePermissionPolicy::evaluate`, before the `ReadOnly` hard deny,
           `force_ask`, the rule fold, `tool_defaults`, `default_verdict`, and
           every grant. L1 is ADVISORY + a fail-safe clamp; it is explicitly NOT
           claimed to be unbypassable (a string classifier cannot be -- see
           89-D1 and SG-F22). L2 is kernel confinement (Landlock) applied in the
           `{shell,terminal}` child between fork and exec, inherited across
           `execve`, which the command cannot undo. Allow requires BOTH. The
           SHIPPED resolver is `PermissionBroker`
           (`src/permission/permission_broker.cpp:216-227`) on the daemon
           (`src/host/workspace_host.cpp:890-898`) and headless
           (`src/cli/headless.cpp:169-180`) paths; the M1-only
           `PermissionGate` is a test seam, not the product path (89-A12). L1
           never returns `None` for a destructive verb with unresolved/ambiguous
           operands (SG5); when L2 is Unavailable, a destructive call
           de-escalates to `Confirm` (fail-to-ASK), never `Allow` (89-D14).
           Amends 09-permissions.md (sec 3.1, sec 3.4, sec 3.5, sec 8/Q1/Q7,
           sec 10, sec 12.2/OQ-P5), 25-ui-ux-errata.md (25-A7/25-A11, force_ask
           precedence), 46-permissions-ui-errata.md (sec 6.1 purity, 46-D2.13
           veto), 07-tools-execution.md (the Spawn seam; `PermissionRequest::root`
           is canonical, 07 X3) and 00-architecture.md sec 18 (the
           ExecutionEnvironment seam) by reference. Adds one policy
           header/source pair and one execution header/source pair.
Depends on: 09-permissions.md (verified; `PermissionPolicy`, `PolicyVerdict`,
           `PermissionRequest`), 07-tools-execution.md (verified; 07 X3 root,
           the Spawn seam), 25-ui-ux-errata.md (verified; `force_ask`),
           46-permissions-ui-errata.md (verified; grant layers, deny-veto,
           purity contract), 00-architecture.md sec 18 (verified;
           ExecutionEnvironment seam).
Supersedes: 09-permissions.md sec 12.2 OQ-P5 ("a robust command parser is out of
           scope") and the sec 10 deliberate-omission line "no per-command shell
           parsing beyond glob matching (OQ-P5)" (`09-permissions.md:1051`,
           `:1312-1317`). Those are replaced by `classify_shell_command`
           (89-D1). It also supersedes the Rev 1 title/claim of an
           "unbypassable" classifier (89-D1; the L1 layer is fail-safe, not
           unbypassable). It does NOT supersede the rule total order (09 sec 3.4
           steps 2-5), the grant scopes (09 sec 3.6), the `ReadOnly` hard deny
           (09 sec 3.4 step 1), or `force_ask` (25-D4); it is additive and runs
           before all of them.
Scope: (a) the L1 classifier (tokenizer, segmentation, wrapper stripping, nested
       shells, path containment, the destructive-verb catalogue, the
       Deny-vs-Ask split, and the fail-safe "never None on ambiguity" contract);
       (b) the exact `evaluate` branch and its precedence; (c) the new
       `ShellCommandRisk`, `shell_guard_text`, `classify_shell_command`,
       `path_within_root` symbols and the additive `evaluate(request, reason)`
       overload;        (d) L2 kernel confinement at the `{shell,terminal}` spawn seam
       (`ConfinementStatus`, `KernelConfinement`, `ConfinementPolicy`,
       `Environment::confinementStatus`); (e) the coupling invariant, the
       clamp/REFUSE guard (`clamp_or_refuse_root`), and the
       `danger-full-access` disposition. It does NOT
       design an in-root recovery/overlay layer (deferred, sec 15), does NOT
       claim MCP/LSP confinement (89-D18), does NOT change any tool schema, and
       does NOT change the wire protocol, any event field, any registry row, or
       any config key.
```

## 1. Purpose, incident, and precedence

### 1.1 The incident (root cause)

The model invoked the `shell` tool with `rm -fr build ~`. The `shell` tool runs
the string under `/bin/bash -lc` with `cwd` set to the workspace root
(`src/tools/builtin_tools.cpp:415-418`, executable `/bin/bash` at `:416`, argv
`{bash, -lc, cmd}` at `:417`). Bash performed tilde expansion, so `~` became the
user's home directory and the command executed `rm -fr build /home/<user>` -- a
recursive, forced delete of the home tree. The user interrupted after roughly
four minutes; the deletion was partial.

The permission system did not protect the user because their configuration
auto-approves `shell`. "Auto-approve" is not a separate code path: it is the
`danger-full-access` preset (`approval = "never"`, `sandbox = "unrestricted"`)
and/or `permissions.default = "allow"`, which `to_permission_config` turns into
`default_verdict = Allow` plus an explicit `default.shell = Allow` rule
(`src/cli/wiring.cpp:132-158`, `master` flag at `:143`, shell rule at
`:150-151`; the preset is `include/ymh/config/config.hpp:161`
(`kDangerFullAccessPreset`), whose `unrestricted` sandbox is mapped at
`src/config/config.cpp:1680`/`:1763`, with `effective_sandbox_mode` at
`:1753`). Both artifacts are consulted *inside* the rule fold of `evaluate`
(`src/policy/permission_policy.cpp:274-326`), reached only after the two
early-return branches (`:266-272`). So the configured "auto-approve" made
`evaluate` return `Allow`, and the command reached `/bin/bash`.

### 1.2 Why a string classifier alone cannot be the fix (the doctrine)

A pure, I/O-free string classifier can **never** be "unbypassable". Counterexamples
that defeat any static scan are trivial to write and are not adversarial-model
hypotheticals: `python3 -c 'import shutil,os;shutil.rmtree(os.path.expanduser("~"))'`,
`base64 -d <<<... | bash`, `$'\x72\x6d' -rf ~`, `${IFS}`, `r''m -rf ~`, symlink
and TOCTOU tricks, and arbitrary interpreters. Rev 1 called the classifier
"unbypassable"; Rev 2 strikes that claim (89-D1).

The corrected doctrine is a two-layer design whose safety rests on the boundary
that cannot be escaped by a *string*:

- **L1 (advisory + fail-safe clamp).** A pure classifier that raises the bar and
  refuses to silently allow ambiguity. Its hard invariants are fail-*safe*, not
  provable-absence-of-bypass: it **never returns `None` for a destructive verb
  whose operands are unresolved or ambiguous** -- it returns `Confirm`/`Forbid`
  (SG5). L1 alone ships nothing as "the fix".
- **L2 (kernel confinement).** The real boundary: Landlock applied in the child
  process between fork and exec, restricting the resolved-path allow-list. It is
  inherited across `execve` and cannot be undone by the command, so it defeats
  the no-token, obfuscated, symlink, and TOCTOU cases that L1 cannot parse.
- **Allow requires BOTH.** `allow(S)` is justified only by
  `L1.None ^ L2.on ^ all-rights-present`. `L1.None => L2.on` (89-D14, coupling
  invariant). When L2 is Unavailable, a destructive call de-escalates to
  `Confirm`, never `Allow`.

### 1.3 What this spec covers

- **L1** (sec 4): the classifier, its `evaluate` insertion, and the fail-safe
  contract.
- **L2** (sec 5): kernel confinement at the `{shell,terminal}` spawn seam, its
  mechanism order, rights catalogue, ABI/availability probe, and fail-to-ASK
  behavior.
- **Coupling, clamp, and the `danger-full-access` disposition** (sec 6,
  89-D14/D15/D16).
- **B1/B2/B3 as spec deliverables**: spawn coverage (B1, 89-D18),
  the `danger-full-access` non-bypass (B2, 89-D15), and the `root == $HOME`
  refusal (B3, 89-D16); see sec 6 (decisions), sec 9 (SG13/SG15/SG16) and
  sec 10 (SG-F21/SG-F23).

### 1.4 Non-goals and the honesty statement (read loudly)

- **No claim of confinement where there is none.** `danger-full-access` disables
  *approval* (it feeds `master`, `src/cli/wiring.cpp:143`); it does not, and
  must not, disable L1 or L2 (89-D15). **Under `danger-full-access` with L2
  Unavailable (for example a 5.15 LTS kernel lacking `FS_TRUNCATE`, or Docker's
  default seccomp profile blocking `landlock_create_ruleset`), ymh does NOT
  claim safety for recognized destructive commands.** In that state a recognized
  destructive call is `Confirm` (fail-to-ASK) and never auto-proceeds: with a
  subscriber it is a live prompt, and without one (`subscriber_count == 0`) the
  broker returns `Deny` (`src/permission/permission_broker.cpp:226-227`). The
  cases that DO proceed under approval-off are (a) an L1-`None`
  recognized-in-root command (for example `rm -rf build`) and (b) an unparsed
  interpreter payload when L2 is off (SG-F22); safety for those is not claimed.
  This paragraph is a required spec deliverable, not a footnote.
- **No OS sandbox beyond Landlock.** bwrap and raw `unshare` are fallbacks
  (sec 5.2) but are not required for correctness; when none is available, L2 is
  `Unavailable` and the coupling invariant fails to `Confirm`.
- **No filesystem I/O in `evaluate`.** L1 resolves paths **lexically**
  (`std::filesystem::path::lexically_normal`), never with
  `canonical`/`weakly_canonical`/`realpath` (09 Q1). L2 is where resolved paths
  are actually mediated.
- **No coverage of MCP/LSP subprocesses** (89-D18). L2 applies to
  `{shell,terminal}` only.
- **No in-root recovery/overlay** (deferred, sec 15/OQ-SG-4, W6): recovery is not
  prevention.

### 1.5 Precedence

This errata is additive. It supersedes 09 OQ-P5 and the matching sec 10
omission line, and it supersedes Rev 1's "unbypassable" claim. Its L1 verdict is
computed **before** the existing `evaluate` branches; it never loosens them. L2
is applied in the child and never relocates the parent's trust decisions.

## 2. Scope boundaries

| Boundary | Owned by | This errata |
|---|---|---|
| Verdict value type (`Allow/Ask/Deny`) | 09 sec 2.1 | unchanged |
| Rule total order (specificity/layer/effect) | 09 sec 3.4 steps 2-5 | unchanged (runs after L1) |
| `ReadOnly` hard deny | 09 sec 3.4 step 1 | unchanged (L1 Forbid runs first) |
| `force_ask` forced review | 25-D4 / 25-A11 | unchanged; both yield `Ask` |
| Grant store / durability | 09 sec 3.6, 46-D2 | unchanged |
| Wire protocol / `permission.*` | 05 sec 7.6 | unchanged |
| Durable decision payload | 01 sec 4.5 | unchanged shape; `reason` reused |
| Tool schema / `destructive` hint | 07 sec 6.8 | unchanged |
| Child-process spawn seam | 07 / 00 sec 18 (`ExecutionEnvironment`) | L2 hook added (does not change the seam contract) |
| MCP/LSP subprocess confinement | 15 / MCP adapter | **NOT claimed** (89-D18) |

## 3. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 89-A1 | 09 sec 3.4 step 1 (hard deny) | `src/policy/permission_policy.cpp:266-272` | `evaluate` gains a **new first branch (L1)**: `classify_shell_command(shell_guard_text(request), request.root, cwd_of(request), request.kernel_confinement)`. `Forbid` returns `Deny`; after the `ReadOnly` deny, `Confirm` returns `Ask`. Both precede the rule fold. |
| 89-A2 | 09 sec 3.5 (hard denies and defaults) | `09-permissions.md:497-508` | adds the L1 **fail-safe** deny/confirm and records that L1 is advisory, not unbypassable (89-D1). |
| 89-A3 | 09 sec 10 + sec 12.2 OQ-P5 | `09-permissions.md:1051`, `:1312-1317` | **supersedes** "no per-command shell parsing" / "a robust command parser is out of scope"; the classifier is that parser, bounded (89-D3/89-D5). |
| 89-A4 | 09 sec 8 Q7 (hard deny absolute) | `09-permissions.md:944-946` | extends Q7: the L1 `Forbid` is a hard deny for the commands it recognizes; Q7 does NOT imply L1 is unbypassable (89-D1); adds SG1-SG17 (sec 9). |
| 89-A5 | 09 sec 3.1 / sec 2.1 (`PermissionPolicy::evaluate`) | `include/ymh/policy/permission_policy.hpp:160` | adds an additive, non-pure-virtual overload `evaluate(request, std::string* reason)`; the existing one-arg call delegates. |
| 89-A6 | 25-D4 / 25-A7 / 25-A11 (`force_ask`) | `25-ui-ux-errata.md:169`, `:173` | pins the precedence: L1 is evaluated first; a `Forbid` beats `force_ask`, and an `Ask` from either branch is the same verdict. |
| 89-A7 | 46 sec 6.1 / 46-D2.13 (purity, veto) | `46-permissions-ui-errata.md:763-772`, `:3062`, `:3498` (46-D2.13 / 46-I37) | records that L1 is pure (no I/O, no blocking), so the purity contract holds, and that L1 runs before the deny-veto loop, so the veto is unaffected. |
| 89-A8 | 09 sec 3 (new policy seam) | new files | adds `include/ymh/policy/shell_guard.hpp` + `src/policy/shell_guard.cpp` with `ShellCommandRisk`, `shell_guard_text`, `classify_shell_command`, `path_within_root`. |
| 89-A9 | 00 sec 18 / 07 (the `ExecutionEnvironment` seam) | `include/ymh/execution/environment.hpp:22-26`, `:37` | adds `virtual ConfinementStatus confinementStatus() const noexcept = 0` to `ExecutionEnvironment`, implemented by `LocalEnvironment` (one probe cached at construction). Adds `ConfinementPolicy` and `KernelConfinement` (`include/ymh/execution/confinement.hpp`). |
| 89-A10 | 07 (the Spawn seam) | `include/ymh/execution/process.hpp`, `include/ymh/execution/pty.hpp` | `ProcessRequest` and `PtyRequest` gain `const ConfinementPolicy* confinement = nullptr;`; the child applies it post-`close_inherited_fds` / pre-`exec`. |
| 89-A11 | 07 sec 6.8 (shell hardening) | `src/tools/builtin_tools.cpp:415-418`, `src/tools/terminal_tool.cpp:134-152`, `src/execution/process.cpp:576-579` | the `{shell,terminal}` spawns add `--noprofile --norc` and spawn with `ProcessEnvMode::Minimal`, which `clearenv()`s the child so `BASH_ENV`/`ENV` are not inherited (`:576-579`); a throwaway `HOME`/`TMPDIR` is set via `request.environment` (`:580-582`) (clamp floor, 89-D16). |
| 89-A12 | 09 sec 3.4 (B2 reconciliation) | `src/policy/permission_policy.cpp:424-437`; `src/permission/permission_broker.cpp:216-227` | pins that BOTH resolvers short-circuit ONLY on `Allow` (`permission_policy.cpp:428`; `permission_broker.cpp:217`), so an L1 `Confirm` under `master=true` is a live prompt (headless/no-subscriber => `Deny`, `permission_broker.cpp:226-227`); `danger-full-access` does not disable L1/L2 (89-D15). The SHIPPED path is `PermissionBroker` (`src/host/workspace_host.cpp:863-864,890-898`; `src/cli/headless.cpp:169-180`), not the M1-only `PermissionGate`. |

## 4. The L1 classification model

### 4.1 Where L1 sits in `evaluate`

```text
RulePermissionPolicy::evaluate(request, reason):
  0. L1 GUARD (89-D1; new; first):
       text := shell_guard_text(request)        // shell / terminal only
       cwd  := cwd_of(request)                   // request.root unless a tracked cd
       if text is non-empty:
           risk := classify_shell_command(text, request.root, cwd,
                                          request.kernel_confinement)
           if risk == Forbid:   reason = "shell-guard:outside-root"; return Deny
  1. ReadOnly hard deny (unchanged, 09 sec 3.4 step 1):
       if sandbox == ReadOnly and tool_is_mutating(request): return Deny
  2. L1 CONFIRM (89-D1):
       if text is non-empty and risk == Confirm:
           reason = "shell-guard:unbounded-in-root"; return Ask
  3. force_ask (unchanged, 25-D4):
       if request.force_ask: return Ask
  4. rule fold (unchanged):
       winner := greatest matching rule (specificity -> layer -> effect)
       narrowing-allow deny-veto (46-D2.13, unchanged)
       no match -> tool_defaults / config_.default_verdict
  5. return winner.effect
```

`reason` is a bounded, echo-free token (SG9); the caller (`PermissionGate`,
`agent_loop`) copies it into `PermissionOutcome.reason` when non-empty. L1 is
computed from the same `request` on the gate path (`:427`) and the no-gate path
(`src/agent/agent_loop.cpp:912-913`), so both share one seam.

### 4.2 Tokenization (89-D2)

A single left-to-right scanner over the command string. It performs **no**
execution, **no** I/O, and **no** expansion; it only splits and annotates.

- **Bare text**: a backslash escapes the next character into the current token.
- **Single quotes** (`'...'`): literal until the closing quote; no escapes; the
  token is marked `single_quoted`.
- **Double quotes** (`"..."`): literal except that backslash escapes `"`, `\`,
  `` ` ``, `$`, and newline; the token is marked `double_quoted`.
- **Operators** (`;`, `&&`, `||`, `|`, `&`, newline) terminate a token and are
  emitted as operator tokens, but only when they occur outside quotes.
- **Function definitions / grouping** (`(`, `)`, `{`, `}`) terminate tokens and
  are emitted as structural tokens.
- **An unterminated quote** consumes the remainder as one token and sets
  `truncated = true` (89-D5).
- **Bounded work**: the scanner caps tokens at `kMaxTokens = 4096` and each
  token at `kMaxTokenBytes = 65536`; past a cap it sets `truncated = true`
  rather than throwing (SG12).

The scanner never calls the shell and never reads the filesystem. `rm -rf
'build' '~'` and `rm -rf build ~` produce the **same** token sequence
`[rm, -rf, build, ~]` (89-D2, SG4).

### 4.3 Segmentation, wrappers, and env prefixes (89-D3/89-D4)

The token stream is split into **simple commands** at unquoted `;`, `&&`, `||`,
`|`, `&`, newline, and structural braces. For each segment the **command word**
is the first token after:

- leading **environment assignments** matching `^[A-Za-z_][A-Za-z0-9_]*=` (one
  or more);
- leading **wrapper commands** from a fixed set: `sudo` (with its options and an
  optional `-u <user>`), `env` (options and assignments), `command`, `exec`,
  `nohup`, `nice`, `time`, `timeout`, `stdbuf`, `setsid`.

A wrapper not in the set leaves the first token as the command word. Combined
short options (`-lc`, `-fr`) are treated as a single option token and expanded
character-wise for flag detection (so `-rf` sets `-r` and `-f`).

### 4.4 Nested shells and command substitution (89-D5)

- If the command word is a POSIX shell (`bash`, `sh`, `dash`, `zsh`, `ksh`,
  `busybox`) invoked with a `-c`/`-lc`/`-ic` option, the next operand is a
  command **string**: the classifier recurses into it.
- A `$( ... )` or backtick span anywhere in the raw text is a command
  substitution: the classifier recurses into its contents.
- **Depth bound `kMaxDepth = 2`** (the top-level command plus at most two
  nested levels). At the bound, the classifier does not recurse and sets
  `truncated = true`.
- **Fail-safe close-out:** if `truncated` is set and **any** destructive verb
  (sec 4.6) appears in the text, the result is `Confirm`, never `None` (SG5).
  A command the classifier cannot fully reduce is never silently allowed.

### 4.5 Path containment, lexically (89-D6)

`path_within_root(root, candidate, single_quoted)` decides whether a token's
expansion is inside the workspace. `root` is canonical (07 X3); the shell's
initial `cwd` is `root` (`src/tools/builtin_tools.cpp:418`).

1. **Empty** candidate: not a target; skip.
2. **Home**: a token that is `~` or begins with `~/`, or that (when not
   single-quoted) is `$HOME`/`${HOME}` or begins with `$HOME/`/`${HOME}/`:
   **outside** root. Documented **over-approximation**: a quoted `~` (which
   POSIX does not expand) is still treated as outside (SG4, 89-D6).
3. **Absolute** (`/...`): lexically normalized; inside iff equal to `root` or
   under `root`.
4. **Relative**: joined to the tracked `cwd` (89-D7) and lexically normalized;
   inside iff under `root`.
5. **Glob** (unquoted token containing `*`, `?`, or `[`): compute the literal
   prefix up to the first metacharacter. If that prefix is absolute or `~`/`$HOME`
   -> **outside**; if the prefix is empty, `.`, or `root` itself -> **broad**
   (`Confirm`); otherwise (a narrow in-root prefix such as `build/`) -> treated
   as an in-root bounded target (`None` unless the verb demands otherwise).
6. **`single_quoted`** suppresses `$HOME` expansion only; the `~`
   over-approximation is unconditional (SG4).

`lexically_normal` collapses `.` and `..` without touching the disk, so `..`
escapes are caught and symlink resolution is explicitly **not** performed. L2
(sec 5) is what mediates resolved paths, including symlinks.

### 4.6 The destructive-verb catalogue (89-D8, 89-D9)

Each simple command is matched against a fixed verb table. `operand` is each
non-option token after the verb (for `dd`/redirection the relevant field is
named). The result is the strongest outcome over all operands. When
`request.kernel_confinement` is `false` (L2 off), every `None` cell below that
could destroy an outside path is promoted to `Confirm` (89-D14); the table shows
the L2-on outcome.

| Verb / shape | Operand analysed | Outside root | Inside root, bounded | Inside root, unbounded / special |
|---|---|---|---|---|
| `rm` (`-r`/`-R`/`-f`/`--recursive`/`--force` or any operand) | each operand | `Forbid` | `None` | broad glob / `.` / root -> `Confirm` |
| `rmdir` | each operand | `Forbid` | `None` | `Confirm` |
| `shred` | each operand | `Forbid` | `None` | broad glob -> `Confirm` |
| `find <path> -delete`, `find <path> -exec <destructive>` | `<path>` and the `-exec` verb | `Forbid` | `Confirm` | `Confirm` |
| `truncate` | each operand | `Forbid` | `None` | `Confirm` |
| `dd ... of=<t>` | `of=` target | device or outside -> `Forbid` | in-root file -> `Confirm` | `Confirm` |
| `mkfs`, `mkfs.*`, `mke2fs`, `newfs`, `wipefs` | (device implied) | `Forbid` always | `Forbid` always | `Forbid` always |
| `chmod -R` / `chown -R` (`--recursive`) | each operand | `Forbid` | `None` (unless broad) | broad -> `Confirm` |
| `mv`, `cp` (`-r`/`-R` or plain) | the **destination** (last operand) | `Forbid` | `None` | broad -> `Confirm` |
| redirection `>`, `>>`, `<>`, `n>`, `&>` | the target token | block device or outside -> `Forbid` | `None` | `Confirm` |
| fork bomb (`:(){ :|:& };:` and structural self-invocation) | -- | `Forbid` | `Forbid` | `Forbid` |

- **Devices**: `/dev/null`, `/dev/stdout`, `/dev/stderr`, `/dev/tty`,
  `/dev/fd/*` are whitelisted (not destructive targets). `/dev/sd*`,
  `/dev/nvme*`, `/dev/hd*`, `/dev/mmcblk*`, `/dev/loop*`, `/dev/disk/*` are
  block-device targets -> `Forbid`.
- `dd if=...` alone (no `of=`) writes to stdout: `None`.
- `rm` operand `.` or `..`: GNU `rm` refuses these, but the classifier still
  treats `.` as the root (broad -> `Confirm`) and `..` as outside -> `Forbid`,
  because `rm -rf ./` and `rm -rf ../` do not.
- A narrow glob inside root (for example `rm -f build/*.o`) is `None`, so the
  common build-clean stays quiet.
- **Ambiguity never yields `None`.** Any operand the tokenizer cannot reduce to
  a concrete in-root literal (an unquoted `$VAR`, a `$(...)`/backtick the depth
  bound left unreduced, a brace expansion, an unquoted glob broader than the
  narrow-prefix rule) yields at least `Confirm` (SG5). This is the fail-safe
  property that replaces Rev 1's "unbypassable".

### 4.7 cwd tracking across a chain (89-D7)

Segments are processed left to right. State: `cwd` (starts at `root`) and
`cwd_known` (starts `true`).

- A `cd`/`pushd` segment with exactly one operand that resolves lexically updates
  `cwd`; a `cd` to `~`/`$HOME`/`/...`/`..` yields a `cwd` that may be outside.
- A `cd` with no operand, `cd -`, or a variable operand sets `cwd_known = false`.
- When a destructive operand is relative and `cwd_known == false`, the target
  cannot be proven inside -> `Confirm` (SG5). When `cwd` is known to be outside
  root, a relative destructive operand is outside -> `Forbid`.

Thus `cd /tmp && rm -rf x` classifies `x` relative to `/tmp` -> `Forbid`, and
`cd /x && rm -rf ~` hits rule 2 (`~`) -> `Forbid`.

### 4.8 Why L1 `Deny`/`Ask` cannot be auto-approvable (traces)

L1 returns before `rule_matches` is ever called
(`src/policy/permission_policy.cpp:274-292`). Both resolvers short-circuit
**only** on `Allow`: the test-path `PermissionGate::resolve`
(`src/policy/permission_policy.cpp:428`) and the SHIPPED-path
`PermissionBroker::State::resolve` (`src/permission/permission_broker.cpp:216-227`;
used by the daemon at `src/host/workspace_host.cpp:890-898` and by headless at
`src/cli/headless.cpp:169-180`). An `Ask` never short-circuits: the gate times
out to `Deny` (`:457-464`); the broker denies with no subscriber
(`permission_broker.cpp:226-227`) or on transport failure (`:268-273`).
Concrete traces:

| Input to `evaluate` | Auto-approve artifact | Branch reached | Verdict |
|---|---|---|---|
| `shell "rm -fr build ~"` | matching `PolicyRule{tool="shell", command="rm -fr build ~", effect=Allow, layer=LocalGrant, literal=true}` | L1 (89-D1) | `Deny` |
| `shell "rm -rf /"` | `default_verdict = Allow` | L1 | `Deny` |
| `shell "rm -rf $HOME"` | `PolicyRule{tool="*", effect=Allow, layer=Global}` | L1 | `Deny` |
| `shell "rm -rf *"` (in root) | `danger-full-access` (`default.shell=Allow` + `default_verdict=Allow`) | L1 confirm | `Ask` (prompt; headless `Deny`) |
| `shell "rm -rf build"` (in root) | same as above | falls through | `Allow` (unchanged) |
| `shell "python3 -c 'shutil.rmtree(~)'"` | any | L1 returns `None` for a non-verb interpreter | Rule fold -- **L2 must block the outside write** (89-D14) |

The last row is the doctrine in one line: L1 does not claim to catch it; L2 does,
or the call fails to `Ask`/`Deny` when L2 is off.

## 5. Kernel confinement (L2)

### 5.1 Why L2 is the real boundary

L1 parses strings; an interpreter, an obfuscated payload, a symlink, or a TOCTOU
window defeats any string parse (sec 1.2). L2 is applied in the **child process
between fork and exec**, before `execve`, and is inherited across `execve`; the
command cannot lift it. Landlock mediates **resolved** paths, so symlinks and
TOCTOU are handled at the kernel boundary rather than guessed at lexically.
Therefore `allow(S)` is justified only when `L1.None` **and** L2 is on
(89-D14).

### 5.2 Mechanism order

`Landlock > bwrap > raw unshare`.

- **Landlock** (preferred): unprivileged, no root, no userns, inherited across
  `execve`, mediates resolved paths. Rules are added with
  `landlock_add_rule`, then `landlock_restrict_self`.
- **bwrap** (fallback): defeated in practice on Ubuntu 24.04 by the AppArmor
  unprivileged-userns restriction (bwrap needs a setuid helper or unprivileged
  userns), and it needs an external binary plus a `$HOME`/credential bind
  matrix; used only if Landlock is absent and bwrap is usable.
- **raw `unshare`** (last resort): `CLONE_NEWNS` + `pivot_root`/bind mounts;
  needs a mount namespace, so it is unavailable in many containers without
  `CAP_SYS_ADMIN`. `Unavailable` if it cannot be entered.
- If none is available, L2 is `Unavailable` and the coupling invariant
  (89-D14) de-escalates destructive calls to `Confirm`.

### 5.3 Rights allow-list

| Path | Access |
|---|---|
| `workspace root` | `RW + MAKE_CHAR/DIR/REG/SOCK/FIFO + REMOVE_FILE/DIR + EXECUTE + REFER` |
| throwaway `HOME` | `RW + MAKE_* + REMOVE_* + EXECUTE` (a fresh per-spawn directory, never the real `$HOME`) |
| throwaway `TMPDIR` | `RW + MAKE_* + REMOVE_*` |
| `/` (read-only) | `READ + EXECUTE` (enough for `/bin`, `/usr`, `/lib*`; no `WRITE`, no `MAKE_*`, no `REMOVE_*`) |
| caches (`~/.cache`, `/tmp` if not the throwaway) | `READ` only |

The allow-list is additive: Landlock grants nothing not listed. A path outside
`workspace root` therefore cannot be unlinked, truncated, created, or
`ioctl`-mutated by the confined child, even if L1 missed the command.

### 5.4 ABI and rights probe

`probe()` calls `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)`:

- `ENOSYS` (kernel without Landlock), `EPERM` (for example Docker's default
  seccomp profile blocking the syscall), or an ABI older than required =>
  `Unavailable`.
- **Require `FS_TRUNCATE`** (Landlock filesystem ABI >= 3, kernel >= 6.2). On a
  5.15 LTS kernel `FS_TRUNCATE` is absent, so `probe()` returns `Unavailable`.
- **Require `FS_IOCTL_DEV`** (ABI >= 5) for the device-`ioctl` class.
- `NotApplicable` when the configured sandbox is `Unrestricted` *and* the
  operator has explicitly accepted the no-gates posture (the
  `danger-full-access` disposition, 89-D15); it is still not an `Enforced`.

The probe is cheap and side-effect-free (a version query only); it is cached
(sec 12).

### 5.5 The spawn seam and fd hygiene

L2 is applied in the child body of the two spawn sites owned by the
`{shell,terminal}` path:

- `LocalProcessService::run` (`src/execution/process.cpp`, fork at `:369`,
  child body `:380-408`, before `execvp`) and `LocalProcessService::spawn`
  (fork at `:537`, child body `~:549-590`) -- used by `ShellTool::execute`
  (`src/tools/builtin_tools.cpp:405-431`).
- `PtySession::open` (`src/execution/pty.cpp`, fork at `:466`, child body
  `:474-490`, before `execve :490`) -- used by `TerminalTool::openAction`
  (`src/tools/terminal_tool.cpp:125-165`).

The child calls `apply_or_abort(*request.confinement)` **after** it closes
inherited fds and **before** `exec`. In `LocalProcessService::run` / `spawn`
that is `close_inherited_fds()` (`src/execution/process.cpp:74-95`, called at
`:583`), and those fds remain `O_CLOEXEC`; the guard never widens inheritance
there. The PTY child (`src/execution/pty.cpp:474-494`) does NOT call
`close_inherited_fds` -- it closes only slave/master/status fds -- so L2 is
applied after those closes and the PTY fd-inheritance surface is a **recorded
gap** (sec 14), not a claim. On any failure in the child, `apply_or_abort`
`_exit(127)`s and writes a status byte to the status pipe, so the parent
surfaces a spawn failure rather than running unconfined.

### 5.6 Failure => UNAVAILABLE => fail-to-ASK

Any of: Landlock absent (`ENOSYS`), blocked (`EPERM`), ABI too old (no
`FS_TRUNCATE`), `apply_or_abort` failing in the child, or `bwrap`/`unshare`
unavailable => `ConfinementStatus::Unavailable`. With L2 `Unavailable`, a
destructive call (even in-root-bounded) is `Confirm` (89-D14); it is never a
silent `Allow`.

### 5.7 Clamp floor and shell hardening

- **Throwaway `HOME`/`TMPDIR`**: every `{shell,terminal}` spawn is given a fresh
  `HOME` and `TMPDIR` under a per-spawn directory so that `~` expansion cannot
  reach the real home tree even before L1 runs.
- **`--noprofile --norc`** is added to the bash argv
  (`src/tools/builtin_tools.cpp:417`, `src/tools/terminal_tool.cpp:136-138`),
  and the spawn uses `ProcessEnvMode::Minimal`
  (`src/execution/process.cpp:576-579`), which `clearenv()`s the child so
  `BASH_ENV`/`ENV` are not inherited; a startup file therefore cannot re-expand
  or re-execute anything.
- **`HOME`/`TMPDIR`/`PWD` clamp**: the child's `HOME` is the throwaway dir;
  `PWD` is the workspace root.

### 5.8 Deployment-kernel caveat (must be stated)

L2 is best-effort by host:

- **5.15 LTS** lacks Landlock `FS_TRUNCATE` (ABI 3); `probe()` =>
  `Unavailable` => destructive calls fail to `Confirm`.
- **Docker's default seccomp profile** blocks `landlock_create_ruleset` =>
  `Unavailable`.
- In both cases the coupling invariant (89-D14) is what keeps the system
  fail-safe; combined with `danger-full-access` (approval off), safety is **not**
  claimed (sec 1.4).

### 5.9 What L2 does NOT cover

L2 is applied only on the `{shell,terminal}` spawn path. **MCP server
subprocesses** (`src/mcp/mcp_transport.cpp:176-205`) and **LSP** subprocesses
are explicitly **not** claimed covered (89-D18). Their risk is out of scope for
this errata and must not be described as confined.

## 6. Decision register (89-D#)

**89-D1 -- L1 is a fail-safe classifier, not "unbypassable".** Rev 1's claim is
struck. L1 is advisory plus a fail-safe clamp. Its hard contract:
`classify_shell_command` **never returns `None` for a destructive verb whose
operands are unresolved or ambiguous -- it returns `Confirm` or `Forbid`**
(SG5). It returns `Forbid` -> `Deny` for outside-root/device/catastrophe
targets, and `Confirm` -> `Ask` for in-root-unbounded or unresolved targets,
before the rule fold. `None` is reserved for a recognized destructive verb with
concrete, proven in-root bounded operands, or for non-destructive commands.
Callers: `PermissionGate::resolve` (`src/policy/permission_policy.cpp:427`) and
the no-gate loop path (`src/agent/agent_loop.cpp:912-913`).

**89-D2 -- Tokenizer respects quoting and escapes and never executes.** The
scanner (sec 4.2) removes shell quoting while retaining `single_quoted` /
`double_quoted` flags, so `rm -rf 'build' '~'` and `rm -rf build ~` tokenize
identically. No execution, no globbing, no variable expansion, no I/O. Total and
bounded (SG12).

**89-D3 -- Segmentation handles chaining.** Unquoted `;`, `&&`, `||`, `|`, `&`,
newline, and braces split the stream (sec 4.3). Each segment is classified
independently; the strongest outcome wins.

**89-D4 -- Wrapper and env prefixes are stripped.** `sudo`, `env`, `command`,
`exec`, `nohup`, `nice`, `time`, `timeout`, `stdbuf`, `setsid`, and leading
`NAME=value` assignments are skipped to reach the real command word (sec 4.3).
`sudo rm -rf ~` therefore classifies `rm`.

**89-D5 -- Nested shells and substitutions are recursed to depth 2; beyond that
the result is conservative.** `bash -c`, `sh -c`, `$(...)`, and backticks are
recursed (`kMaxDepth = 2`). At or past the bound, `truncated = true`; if any
destructive verb is present anywhere, the result is `Confirm`, never `None`
(SG5).

**89-D6 -- Containment is lexical and `~`/`$HOME` are unconditionally
outside.** `path_within_root` normalizes lexically and never resolves symlinks
(no I/O). `~` and `$HOME`/`${HOME}` (and `~/`, `$HOME/`) are outside root;
quoting does not exempt `~` (over-approximation, justified by the incident's
cost asymmetry). Resolved-path safety is L2's job (sec 5).

**89-D7 -- `cd` changes resolution; an unknown cwd downgrades to `Confirm`.**
Relative destructive operands use the tracked `cwd`. If `cwd` is not provably
inside root (unknown or outside), the operand cannot be shown to be in-root;
unknown -> `Confirm`, known-outside -> `Forbid` (sec 4.7).

**89-D8 -- Deny-vs-Ask split.** `Forbid` is reserved for targets that are
provably **outside** the workspace root, block devices, filesystem-format
commands, and fork bombs: workspace-boundary escapes with no legitimate
in-workspace purpose. `Confirm` is for destructive operations **inside** root
whose blast radius is unbounded or unprovable (broad in-root globs, in-root
`find -delete`, `dd` over an in-root file, and every ambiguous/truncated operand
-- SG5). Inside-root, bounded literal operations (`rm -rf build`) are `None`.

**89-D9 -- `terminal write` input is classified.** `shell_guard_text` returns
the text to classify: `shell` -> `arguments["command"]`; `terminal`
`action=open` -> `command` or the joined `argv`; `terminal` `action=write` ->
`arguments["input"]` (the bytes typed into an already-open PTY). This closes the
"open a PTY, then type the destructive command" case at L1. `command_pattern`
(`src/policy/permission_policy.cpp:116-145`, used for rule matching) is
**unchanged**; the guard uses its own extractor (SG11).

**89-D10 -- A bounded guard reason is threaded additively.**
`PermissionPolicy` gains a non-pure-virtual `evaluate(const PermissionRequest&,
std::string* reason)`; the existing one-arg method is kept and delegates.
`PermissionGate::resolve` (`:427-437`) and the no-gate loop path pass a local
`std::string` and return it in `PermissionOutcome.reason` (falling back to
`"rule"` when empty). Reason tokens are fixed and echo-free:
`"shell-guard:outside-root"`, `"shell-guard:unbounded-in-root"` (SG9).

**89-D11 -- There is no config escape hatch for L1 or L2; neither can be
weakened at any layer.** No `[permissions]` key disables or downgrades L1 or
L2, at the global or the workspace layer. Rationale: a disable switch
recreates the incident under a new name, and the incident's enabler was
precisely a config switch ("auto-approve"). The 09/26 precedent for a
deliberate global-layer-only opt-out exists (`26-dsh-alignment.md:1501-1507`,
`52-D15` at `include/ymh/config/config.hpp:152-157`) but is deliberately **not**
used (OQ-SG-1). `danger-full-access` is not an exception (89-D15).

**89-D12 -- The classifier is the single shell-text parser; the pre-existing
glob matcher is untouched.** Rule matching still uses `command_pattern` globs
(09 sec 3.3); L1 is an independent, structured classifier. This satisfies 89-A3
(OQ-P5 superseded) without changing rule semantics.

**89-D13 -- Precedence vs `ReadOnly` and `force_ask`.** `Forbid` is evaluated
before the `ReadOnly` deny (both are `Deny`; L1 gives the specific reason).
`Confirm` is evaluated after the `ReadOnly` deny and before `force_ask`;
`Confirm` and `force_ask` both yield `Ask`, so the ordering only decides the
recorded `reason`. `force_ask` is never weakened.

**89-D14 -- Coupling invariant: `L1.None => L2.on`.** An `Allow` from the rule
fold is justified only when L1 returned `None` **and** L2 is on
(`request.kernel_confinement == true`, set from
`ConfinementStatus::Enforced`). When L2 is `Unavailable` (or `NotApplicable`),
every call whose L1 verb is a RECOGNIZED destructive verb (sec 4.6) is at least
`Confirm` (fail-to-ASK), never `Allow`: the classifier receives
`kernel_confinement = false` and promotes the relevant `None` cells to
`Confirm`. An interpreter payload L1 does not recognize returns `None` and, with
L2 off, is not promoted (SG-F22) -- a stated residual, not a claim.
`apply_or_abort` failing in the child ->
`_exit(127)` -> spawn failure, not an unconfined run. The invariant is stated
once and implemented at two sites: the classifier flag (L1) and the child apply
(L2).

**89-D15 -- B2 disposition: `danger-full-access` disables approval, not L1/L2.**
`master = true` (`src/cli/wiring.cpp:143`) sets `default_verdict = Allow` and
`default.shell = Allow`, but those live inside the rule fold. L1 runs first, so
the incident command returns `Deny`, and a recognized in-root-destructive command
returns `Ask`. **Both shipped resolvers short-circuit ONLY on `Allow`:** the
broker (`src/permission/permission_broker.cpp:216-227`; used by the daemon at
`src/host/workspace_host.cpp:890-898` and by headless at
`src/cli/headless.cpp:169-180`) denies when `subscriber_count == 0`
(`:226-227`); the gate (`src/policy/permission_policy.cpp:428`) times out to
`Deny` (`:457-464`). Therefore **a guard `Confirm` is structurally not
auto-approvable under `master=true`** -- it is a live prompt with a subscriber
and a `Deny` without one; it never silently proceeds. The ONLY approval-off cases
that proceed are an L1-`None` recognized-in-root command (for example
`rm -rf build`) and an unparsed interpreter payload when L2 is off (SG-F22);
safety for those is not claimed (sec 1.4). Resolution chosen (reconciling the
bundle OPEN): we **pin this outcome as the resolution** and additionally pin that
the preset's semantics are "disable approval only -- never L1 or L2"; we do
**not** redefine the `approval=never` -> `sandbox=unrestricted` mapping (that is
a code change outside 89's scope, and 89-D11 already forbids a guard/L2
kill-switch). The rejected alternative ("redefine the preset so `approval=never`
no longer implies `unrestricted`") is recorded here with its reason.

**89-D16 -- Clamp/REFUSE when the workspace root is home or an ancestor of
it.** A workspace whose canonical root `== $HOME`, or is an ancestor of
`$HOME`, `TMPDIR`, or another sensitive path (for example `/`, `/home`,
`/etc`, `/usr`), is **refused at startup** (exit 2, `ConfigError`-style) before
any daemon/turn runs. Reason: L2 grants write access to the workspace root by
construction, so a root that is (or contains) `$HOME` would bless exactly the
deletion the incident caused. The check is on canonical paths and runs at the
daemon-startup seam near `effective_sandbox_mode` (`src/config/config.cpp:1753`)
/ `src/cli/wiring.cpp:263`. New symbol (named, 89-D16):
`std::optional<std::string> clamp_or_refuse_root(const std::filesystem::path&
canonical_root, const std::filesystem::path& home);` -- returns a refusal detail
(non-empty) when the root is refused, `std::nullopt` otherwise. Concrete caller:
the daemon/CLI startup path (`src/cli/wiring.cpp:263`, `src/cli/headless.cpp`);
tested by `errata89_clamp_test.cpp`.

**89-D17 -- Confinement interface (symbols + callers).**
`enum class ConfinementStatus : uint8_t { Enforced, Unavailable, NotApplicable };`
`struct ConfinementPolicy { std::filesystem::path root; std::optional<
std::filesystem::path> home, tmpdir; std::vector<std::filesystem::path>
ro_paths; };` `class KernelConfinement { virtual ConfinementStatus probe() const
= 0; virtual void apply_or_abort(const ConfinementPolicy&) = 0; };`
`LandlockConfinement` implements it (`src/execution/landlock.cpp`).
`ExecutionEnvironment` gains `virtual ConfinementStatus confinementStatus()
const noexcept = 0;` (89-A9); `LocalEnvironment` caches one `probe()`.
`ProcessRequest`/`PtyRequest` gain `const ConfinementPolicy* confinement`.
`classify_shell_command` is revised to take the L2 flag:
`ShellCommandRisk classify_shell_command(std::string_view text, const
std::filesystem::path& root, const std::filesystem::path& cwd, bool
kernel_confinement);` Callers: `RulePermissionPolicy::evaluate`; the spawn
sites (`process.cpp`, `pty.cpp`); tests. `PermissionRequest` gains
`bool kernel_confinement = false;`.

**89-D18 -- L2 scope: `{shell,terminal}` only; MCP/LSP not claimed.**
Confinement is applied on the two spawn sites reachable from `ShellTool` and
`TerminalTool`. MCP server subprocesses (`src/mcp/mcp_transport.cpp:176-205`)
and LSP subprocesses are explicitly **not** claimed. B1 (the spawn census) is
proven at the single dispatch chokepoint `ToolRegistry::execute`
(`src/tools/tool_registry.cpp:323`, calling `tool->execute` at `:354`), whose
only caller is `agent_loop.cpp:990`, after the permission decision block
(`:894-946`).

## 7. New symbols and their callers

| Symbol | Kind | Caller |
|---|---|---|
| `enum class ShellCommandRisk { None, Confirm, Forbid }` | new type (policy) | return type of `classify_shell_command`; consumed by `RulePermissionPolicy::evaluate` |
| `std::string shell_guard_text(const PermissionRequest&)` | new free fn | `RulePermissionPolicy::evaluate` (`src/policy/permission_policy.cpp`) |
| `ShellCommandRisk classify_shell_command(std::string_view, const std::filesystem::path&, const std::filesystem::path&, bool)` | new free fn (revised in Rev 2 to take the L2 flag) | `RulePermissionPolicy::evaluate` |
| `bool path_within_root(const std::filesystem::path&, std::string_view, bool)` | new free fn | `classify_shell_command`; `tests/unit/errata89_shell_guard_test.cpp` |
| `PolicyVerdict PermissionPolicy::evaluate(const PermissionRequest&, std::string*)` | new virtual (defaulted) | `PermissionGate::resolve`; `agent_loop` no-gate path; `RulePermissionPolicy` override |
| `bool PermissionRequest::kernel_confinement` | new field | set in the request-build block (`src/agent/agent_loop.cpp:871-889`) from `confinementStatus()`; read by `classify_shell_command` |
| `enum class ConfinementStatus { Enforced, Unavailable, NotApplicable }` | new type (execution) | `ExecutionEnvironment::confinementStatus`; `agent_loop`; `wiring` clamp |
| `struct ConfinementPolicy` | new type (execution) | built per spawn in `ShellTool`/`TerminalTool`; consumed by `apply_or_abort` |
| `class KernelConfinement` | new interface (execution) | implemented by `LandlockConfinement`; held by `LocalEnvironment`; called in the child bodies of `process.cpp`/`pty.cpp` |
| `ConfinementStatus ExecutionEnvironment::confinementStatus() const noexcept` | new virtual | `agent_loop.cpp` (fills `request.kernel_confinement`); `wiring.cpp` (clamp/refuse) |
| `std::optional<std::string> clamp_or_refuse_root(const std::filesystem::path&, const std::filesystem::path&)` | new free fn (wiring/execution) | daemon/CLI startup predicate (`src/cli/wiring.cpp:263`, `src/cli/headless.cpp`); `tests/unit/errata89_clamp_test.cpp` |
| `const ConfinementPolicy* ProcessRequest::confinement` | new field | child body `src/execution/process.cpp:380-408` / `:549-590` |
| `const ConfinementPolicy* PtyRequest::confinement` | new field | child body `src/execution/pty.cpp:474-490` |
| `kMaxTokens`, `kMaxTokenBytes`, `kMaxDepth` | constants | the scanner / recursion in `src/policy/shell_guard.cpp` |
| token scanner, segment/verb helpers | file-local (anonymous namespace) | called only from `classify_shell_command` |

## 8. C++ interface sketches

```cpp
// include/ymh/policy/shell_guard.hpp (new; 89-D1/89-D6/89-D9/89-D17)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "ymh/policy/permission_policy.hpp"   // PermissionRequest

namespace ymh {

// 89-D1. `None` => normal policy; `Confirm` => fail-to-ASK; `Forbid` => hard
// Deny. Order: Forbid > Confirm > None. NEVER `None` for a destructive verb
// with unresolved/ambiguous operands (SG5).
enum class ShellCommandRisk : std::uint8_t { None, Confirm, Forbid };

// 89-D9: the text the guard classifies. Empty => non-shell tool, no opinion.
// shell          -> arguments["command"]
// terminal open  -> arguments["command"] or joined arguments["argv"]
// terminal write -> arguments["input"]
[[nodiscard]] std::string shell_guard_text(const PermissionRequest& request);

// 89-D1/89-D2..89-D9/89-D14/89-D17: pure, total, I/O-free. Never executes the
// command, never reads the filesystem, never resolves symlinks. `root` and
// `cwd` are canonical. `kernel_confinement == false` promotes ambiguous
// in-root outcomes to Confirm (89-D14).
[[nodiscard]] ShellCommandRisk classify_shell_command(
    std::string_view text,
    const std::filesystem::path& root,
    const std::filesystem::path& cwd,
    bool kernel_confinement);

// 89-D6: lexical containment. `single_quoted` suppresses $HOME expansion only.
[[nodiscard]] bool path_within_root(const std::filesystem::path& root,
                                    std::string_view candidate,
                                    bool single_quoted);

} // namespace ymh
```

```cpp
// include/ymh/execution/confinement.hpp (new; 89-D17 / L2)
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace ymh {

// 89-D14/L2: `Enforced` => L2.on; `Unavailable` => coupling fails to Confirm;
// `NotApplicable` => sandbox Unrestricted (danger-full-access disposition).
enum class ConfinementStatus : std::uint8_t { Enforced, Unavailable, NotApplicable };

// 89-D17: the per-spawn allow-list. `root` is the workspace root (RW+MAKE+
// REMOVE+EXEC); `home`/`tmpdir` are throwaway dirs; `ro_paths` get READ(+EXEC).
struct ConfinementPolicy {
    std::filesystem::path                      root;
    std::optional<std::filesystem::path>       home;
    std::optional<std::filesystem::path>       tmpdir;
    std::vector<std::filesystem::path>         ro_paths;
};

// 89-D17: probed once per environment, applied once per spawn.
class KernelConfinement {
public:
    virtual ~KernelConfinement() = default;
    // ABI/rights probe; cheap, side-effect-free (LANDLOCK_..._VERSION).
    [[nodiscard]] virtual ConfinementStatus probe() const = 0;
    // Called in the CHILD between fork and exec. On failure: `_exit(127)` +
    // status-pipe byte (never returns unconfined).
    virtual void apply_or_abort(const ConfinementPolicy&) = 0;
};

} // namespace ymh
```

```cpp
// include/ymh/policy/permission_policy.hpp (changed; 89-A5/89-D10)
class PermissionPolicy {
public:
    virtual ~PermissionPolicy() = default;
    virtual PolicyVerdict evaluate(const PermissionRequest&) const = 0;
    // 89-D10: additive; writes a bounded, echo-free reason when non-null.
    virtual PolicyVerdict evaluate(const PermissionRequest& request,
                                   std::string* reason) const {
        (void)reason;
        return evaluate(request);
    }
};

struct PermissionRequest {
    // ... unchanged ...
    bool destructive = false;
    bool force_ask   = false;
    bool kernel_confinement = false;   // 89-D17: set from confinementStatus()
};
```

```cpp
// src/policy/permission_policy.cpp (changed; 89-D1/89-D10/89-D14)
PolicyVerdict RulePermissionPolicy::evaluate(const PermissionRequest& request,
                                             std::string* reason) const {
    const std::string command = shell_guard_text(request);
    const ShellCommandRisk risk =
        command.empty()
            ? ShellCommandRisk::None
            : classify_shell_command(command, request.root, request.root,
                                     request.kernel_confinement);

    if (risk == ShellCommandRisk::Forbid) {           // L1 hard deny
        if (reason != nullptr) *reason = "shell-guard:outside-root";
        return PolicyVerdict::Deny;
    }
    if (request.sandbox == SandboxMode::ReadOnly && tool_is_mutating(request)) {
        return PolicyVerdict::Deny;
    }
    if (risk == ShellCommandRisk::Confirm) {          // L1 fail-to-ASK
        if (reason != nullptr) *reason = "shell-guard:unbounded-in-root";
        return PolicyVerdict::Ask;
    }
    if (request.force_ask) {
        return PolicyVerdict::Ask;
    }
    // ... existing rule fold unchanged ...
}
```

```cpp
// src/execution/process.cpp (changed; 89-A10 / L2)
// child body, after close_inherited_fds() (:74-95), before execvp():
if (request.confinement != nullptr) {
    confinement_->apply_or_abort(*request.confinement);   // _exit(127) on failure
}
// src/execution/pty.cpp: identical call in the child before execve (:490).
```

```cpp
// src/cli/wiring.cpp / src/cli/headless.cpp (new; 89-D16 / L6)
// Startup predicate: refuses a workspace whose canonical root is (or contains)
// $HOME / TMPDIR / sensitive paths. Non-nullopt => exit 2 before any turn.
[[nodiscard]] std::optional<std::string> clamp_or_refuse_root(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& home);
// Caller: the daemon/CLI startup path near effective_sandbox_mode
// (src/config/config.cpp:1753) / src/cli/wiring.cpp:263.
```

## 9. Invariants (SG1-SG17)

| ID | Invariant |
|---|---|
| **SG1** | L1 is total, deterministic, and I/O-free: it never executes the command, never reads or writes the filesystem, never resolves symlinks, never blocks, and never throws (89-D1, 09 Q1/Q5). |
| **SG2** | The L1 verdict is returned before `tool_defaults`, `default_verdict`, any `PolicyRule`, and any grant; no grant, `default = "allow"`, wildcard rule, or preset can change it (89-D1/89-D15). |
| **SG3** | A destructive verb whose target is provably outside `request.root` yields `Deny` (89-D6/89-D8). |
| **SG4** | Quoting does not hide a dangerous target: `rm -rf 'build' '~'` and `rm -rf build ~` classify identically; `~` is outside regardless of quoting (89-D2/89-D6). |
| **SG5** | **Fail-safe, strengthened:** `classify_shell_command` NEVER returns `None` for a destructive verb whose operands are unresolved or ambiguous (unquoted `$`, unreduced `$(...)`/backtick, brace expansion, broad/unknown glob, unknown cwd, truncated input, or L2 off). The fallback is always `Confirm` or `Forbid`, never `None`. |
| **SG6** | `terminal write` input is classified; a destructive command typed into a PTY is not silently allowed (89-D9). |
| **SG7** | An inside-root, bounded literal destructive command (`rm -rf build`) is not denied and not forced to ask **when L2 is on**; it falls through to the unchanged rule fold (89-D8/89-D14). |
| **SG8** | For any tool other than `shell` and `terminal`, `shell_guard_text` is empty and L1 returns `None`; no other tool's verdict changes (`skill_tool_usable` at `src/agent/workspace_runtime.cpp:228` is unaffected). |
| **SG9** | The L1 reason is a fixed, bounded, echo-free token; it never contains the command text, a path, or an argument (09 Q11). |
| **SG10** | Neither L1 nor L2 can be weakened by config at any layer: no key enables, disables, or downgrades them (89-D11/89-D15). |
| **SG11** | `command_pattern` and rule matching are unchanged; L1 is a separate extractor+classifier (89-D9/89-D12). |
| **SG12** | On adversarial input (empty, huge, thousands of tokens, unterminated quotes, deep nesting) L1 terminates in bounded time and bounded memory and never throws. |
| **SG13** | **Coupling:** `L1.None => L2.on`. An `Allow` is justified only when L1 returned `None` and L2 is `Enforced`; when L2 is `Unavailable`/`NotApplicable`, a call with a RECOGNIZED destructive verb is at least `Confirm` (89-D14). An unparsed interpreter payload (`None`) is out of claim (SG-F22). |
| **SG14** | L1 never claims confinement and L2 never claims parsing: L1 is advisory (bypassable by an interpreter), and the boundary is L2's resolved-path mediation. Neither is presented as sufficient alone (sec 1.2, 89-D1). |
| **SG15** | At startup, a workspace root equal to `$HOME` or an ancestor of `$HOME`/sensitive paths is REFUSED; no turn runs (89-D16). |
| **SG16** | L2 is applied only on the `{shell,terminal}` spawn path; MCP and LSP are explicitly not claimed covered (89-D18). |
| **SG17** | Never fail-open: with L2 off, no destructive verb returns a silent `Allow`; failure to apply L2 in the child is a spawn failure (`_exit(127)`), not an unconfined run (89-D14, 89-D17). |

## 10. Failure modes (SG-F#, F# tagged per 00 sec 54)

F# is the architecture review-finding class (`00-architecture.md:4832-4856`):
F1 path/process isolation, F2 background permission, F8 resource caps. Each
SG-F# maps to the class it belongs to -- path/isolation failures are F1,
fail-to-ASK and background-resolver failures are F2, resource/fd exhaustion is
F8.

| SG-F# | Failure | Detection | Required behavior | F# |
|---|---|---|---|---|
| **SG-F1** | `rm -fr build ~` (the incident) | `~` operand (89-D6 rule 2) | `Forbid` -> `Deny`; no spawn | F1 |
| **SG-F2** | `rm -rf /` / `rm -rf /tmp/x` | absolute outside root | `Forbid` | F1 |
| **SG-F3** | `rm -rf $HOME` / `rm -rf ${HOME}/x` | home expansion | `Forbid` | F1 |
| **SG-F4** | `rm -rf ../x` / `rm -rf ..` | `..` escapes root | `Forbid` | F1 |
| **SG-F5** | `rm -rf *` in root | broad in-root glob | `Confirm` (fail-to-ASK) | F1 |
| **SG-F6** | `dd if=/dev/zero of=/dev/sda` | `of=` block device | `Forbid` | F1 |
| **SG-F7** | `mkfs.ext4 /dev/sda` | `mkfs*` | `Forbid` | F1 |
| **SG-F8** | `:(){ :|:& };:` | fork-bomb structure | `Forbid` | F8 |
| **SG-F9** | `chmod -R 000 /etc` / `chown -R u /home/x` | recursive verb + outside operand | `Forbid` | F1 |
| **SG-F10** | `sudo rm -rf ~` / `env X=1 rm -rf ~` | wrapper stripped, then `~` | `Forbid` | F1 |
| **SG-F11** | a symlink inside root pointing outside, removed via a trailing-slash operand | L1 lexical only; L2 mediates the **resolved** path | L1 `None`; L2 blocks the outside unlink (EPERM). Without L2, normal policy; not claimed (SG-F22) | F1 |
| **SG-F12** | `bash -c 'rm -rf ~'` / `sh -lc "rm -rf /"` | nested shell depth 1 | `Forbid` | F1 |
| **SG-F13** | `echo $(rm -rf ~)` | command substitution depth 1 | `Forbid` | F1 |
| **SG-F14** | `terminal write` with `input = "rm -rf ~\n"` | 89-D9 extractor | `Forbid` | F1 |
| **SG-F15** | a destructive command inside three or more nested shells / an unterminated quote | `truncated = true` with a destructive verb present | `Confirm`, never `None` (SG5) | F1 |
| **SG-F16** | `rm -rf build` in root (legit build-clean) | verb `rm`, in-root bounded operand, L2 on | `None`; normal policy decides (SG7) | F1 |
| **SG-F17** | `mv x /outside` / `cp -r x /outside` | destination outside root | `Forbid` | F1 |
| **SG-F18** | `find / -delete` / `find ~ -delete` | `find` start path outside root | `Forbid` | F1 |
| **SG-F19** | `> /dev/sda` / `truncate -s 0 /etc/x` | redirection/truncate device or outside | `Forbid` | F1 |
| **SG-F20** | L2 `Unavailable` (5.15 LTS lacks `FS_TRUNCATE`; Docker seccomp blocks `landlock_create_ruleset`) and a recognized destructive in-root command | `confinementStatus() == Unavailable` | `Confirm` (fail-to-ASK); gate `subscribers_attached == false` or broker `subscriber_count == 0` -> `Deny`; never `Allow` (SG13/SG17) | F2 |
| **SG-F21** | workspace root `== $HOME` (or an ancestor of `$HOME`/sensitive) | canonical-path check at startup (`clamp_or_refuse_root`) | REFUSE at startup (exit 2); no turn runs (SG15/89-D16) | F1 |
| **SG-F22** | the no-token counterexample `python3 -c 'shutil.rmtree(os.path.expanduser("~"))'` | L1 sees no recognized destructive verb -> `None` | L2 blocks the outside write (EPERM); **with L2 off the rule fold decides (auto-approve -> `Allow`)** -- the genuinely unclaimed residual (sec 1.2/1.4). L1 does not claim to catch it. | F1 |
| **SG-F23** | a `{shell,terminal}` spawn that never passed `evaluate` (B1) | spawn census: `ToolRegistry::execute` (`tool_registry.cpp:323`) preceded by an `evaluate`; `agent_loop.cpp:990` is the sole caller and follows `:894-946`; both `PermissionBroker` (`headless.cpp:169-180`, `workspace_host.cpp:890-898`) and the gate call `evaluate` | B1 proof must pass; else the plan is unsound and halts (sec 14, 89-D18) | F1 |
| **SG-F24** | `apply_or_abort` fails in the child (partial ruleset) | status-pipe byte + child `_exit(127)` | spawn failure surfaced to the parent; no unconfined run (SG17) | F1 |
| **SG-F25** | fd smuggling into a `run`/`spawn` child | `close_inherited_fds` (`process.cpp:74-95`); all inheritable fds `O_CLOEXEC`. NOTE: the PTY child (`pty.cpp:474-494`) does NOT call `close_inherited_fds`; it closes only slave/master/status fds, so L2 must run after that and the wording is scoped to `run`/`spawn` | no inheritable fd beyond the status pipe in `run`/`spawn`; the guard never widens inheritance; the PTY gap is recorded (sec 14) | F8 |
| **SG-F26** | a startup file (`~/.bashrc`, `$BASH_ENV`) re-expands or re-executes | `--noprofile --norc` + `ProcessEnvMode::Minimal` (`process.cpp:576-579` clears `BASH_ENV`/`ENV`) | no profile/rc sourcing; throwaway `HOME` (89-A11/sec 5.7) | F1 |
| **SG-F27** | `danger-full-access` + L2 `Unavailable` | `master == true` and `confinementStatus() != Enforced` | safety is **not claimed** for recognized destructive calls (which are `Confirm` -> live prompt, or `Deny` with no subscriber) nor for L1-`None` recognized-in-root calls and unparsed payloads (which the rule fold may `Allow`) (sec 1.4/89-D15) | F2 |

## 11. dsh (DeepSeek Harness) mapping

| dsh surface | ymh mirror | Justification |
|---|---|---|
| per-call approval decision (allow/ask/deny) | `PermissionPolicy::evaluate` + the L1 classifier as its first branch | Mirror refinement: L1 computes the same approval one level deeper (per command), advisory. Anchor: `09-permissions.md:443-495` (evaluate algorithm), `src/policy/permission_policy.cpp:266-272`. |
| interactive confirmation for destructive actions | L1 `Confirm` -> `Ask` for in-root unbounded commands | Mirror: reuses the existing `Ask`/attention path (`src/policy/permission_policy.cpp:424-455`). Anchor: `25-ui-ux-errata.md:173` (25-A11). |
| robust shell parsing before approval | `classify_shell_command` (fail-safe) | Mirror: it is the parser 09 deferred, now with a never-`None`-on-ambiguity contract. Anchor: `09-permissions.md:1312-1317` (OQ-P5), `:1051`. |
| sandboxed tool execution / OS isolation | L2 Landlock confinement at the `{shell,terminal}` child | **Mirror (partial):** Landlock is the preferred mechanism; bwrap/`unshare` are fallbacks because Ubuntu 24.04's AppArmor unprivileged-userns restriction and external-binary/credential costs make bwrap unreliable. Anchor: `src/execution/process.cpp:369`, `src/execution/pty.cpp:466`. |
| append-only approval trace | the bounded L1 reason in the durable `PermissionDecision.reason` | Mirror: the loop appends the decision it already records (`src/agent/agent_loop.cpp:934-953`). Anchor: `09-permissions.md:751-782`. |
| a configurable "trust this workspace" opt-out for dangerous actions | **NON-MIRROR**: no key enables/disables L1 or L2 | Non-mirror reason: a disable switch reproduces the incident (its enabler was a config switch, `src/cli/wiring.cpp:143`); the workspace boundary is deliberately not configurable. The global-layer-only precedent (`26-dsh-alignment.md:1501-1507`) is recorded and deliberately not used. Anchor: `26-dsh-alignment.md:1501-1507`, `09-permissions.md:944-946` (Q7). |
| confinement of *all* tool subprocesses | **NON-MIRROR**: L2 covers `{shell,terminal}` only; MCP/LSP not claimed | Non-mirror reason: MCP servers are operator-configured external processes (`src/mcp/mcp_transport.cpp:176-205`) and LSP is deferred; extending confinement needs a per-tool capability declaration that does not exist yet. Recorded as tracked work, not silently assumed. Anchor: `89-D18`, `src/mcp/mcp_transport.cpp:176-205`. |
| kernel-level sandbox guarantees | **NON-MIRROR**: L2 is best-effort per host, fails to ASK | Non-mirror reason: no kernel guarantee exists on 5.15 LTS (no `FS_TRUNCATE`/ABI 3) or under Docker's default seccomp (blocks `landlock_create_ruleset`); the design degrades to `Confirm`, never to a false `Enforced`. Anchor: `09-permissions.md:1051` (unit ships no OS-sandbox guarantee), `include/ymh/config/config.hpp:161` (`kDangerFullAccessPreset`; unrestricted mapping `src/config/config.cpp:1680`/`:1763`). |

## 12. State-lifetime table

L1 classifier state is call-scoped. L2 adds process-lifetime and per-spawn
state. The only durable artifact remains the bounded reason in the decision.

| State | Type / where | Created | Destroyed / cleared | Owner | Survives process restart? |
|---|---|---|---|---|---|
| token vector | `std::vector<Token>`, local in `classify_shell_command` | per call | at call return | call frame | no |
| `single_quoted`/`double_quoted`/`truncated` flags | per-token / scan flags | per call | at call return | call frame | no |
| `cwd`, `cwd_known` | locals in the chain walk | per call (`cwd = root`) | at call return | call frame | no |
| recursion depth counter | `int`, local | per call | at call return | call frame | no |
| `ShellCommandRisk` | enum value | per call | consumed by `evaluate` | call frame | no |
| `PermissionRequest::kernel_confinement` | `bool` field | per call (set in the request-build block `src/agent/agent_loop.cpp:871-889`) | at call end | call frame | no |
| L1 guard reason token | `std::string` (out-param) | in `evaluate` | copied into `PermissionOutcome.reason`, then `payload::PermissionDecision.reason` | `evaluate` -> gate -> session | yes, as part of the durable decision (event log) |
| `ConfinementStatus` cache | `ConfinementStatus`, one per `LocalEnvironment` | once at environment construction (one `probe()`) | with the environment | `LocalEnvironment` | no (re-probed on daemon restart; the kernel/host may change) |
| `KernelConfinement` instance | object | with `LocalEnvironment` | with the environment | `LocalEnvironment` | no |
| `ConfinementPolicy` | value built per spawn (root, throwaway `home`/`tmpdir`, `ro_paths`) | in `ShellTool`/`TerminalTool` before `run`/`open` | at spawn call return | tool call frame | no |
| throwaway `HOME`/`TMPDIR` dirs | filesystem dirs | per spawn (sec 5.7) | removed after the spawn ends | tool call | no |
| `ProcessRequest::confinement` / `PtyRequest::confinement` | pointers | per call | at call return | call frame | no |
| `kMaxTokens`/`kMaxTokenBytes`/`kMaxDepth` | compile-time constants | n/a | n/a | n/a | n/a |

## 13. Test plan (hermetic)

No real LLM, no network. L2 kernel tests are skipped (not failed) on hosts where
`probe() == Unavailable`, but the Unavailable path is tested explicitly. Files:

- `tests/unit/errata89_shell_guard_test.cpp` -- L1 table-driven + `evaluate`
  precedence. Includes SG1-SG12 as small cases, the incident
  (`rm -fr build ~` -> `Forbid`), quoted variants, the depth/quote `Confirm`
  cases, positive controls (`rm -rf build` -> `None`, `mkdir`, `echo`), SG2
  (grant/wildcard/`default=allow` cannot override `Forbid`), SG7 (`rm -rf build`
  in-root falls through), SG8 (`skill` probe unaffected), SG9 (echo-free
  reason), and SG12 (empty, large, many-token, unterminated-quote, depth-64
  inputs terminate).
- `tests/unit/errata89_landlock_test.cpp` -- `probe()` semantics and the rights
  bitmask (require `FS_TRUNCATE`/ABI 3, `FS_IOCTL_DEV`/ABI 5); `Unavailable` on
  `ENOSYS`/`EPERM`.
- `tests/integration/errata89_l2_confinement_test.cpp` -- kernel-level oracle:
  under `Enforced`, a confined child attempting `unlink`/`truncate` of a path
  outside root gets `EPERM`; a path inside root succeeds.
- `tests/unit/errata89_clamp_test.cpp` -- 89-D16: root `== $HOME` refuses
  (exit 2); root an ancestor of `$HOME`/a sensitive path refuses; an in-root
  workspace is accepted.
- `tests/unit/errata89_l2_absent_test.cpp` -- the kernel-absent-L2 path: force
  `ConfinementStatus::Unavailable`, assert (a) destructive in-root
  `rm -rf build` -> `Confirm` (not `Allow`), (b) headless resolver -> `Deny`,
  (c) no silent `Allow` (SG13/SG17).
- `tests/integration/errata89_b1_spawn_census_test.cpp` -- B1: instrument
  `ToolRegistry::execute` (`src/tools/tool_registry.cpp:323`) and assert every
  `{shell,terminal}` execution is preceded by an `evaluate`; static half asserts
  `ToolRegistry::execute` is the sole `tool->execute` caller and
  `agent_loop.cpp:990` follows the decision block (`:894-946`); enumerates
  subagent (`src/tools/subagent_tools.cpp`), job/plan tools, the daemon resolver
  (`PermissionBroker`, `src/host/workspace_host.cpp:890-898`) and the headless
  resolver (`src/cli/headless.cpp:169-180`) -- all route through `agent_loop`
  (`:912-931`), and both resolvers call `evaluate`
  (`permission_broker.cpp:216`, `permission_policy.cpp:427`); confirms MCP is
  out of claim (89-D18).
- `tests/integration/errata89_b2_danger_test.cpp` -- B2: build `danger-full-access`
  (`master=true`, preset `include/ymh/config/config.hpp:161`), submit the
  incident command; assert `evaluate` returns `Deny` (outside root), never
  `Allow`; assert BOTH shipped resolvers never short-circuit on an `Ask` -- the
  SHIPPED `PermissionBroker` (`src/permission/permission_broker.cpp:216-227`; no
  subscriber -> `Deny`, `:226-227`) and the test-path `PermissionGate`
  (`src/policy/permission_policy.cpp:428` only on `Allow`); assert the headless
  path (`src/cli/headless.cpp:169-180`, no subscribers) -> `Deny` (89-D15).
- `tests/integration/errata89_canary_test.cpp` -- differential canary oracle
  (falsifier only, NOT a prover): run N destructive strings in an isolated
  tmpfs; any string L1 rates `None` whose canary shows an outside-root effect
  fails the test.
- `tests/unit/errata89_corpus_test.cpp` -- 20-item regression corpus
  (table-driven), not a defense claim.
- `tests/integration/errata89_l2_na_test.cpp` -- L2 oracle on a host WITHOUT the
  rights: `confinementStatus() == Unavailable` => no destructive in-root op is
  silently allowed (integration mirror of `errata89_l2_absent_test`).

Regression: `tests/unit/permissions_test.cpp`,
`tests/unit/errata46_permissions_test.cpp` (extended with the guard-`Ask` case),
and `tests/unit/permission_broker_test.cpp` must stay green. Failure-mode
coverage: SG-F1..SG-F27. Invariant coverage: SG1-SG17.

## 14. Recorded risks and limitations

1. **L1 is not a boundary.** It is advisory and cannot be made unbypassable
   (sec 1.2). Presenting it as the fix is false confidence; L2 is the boundary,
   and `Allow` requires both (89-D14).
2. **L2 is best-effort.** On 5.15 LTS (no `FS_TRUNCATE`) and under Docker's
   default seccomp (blocks `landlock_create_ruleset`) L2 is `Unavailable`, so a
   recognized destructive in-root command fails to `Confirm` (a live prompt, or
   `Deny` with no subscriber -- it never auto-proceeds); safety is not claimed
   (sec 1.4).
3. **`danger-full-access` residual risk.** The preset turns approval off (it
   only feeds `master`, `src/cli/wiring.cpp:143`); a recognized destructive call
   is still `Confirm` (never auto-approved), but an L1-`None`
   recognized-in-root call (`rm -rf build`) and an unparsed interpreter payload
   when L2 is off proceed. That is the genuine no-gates residual, and the spec
   says so loudly (89-D15, SG-F22, SG-F27).
4. **MCP/LSP out of scope.** No confinement is claimed for their subprocesses
   (89-D18). Their risk must not be described as bounded by this errata.
5. **Root near home.** 89-D16 refuses `root == $HOME`/ancestor at startup;
   `root` symlinked into `$HOME` is caught by canonicalization only if the
   symlink is resolved before the check (recorded; the check uses canonical
   paths).
6. **In-root recovery deferred.** W6 (overlay/snapshot for `rm -fr build src`) is
   deferred; recovery is not prevention (OQ-SG-4).
7. **PTY fd-inheritance gap.** `close_inherited_fds` (`process.cpp:74-95`)
   covers `run`/`spawn` only; the PTY child (`pty.cpp:474-494`) closes just its
   own slave/master/status fds. L2 runs after those closes, but the PTY child's
   inherited-fd surface is not covered by the same hygiene as `run`/`spawn`
   (SG-F25). Recorded; a follow-up may extend fd hygiene to the PTY child.

## 15. Open questions

- **OQ-SG-1 -- an operator escape hatch. [deferred]** Should a global-layer-only
  key disable L1/L2? Pinned default: **no key** (89-D11). Revisit only with a
  documented operator need; any such key must not be per-workspace.
- **OQ-SG-2 -- `rm` without `-r`/`-f`.** The catalogue guards any `rm` operand
  by containment; whether a plain in-root `rm` merits a `Confirm` is deferred
  (it stays `None`).
- **OQ-SG-3 -- MCP/skill shell coverage.** Extending L2/the guard to
  subprocess-backed non-shell tools is deferred; needs a per-tool capability
  declaration (89-D18).
- **OQ-SG-4 -- in-root recovery/overlay.** W6; only if W1-W5 land early.
- **OQ-SG-5 -- bundle OPEN reconciliation.** Resolved in 89-D15 (pin "guard
  `Confirm` is structurally not auto-approvable"; do not redefine the preset).
  Kept listed for reviewer traceability.

## 16. Revision log

- **Rev 3 (draft).** Fixes the Rev 2 gate (0 HIGH / 4 MEDIUM / 6 LOW;
  `/tmp/opencode/gate-89.md`). M1: B1/B2 now cite and test the SHIPPED
  `PermissionBroker` resolver (`permission_broker.cpp:216-227`,
  `workspace_host.cpp:890-898`, `headless.cpp:169-180`), not only the M1-only
  `PermissionGate` (89-A12, sec 4.8, 89-D15, test plan). M2: the
  `danger-full-access` anchor is corrected to `config.hpp:161` /
  `config.cpp:1680`/`:1763` (root cause and dsh row). M3: the honesty statement
  (sec 1.4, SG-F27, risk 2/3) now matches 89-D15 -- a `Confirm` never
  auto-proceeds; only L1-`None` recognized-in-root calls and unparsed payloads
  proceed. M4: sec 10 carries 00 sec 54 F# tags. L1: state-lifetime/symbol
  caller line fixed to `agent_loop.cpp:871-889`. L2: `BASH_ENV`/`ENV` seam now
  references `ProcessEnvMode::Minimal` (`process.cpp:576-579`). L3: fd-hygiene
  wording scoped to `run`/`spawn`; PTY gap recorded (SG-F25, risk 7). L4: 46
  anchor corrected to `:3062`/`:3498`. L5: D14/SG13 scoped to recognized verbs;
  SG-F22 corrected. L6: `clamp_or_refuse_root` added to the symbol table and
  sketches with a concrete caller. Not yet reviewed.
- **Rev 2 (draft).** Applies the approved plan's W0 edits. Strikes
  "unbypassable" (89-D1) and replaces it with the fail-safe contract never
  returning `None` for a destructive verb with unresolved/ambiguous operands
  (SG5). Adds L2 kernel confinement (sec 5: Landlock > bwrap > unshare, rights
  allow-list, ABI probe requiring `FS_TRUNCATE`/ABI 3 and `FS_IOCTL_DEV`/ABI 5,
  Docker-seccomp/`ENOSYS`/`EPERM` => Unavailable, spawn-seam apply, fd hygiene,
  throwaway `HOME`/`TMPDIR`, `--noprofile --norc`, deployment caveats, MCP/LSP
  not claimed). Adds 89-D14 (coupling), 89-D15 (B2 disposition, bundle OPEN
  reconciled), 89-D16 (clamp/REFUSE), 89-D17 (confinement interface),
  89-D18 (scope). Adds SG13-SG17, SG-F20-SG-F27, amendment rows A9-A12, the L2
  dsh rows, confinement state-lifetime rows, and the ten named test files.
  Reconciles the loud `danger-full-access` + no-L2 honesty statement (sec 1.4).
  Not yet reviewed.
- **Rev 1 (draft).** Initial errata (classifier only; claimed "unbypassable",
  now struck). Superseded 09 OQ-P5 and the sec 10 omission line.
