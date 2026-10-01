# 69 — Host Startup Diagnosability Errata

```
Status: verified (Rev 3) · reviewer: see `DESIGN_STATUS.md` row 69 · gate: 0 HIGH / 0 MEDIUM
Revision: 3
Component: 69 (errata) — amends 04-workspace-host-daemon.md (§2.2, §3.2, §3.3,
           §6.2, §11.2) and 11-m2-errata.md (§9.2) by reference, and
           10-supervisor-tui.md (§6.3 status bar) for the multi-line notice
           render; adds no wire, model, persistence, or exit-code change
Depends on: 04-workspace-host-daemon.md (verified), 11-m2-errata.md (verified;
            M2 frozen interfaces), 16-daemon-ownership.md (verified; supervisor-
            owned daemons), 12-m1-drift-errata.md (verified register)
Scope: the reason channel AND the presentation of daemon startup failure. Every
       `StartupRejected` (exit 16) and every dynamic-loader failure that prevents
       the daemon from publishing its claim must reach the supervisor as a short,
       actionable cause instead of the bare code string, and that cause must lead
       a multi-line, labelled, bounded message that renders readably in both the
       CLI and the TUI (Rev 2).
```

## 1. Purpose, scope, and precedence

The user report is *"I copied the `ymh` binary from another Ubuntu and got
`cannot start workspace: daemon did not become ready: startup was rejected
(StartupRejected)`. This is likely related to some missing library on this PC."*
The report is correct: the daemon child `dup2`s its stderr into the log sink
before `execve` (`src/host/workspace_host.cpp`, `ForkExecLauncher::spawn`), so a missing shared
library or interpreter, an unwritable log sink, a `chdir`/state-dir failure, a
registry/claim failure, and a provider-setup failure all collapse into the single
opaque code `StartupRejected = 16`, and the child's own error text never reaches
the supervisor. The errno that named the cause is discarded at the `_exit`.

This errata pins a **diagnosability** fix, not a behavioural one:

- the exit-code values stay frozen (69-D1) — `StartupRejected = 16` is the
  wire/exit contract pinned by spec 11 §9;
- a child→parent pre-exec diagnostic channel carries `strerror(errno)` for the
  two child-only failures (log-sink open, `execve`);
- the supervisor reads a bounded, redacted tail of the daemon's log sink to
  surface the daemon's own stderr (including the dynamic loader's
  `error while loading shared libraries: …`) when the daemon exits before
  readiness;
- every `StartupRejected` return site inside `WorkspaceHost::Impl` emits its own
  specific stderr diagnostic, so no path can produce a bare code string.

It is **additive**: the only struct growth is a defaulted `SpawnResult` field
(69-D2), the only new state is a parent-owned pipe fd whose lifetime is pinned
(69-D5), and no transport, registry schema, config key, or event changes.

**Precedence.** This errata wins over `04 §3.2` for the child's fd keep-set
(one fd is preserved across `close_extra_fds` for the diagnostic pipe),
over `04 §6.2` for the `ensureRunning` failure message composition, and over
`04 §11.2` for `D-F18` (the log-sink reason is now specific, not bare). It does
not change `04 §2.2`'s `HostErrorCode` set or `11 §9`'s frozen interfaces beyond
the additive field.

**Rev 2 (presentation).** Rev 1 made the reason *specific* but not *readable*.
On the real failure the supervisor produced one concatenated line that buried the
cause behind trailing noise:

```text
cannot start workspace: daemon did not become ready: startup was rejected
(StartupRejected): 2026-09-30 16:41:42.927 [tool] [warning] …SKILL.md:
description truncated to 512 bytes/2026-09-30 16:41:42.927 [tool] [warning] …
```

Two defects, both presentation:

1. **Newlines are lost.** The Rev 1 `sanitize_log_tail` joined the captured tail with
   `" / "` (`workspace_host.cpp`, the join loop), and FTXUI's `text()` drops an
   embedded `'\n'` outright, rendering the whole notice on one row. The lines
   run together.
2. **The tail surfaced the wrong lines.** The cause was selected by *position*
   (the last N lines). The last lines were benign skill-loading warnings emitted
   while `cleanupStartupFailure()` tore the runtime down *after* the failure
   diagnostic had already been `fprintf`'d, so the actual reason — already
   emitted by the 69-A4 sites — was pushed out of the window and never led.

Rev 2 fixes only the presentation: the cause is selected by *content* (69-D8) and
leads a multi-line, labelled, bounded message (69-D9/69-D10) that both the CLI
and the TUI render without dropping newlines (69-D11). No new channel, no
`HostExitCode` change, no wire change.

