# 88 - Doctor Ownership Integrity Errata: flagging cross-workspace owner corruption

```
Status: verified (Rev 2)
Revision: 2
Verification status: adversarial reviewer (independent, Rev 2): GATE PASS
           (0 HIGH / 0 MEDIUM, 4 LOW); Oracle confirm PASS
Component: 88 (errata) -- adds a read-only `ymh doctor` diagnostic and an
           explicit `ymh workspace reap` remediation that detect and (only on
           explicit user action) clear a registry `workspaces.host_pid` claim or
           a sidecar `sessions.lock` lease whose recorded owner pid is NOT the
           workspace's daemon -- a pid whose `/proc/<pid>/cmdline --root` names a
           different workspace root. Closes the observability gap behind the
           louvre incident (a row naming a bb_tedi daemon). Amends
           03-workspace-registry.md (sec 6.3/6.4/6.5, R5/R6/R10/R11),
           04-workspace-host-daemon.md (sec 5.2/6.2 by reference) and
           16-daemon-ownership.md (sec 1.3.1, O11) by reference only.
Depends on: 03-workspace-registry.md (verified; `host_pid`/`reapHost`/scan),
            04-workspace-host-daemon.md (verified; daemon argv),
            16-daemon-ownership.md (verified; never kill a live daemon),
            76-host-startup-residue-recovery-errata.md (verified; the startup
            probe whose target the corruption misdirects)
Supersedes: nothing. This errata is purely additive: it adds a diagnostic
            command and a narrow, gated remediation. It does NOT change the
            ownership model (16 O1..O22), the liveness contract (03 R5/R6), or
            any existing startup/reap path. `ymh workspace reap` reuses the
            existing `WorkspaceRegistry::reapHost` gate (lock-absence, R5) and
            adds no new mutation semantics to the registry.
Scope: (a) the `ymh doctor` CLI surface, its checks, output, exit codes, and
       invariants; (b) the `ymh workspace reap` remediation and its
       confirmation contract; (c) new seams (`diagnose` with an injected path
       canonicalizer, `socketOwnerPid`, a non-creating lock probe, a
       `readProcessTableChecked` availability report). It does NOT design
       `ymh workspace stop` (already shipped), `/sessions`, the switcher, any
       wire method, any event, or any `registry.db` schema change.
```

## 1. Purpose, scope, and precedence

### 1.1 The incident (root cause)

A bare `ymh` in workspace `.../bb_tcat/louvre` failed with
`cannot create session: host connection: write: Broken pipe`. The read-only trace
(`/tmp/opencode/oracle-76.md`) found that the workspace's ownership state named
the wrong daemon: the registry row's `workspaces.host_pid` **and** the sidecar
`sessions.lock` diagnostic both recorded pid `4117802`, whose
`/proc/4117802/cmdline` was a `ymh --host ... --root .../bb_tedi` process -- a
**different workspace's daemon**. The startup probe (`HostLifecycle::ensureRunning`,
`src/host/workspace_host.cpp:1699-1740`, `probeLive`) then attached against the
wrong target, because it trusts the recorded owner pid/socket without corroborating
that the pid is *this* workspace's daemon.

The daemon's argv is fully self-describing (`ymh --host --workspace <uuid>
--root <canonical-root> --socket <path> [...]`, built by
`ForkExecLauncher::build_argv`, `src/host/workspace_host.cpp:1379-1399`; usage at
`src/cli/cli.cpp:223-226`). That makes the corruption detectable without any
protocol handshake: canonicalize the recorded pid's `--root` and compare it to the
row's `canonical_path`. There is currently **no command that performs this
comparison**, so a user has no way to discover the corruption short of reading
`/proc` by hand.

### 1.2 What this spec covers

1. A new read-only command `ymh doctor` that scans every registered workspace and
   flags mismatches between the recorded owners (registry `host_pid`, sidecar lease
   diagnostic `pid`) and reality (`/proc/<pid>/cmdline`, the socket file's
   inode owner).
2. A new explicit remediation `ymh workspace reap <target>`, which clears a stale
   registry host claim **only when the sidecar lock is free** (reusing the shipped
   `reapHost` gate) and **only after a user confirmation** (`--yes` for scripts).
3. The pure seams and a hermetic test plan.

### 1.3 Non-goals (explicit)

- **No kill.** Neither command sends a terminating signal; a foreign or stale
  daemon is stopped only by the user via the existing `ymh workspace stop`. This
  preserves 03 R11 ("a scan never kills"), `include/ymh/registry/process_scan.hpp:6-9`,
  and 16 O11 ("never kill a live daemon").
- **No automatic mutation.** `ymh doctor` is read-only (88-D1). It *suggests* a
  remediation string; it never runs it.
- **No wire method, no event, no schema change.** Doctor is CLI/registry-local.
- **No forcing of a held lock.** A held `sessions.lock` means a live daemon owns
  the workspace (kernel truth, 03 R5/R6); `reap` refuses. Clearing while held is
  out of scope and unsafe (16 O11).
- **No hand-launched-daemon authority.** `ForkExecLauncher::spawn` always mints a
  boot id and passes it: `boot_id = config.boot_id.value_or(generate_uuid_v4())`,
  `effective.boot_id = boot_id`, then `build_argv(effective, ...)`
  (`src/host/workspace_host.cpp:1418-1422`), and `build_argv` emits `--boot-id`
  whenever the config carries one (`:1394-1397`; contract "supervisor
  mint-and-pass", `include/ymh/host/workspace_host.hpp:102`). So every
  supervisor-spawned daemon carries `--boot-id` and the check is always
  available. A daemon started by hand without `--boot-id` has no argv boot id;
  doctor then skips that check rather than guessing (88-D3; DOC9).

### 1.4 Precedence

This errata is subordinate to `00-architecture.md`, `03`, `04`, `05`, and `16`.
Where any statement here could be read as loosening 03 R5/R6 or 16 O11, the
baseline wins and this spec is in error. The liveness contract is unchanged:
"the sidecar flock is the only authoritative liveness signal; `kill(pid, 0)` and
heartbeat staleness are hints only and are never sufficient to clear a claim"
(`include/ymh/registry/liveness.hpp:4-9`).

## 2. Amendment register

| # | Target | Change | Kind |
|---|---|---|---|
| 88-A1 | `include/ymh/cli/cli.hpp` | add `Command::Doctor`; add `doctor_json` (bool); add `workspace_yes` (bool) | additive |
| 88-A2 | `src/cli/cli.cpp` parse/dispatch | register `doctor`; register a `workspace reap <target>` subcommand + `--yes`; add a `reap` arm to the `Command::Workspace` dispatch (beside the `stop` arm, `src/cli/cli.cpp:1422-1427`) calling `run_workspace_reap(workspace_args, workspace_yes, out, err)`; add `Doctor` to the non-config branch of `command_loads_config` (`src/cli/cli.cpp:166-184`). The `--yes`/non-tty confirmation is handled inside `run_workspace_reap` (DOC8) | additive |
| 88-A3 | `include/ymh/registry/liveness.hpp`, `src/registry/liveness.cpp` | add `probeWorkspaceLock(path, bool create_if_missing)`; keep the 1-arg overload delegating `true` (no behaviour change for existing callers) | additive overload |
| 88-A4 | `include/ymh/registry/process_scan.hpp`, `src/registry/process_scan.cpp` | add `socketOwnerPid(path)` (`/proc/net/unix` + `/proc/<pid>/fd`) and `ProcessTable` + `readProcessTableChecked()` (availability; 88-A4 seam for 88-D9 exit 3); `readProcessTable()` becomes a thin wrapper returning `.entries` (no contract change) | additive |
| 88-A5 | new `include/ymh/registry/doctor.hpp`, `src/registry/doctor.cpp` | the injectable check core + renderers | new |
| 88-A6 | new `include/ymh/registry/doctor_cli.hpp`, `src/registry/doctor_cli.cpp` | `run_doctor` (the `ymh doctor` driver) **and** `run_workspace_reap` | new |
| 88-A7 | `src/registry/workspace_cli.cpp` (`run_workspace_command`) | **no change.** `reap` is dispatched in `cli.cpp` (88-A2) exactly like `stop`, so `run_workspace_command`'s shipped signature/contract is untouched | none |
| 88-A8 | `tests/CMakeLists.txt` | add `unit/errata88_doctor_test.cpp` | additive |

No existing symbol's contract changes. `run_workspace_command`'s signature is
untouched (88-A7); `reap` is routed in `cli.cpp` beside `stop`. The 1-arg
`probeWorkspaceLock` keeps creating the lock file (`O_RDWR|O_CREAT`,
`src/registry/liveness.cpp:19`) for all callers; only doctor uses the
non-creating overload (88-D5). `readProcessTable()` keeps returning
`std::vector<ProcessEntry>`; `readProcessTableChecked()` is the additive
availability seam (88-A4).

## 3. Decision register (`88-D#`)

