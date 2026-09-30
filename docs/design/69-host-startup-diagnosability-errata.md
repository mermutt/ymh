# 69 — Host Startup Diagnosability Errata

```
Status: verified (Rev 1) · reviewer: see `DESIGN_STATUS.md` row 69 · gate: 0 HIGH / 0 MEDIUM
Revision: 1
Component: 69 (errata) — amends 04-workspace-host-daemon.md (§2.2, §3.2, §3.3,
           §6.2, §11.2) and 11-m2-errata.md (§9.2) by reference; adds no wire,
           model, persistence, or exit-code change
Depends on: 04-workspace-host-daemon.md (verified), 11-m2-errata.md (verified;
            M2 frozen interfaces), 16-daemon-ownership.md (verified; supervisor-
            owned daemons), 12-m1-drift-errata.md (verified register)
Scope: the reason channel of daemon startup failure. Every `StartupRejected`
       (exit 16) and every dynamic-loader failure that prevents the daemon from
       publishing its claim must reach the supervisor as a short, actionable
       cause instead of the bare code string.
```

## 1. Purpose, scope, and precedence

The user report is *"I copied the `ymh` binary from another Ubuntu and got
`cannot start workspace: daemon did not become ready: startup was rejected
(StartupRejected)`. This is likely related to some missing library on this PC."*
The report is correct: the daemon child `dup2`s its stderr into the log sink
before `execve` (`src/host/workspace_host.cpp:1301-1302`), so a missing shared
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

**Gate (per `AGENTS.md`).** Independent Oracle review marks this `verified` once
the reason channel, the redaction bound, and the fd lifetime are confirmed
(see `DESIGN_STATUS.md` row 69).

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 69-A1 | 04 §2.2 | `include/ymh/host/workspace_host.hpp:58-68` | document that `StartupRejected`'s *reason* is carried out-of-band; the numeric value is unchanged |
| 69-A2 | 04 §3.2 spawn sequence | `src/host/workspace_host.cpp:1244-1345` (`spawn`); child write ends `1293-1300`, `1321-1334` | add the CLOEXEC diagnostic pipe; the child writes `strerror(errno)` + path on log-sink-open / `execve` failure before `_exit` |
| 69-A3 | 04 §3.2 fd cleanup | `src/host/workspace_host.cpp:114-132` (`close_extra_fds`) | `close_extra_fds` gains a `keep_fd` parameter so the diagnostic pipe survives to `execve` |
| 69-A4 | 04 §3.3 startup order | `src/host/workspace_host.cpp:404-439` (`run`; owner-watchdog site `408-413`), `641-747` (`startup`; sites `669-672`, `676-682`, `736-747`) | each `StartupRejected` return site emits a specific `stderr` diagnostic (owner-watchdog, chdir, state dir, runtime detail, no provider) |
| 69-A5 | 04 §6.1 launcher seam | `include/ymh/host/host_launcher.hpp:44-58` | `SpawnResult` gains `int diagnostic_fd{-1}` (parent-owned) |
| 69-A6 | 04 §6.2 `ensureRunning` | `src/host/workspace_host.cpp:1406-1492` (`spawnAndAttach`; closer `1414-1421`, compose `1433`, throw `1491-1492`) | compose the readiness error as `describe_host_exit(status) + ": " + <specific reason>`; read the pipe and the bounded log tail |
| 69-A7 | 04 §11.2 `D-F18` | `src/host/workspace_host.cpp:1291-1300` | log-sink open failure reports `cannot open log sink <path>: <strerror(errno)>` |
| 69-A8 | 11 §9.2 / 04 §3.2 exec failure | `src/host/workspace_host.cpp:1321-1334` | `execve` failure reports `execve failed: <strerror(errno)> (<path>)` plus the `ldd` hint on absolute-path `ENOENT` |

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
`token=` is dropped before inclusion (69-D6). Tail reading happens only on a
startup failure, never on the success path.