**Gate (per `AGENTS.md`).** Rev 2 was marked `verified` once the reason channel,
the redaction bound, the fd lifetime, AND the Rev 2 cause-selection/format/
rendering decisions were confirmed. Rev 3 re-gated after the 69-D4/69-D12/69-D13
changes: final **PASS (0 HIGH / 0 MEDIUM)** (see `DESIGN_STATUS.md` row 69).

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 69-A1 | 04 §2.2 | `include/ymh/host/workspace_host.hpp:58-68` | document that `StartupRejected`'s *reason* is carried out-of-band; the numeric value is unchanged |
| 69-A2 | 04 §3.2 spawn sequence | `src/host/workspace_host.cpp` `ForkExecLauncher::spawn` (child log-sink write; `execve`-failure write) | add the CLOEXEC diagnostic pipe; the child writes `strerror(errno)` + path on log-sink-open / `execve` failure before `_exit` |
| 69-A3 | 04 §3.2 fd cleanup | `src/host/workspace_host.cpp` `close_extra_fds` | `close_extra_fds` gains a `keep_fd` parameter so the diagnostic pipe survives to `execve` |
| 69-A4 | 04 §3.3 startup order | `src/host/workspace_host.cpp` `WorkspaceHost::Impl::run` (owner-watchdog guard) and `WorkspaceHost::Impl::startup` (chdir, state dir, runtime detail, no provider) | each `StartupRejected` return site emits a specific `stderr` diagnostic (owner-watchdog, chdir, state dir, runtime detail, no provider) |
| 69-A5 | 04 §6.1 launcher seam | `include/ymh/host/host_launcher.hpp:44-58` | `SpawnResult` gains `int diagnostic_fd{-1}` (parent-owned) |
| 69-A6 | 04 §6.2 `ensureRunning` | `src/host/workspace_host.cpp` `HostLifecycle::spawnAndAttach` (diagnostic-fd closer; compose; throw) | compose the readiness error as `describe_host_exit(status) + ": " + <specific reason>`; read the pipe and the bounded log tail (body superseded by 69-A9) |
| 69-A7 | 04 §11.2 `D-F18` | `src/host/workspace_host.cpp` `ForkExecLauncher::spawn` log-sink-open failure | log-sink open failure reports `cannot open log sink <path>: <strerror(errno)>` |
| 69-A8 | 11 §9.2 / 04 §3.2 exec failure | `src/host/workspace_host.cpp` `ForkExecLauncher::spawn` `execve` failure | `execve` failure reports `execve failed: <strerror(errno)> (<path>)` plus the `ldd` hint on absolute-path `ENOENT` |
| 69-A9 | 04 §6.2 `ensureRunning` compose (supersedes 69-A6's join) | `src/host/workspace_host.cpp` `sanitize_log_lines`/`read_log_sink_lines`/`select_primary_cause`/`without_host_diagnostic_prefix`/`startup_failure_reason`; `HostLifecycle::spawnAndAttach` throw | select the cause by marker, not position (69-D8); compose the readiness body as 69-D9's multi-line labelled block with `'\n'` preserved and no `" / "` join; keep the bounded redacted context (69-D10) |
| 69-A10 | 10 §6.3 status bar | `src/ui/ui_render.cpp` (`render_status`) | render a notice that contains `'\n'` as stacked rows (`vbox`), never one `ftxui::text` (which drops `'\n'`); flatten to `" · "` only for the inline active-session segment (69-D11) |

## 3. Decision register

**69-D1 — The exit-code numbering is frozen; only the reason changes.** No value
of `HostExitCode` (`include/ymh/host/workspace_host.hpp:58-68`) changes.
`StartupRejected = 16` remains the contract asserted by
`WorkspaceHostConfig.ExitCodeValuesArePinned`
(`tests/unit/workspace_host_test.cpp:246-256`) and the frozen interfaces of
`11-m2-errata.md` §9. This errata improves the *string* the supervisor throws and
the CLI/TUI render, never the code.

**69-D2 — Child→parent pre-exec diagnostic pipe.** `ForkExecLauncher::spawn`
creates a `pipe2(…, O_CLOEXEC)` before `fork`. The child keeps the write end
across `close_extra_fds` (via 69-A3) and the kernel closes it on a successful
`execve` (CLOEXEC). On the two child-only failures it writes a bounded,
single-line cause with `strerror(errno)` and then `_exit(StartupRejected)`:
`cannot open log sink <path>: <strerror>` (69-A7) and
`execve failed: <strerror> (<path>)` (69-A8). The read end is returned to the
parent as `SpawnResult::diagnostic_fd` (69-A5). On an absolute-path `ENOENT` —
`execve` cannot distinguish "file absent" from "missing interpreter/loader
dependency" — the reason appends
`(a missing shared library or interpreter can cause this: run 'ldd <path>')`.
The hint is honest: it states a possibility, not a certainty. If the pipe cannot
be created, the launcher degrades to the log-tail path only (69-F5); it never
fails the spawn for want of a diagnostic.

**69-D3 — Bounded, redacted log-sink tail.** When a spawned daemon exits before
readiness, the supervisor reads the tail of `HostConfig::log_sink` (the daemon's
`stderr`) and appends it to the reason. This is the channel that carries the
dynamic loader's own message on a missing shared library (the loader writes to
`fd 2` and exits 127), and the daemon's own `fprintf(stderr, …)` diagnostics for
post-`execve` startup failures (69-A4). The tail is bounded to **at most 4
non-empty lines / 512 bytes total**, each line truncated to 240 bytes, and any
line whose text (case-insensitively) contains `api_key`, `api-key`, `apikey`,
`authorization`, `bearer `, `secret`, `password`, `passwd`, `credential`, or
`token=` is **redacted in place** — the marker and the rest of its line become
`[redacted]`, and the line is **not** dropped (69-D6/69-D12). Tail reading
happens only on a startup failure, never on the success path.