- **88-D1 -- `ymh doctor` is read-only; `ymh workspace reap` is the only
  mutation.** Detection and remediation are separate commands so that "flag"
  and "fix" can never be confused. Doctor creates no file, writes no row, sends
  no socket byte, and sends no signal at all (the check model never calls
  `kill`; process existence comes from `/proc`, DOC2/L7).
- **88-D2 -- The corruption predicate, via an injected canonicalizer.**
  `diagnose` is pure over its arguments; every filesystem-touching step is
  supplied from outside. Path canonicalization is the one such step and is an
  injected `Canonicalizer` (`std::function<std::optional<path>(const path&)>`):
  production injects a `std::filesystem::canonical` wrapper in `run_doctor`,
  tests inject an identity or a stub map (88-D2 fixes M1). For a recorded owner
  pid `P` of workspace `w`, doctor canonicalizes `P`'s `--root` from
  `/proc/P/cmdline` through the injected canonicalizer and compares it to
  `w.canonicalPath`. A mismatch is `root-mismatch` (HIGH); the observed root is
  reported so the user sees *which* workspace's daemon was recorded. Two further
  argv cross-checks (`--workspace` == `w.id`, `--socket` == the recorded socket)
  graduate to `workspace-id-mismatch` / `socket-arg-mismatch`.
- **88-D3 -- Both recorded owners are checked independently.** The registry
  `host_pid` (`WorkspaceRecord::host`, `include/ymh/registry/registry.hpp:70`) and
  the sidecar diagnostic `pid` (`probeWorkspaceLock(...).pid`,
  `include/ymh/registry/liveness.hpp:20-25`) are checked as owners; a
  disagreement between them is itself a finding (`lease-pid-mismatch`). The two
  are distinct records and may be corrupted separately. `--boot-id` is compared
  to `host_boot_id` on **both** owners when argv carries it: for the registry
  owner (`boot-mismatch`, C4b) and for the lease owner (`lease-boot-mismatch`,
  C8c). Since the supervisor always mints and passes `--boot-id`
  (`src/host/workspace_host.cpp:1418-1422`), this check is always available for
  supervisor-spawned daemons.
- **88-D4 -- Only the kernel flock is authoritative.** `WorkspaceLockProbe::held`
  (`src/registry/liveness.cpp:23-32`) is the sole authoritative liveness bit;
  the diagnostic `pid`/`bootId` are self-reported and non-authoritative
  (`include/ymh/registry/liveness.hpp:8-9`). Doctor therefore *flags* mismatches
  but never clears a claim; clearing is `reap`, gated on `held == false`.
- **88-D5 -- Doctor must not create the lock file.** The shipped
  `probeWorkspaceLock` opens with `O_RDWR | O_CREAT`
  (`src/registry/liveness.cpp:19`), which would create `<ws>/.ymh/sessions.lock`
  merely by running a diagnostic. Doctor uses the additive
  `probeWorkspaceLock(path, /*create_if_missing=*/false)`: `O_RDONLY` (no
  `O_CREAT`). An absent lock file yields `held == false` without side effects.
  `flock(LOCK_EX|LOCK_NB)` is valid on an `O_RDONLY` fd on Linux.
- **88-D6 -- Doctor does not connect to any socket.** Socket-owner identity is
  obtained from `/proc/net/unix` (path -> inode) cross-referenced with
  `/proc/<pid>/fd` (`socket:[<inode>]` symlinks) -- pure file reads (88-A4). An
  `SO_PEERCRED` probe is deliberately not offered: `connect(2)` wakes the peer
  and is a side effect one should not take merely to *diagnose* (see R5).
- **88-D7 -- Reap is lock-gated and column-narrow.** `ymh workspace reap`
  resolves a single target, re-probes the sidecar lock, and delegates to the
  shipped `WorkspaceRegistry::reapHost` (`src/registry/registry.cpp:1187-1210`),
  which clears only `host_pid/host_boot_id/host_socket/host_heartbeat` and only
  when the lock is free. If the lock is held, reap refuses and prints the doctor
  remediation hint. Reap is standalone: `run_workspace_reap(args, assume_yes,
  out, err)` is dispatched in `cli.cpp` beside `run_workspace_stop`
  (`src/cli/cli.cpp:1422-1427`); `run_workspace_command` is unchanged (88-A7).
  Note: `reapHost`'s gate calls the **1-arg** `probeWorkspaceLock`, which
  `O_CREAT`s the lock file if absent (L5, sec 10) -- a reap of a row whose lock
  was deleted recreates an empty lock file as a side effect.
- **88-D8 -- Output is deterministic and scriptable.** Rows in
  `canonical_path ASC` order (matching `listWorkspaces`,
  `src/registry/registry.cpp:813-826`); findings within a row in the frozen check
  order of sec 4. Human output is tab-separated; `--json` emits schema v1
  (sec 6).
- **88-D9 -- Exit codes** (sec 5.3): doctor `0` clean / `1` findings / `2`
  usage-or-unknown-target / `3` registry-or-`/proc` unavailable; reap `0`
  cleared-or-nothing-to-do / `1` refused (lock held or declined) / `2`
  usage-or-unknown-target / `3` registry unavailable. This **deliberately
  diverges** from the shipped `ymh workspace stop`, which collapses
  unknown-target and registry-unavailable into `1` (`src/cli/cli.cpp:333-356`):
  doctor/reap separate findings / caller error / infrastructure so scripts can
  branch (L4).
- **88-D10 -- `clean` means no finding at severity >= MEDIUM.** LOW-only reports
  (a missing `/proc/net/unix`, an unreadable diagnostic) are informational and do
  not flip the exit code or the `clean` flag.
- **88-D11 -- The scan is bounded and single-pass.** One
  `readProcessTableChecked()` (`src/registry/process_scan.cpp:100-131`); one
  registry `listWorkspaces()`; at most one `probeWorkspaceLock` and one
  `socketOwnerPid` per row. No recursion beyond the one `/proc/net/unix` parse.
- **88-D12 -- Doctor never terminates a process, and sends no signal at all.**
  Doctor does not call `kill` in any form; process existence comes from the
  `/proc` table (C1), not `killHint` (`src/registry/liveness.cpp:55-63`).
  `killHint` remains a shipped helper used elsewhere; doctor does not use it
  (L7/DOC2).
- **88-D13 -- An unknown is not a finding.** If `/proc/<pid>/cmdline` is
  unreadable (permissions), `/proc/net/unix` is absent, or a canonicalization
  fails, the check yields `unknown` / `socket-owner-unknown` (LOW), never a
  fabricated mismatch.
- **88-D14 -- Remediation for a held foreign lock is `workspace stop`, never a
  pid kill.** When the lock is held (so reap refuses) and the owner is foreign,
  doctor names the owning workspace (resolved from the observed `--root` against
  the registry) and suggests `ymh workspace stop <that-id>`, the shipped,
  owner-aware command (`src/cli/cli.cpp:330-389`). If the observed root maps to no
  registered workspace, doctor says so and suggests the path be registered first.