**69-D4 — Every `StartupRejected` return is specific.** Each of the return sites
`src/host/workspace_host.cpp:413,672,681,739-747` emits a distinct `stderr`
diagnostic naming the cause (and, for `execve`/chdir/open, `strerror(errno)`).
The readiness error composes
`"daemon did not become ready: " + describe_host_exit(status) + ": " + reason`
when a specific reason exists, so a user never sees only
`startup was rejected (StartupRejected)`.

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
is bounded and redacted per 69-D3. This honours `AGENTS.md` ("never dump full
prompts or sensitive tool output to normal logs by default") and does not consume
`logging.log_prompts`.

**69-D7 — Readiness-error format is pinned.** The single string the supervisor
throws is:

```text
daemon did not become ready: <describe_host_exit(status)>: <reason>
```

where `<reason>` is the pipe diagnostic (69-D2) when present, otherwise the
bounded daemon-stderr tail (69-D3). The pipe is authoritative and the tail is
never mixed in beside it: a log-sink-open failure means the sink was never
truncated, so a pre-existing file could supply a stale, misleading tail. The
`HostError` code stays `protocol::HostErrorCode::HostUnreachable`.

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
      pipe has bytes?  ── yes ──> reason = pipe text
      else log_sink tail (bounded/redacted) ──> reason = tail
      throw HostError(HostUnreachable,
                      "daemon did not become ready: " + describe_host_exit(status) + ": " + reason)
```

The loader case is the one the user hit. A missing shared library makes the
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

## 6. Invariants

| ID | Invariant |
|---|---|
| **69-I1** | For every `StartupRejected` exit, the readiness `HostError::what()` is strictly more than the bare `describe_host_exit(16)` string and names a cause (errno text, a named cause, or a log tail). No path yields a bare `StartupRejected`. |
| **69-I2** | The `execve`-failure reason contains `strerror(errno)` (e.g. `No such file or directory`, `Permission denied`, `Exec format error`). |
| **69-I3** | The log-sink-open-failure reason is textually distinct from the `execve`-failure reason (it contains `log sink`; the latter contains `execve`). |
| **69-I4** | No `HostExitCode` numeric value changes; `StartupRejected == 16`. |
| **69-I5** | The diagnostic pipe's write end is CLOEXEC and is the only fd preserved across `close_extra_fds`; the daemon inherits no diagnostic fd. |
| **69-I6** | The log tail surfaced is ≤4 lines / ≤512 bytes and contains no line matching the 69-D6 redaction set. |
| **69-I7** | On the success path (claim published, or winner-attach), the parent closes `diagnostic_fd`; no fd leak accrues across repeated spawns. |
| **69-I8** | A pipe-creation failure degrades to the daemon-stderr/describe path; `spawn` never throws for want of a diagnostic. |
| **69-I9** | The child writes at most one bounded single-line diagnostic and uses only `write(2)` on the pipe (no unbounded allocation, no secret material, no `HostConfig` dump). |

## 7. Failure modes (`69-F#`)

Component-local to this errata; they refine `04 §11.2 D-F18` and add the
diagnostics for `D-F1`/`D-F4`/`D-F21`.

| 69-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **69-F1** | `execve` fails (`ENOENT`/`EACCES`/`ENOEXEC`) | `execv` returns, `errno` set, child `_exit(16)` | Child writes `execve failed: <strerror> (<path>)` to the pipe; supervisor surfaces it and the code text (69-I1, 69-I2) |
| **69-F2** | Missing shared library / interpreter | loader prints to `fd 2` and exits 127, or `execve` `ENOENT` on an absolute path | Loader text surfaced from the bounded log tail; the pipe reason carries the `ldd` hint (69-D2, 69-D3) |
| **69-F3** | Log sink unwritable | `open(log_sink, …)` < 0 in the child | Child writes `cannot open log sink <path>: <strerror>` to the pipe; never bare (69-A7, 69-I3) |
| **69-F4** | Provider setup / runtime build failure | `WorkspaceRuntime::create` returns an error | Daemon prints `runtime setup failed (<code>): <detail>`; supervisor surfaces the log tail (69-A4) |
| **69-F5** | Diagnostic pipe cannot be created | `pipe2` < 0 | Spawn proceeds; readiness falls back to `describe_host_exit` + log tail; never a new failure (69-I8) |
| **69-F6** | Log tail contains credential-like text | line matches the 69-D6 redaction set | Line dropped before inclusion (69-I6) |
| **69-F7** | Daemon exits 0 before readiness | `tryReap` → 0 | Unchanged: `describe_host_exit(0)` = `exited cleanly`; no reason invention |
| **69-F8** | Supervisor dies with the read end open | process teardown | The kernel closes the fd; no daemon impact (the child end is already CLOEXEC-closed) (69-I5, 69-I7) |

## 8. dsh (DeepSeek Harness) mapping

| dsh surface | ymh mirror | Justification |
|---|---|---|
| process spawn error propagation | `SpawnResult::diagnostic_fd` + bounded log tail (69-D2/69-D3) | dsh surfaces spawn errors as typed exceptions with stderr captured; ymh keeps its frozen exit-code contract and adds an out-of-band reason channel, so this is a mirror of the *capability*, not the exact mechanism (`11 §9` freezes the exit codes). |
| "do not leak secrets in errors" | 69-D6 redaction + bound | Non-mirror: dsh has no explicit redaction set in its spawn path; ymh adds one because its log sink is a persistent per-workspace file. Anchor: `AGENTS.md` logging rule; `39-session-persist-prompt-text-errata.md` keeps prompt text out of the log by default. |

## 9. Test plan

Hermetic (`tests/unit/workspace_host_test.cpp`, no real LLM, no network):

| Test | Setup | Assertion |
|---|---|---|
| `StartupDiagnosability.ExecveFailureSurfacesErrno` | real `ForkExecLauncher` with an absolute path that cannot be executed (a non-existent `/…/ymh-does-not-exist`), a registered workspace row | the thrown `HostError::what()` contains `execve failed`, `No such file or directory`, and the `ldd` hint; it is **not** the bare `startup was rejected (StartupRejected)`; `HostError::code()` is `HostUnreachable` (69-I1, 69-I2) |
| `StartupDiagnosability.UnwritableLogSinkSurfacesDistinctReason` | real `ForkExecLauncher`; make `<root>/.ymh/host.log` a directory so `open(…, O_WRONLY)` returns `EISDIR` | the message contains `log sink` and `Is a directory`; it does not contain `execve`; distinct from 69-F1 (69-I3, 69-A7) |
| `StartupDiagnosability.LogTailSurfacesDaemonStderr` | real `ForkExecLauncher` pointed at an executable `/bin/sh` script that writes a marker to stderr and exits 16 | the message contains the marker from the log sink (69-D3, 69-F2) |
| `StartupDiagnosability.RedactionDropsSecretLines` | script writes a `api_key=…` line and a benign marker | the marker appears; `api_key=` does not (69-D6, 69-I6, 69-F6) |
| `WorkspaceHostConfig.ExitCodeValuesArePinned` (existing) | — | unchanged: `StartupRejected == 16` (69-I4) |
| `HostLifecycleTest.ExitedDaemonFailsFastWithRealReason` (existing) | `FakeLauncher`, `reap_status = RegistryFailed` | still names `RegistryFailed`; a absent log sink yields no spurious tail (69-I7, 69-F5) |

**Pre-fix evidence.** Each new test is written to fail against the pre-errata
tree: pre-fix the readiness message is exactly
`daemon did not become ready: startup was rejected (StartupRejected)` (or
`… exit code 127`), which lacks `execve failed`, `No such file or directory`,
`log sink`, `Is a directory`, and every tail marker. The transcript of the
pre-fix run is recorded in the change set / report.

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