**69-D4 — Every `StartupRejected` return is specific.** Each return site
(`WorkspaceHost::Impl::run`'s owner-watchdog guard; `WorkspaceHost::Impl::startup`'s
`chdir`, state-dir, runtime-detail, and no-provider arms; `ForkExecLauncher::spawn`'s
log-sink-open and `execve` arms — all in `src/host/workspace_host.cpp`) emits a
distinct `stderr` diagnostic naming the cause (and, for `execve`/chdir/open,
`strerror(errno)`). The readiness error composes
`"daemon did not become ready: " + describe_host_exit(status)` followed by
69-D9's multi-line block (a `\n  reason:` line when a reason exists, then the
labelled, bounded context), so a user never sees only
`startup was rejected (StartupRejected)`. (Rev 3 reconciles this cell with
69-D9; the Rev 1/2 `… + ": " + reason` single-line form is superseded by
69-A9/69-D9.)

**69-D5 — Diagnostic-fd lifetime.** `SpawnResult::diagnostic_fd` is
parent-owned and single-consumer. `HostLifecycle::spawnAndAttach` wraps it in an
RAII closer so it is closed on **every** return and throw path — the success
(claim published / winner-attach) path and both failure paths. The fd is read at
most once, on the reaped-before-readiness path. A supervisor that leaks the fd
at process exit is bounded by process termination; no fd survives into the daemon
(the child end is CLOEXEC).