## 4. The check model

Doctor builds one `WorkspaceObservation` per row (sec 7): the row, its sidecar
lock probe (non-creating), and the socket-inode owner pid (`nullopt` = unknown).
It takes one `ProcessTable` (from `readProcessTableChecked()`), an injected
canonicalizer, and evaluates the checks below in this frozen order; every check
that fires on a row is emitted (a row may have several findings). The core does
no I/O of its own (M1): process lookup is table reads, and path canonicalization
is the injected `canon` (88-D2).

Reality inputs:
- `table.complete` = whether `/proc` was fully readable (`readProcessTableChecked`,
  88-A4). When false, no process is observable: every pid-dependent check
  (C1..C5, C4b, C9, C10) is skipped and yields `process-table-unavailable` (C12,
  LOW); exit 3 (88-D9/M5).
- `P_reg` = `record.host->pid` (registry), else none.
- `P_lease` = `probe.pid` (sidecar diagnostic), else none.
- `argv(P)` = `table.entries` entry for pid `P` (from `/proc/P/cmdline`), else
  absent.
- `root(P)`, `wid(P)`, `sock(P)`, `bid(P)` = the `--root`, `--workspace`,
  `--socket`, `--boot-id` values from `argv(P)`.
- `P_sock` = `socketOwnerPid(socketPath)` (the pid holding the socket inode), else
  unknown.
- `canon(x)` = the injected canonicalizer, production = a
  `std::filesystem::canonical` wrapper; on failure, `unknown` (tests inject an
  identity/stub, so synthetic fixture roots canonicalize deterministically).

`state` per row: `corrupt` when at least one emitted finding is >= MEDIUM, else
`not-running` when `record.host == nullopt` (registered, no daemon), else
`clean`. `state` is a summary; findings are the substance.

| # | Code | Fires when | Severity |
|---|---|---|---|
| C0 | (no code) | `record.host` is `nullopt` (no claim): checks C1..C5 need a recorded owner and do not run. The row is `not-running` and emits nothing **unless** C9 fires. | (none) |
| C1 | `dead-pid` | `table.complete` and `P_reg` set and `/proc/P_reg` absent (process gone). | MEDIUM |
| C2 | `foreign-pid` | `table.complete` and `P_reg` set and `/proc/P_reg` present but `is_ymh_host_process(argv(P_reg))` is false (`src/registry/process_scan.cpp:39-50`). | HIGH |
| C3 | `root-mismatch` | `P_reg` set and a `ymh --host` process, but `canon(root(P_reg))` != `record.canonicalPath` (canonicalization succeeded and differs). | HIGH |
| C4 | `workspace-id-mismatch` | `P_reg` set, `ymh --host`, and `wid(P_reg)` != `record.id.value`. | HIGH |
| C4b | `boot-mismatch` | `P_reg` set, `ymh --host`, `bid(P_reg)` present in argv, and `bid(P_reg)` != `record.host->bootId.value` (the recorded pid is a different daemon instance than the row claims -- the pid-reuse signal, M2). | MEDIUM |
| C5 | `socket-arg-mismatch` | `P_reg` set, `ymh --host`, `record.host->socketPath` non-empty, `sock(P_reg)` present and `canon(sock(P_reg))` != `canon(record.host->socketPath)`. | MEDIUM |
| C6 | `lease-missing` | `P_reg` set, `record.host` present, but `probe.held == false` (registry claims a daemon that the kernel says is not live). | MEDIUM |
| C7 | `lease-pid-mismatch` | `probe.held == true` and `P_lease` present and `P_lease != P_reg`. | MEDIUM |
| C8a | `lease-unreadable` | `probe.held == true` but the diagnostic did not parse (`probe.pid == nullopt`). | LOW |
| C8b | `lease-root-mismatch` | `P_lease` present and `is_ymh_host_process(argv(P_lease))` and `canon(root(P_lease))` != `record.canonicalPath` and canonicalization succeeded. This is the *lease-side* form of C3 -- the exact check that catches the louvre case when the lock diagnostic is the corrupt record. | HIGH |
| C8c | `lease-boot-mismatch` | `probe.held == true`, `bid(P_lease)` present in argv, and `bid(P_lease)` != `record.host->bootId.value`. | MEDIUM |
| C9 | `orphan-daemon` | `table.complete` and a live `ymh --host` process exists whose `--root` matches `record.canonicalPath` (or `--socket` matches the recorded socket) but whose pid matches neither `P_reg` nor `P_lease`. Emitted once per orphan. | MEDIUM |
| C10 | `socket-owner-mismatch` | `table.complete` and `P_sock` known and `P_reg` set and `P_sock != P_reg` (the socket is actually held by a process other than the one recorded). | HIGH |
| C11 | `socket-owner-unknown` | `record.host->socketPath` non-empty and the socket file exists, but `P_sock` is unknown (`/proc/net/unix` absent/unparseable/entry not found). | LOW |
| C12 | `process-table-unavailable` | `table.complete == false`: `/proc` could not be read, so C1..C5/C4b/C9/C10 cannot run. Emitted once per row; the process checks are skipped, never guessed. | LOW (exit 3) |

Notes on the check set:
- C3 is the load-bearing check for the incident. C8b is its lease-side twin.
- C2's `is_ymh_host_process` requires `--workspace` and `--socket` to be present
  and non-empty (`process_scan.cpp:48-49`); a `ymh --host` process missing those
  is not a host and is treated as `foreign-pid`.
- C3/C4/C5/C4b do not compare `--config` or `executable` beyond the basename
  check in C2; those are non-identifying. `--boot-id` is compared by C4b/C8c.
- C6 is *not* redundant with C1: the lock can be free while the pid still
  exists (the daemon is draining, or a stale claim points at an unrelated live
  process that happened to reuse the pid).
- C2 (`foreign-pid`) yields **no** observed root: a non-host process carries no
  `--root`, so `root(P)`/`observedRoot` are empty (M6). Its remediation is
  therefore *not* the root-keyed one (sec 4.2).
- The rows below are evaluated in order; a row may fire several. The
  `probe.held == false` / `held == true` qualifier on each suggestion is
  resolved at emit time from the same `probe`, so a row never emits two
  contradictory `fix:` strings.

### 4.1 Severity and `clean`

`clean == (no finding with severity >= MEDIUM)`. HIGH and MEDIUM both make doctor
exit `1` (88-D9). LOW findings are printed but do not affect the exit code
(severities are Low/Medium/High only; there is no `Ok`/`Info`, L3).

### 4.2 Remediation suggestions (printed, never run)

| Fired code(s) | Suggested command |
|---|---|
| `dead-pid` / `lease-missing` / `lease-pid-mismatch` / `lease-unreadable` / `lease-boot-mismatch` / `boot-mismatch` and `probe.held == false` | `ymh workspace reap <id> --yes` |
| `foreign-pid` and `probe.held == false` | `ymh workspace reap <id> --yes` (a non-host pid can carry no observed root; the stale claim is cleared once the lock is free) |
| `foreign-pid` and `probe.held == true` | `ymh workspace stop <id> --force` (a live lock blocks reap; the owner-aware stop releases it, then reap the stale claim) |
| `root-mismatch` / `lease-root-mismatch` / `socket-owner-mismatch` / `workspace-id-mismatch` (observed root known) | if `canon(root(P))` matches a registered row `r != w`: `ymh workspace stop <r.id> --force`; else: `ymh workspace add <observed-root>` |
| `dead-pid` / `root-mismatch` / `socket-owner-mismatch` while `probe.held == true` | `ymh workspace stop <id> --force` (a live lock blocks reap; stop is owner-aware, 16 sec 4.6) |
| `orphan-daemon` | `(no action; the next \`ymh\` re-attaches to it)`, or `ymh workspace stop <id> --force` to tear the orphan down. Reap is not offered here: there is no registry claim to clear. |
| `socket-arg-mismatch` | `ymh workspace add <observed-root>` if unregistered, else `ymh workspace stop <id> --force` |
| `process-table-unavailable` | `(none; /proc is unreadable -- rerun with a readable /proc)` |