**69-D6 — No secret or prompt leakage.** The errata never writes prompts,
credentials, environment dumps, or `HostConfig` contents into the reason. The
child writes only `strerror(errno)` + the executable/log-sink path. The log tail
is bounded and redacted per 69-D3/69-D12: masking starts at the first matched
marker, so bytes before it are preserved verbatim (a credential with no marker
word is not caught — recorded in 69-OQ2). This honours `AGENTS.md` ("never dump
full prompts or sensitive tool output to normal logs by default") and does not
consume `logging.log_prompts`.

**69-D12 — Redact in place; never drop the line (Rev 3).** The Rev 1/2 redactor
dropped any line containing a marker. That contradicted absolute 69-I1: a
diagnostic that itself named a marker (e.g. `ymh --host: cannot read token=…`)
was deleted, so `select_primary_cause` found no cause and the user got the bare
`startup was rejected (StartupRejected)` back. Rev 3 masks instead of dropping:
`redact_secrets` rewrites the line from the first marker onward to `[redacted]`,
preserving the text before it — including the `ymh --host:` diagnostic prefix, so
the line stays selectable as the cause. A line is never removed for containing a
marker.

**69-D13 — A signaled exit is named (Rev 3).** `ForkExecLauncher::tryReap`
encodes a signal death as `128 + WTERMSIG`. `describe_host_exit` gains a branch
that renders `exit code <128+sig> (killed by <SIGNAME>)` (e.g. `exit code 139
(killed by SIGSEGV)`) instead of the bare number, for the daemon that dies before
writing anything. Unknown signal numbers render `(killed by signal <n>)`. The
encoding is a heuristic: a normal `exit(128+n)` is indistinguishable through the
`int` seam and is rendered the same way (recorded in 69-I14).

**69-D7 — Readiness-error envelope and pipe precedence.** The thrown message is
always prefixed `daemon did not become ready: <describe_host_exit(status)>`; the
body after that prefix is 69-D9's multi-line block (Rev 2). The pipe diagnostic
(69-D2) is authoritative when present, and the log context is never mixed in
beside it: a log-sink-open failure means the sink was never truncated, so a
pre-existing file could supply a stale, misleading tail. The `HostError` code
stays `protocol::HostErrorCode::HostUnreachable`.

**69-D8 — The cause is selected by content, not position (Rev 2).** The reason
no longer takes the last non-empty lines as the cause. The supervisor scans a
bounded window of the daemon's log sink and classifies each non-empty,
non-redacted line:

1. a line containing `error while loading shared libraries` is the loader cause
   (takes priority);
2. otherwise the last line carrying the daemon's own diagnostic marker
   `ymh --host:` is the cause, surfaced with that prefix stripped;
3. otherwise there is no cause and the headline is only
   `describe_host_exit(status)`.

Position selects only the *context* block, never the cause. This is what surfaces
the `fprintf(stderr, …)` diagnostic the 69-A4 sites already emit even when later
shutdown noise (e.g. skill-loading warnings during `cleanupStartupFailure`)
follows it. The scan window is bounded (`≤ 256 KiB`) and every scanned line is
redacted/truncated by 69-D6 before classification.

**69-D9 — Multi-line, labelled message format (Rev 2).** `'\n'` is preserved;
lines are never joined with `/`. The full message is:

```text
daemon did not become ready: <describe_host_exit(status)>
  reason: <primary cause>
  recent daemon log:
    <context line>
    <context line>
```

- the `reason:` line is present only when a cause exists (69-D8's content rule, or
  the pipe); the cause leads,
  before any context;
- the `recent daemon log:` label and its indented lines are present only when
  non-duplicate context lines remain; the block is explicitly *context*, never
  presented as the cause;
- when the context is dropped by the 69-D10 bound but the sink has more content,
  a pointer line `  daemon log: <path>` is emitted so the user can read the rest;
- the pipe case emits only `reason:` (no context), per 69-D7.

**69-D10 — Bounded context, not the cause.** The context block is bounded to
**≤ 4 non-empty lines / ≤ 512 bytes total**, each line truncated to 240 bytes,
redacted by 69-D6. The bound applies to the context block only; the cause line is
always surfaced (it is a single bounded line). A huge tail cannot flood the
terminal: the scan window is bounded (69-D8) and only the bounded context is
rendered.

**69-D11 — The TUI renders the message on separate rows (Rev 2).** FTXUI's
`text()` ignores an embedded `'\n'` (it advances one row and concatenates the
segments) and the status bar's inline segments are single-line and width-fitted,
so a multi-line notice is always rendered as a `vbox` block by
`render_notice_block`:

- with no active session, the block replaces the status text (the existing
  workspace-independent notice-ring branch);
- with an active session, the single-line status line is built **without** the
  notice and the block is appended as a row below it — so the long startup
  failure cannot be silently dropped by the inline width fit (69-F10b). Only
  single-line notices keep the inline segment behavior.

The already-computed chrome budget (`min_rows`) reserves the extra rows. The CLI
prints `what()` verbatim to stderr; `'\n'` already renders as separate lines.

## 4. The failure-reporting model

```text
supervisor                         fork child (pre-exec)            daemon (post-exec)
────────────                       ─────────────────────            ──────────────────
spawn():
  pipe2(CLOEXEC) ──┐
                   ├─ fork ─────────> close(read end)
                   │                  open log_sink ──fail──> write "cannot open log sink …"
                   │                  │                                      └─ _exit(16)
                   │                  dup2(log_fd,1/2)
                   │                  close_extra_fds(keep=write end)
                   │                  execve ──fail──> write "execve failed: ENOENT …"
                   │                  │                        └─ _exit(16)
                   │                  execve ──ok──> (CLOEXEC closes write end)
                   │                                      chdir/state/registry/runtime/provider
                   │                                        └─ fprintf(stderr, specific) → log sink
                   │                                        └─ exit 16 / loader exit 127
  <── SpawnResult{pid, bootId, socketPath, diagnostic_fd}
  readiness poll:
    tryReap → status
      pipe has bytes?  ── yes ──> reason = pipe text            (69-D7)
      else scan log_sink (≤256 KiB) and classify each line:     (69-D8)
             loader line "error while loading shared libraries"? → cause
             else last "ymh --host:" diagnostic?                  → cause (prefix stripped)
             last ≤4 lines (bounded/redacted, cause excluded)     → context (69-D10)
      throw HostError(HostUnreachable,
        "daemon did not become ready: " + describe_host_exit(status)
        + "\n  reason: " + cause            (if any)
        + "\n  recent daemon log:\n    " + context lines (if any))
```

The loader case is the harder of the two and the one 69-D3 was built for; the
Rev 2 user report was the post-`execve` case where the cause line was followed by
shutdown noise (see §1). A missing shared library makes the
kernel start the interpreter (`ld.so`), which prints
`… error while loading shared libraries: libX.so: cannot open shared object file`
to `fd 2` (the log sink) and exits 127; a missing *interpreter* makes `execve`
return `ENOENT` and the child writes the `ldd` hint to the pipe. Both reach the
user through this model.

## 5. Per-path reason table

| Path | Exit | Reason source | Example reason text |
|---|---|---|---|
| Owner watchdog not armed | 16 | daemon stderr (69-A4) | `startup rejected: owner watchdog is not armed (require_owner && !watchdog_disabled)` |
| `chdir(root)` fails | 16 | daemon stderr + `strerror` | `startup rejected: chdir to /ws failed: Permission denied` |
| `.ymh` state dir uncreatable | 16 | daemon stderr + `error.message()` | `startup rejected: cannot create state dir /ws/.ymh: …` |
| Runtime create fails (provider) | 16 | `WorkspaceRuntimeError::detail` | `runtime setup failed (ProviderSetupFailed): <detail>` |
| No provider configured | 16 | daemon stderr | `startup rejected: no LLM provider is configured` |
| Log-sink open fails | 16 | pipe + `strerror` | `cannot open log sink /ws/.ymh/host.log: Is a directory` |
| `execve` fails | 16 | pipe + `strerror` + path | `execve failed: No such file or directory (/opt/ymh) (a missing shared library or interpreter can cause this: run 'ldd /opt/ymh')` |
| Missing shared library | 127 | log tail (loader) | `error while loading shared libraries: libfoo.so: cannot open shared object file` |
| Missing interpreter | 16 | pipe + hint | `execve failed: No such file or directory (/opt/ymh) (… run 'ldd /opt/ymh')` |

Rev 2 (69-D8) changes only the *selection* for the post-`execve` rows: the
daemon-stderr source is scanned by content, so the `ymh --host:` line is found
even when trailing shutdown noise follows it. The pipe rows are unchanged.

## 6. Invariants

| ID | Invariant |
|---|---|
| **69-I1** | For every `StartupRejected` exit, the readiness `HostError::what()` is strictly more than the bare `describe_host_exit(16)` string and names a cause (errno text, a named cause, or a log tail). No path yields a bare `StartupRejected`. **Rev 3:** a cause line carrying a redaction marker is masked in place (69-D12), never dropped, so the content-based cause selection still finds it; a marker-only `ymh --host:` diagnostic therefore still yields `reason: [redacted]`, not the bare code. |
| **69-I2** | The `execve`-failure reason contains `strerror(errno)` (e.g. `No such file or directory`, `Permission denied`, `Exec format error`). |
| **69-I3** | The log-sink-open-failure reason is textually distinct from the `execve`-failure reason (it contains `log sink`; the latter contains `execve`). |
| **69-I4** | No `HostExitCode` numeric value changes; `StartupRejected == 16`. |
| **69-I5** | The diagnostic pipe's write end is CLOEXEC and is the only fd preserved across `close_extra_fds`; the daemon inherits no diagnostic fd. |
| **69-I6** | The log context block surfaced is ≤4 lines / ≤512 bytes / ≤240 bytes per line. **Rev 3 (69-D12):** a marker-bearing line is not excluded from the context — it is included with its marker and everything after it replaced by `[redacted]`, so the context never contains an unmasked marker value and the line count/bounds still hold. |
| **69-I7** | On the success path (claim published, or winner-attach), the parent closes `diagnostic_fd`; no fd leak accrues across repeated spawns. |
| **69-I8** | A pipe-creation failure degrades to the daemon-stderr/describe path; `spawn` never throws for want of a diagnostic. |
| **69-I9** | The child writes at most one bounded single-line diagnostic and uses only `write(2)` on the pipe (no unbounded allocation, no secret material, no `HostConfig` dump). |
| **69-I10** | The readiness message contains no `" / "` join; every `'\n'` in the composed body is preserved in `HostError::what()`. |
| **69-I11** | When a cause exists it appears before the `recent daemon log:` context (69-D9); the context is always labelled, never presented as the reason. The cause is selected by 69-D8's content rule, not by line position. |
| **69-I12** | The TUI renders a notice containing `'\n'` on separate rows and never concatenates **or silently drops** it, in both the no-active-session and active-session status layouts; the CLI renders `what()` verbatim. |
| **69-I13** | A daemon log whose cause line is followed by ≥N lines of benign noise still yields the cause as the `reason:` headline (the 69-D8 scan is bounded to 256 KiB and independent of the context's 4-line bound). |
| **69-I14** | A reap status above 128 renders as `exit code <status> (killed by <SIGNAME>)` with a named signal for the standard signals, or `(killed by signal <n>)` otherwise; it is never a bare `exit code <n>`. The `128+n` encoding conflates a signaled death with a normal `exit(128+n)` through the `int` `tryReap` seam; both render identically (recorded, accepted). |

## 7. Failure modes (`69-F#`)

Component-local to this errata; they refine `04 §11.2 D-F18` and add the
diagnostics for `D-F1`/`D-F4`/`D-F21`.

| 69-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **69-F1** | `execve` fails (`ENOENT`/`EACCES`/`ENOEXEC`) | `execv` returns, `errno` set, child `_exit(16)` | Child writes `execve failed: <strerror> (<path>)` to the pipe; supervisor surfaces it and the code text (69-I1, 69-I2) |
| **69-F2** | Missing shared library / interpreter | loader prints to `fd 2` and exits 127, or `execve` `ENOENT` on an absolute path | Loader text surfaced from the bounded log tail; the pipe reason carries the `ldd` hint (69-D2, 69-D3) |
| **69-F3** | Log sink unwritable | `open(log_sink, …)` < 0 in the child | Child writes `cannot open log sink <path>: <strerror>` to the pipe; never bare (69-A7, 69-I3) |
| **69-F4** | Provider setup / runtime build failure | `WorkspaceRuntime::create` returns an error | Daemon prints `runtime setup failed (<code>): <detail>`; supervisor surfaces it as the `reason:` headline (69-A4, 69-D8) |
| **69-F5** | Diagnostic pipe cannot be created | `pipe2` < 0 | Spawn proceeds; readiness falls back to `describe_host_exit` + log tail; never a new failure (69-I8) |
| **69-F6** | Log tail contains credential-like text | line matches the 69-D6 redaction set | Line is **redacted in place** (`[redacted]` from the marker onward) and included; never dropped, so a marker-bearing cause is not lost (69-D12/69-I1/69-I6) |
| **69-F7** | Daemon exits 0 before readiness | `tryReap` → 0 | Unchanged: `describe_host_exit(0)` = `exited cleanly`; no reason invention |
| **69-F8** | Supervisor dies with the read end open | process teardown | The kernel closes the fd; no daemon impact (the child end is already CLOEXEC-closed) (69-I5, 69-I7) |
| **69-F9** | The daemon's cause line is followed by benign shutdown noise | 69-D8 scan finds the `ymh --host:` line above the noise | The cause is the `reason:` headline; the noise is bounded, labelled context (69-I11, 69-I13) |
| **69-F10** | A multi-line notice reaches the TUI status bar | `render_status` sees `'\n'` in the notice | The rows are stacked as a block; only single-line notices use the inline segment (69-D11, 69-I12) |
| **69-F10b** | A long startup notice would not fit the inline status width while a session is active | `render_status` skips the inline include for a `'\n'` notice | The block is still rendered as a row below the status line; it is never silently dropped (69-D11, 69-I12) |
| **69-F11** | The log sink holds more than the 69-D10 context bound | `read_log_sink_lines` reaches the scan bound | No error; the cause is still surfaced; context is truncated to the bound and the `daemon log: <path>` pointer is emitted (69-D10) |
| **69-F12** | Daemon killed by a signal before readiness | `tryReap` returns `128 + WTERMSIG` (>128) | `describe_host_exit` names the signal (69-D13/69-I14); a `SIGSEGV` daemon surfaces `exit code 139 (killed by SIGSEGV)`, never a bare number |

## 8. dsh (DeepSeek Harness) mapping

| dsh surface | ymh mirror | Justification |
|---|---|---|
| process spawn error propagation | `SpawnResult::diagnostic_fd` + bounded log tail (69-D2/69-D3) | dsh surfaces spawn errors as typed exceptions with stderr captured; ymh keeps its frozen exit-code contract and adds an out-of-band reason channel, so this is a mirror of the *capability*, not the exact mechanism (`11 §9` freezes the exit codes). |
| "do not leak secrets in errors" | 69-D6 redaction + bound | Non-mirror: dsh has no explicit redaction set in its spawn path; ymh adds one because its log sink is a persistent per-workspace file. Anchor: `AGENTS.md` logging rule; `39-session-persist-prompt-text-errata.md` keeps prompt text out of the log by default. |
| human-readable startup errors | 69-D9 multi-line labelled block + 69-D11 TUI row stacking | dsh raises a typed error whose text is rendered by the caller; ymh pins the *presentation* (cause first, labelled context, no newline loss) because its status bar renders via FTXUI `text()` which drops `'\n'`. Anchor: `10-supervisor-tui.md` §6.3 (status bar) and `AGENTS.md` ("every `StartupRejected` return site emits its own specific diagnostic"). |

## 9. Test plan

Hermetic (`tests/unit/workspace_host_test.cpp`, no real LLM, no network):

| Test | Setup | Assertion |
|---|---|---|
| `StartupDiagnosability.ExecveFailureSurfacesErrno` | real `ForkExecLauncher` with an absolute path that cannot be executed (a non-existent `/…/ymh-does-not-exist`), a registered workspace row | the thrown `HostError::what()` contains `execve failed`, `No such file or directory`, and the `ldd` hint; it is **not** the bare `startup was rejected (StartupRejected)`; `HostError::code()` is `HostUnreachable` (69-I1, 69-I2) |
| `StartupDiagnosability.UnwritableLogSinkSurfacesDistinctReason` | real `ForkExecLauncher`; make `<root>/.ymh/host.log` a directory so `open(…, O_WRONLY)` returns `EISDIR` | the message contains `log sink` and `Is a directory`; it does not contain `execve`; distinct from 69-F1 (69-I3, 69-A7) |
| `StartupDiagnosability.LogTailSurfacesDaemonStderr` | real `ForkExecLauncher` pointed at an executable `/bin/sh` script that writes a marker to stderr and exits 16 | the message contains the marker from the log sink (69-D3, 69-F2) |
| `StartupDiagnosability.RedactionMasksSecretLinesInPlace` (Rev 3; was `RedactionDropsSecretLines`) | script writes an `api_key=…` line and a benign marker | the benign marker appears; the secret value does not; the marker-bearing line is present as `[redacted]` rather than removed (69-D6/69-D12, 69-I6, 69-F6) |
| `StartupDiagnosability.RedactionKeepsMarkerOnlyCause` (Rev 3) | script writes a `ymh --host: startup rejected: cannot read token=sk-secret` line, exit 16 | the message contains a `reason:` line carrying `startup rejected: cannot read [redacted]` (the `ymh --host:` prefix is stripped by `without_host_diagnostic_prefix`), does **not** contain `sk-secret`, and is not the bare `startup was rejected (StartupRejected)` (69-D12, 69-I1) |
| `StartupDiagnosability.SignaledExitIsNamed` (Rev 3) | script `kill -SEGV $$` (killed before readiness, before writing) | the message contains `exit code 139 (killed by SIGSEGV)`, not a bare `exit code 139` (69-D13, 69-I14, 69-F12) |
| `StartupDiagnosability.SpecificReasonLeadsOverTrailingNoise` (Rev 2) | script writes `ymh --host: startup rejected: no LLM provider is configured (provider setup failed)` **then** 20 benign `[tool] [warning]` lines, exit 16 | the message contains the reason with the `ymh --host:` prefix stripped; the reason's byte offset is **less than** the first context line's; the message contains no `" / "` (69-D8, 69-I10, 69-I11, 69-I13, 69-F9) |
| `StartupDiagnosability.MessageIsMultiLineIndentedAndLabelled` (Rev 2) | same script | the message contains `"\n  reason: "` and `"\n  recent daemon log:\n    "`; the context lines are on separate `'\n'`-separated indented rows; `HostError::what()` has ≥3 lines (69-D9, 69-I11) |
| `StartupDiagnosability.LongTailIsBounded` (Rev 2) | script writes the cause then 500 × 300-byte warning lines | the cause still appears as `reason:`; the message length is bounded (`< 2048` bytes); the context block holds ≤4 lines and no surfaced line exceeds 240 bytes (69-D10, 69-F11) |
| `StartupDiagnosability.LoaderLineOutranksDaemonDiagnostic` (Rev 2) | script writes a `ymh --host:` line **then** an `error while loading shared libraries:` line | `reason:` is the loader line; the daemon line is not presented as the reason (69-D8) |
| `UiRenderGolden.UI69_MultiLineStartupNoticeRendersOnSeparateRows` (Rev 2) | `UiModel` with no workspaces, `pushNotice` a two-line startup message, `render_to_ansi` | the rendered screen has the two lines on **different** rows, never concatenated (69-D11, 69-I12, 69-F10) |
| `UiRenderGolden.UI69_MultiLineStartupNoticeRendersWithActiveSession` (Rev 2) | `build_model()` (active session) + the same two-line notice, `render_to_ansi` | the notice is still rendered on separate rows below the status line and not dropped by the inline width fit (69-D11, 69-I12, 69-F10b) |
| `WorkspaceHostConfig.ExitCodeValuesArePinned` (existing) | — | unchanged: `StartupRejected == 16` (69-I4) |
| `HostLifecycleTest.ExitedDaemonFailsFastWithRealReason` (existing) | `FakeLauncher`, `reap_status = RegistryFailed` | still names `RegistryFailed`; a absent log sink yields no spurious tail (69-I7, 69-F5) |

**Pre-fix evidence.** Each new test is written to fail against the pre-errata
tree: pre-fix the readiness message is exactly
`daemon did not become ready: startup was rejected (StartupRejected)` (or
`… exit code 127`), which lacks `execve failed`, `No such file or directory`,
`log sink`, `Is a directory`, and every tail marker. The Rev 2 tests fail
against the Rev 1 tree too, and that failure is the point: Rev 1 joins the tail
with `" / "` (no `"\n  reason: "`, no labelled block), selects the cause by
position so trailing noise wins (`SpecificReasonLeadsOverTrailingNoise`), and
FTXUI concatenates the notice rows (`MultiLineNoticeRendersOnSeparateRows`). The
transcripts of the pre-fix runs are recorded in the change set / report.

**Rev 3 pre-fix evidence.** `RedactionKeepsMarkerOnlyCause` fails against the
Rev 2 tree: the redactor drops the `ymh --host:` cause line, so no cause is
selected and the message is the bare `startup was rejected (StartupRejected)`.
`SignaledExitIsNamed` fails against the Rev 2 tree: `describe_host_exit(139)`
returns the bare `exit code 139`. `RedactionMasksSecretLinesInPlace`'s
`[redacted]` presence fails pre-fix (the line was absent).

**Verification.** `cmake --build build -j` (warnings are errors) and
`ctest --test-dir build --output-on-failure --timeout 120` must be 100 % green,
with the existing suite count unchanged apart from the new cases. A live
two-process run must still attach successfully (no reason text on the happy
path).

## 10. Open questions

- **69-OQ1 (LOW).** Should the `ldd` hint also be emitted for `EACCES`? Today it
  is emitted only for absolute-path `ENOENT` (the loader/interpreter case). An
  `EACCES` reason already names the permission cause, so the hint is redundant;
  kept out to avoid noise. Revisit if users report confusion.
- **69-OQ2 (LOW).** The redaction set is a fixed token list, not a full
  secret-scanner. A secret with no marker word could pass. Accepted: the log
  sink's startup-time content is the daemon's own `fprintf` diagnostics, not
  prompts (prompts are gated by `logging.log_prompts`, default false).
- **69-OQ3 (LOW).** Loader detection matches glibc's
  `error while loading shared libraries`; musl's
  `Error loading shared library` (singular, different wording) would fall
  through to context-only. Out of the project's Ubuntu/glibc scope; revisit only
  if a musl target is added.
- **69-OQ4 (LOW).** The 256 KiB scan window boundary is not exercised by a test
  (the long-tail test stays inside the window so the cause is found). Accepted
  for now: the window is a safety bound, not a correctness contract, and the
  cause is emitted near the end of the daemon's life in the real failure.

## 11. Revision log

| Date | Revision | Note |
|---|---|---|
| 2026-09-30 | Rev 1 (draft) | Reason channel (pipe + bounded log tail), specific `StartupRejected` diagnostics, exit codes frozen. |
| 2026-09-30 | Rev 2 (draft) | Presentation: cause selected by content (69-D8), multi-line labelled block (69-D9/69-D10), TUI stacked rows (69-D11). |
| 2026-10-01 | Rev 3 (draft) | Independent verification fixes: 69-D4's single-line `… + ": " + reason` envelope was stale and now agrees with 69-D9; the 69-D6 redactor masks in place instead of dropping a marker-bearing line (69-D12/69-I6/69-F6), so a marker-named cause no longer regresses 69-I1 to a bare code; `describe_host_exit` names a signaled exit (69-D13/69-I14/69-F12). Tests: `RedactionKeepsMarkerOnlyCause`, `SignaledExitIsNamed`, `RedactionMasksSecretLinesInPlace`. |