Every suggestion that interpolates a path/root is emitted **only** when that
value is known and non-empty (M6). A finding with no derivable command carries an
empty `remediation`. The suggestion is a string in the finding; doctor does not
execute it (88-D1).

## 5. CLI surface

### 5.1 `ymh doctor`

```
ymh doctor [--json] [--workspace <workspace-id|path>]
```

- No flags: scan every registered workspace.
- `--workspace <target>`: restrict to one row; resolved by `findById` then, on
  miss, `findByCanonicalPath` (the same resolution as `workspace stop`,
  `src/cli/cli.cpp:345-356`). Unknown target: usage error (exit 2), message on
  stderr.
- `--json`: emit JSON (sec 6.2) instead of the human report.
- Doctor takes no `--force`, no `--fix`, no `--reap`. There is nothing to force.

Registered in `parse_cli` (`src/cli/cli.cpp:1148-1209`) as a top-level subcommand
`doctor`; `CliInvocation::Command::Doctor`; the options land in
`invocation.workspace` (reuse) and a new `invocation.doctor_json` bool (88-A1;
the flag is registered with `add_common(*doctor, invocation)` plus
`doctor->add_flag("--json", invocation.doctor_json, ...)`). Doctor is added to the
**false** branch of `command_loads_config` (sec 88-A2): it needs no layered
config, only `default_registry_config()`.

### 5.2 `ymh workspace reap`

```
ymh workspace reap <workspace-id|path> [--yes]
```

- Resolves one target (id, then canonical path). Unknown target: usage error
  (exit 2), message on stderr.
- Prints the doctor findings for that target, then:
  - if `probe.held == true`: refuse, exit 1, hint `ymh workspace stop <id>`.
  - else prompt `clear host claim for <id>? [y/N] `; accept only `y`/`yes`.
    `--yes` skips the prompt. A non-tty stdin without `--yes` refuses (mirrors
    `workspace_stop_may_proceed`'s scripted-use rule, `include/ymh/cli/cli.hpp:77-83`).
  - on confirmation, call `registry->reapHost(id)`; print `reaped <id>` when
    `true`, `nothing to reap <id>` when `false` (already clear), and exit 0.
- **Wiring (M4).** Reap is routed exactly like the shipped `workspace stop`
  (`src/cli/cli.cpp:1422-1427`), not through `run_workspace_command`:
  `parse_cli` maps the `reap` subcommand to `workspace_args == {"reap",
  "<target>"}` and sets the new `CliInvocation::workspace_yes`; the
  `Command::Workspace` dispatch case gains an arm
  `if (workspace_args[0] == "reap") return run_workspace_reap(workspace_args,
  invocation.workspace_yes, out, err);` beside the existing `stop` arm.
  `run_workspace_command`'s signature and the `{add,list}` arms are untouched
  (88-A7). The concrete caller of `run_workspace_reap` is this dispatch arm.

### 5.3 Exit codes

| Code | `ymh doctor` | `ymh workspace reap` |
|---|---|---|
| 0 | clean (no >= MEDIUM finding) | claim cleared, or nothing to do |
| 1 | at least one >= MEDIUM finding | refused (lock held, or declined) |
| 2 | usage error / unknown `--workspace` target | usage error / unknown target |
| 3 | registry or `/proc` unavailable (C12) | registry unavailable |

An **unknown target** is a caller error and is exit `2` for both commands
(M3). This is a **deliberate divergence** from the shipped `ymh workspace stop`,
which returns `1` for both an unknown workspace and an unavailable registry
(`src/cli/cli.cpp:333-356`); doctor/reap split "caller error" (`2`) from
"infrastructure unavailable" (`3`) so scripts can branch (88-D9/L4).

`ymh doctor` never exits `1` merely because a LOW finding (e.g.
`socket-owner-unknown`, `process-table-unavailable`) is present; exit `3` is
reserved for the case where the process table is unavailable (C12), which is
infrastructure, not a finding.

## 6. Output format

### 6.1 Human (default, ASCII)

One block per workspace, in `canonical_path ASC` order:

```
<id>\t<canonical_path>\t<state>
  <SEVERITY>\t<code>\t<detail>
    fix: <remediation>
```

- `<state>` is `clean`, `not-running`, or `corrupt` (any >= MEDIUM finding).
- `<SEVERITY>` is `HIGH`, `MEDIUM`, or `LOW` (the only three severities).
- `<detail>` is a single line, e.g.
  `recorded pid 4117802 is a ymh host for root /home/u/prjs/bb_tedi (expected /home/u/prjs/louvre)`.
- `fix:` lines appear only when a remediation string exists.
- After all rows: `doctor: <N> workspaces, <H> high, <M> medium, <L> low` on one
  line. Empty registry: `doctor: 0 workspaces` then newline.

Example:

```
a1b2...\t/home/u/prjs/louvre\tcorrupt
  HIGH\troot-mismatch\trecorded pid 4117802 is a ymh host for root /home/u/prjs/bb_tedi (expected /home/u/prjs/louvre)
    fix: ymh workspace stop 9f3c... --force
  MEDIUM\tlease-pid-mismatch\tregistry host_pid 4117802 != lease diagnostic pid 4201234
doctor: 1 workspaces, 1 high, 1 medium, 0 low
```

### 6.2 `--json` (schema v1)

```json
{
  "schema": 1,
  "clean": false,
  "process_table_complete": true,
  "counts": {"high": 1, "medium": 1, "low": 0},
  "workspaces": [
    {
      "id": "a1b2...",
      "canonical_path": "/home/u/prjs/louvre",
      "state": "corrupt",
      "recorded_pid": 4117802,
      "host_boot_id": "c0de...",
      "socket": "/home/u/prjs/louvre/.ymh/host.sock",
      "lease": {"held": true, "diagnostic_pid": 4117802, "diagnostic_boot_id": "c0de..."},
      "socket_owner_pid": 4117802,
      "findings": [
        {
          "code": "root-mismatch",
          "severity": "high",
          "recorded_pid": 4117802,
          "observed_pid": 4117802,
          "observed_root": "/home/u/prjs/bb_tedi",
          "detail": "recorded pid 4117802 is a ymh host for root /home/u/prjs/bb_tedi (expected /home/u/prjs/louvre)",
          "remediation": "ymh workspace stop 9f3c... --force"
        }
      ]
    }
  ]
}
```

- `code` tokens are exactly the C1..C12 code strings (C0 emits no finding);
  `severity` is lowercase (`high`/`medium`/`low`).
- `recorded_pid`/`observed_pid` are `0` when none; `observed_root` is omitted
  when unknown; `lease` is `{"held": false}` when the lock is free; a `fix:`
  line and the JSON `remediation` key are omitted when no command is derivable;
  keys are omitted rather than emitted empty.
- `process_table_complete` is `false` exactly when exit code is `3` (C12); the
  per-workspace `findings` then carry `process-table-unavailable`.
- `ymh doctor --json` prints exactly one JSON document on stdout; diagnostics go
  to stderr.

## 7. Interface sketches (C++)

```cpp
// include/ymh/registry/liveness.hpp (88-A3; additive overload)
//
// Doctor uses create_if_missing == false so that merely diagnosing a workspace
// creates no <ws>/.ymh/sessions.lock. The 1-arg overload keeps the shipped
// O_RDWR|O_CREAT behaviour and is unchanged for existing callers
// (src/host/workspace_host.cpp, src/registry/registry.cpp reapHost, spec 76).
[[nodiscard]] WorkspaceLockProbe probeWorkspaceLock(
    const std::filesystem::path& workspaceRoot,
    bool create_if_missing);
// Caller: run_doctor (create_if_missing=false); all existing callers keep the
// 1-arg overload.

// include/ymh/registry/process_scan.hpp (88-A4)
//
// The pid holding the AF_UNIX socket inode at `socketPath`, or nullopt when
// unknown. Pure reads: /proc/net/unix (path -> inode) then a scan of
// /proc/<pid>/fd for `socket:[<inode>]`. Never connects; never signals.
// Caller: run_doctor.
[[nodiscard]] std::optional<std::int32_t> socketOwnerPid(
    const std::filesystem::path& socketPath);

// Availability-aware process table (88-A4). `complete == false` means /proc
// could not be enumerated; the shipped `readProcessTable()` keeps its signature
// and returns `.entries` (empty when incomplete). Caller of the checked form:
// run_doctor; the doctor core reads only `.complete` and `.entries`.
struct ProcessTable {
    std::vector<ProcessEntry> entries;
    bool                      complete = true;
};
[[nodiscard]] ProcessTable readProcessTableChecked();

// include/ymh/registry/doctor.hpp (new, 88-A5)
namespace ymh {

enum class DoctorSeverity : std::uint8_t { Low, Medium, High };

enum class DoctorFindingCode : std::uint8_t {
    DeadPid, ForeignPid, RootMismatch, WorkspaceIdMismatch, BootMismatch,
    SocketArgMismatch, LeaseMissing, LeasePidMismatch, LeaseUnreadable,
    LeaseRootMismatch, LeaseBootMismatch, OrphanDaemon,
    SocketOwnerMismatch, SocketOwnerUnknown, ProcessTableUnavailable,
};

// Injected path canonicalizer (88-D2/M1): the core's ONLY external dependency.
// Production: a `std::filesystem::canonical` wrapper (nullopt on error). Tests:
// identity or a stub map, so synthetic roots canonicalize deterministically.
using Canonicalizer =
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path&)>;

// One row plus its two observations. Built by the driver; pure core input.
struct WorkspaceObservation {
    WorkspaceRecord             record;
    WorkspaceLockProbe          lease;              // from probeWorkspaceLock(root,false)
    std::optional<std::int32_t> socketOwnerPid;     // from socketOwnerPid(record.host->socketPath)
};

struct DoctorFinding {
    DoctorFindingCode     code     = DoctorFindingCode::DeadPid;
    DoctorSeverity        severity = DoctorSeverity::Low;
    std::int32_t          recordedPid = 0;   // 0 == none
    std::int32_t          observedPid = 0;   // 0 == unknown/none
    std::filesystem::path observedRoot;      // empty == unknown
    std::string           detail;            // one line, ASCII
    std::string           remediation;       // suggested command, or empty
};

// One reported workspace: the row facts plus its findings (check order). Every
// registry row yields exactly one entry, so a clean row is `state=Clean` with an
// empty `findings` (sec 6.1 renders it). `recordedPid == 0` == no host claim.
struct DoctorWorkspaceReport {
    enum class State : std::uint8_t { Clean, NotRunning, Corrupt };
    WorkspaceId                 id;
    std::filesystem::path       canonicalPath;
    State                       state = State::Clean;
    std::int32_t                recordedPid = 0;
    std::optional<HostBootId>   bootId;             // when the row has a claim
    std::filesystem::path       socket;             // recorded host_socket
    WorkspaceLockProbe          lease;              // {held, pid?, bootId?} snapshot
    std::optional<std::int32_t> socketOwnerPid;     // nullopt == unknown
    std::vector<DoctorFinding>  findings;           // frozen check order
};

struct DoctorReport {
    std::vector<DoctorWorkspaceReport> workspaces;  // canonical_path ASC (88-D8)
    bool        clean = true;              // no >= MEDIUM finding anywhere
    std::size_t high = 0;
    std::size_t medium = 0;
    std::size_t low = 0;
};

// Pure over its arguments (88-D2/M1): the core performs no I/O of its own.
// `table` is one readProcessTableChecked() snapshot; `canon` is the injected
// canonicalizer (production = std::filesystem::canonical wrapper; tests inject
// identity/stub); `observations` is one per registry row, in canonical_path
// order. Returns one report entry per observation, with findings in the frozen
// check order (sec 4). Total: never throws (88-D13).
// Caller: run_doctor (production); errata88_doctor_test (fixtures).
[[nodiscard]] DoctorReport diagnose(
    std::span<const WorkspaceObservation> observations,
    const ProcessTable&                   processTable,
    const Canonicalizer&                  canon);

// Stable lowercase token for `--json` and the human code column. Pure.
// Caller: render_doctor_json / render_doctor_human.
[[nodiscard]] std::string_view doctor_code_token(DoctorFindingCode) noexcept;
[[nodiscard]] std::string_view doctor_severity_token(DoctorSeverity) noexcept;

// Pure renderers of the whole report (both already carry id/canonicalPath).
// Callers: run_doctor (both), errata88_doctor_test (golden).
void render_doctor_human(const DoctorReport&, std::ostream&);
void render_doctor_json(const DoctorReport&, std::ostream&);

} // namespace ymh

// include/ymh/registry/doctor_cli.hpp (new, 88-A6)
namespace ymh {
struct DoctorOptions {
    bool        json = false;
    std::string workspace;   // empty => all rows
};
// Resolves the target if set, opens the registry read-only, calls
// probeWorkspaceLock(root,false) + socketOwnerPid(sock) +
// readProcessTableChecked(), injects the production canonicalizer, folds them
// through diagnose(), renders, and returns the 88-D9 exit code (3 when
// table.complete == false). Caller: run_cli dispatch (`Command::Doctor`,
// src/cli/cli.cpp:1422-1455 area).
int run_doctor(const DoctorOptions&, std::ostream& out, std::ostream& err);

// Executes `ymh workspace reap <id|path> [--yes]`. Resolves one target, re-probes
// the sidecar lock (1-arg probe, via reapHost), and delegates to
// WorkspaceRegistry::reapHost; refuses when the lock is held or when the user
// declines; returns the 88-D9 exit code. Caller: the `Command::Workspace`
// dispatch arm `if (workspace_args[0] == "reap")` in cli.cpp (88-A2/M4), exactly
// where `run_workspace_stop` is called. `run_workspace_command` is unchanged.
int run_workspace_reap(const std::vector<std::string>& args, bool assume_yes,
                       std::ostream& out, std::ostream& err);
} // namespace ymh
```

Notes:
- `diagnose` is total: it never throws for a malformed table/observation; it
  classifies to LOW/unknown (88-D13). `run_doctor` catches registry errors and
  maps them to exit 3, and maps `!table.complete` to exit 3 via C12.
- `socketOwnerPid` reuses the `/proc/<pid>` walk plus a new `/proc/net/unix`
  parse; it is `nullopt` on any failure.

## 8. Invariants (`DOC1-DOC11`)

| ID | Invariant |
|---|---|
| **DOC1** | **`ymh doctor` is read-only.** It opens no file with `O_CREAT`, creates no path, writes no registry/config/session row, sends no bytes on any socket, and sends **no signal at all** (it never calls `kill`; process existence comes from `/proc`, L7). It may transiently take and immediately release a non-blocking `flock` on an existing lock file. |
| **DOC2** | **No process is ever terminated.** Neither doctor nor reap sends a terminating signal; reap changes only registry columns. (03 R11, 16 O11.) |
| **DOC3** | **Reap clears only the host claim, only when the lock is free.** `ymh workspace reap` mutates only `workspaces.host_pid/host_boot_id/host_socket/host_heartbeat` and only via `reapHost`, whose lock-absence gate (03 R5) is preserved. It may create an empty `sessions.lock` when absent (reapHost's 1-arg probe, L5/sec 10). |
| **DOC4** | **Unknown is never a mismatch.** A missing `/proc` entry, an unparsable diagnostic, an unreadable `/proc/net/unix`, or a failed canonicalization yields `dead-pid`/`lease-unreadable`/`socket-owner-unknown`/`process-table-unavailable`, never `root-mismatch` or `socket-owner-mismatch`. |
| **DOC5** | **Root comparison is canonical.** `root-mismatch` / `lease-root-mismatch` require both sides to have canonicalized successfully and to differ. `build_argv` passes the already-canonical `config.workspace_root` (`src/host/workspace_host.cpp:1386-1387`), so the production side is canonical by construction. |
| **DOC6** | **Deterministic output.** Rows are emitted in `canonical_path ASC` order; findings within a row follow the sec 4 table order; JSON keys appear in the schema's order. Two runs on identical inputs are byte-identical. |
| **DOC7** | **Doctor exits 1 iff a finding is >= MEDIUM.** LOW-only reports exit 0 and set `clean == true`; exit 3 is reserved for `process-table-unavailable` (C12). |
| **DOC8** | **Reap is confirmed.** Without `--yes`, reap clears nothing unless stdin is a tty and the user answers `y`/`yes`; a scripted caller without `--yes` is refused (exit 1, no mutation). |
| **DOC9** | **The boot id is checked when argv carries it, which is always for supervisor-spawned daemons.** C4b (registry owner) and C8c (lease owner) fire only when `--boot-id` is present; the supervisor always mints and passes it (`src/host/workspace_host.cpp:1418-1422`). A hand-launched daemon without `--boot-id` skips the check rather than guessing (M2). |
| **DOC10** | **Doctor never needs config or a live daemon.** It opens the registry read-only and reads `/proc`; it works with every daemon stopped and with the global config absent. |
| **DOC11** | **The core is injectable and deterministic.** `diagnose` performs no I/O of its own: process data is a `ProcessTable` argument and the sole filesystem step (path canonicalization) is the injected `Canonicalizer` (88-D2). Tests drive synthetic `ProcessEntry`/`Canonicalizer` fixtures with no filesystem or daemon (M1; enables 88-U2/U3). |

## 9. Failure modes (`DF1-DF11`, F# tagged per 00 sec 54)

F# tags use the `00` sec 54 finding taxonomy (`00-architecture.md:4832-4842`):
F1 path/process isolation, F3 late event after close, F8 resource caps, F9
cancellation scoping.

| ID | F# | Failure | Detection | Handling |
|---|---|---|---|---|
| **DF1** | F1 | **Cross-workspace ownership (the louvre case).** `host_pid`/lease pid is another workspace's daemon. | C2/C3/C4/C4b/C8b/C10 | Flag with the observed root and a `workspace stop <foreign-id> --force` suggestion. Never kill, never auto-clear. |
| **DF2** | F3 | **TOCTOU snapshot.** The process exits or the lock changes between the `/proc` read and the lock probe. | The report is a snapshot; `held` may disagree with liveness by one instant. | Accepted: doctor reports what it observed; classification is explicitly a hint (DOC4). No claim is cleared, so a stale snapshot cannot cause harm. |
| **DF3** | F8 | **Unbounded scan.** A very large `/proc` table or registry. | `readProcessTableChecked` is a single directory walk; registry rows are user-bounded. | Bounded single pass (88-D11); one lock probe + one socket-owner lookup per row; no recursion. |
| **DF4** | F1 | **Unreadable `/proc/<pid>`** (foreign uid, hidepid). | `std::ifstream` on `/proc/<pid>/cmdline` yields empty argv; `read_symlink` fails. | `foreign-pid`/`unknown` classification, LOW where ambiguous; never a fabricated `root-mismatch`. |
| **DF4b** | F3 | **`/proc` entirely unreadable.** | `readProcessTableChecked()` returns `complete == false` (`src/registry/process_scan.cpp:100-133`; the shipped wrapper silently returns an empty list). | All process checks are skipped and one `process-table-unavailable` (C12, LOW) is emitted per row; doctor exits 3 (never a mass `dead-pid`, M5). |
| **DF5** | F9 | **Cancellation mid-scan** (SIGINT/dropped tty). | The process is interrupted while rendering. | Read-only: no state to roll back. Exit the usual way; no partial mutation exists (DOC1). |
| **DF6** | F3 | **Reap races a fresh spawn.** A daemon claims the workspace after the lock probe but before the registry update. | `reapHost` re-checks `probeWorkspaceLock(...).held` and conditions its `UPDATE` on `host_pid = ? AND host_boot_id = ?` (`src/registry/registry.cpp:1187-1210`). | The new claim installed a fresh `boot_id`, so the conditioned `UPDATE` matches zero rows and clears nothing. No stale-lock race is introduced. |
| **DF7** | F1 | **Symlinked or non-canonical `--root`.** A daemon launched with a symlinked root. | Both sides are canonicalized (DOC5). | Compare canonical forms; a failure to canonicalize is `unknown`, not `root-mismatch`. |
| **DF8** | F8 | **`/proc/net/unix` absent or unparsable** (hardened kernel). | `socketOwnerPid` returns `nullopt`. | `socket-owner-unknown` (LOW); the other checks still run; exit unaffected (88-D10). |
| **DF9** | F3 | **Registry absent/corrupt.** | `WorkspaceRegistry::openReadOnly` throws (`src/cli/cli.cpp:333-339` pattern). | Exit 3, message on stderr, no panic. |
| **DF10** | F1 | **Pid reuse.** A recorded pid now names a different live process. | If it is not `ymh --host`, C2. If it is a `ymh --host` for another root, C3. If its `--boot-id` differs from `host_boot_id`, C4b (registry) or C8c (lease). | Flag; never clear while the lock is held. Supervisor-spawned daemons always carry `--boot-id` (DOC9), so the mismatch check is reliable for them. |
| **DF11** | F3 | **Reap's own probe recreates a deleted lock file.** `reapHost` uses the 1-arg `probeWorkspaceLock` (`O_RDWR|O_CREAT`, `src/registry/registry.cpp:1194`). | A workspace whose `sessions.lock` was deleted is reaped. | An empty lock file is created as a side effect; harmless (the daemon recreates it anyway) and recorded in sec 10 (L5). |

## 10. State lifetime

Doctor owns **no durable state**. Every row records create/owner/destroy for the
transient state this errata introduces (AGENTS.md "State lifetime is part of the
spec").

| State | Type | Created | Owner | Destroyed | Restart / crash |
|---|---|---|---|---|---|
| `DoctorOptions` | value | `parse_cli` | `CliInvocation` (stack) | end of `run_cli` | gone |
| `WorkspaceObservation` | value (one per row) | `run_doctor` after `listWorkspaces()` | `run_doctor` (stack-local vector) | end of `run_doctor` | gone |
| `ProcessTable` (`/proc` snapshot + `complete`) | value | `readProcessTableChecked()` in `run_doctor` | `run_doctor` (stack-local) | end of `run_doctor` | gone |
| `Canonicalizer` (production closure over `std::filesystem::canonical`) | `std::function` value | `run_doctor` (constructed once) | `run_doctor` (stack-local) | end of `run_doctor` | gone |
| `WorkspaceLockProbe` | value | `probeWorkspaceLock(root,false)` per row | `WorkspaceObservation` | end of `run_doctor` | gone; the transient flock is released immediately |
| `Socket owner` lookup | `std::optional<int32_t>` | `socketOwnerPid(sock)` per row | `WorkspaceObservation` | end of `run_doctor` | gone |
| `DoctorReport` / `DoctorFinding` | value | `diagnose()` | `run_doctor` (stack-local) | end of `run_doctor` (after rendering) | gone; never persisted, never on the wire |
| Rendered report | bytes on `std::ostream&` | `render_doctor_*` | the caller's stream | end of `run_doctor` | gone |
| Registry read-only handle | `unique_ptr<WorkspaceRegistry>` (read-only open) | `run_doctor` / `run_workspace_reap` | the command scope | end of the command | closed; no WAL/lease state owned |
| `sessions.lock` file (doctor) | file | **not created by doctor** (88-D5, `create_if_missing=false`) | the daemon (baseline) | baseline | unchanged; if absent it stays absent |
| `sessions.lock` file (reap) | file | **created by `reapHost`'s 1-arg probe** (`O_RDWR|O_CREAT`, `src/registry/registry.cpp:1194`) when absent (DF11/L5) | the daemon (baseline) | baseline | an empty file persists if the daemon does not start; the daemon reuses it |
| Registry host-claim columns | `registry.db` row | baseline `claimHost` | registry (durable) | `run_workspace_reap` -> `reapHost` (explicit, confirmed) | survive; only reap changes them, only when the lock is free |

No new member, cache, map, event field, wire field, or `registry.db` column is
introduced. Doctor's only durable touch is a read of `registry.db`; reap's only
durable touches are the four host-claim columns and (when absent) an empty
`sessions.lock`.

## 11. dsh (DeepSeek Harness) mapping

dsh is a headless harness with no interactive client and no registry-doctor
command. Every non-mirror cell carries its reason and a concrete anchor (56-D6,
`docs/design/56-dsh-fidelity-gap-closure-errata.md` sec 3).

| dsh concept | ymh realization (88) | Anchor / non-mirror justification |
|---|---|---|
| ownership recorded as a pid | reused unchanged: `workspaces.host_pid` (`include/ymh/registry/registry.hpp:54-62`) | Mirrors 03 sec 6.1/DDL (`docs/design/03-workspace-registry.md`, `registry.cpp:1061-1087`). No divergence. |
| liveness by kernel lock | reused unchanged: `probeWorkspaceLock` (`src/registry/liveness.cpp:16-53`) | Mirrors 03 R5/R6 (`include/ymh/registry/liveness.hpp:4-9`). No divergence. |
| self-describing daemon argv | reused unchanged: `--workspace/--root/--socket/--boot-id` (`src/host/workspace_host.cpp:1379-1399`) | Mirrors 04 sec 5.2 (`docs/design/04-workspace-host-daemon.md`). No divergence; doctor only reads it. |
| an ownership-consistency diagnostic | **non-mirror, ymh extension**: `ymh doctor` | dsh has no registry and no multi-workspace ownership surface, so there is nothing to doctor; ymh's registry is its own (03 sec 1). This is a **deliberate scope decision** grounded in the incident's missing observability. Anchor: 03 R5/R6 (self-reported diagnostics are non-authoritative -- the gap this command fills). |
| a stale-claim reaper | **non-mirror, ymh-native**: `ymh workspace reap` wrapping `reapHost` | dsh has no durable claim rows to clear. The mutation already exists as `reapHost` (`src/registry/registry.cpp:1187-1210`); this errata only exposes it on the CLI. Anchor: `include/ymh/registry/registry.hpp:278` (the `bool reapHost(WorkspaceId)` declaration, L1). |
| process enumeration for discovery | reused: `readProcessTable` / `scanHosts` (`src/registry/process_scan.cpp:52-131`) | Mirrors 03 sec 6.5/R10 (`include/ymh/registry/process_scan.hpp:4-9`). Doctor reuses it read-only. No divergence. |
| deterministic, injectable I/O for tests | **non-mirror, ymh-native**: `ProcessTable` + injected `Canonicalizer` (`diagnose`) | dsh's harness has no ownership-doctor; the injection is a ymh testability seam for the pure core. Anchor: 03 sec 6.5 (the scan is already pure over an injected table, `process_scan.hpp`) + 88-D2/DOC11. |
| socket-owner identity | **non-mirror mechanism within a mirrored concern**: `/proc/net/unix` + fd scan (`socketOwnerPid`) | dsh has no AF_UNIX workspace socket. ymh's 05 transport is Unix-socket-based (`docs/design/05-transport.md`); this is a ymh-specific way to read the socket owner without connecting. Anchor: 05 sec 2 (the socket is the attached-client transport) + 88-D6 (connect is a side effect). |
| a "repair" that mutates | **non-mirror: deliberately absent** | Doctor never mutates; reap is a separate, confirmed command. There is no dsh analogue because dsh has no ownership state to repair. Anchor: 56-D6 (a deliberate scope decision) + 88-D1. |

## 12. Test plan (hermetic)

New file `tests/unit/errata88_doctor_test.cpp` (added to `tests/CMakeLists.txt`
beside `unit/workspace_registry_test.cpp`). It reuses the established pattern:
build a temp state dir + `WorkspaceRegistry` (as `workspace_registry_test.cpp`
does) and build a synthetic `std::vector<ProcessEntry>` (as
`workspace_registry_test.cpp:700-739` does with `is_ymh_host_process`/`scanHosts`).
The core is `diagnose`, so no daemon, socket, LLM, or tty is required. Because
the core's sole filesystem step is the injected `Canonicalizer` (88-D2/M1),
fixtures supply a stub: `canon = [](p){ return p; }` (identity) or a map from
synthetic root -> canonical root. Synthetic roots therefore need not exist on
disk, and 88-U2/U3 are implementable. `readProcessTableChecked` is not called by
the unit cases (they build a `ProcessTable` directly); the production driver is
covered by one integration case that fabricates a lock file directly.

Fixtures (all constructed in the test, all ASCII):
- `mkHost(pid, root, id, sock, boot?)` -> a `ProcessEntry` with argv
  `{"ymh","--host","--workspace",id,"--root",root,"--socket",sock[,"--boot-id",boot]}`.
- `mkRow(id, path, pid?, boot?, sock?)` -> a `WorkspaceRecord` with the host
  claim set (or `nullopt`).
- `table(complete, {entries...})` -> a `ProcessTable` (or
  `ProcessTable{.entries={}, .complete=false}` for C12).
- `observe(row, held, leasePid?, leaseBoot?, sockOwner?)` -> a
  `WorkspaceObservation` without touching the filesystem.
- `call(observations, table)` -> `diagnose(observations, table, identity_canon)`.

| ID | Test | Fixture | Asserts |
|---|---|---|---|
| 88-U1 | `CleanWhenRecordedOwnerMatches` | row(louvre,pid=42,sock) + table host(42, louvre, id_louvre, sock) + lease{held,42} + sockOwner 42 | `clean`, no findings |
| 88-U2 | `RootMismatchIsHigh` (the incident) | row(louvre,pid=42) + table host(42, **bb_tedi**, id_tedi, sock_tedi) + lease{held,42} | one `RootMismatch` HIGH, `observedRoot == bb_tedi`; remediation contains `workspace stop` |
| 88-U3 | `LeaseRootMismatchIsHigh` | row(louvre,pid=42,sock) + table host(42,louvre,id,sock) + lease{held,pid=99} + table host(99, bb_tedi, id_tedi, sock_tedi) | `LeaseRootMismatch` HIGH with `observedRoot == bb_tedi`; C2/C3 do not fire (the registry owner 42 is correct) |
| 88-U4 | `DeadPid` | row(louvre,pid=42) + empty table + lease{held=false} | `DeadPid` MEDIUM + `LeaseMissing` MEDIUM; remediation `workspace reap <id> --yes` |
| 88-U5 | `ForeignPidNotHost` | row(pid=42) + table entry pid 42 argv `{"/bin/sh","-c","..."}` | `ForeignPid` HIGH, no `RootMismatch` |
| 88-U6 | `WorkspaceIdMismatch` | host(42, louvre, **other-id**, sock) | `WorkspaceIdMismatch` HIGH |
| 88-U7 | `SocketArgMismatch` | host(42, louvre, id, **other-sock**) | `SocketArgMismatch` MEDIUM |
| 88-U8 | `LeasePidMismatch` | registry pid 42, lease pid 99, both live/held | `LeasePidMismatch` MEDIUM |
| 88-U9 | `LeaseUnreadableIsLow` | lock `held` true, `lease.pid == nullopt` | `LeaseUnreadable` LOW; `clean == true`; exit-code mapping `0` |
| 88-U10 | `SocketOwnerMismatch` | sockOwner 42, registry pid 77 | `SocketOwnerMismatch` HIGH |
| 88-U11 | `SocketOwnerUnknownIsLow` | sockOwner `nullopt`, socket path set | `SocketOwnerUnknown` LOW; `clean == true` |
| 88-U12 | `OrphanDaemon` | no host claim, table host(42, louvre, id, sock) | `OrphanDaemon` MEDIUM, `observedPid == 42` |
| 88-U13 | `BootMismatchOnlyWhenArgvHasBootId` | host(42,...,boot="X") with row boot="Y" | `BootMismatch` (C4b) MEDIUM for the registry owner; and with no `--boot-id` in argv -> no such finding (DOC9/M2) |
| 88-U13b | `LeaseBootMismatch` | host(42,louvre,id,sock,boot="X") + lease{held,pid=42,leaseBoot="Y"} | `LeaseBootMismatch` (C8c) MEDIUM |
| 88-U14 | `NotRunningEmitsNothing` | no host claim, empty table | no findings, `state == not-running`, `clean == true` |
| 88-U15 | `DeterministicOrder` | two rows out of canonical order | `diagnose`/renderer emit canonical_path ASC; two runs byte-identical |
| 88-U16 | `HumanRenderGolden` | the 88-U2 report | exact tab-separated lines + `doctor:` summary line |
| 88-U17 | `JsonRenderGolden` | the 88-U2 report | schema==1, `process_table_complete`, `counts`, `findings[0].code == "root-mismatch"`, `severity == "high"`, omitted-vs-empty keys per sec 6.2 |
| 88-U18 | `NonCreatingLockProbe` | a temp root with no `.ymh/sessions.lock` | `probeWorkspaceLock(root,false)` returns `held=false` and the file is still absent; `probeWorkspaceLock(root,true)` creates it |
| 88-U19 | `SocketOwnerLookup` | guard with `GTEST_SKIP` when `/proc/net/unix` lacks the test socket; otherwise assert `socketOwnerPid(testSocket)` finds the test process | bounded, skip-safe |
| 88-U20 | `ReapRefusesWhenHeld` | tmp workspace with a held lock (open + flock in the test) | `run_workspace_reap({"reap",id}, /*assume_yes=*/true, ...)` returns 1 and does not clear the row (reapHost's gate) |
| 88-U21 | `ReapClearsWhenFree` | tmp workspace, lock free, row has a stale claim | `run_workspace_reap({"reap",id}, true, ...)` returns 0 and the row's `host_pid` becomes NULL; sessions untouched |
| 88-U22 | `ExitCodeMapping` | clean / LOW-only / MEDIUM reports, and `complete=false` | `0` / `0` / `1` / `3` respectively |
| 88-U23 | `ProcessTableUnavailable` | `table(complete=false, {})` with two claimed rows | no `DeadPid`; one `ProcessTableUnavailable` LOW per row; `clean` unaffected but the report marks incomplete |
| 88-U24 | `StubCanonicalizerDrivesMismatch` | `canon` returns `/canon/louvre` for any input; row path `/canon/louvre`, observed root `/x` | with identity canon: `RootMismatch`; with the stub the comparison is deterministic and filesystem-free (M1) |

Notes:
- 88-U18/U19/U20/U21 touch the real filesystem/tmpdir but no daemon, no socket
  protocol, no LLM: hermetic.
- No test starts `ymh --host`; pid/root fixtures are synthetic `ProcessEntry`
  rows, exactly as the baseline scan tests do. `diagnose` receives a stub
  `Canonicalizer`, so no fixture path needs to exist on disk (M1).
- `probeWorkspaceLock` overload and `socketOwnerPid` get focused unit coverage
  (88-U18/U19) independent of `diagnose`.

## 13. Scope boundaries and recorded risks

- **R1 -- Snapshot, not a lock.** Doctor's verdict is a point-in-time snapshot;
  a workspace that is mid-spawn or mid-shutdown may be reported inconsistently
  (DF2). Accepted: doctor only flags and never clears, so a false-positive
  HIGH costs the user a look, not data.
- **R2 -- Self-reported diagnostics.** The lease pid/boot id and the argv
  `--root` are self-reported; only the flock `held` bit is kernel truth (03 R6).
  Doctor labels accordingly and never treats a diagnostic as proof.
- **R3 -- flock-holder identity is unreadable.** Linux exposes no API for the
  flock holder's pid, so doctor checks the diagnostic content, not the holder.
  This is why `lease-unreadable` exists and why `reap` is lock-gated (the kernel
  bit), not diagnostic-gated.
- **R4 -- Supervisor-spawned daemons always carry `--boot-id`, so the boot-id
  check is reliable for them.** `ForkExecLauncher::spawn` mints `boot_id` and
  passes it to `build_argv` (`src/host/workspace_host.cpp:1418-1422`,
  `:1394-1397`; contract "supervisor mint-and-pass",
  `include/ymh/host/workspace_host.hpp:102`). Only a hand-launched daemon that
  omits `--boot-id` skips C4b/C8c (DOC9); it is not evidence of corruption.
  (Corrects Rev 1's "boot id often absent".)
- **R5 -- `SO_PEERCRED` deferred.** A connected-socket peer-id check would be
  stronger than the `/proc`-inode walk but requires `connect(2)`; deferred to keep
  doctor side-effect-free (88-D6). Recorded as a candidate, not a requirement.
- **R6 -- No auto-repair.** `ymh doctor --fix` is deliberately not offered
  (88-D1); a future spec may compose doctor's remediation with an explicit
  confirm flow, but this errata stops at flag + separate reap.
- **R7 -- The non-creating probe takes a transient flock.** Doctor's lease probe
  opens `O_RDONLY` and briefly takes `LOCK_EX|LOCK_NB` on a free lock
  (`src/registry/liveness.cpp:16-53`). The daemon acquires the same lock with
  `LOCK_EX|LOCK_NB` and does not retry (`src/session/session_persistence.cpp:768-771`);
  a microsecond overlap could make a concurrent daemon startup see `EWOULDBLOCK`.
  The pattern pre-exists in `probeLiveness` (`src/host/workspace_host.cpp:1553-1560`),
  so doctor only adds one more concurrent prober. Accepted; recorded, not fixed
  (L6).
- **R8 -- A reap side-effect creates an empty lock file.** `reapHost`'s 1-arg
  probe `O_CREAT`s `sessions.lock` when absent (DF11/sec 10); harmless, but it is
  a mutation doctor deliberately avoids (88-D5).

## 14. Revision log

| Rev | Date | Change |
|---|---|---|
| 1 | 2026-10-16 | Initial draft: `ymh doctor` (read-only) + `ymh workspace reap` (explicit, lock-gated), the C0-C11 check set, DOC1-DOC10 invariants, DF1-DF10 failure modes, the 88-D1..D14 decisions, and the hermetic test plan. |
| 2 | 2026-10-16 | Gate fixes (Rev 1 FAIL, 0 HIGH / 6 MEDIUM): **M1** `diagnose` made pure via an injected `Canonicalizer` + `ProcessTable` (DOC11, 88-U24); **M2** boot-id rationale corrected (supervisor always passes `--boot-id`; new C4b, DOC9, R4); **M3** reap unknown-target exit code pinned to `2`; **M4** `--yes` wiring pinned as `run_workspace_reap` dispatched in `cli.cpp` beside `stop` (no `run_workspace_command` change, 88-A7); **M5** `readProcessTableChecked` seam added so exit `3` is implementable (C12/DF4b); **M6** `foreign-pid` remediation fixed (no observed root; reap/stop instead of a malformed `add`). LOWs: L1 anchor, L2 `doctor_json`, L3 enum trim, L4 divergence note, L5 reap lock-file side effect, L6 transient flock (R7), L7 no `killHint`, L8 citations. |
