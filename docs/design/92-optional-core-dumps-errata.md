# 92 - Optional Core Dumps for Daemon Crash Backtraces: opt-in RLIMIT_CORE, effective-limit diagnostics, and a protected output directory

```
Status: verified (Rev 3)
Revision: 3
Verification status: adversarial reviewer (independent, Rev 3): GATE PASS (0 HIGH / 0 MEDIUM); Oracle confirm PASS
Component: 92 (errata) - a single, opt-in startup action in the workspace
           daemon that raises the RLIMIT_CORE soft limit to the observed hard
           limit (never above it), creates a protected owner-only output
           directory under <workspace>/.ymh/, and emits exactly one bounded
           startup line naming the effective soft/hard limits, the system
           core_pattern, and the recommended directory. It is OFF by default
           and can only be enabled from the global config layer or the
           YMH_CORE_DUMPS environment variable. It adds no signal handler, no
           in-process core writer, no wire/DB schema/UI change, and no change
           to any HostExitCode.
Depends on: 04-workspace-host-daemon.md (verified; sec 3.2 spawn, sec 3.3
           startup order, sec 4.1 HostConfig), 21-config-jsonc-errata.md
           (verified; JSONC schema, logging section, global-layer rule),
           69-host-startup-diagnosability-errata.md (verified; stderr ->
           host.log reason channel), 74-llm-transcript-errata.md (verified;
           the global-layer-only logging-key convention), 00-architecture.md
           sec 40 (logging security rule) and sec 54 D17/D18 (daemon split),
           16-daemon-ownership.md (verified; the daemon is the supervisor-owned
           long-lived process), 07-tools-execution.md (verified; the Spawn
           seam whose children inherit the limit), and
           56-dsh-fidelity-gap-closure-errata.md (verified; 56-D6 non-mirror
           justification convention).
Amends: 04 sec 3.3 (adds one optional, non-fatal startup step between the
        SIGPIPE disposition and the first thread), 21 sec 3.3 and sec 7.6 (adds
        the `logging.core_dumps` key), 21 sec 8.2 (adds one commented scaffold
        line), 74 sec 3.1 (extends the global-layer-only logging-key
        convention to this key by reference).
Supersedes: the rejected uncommitted proposal that would have inserted a
        default-on `setrlimit(RLIMIT_CORE, ...)` call after
        `::signal(SIGPIPE, SIG_IGN)` in `src/host/workspace_host.cpp:822`.
        That proposal is not adopted for the four reasons in sec 1.2; this
        spec replaces it with 92-D1..D13. No verified spec currently defines
        core dumps, so this errata is otherwise additive.
Revision note: Rev 3 fixes the gate-92 Rev 2 finding M-1 (I8 vs the fixed
        internal-error variant) and the still-open LOWs L-2..L-13: the
        internal-error line becomes a pinned constant with no dir/pattern
        fields, I8/D4 are scoped precisely, and the caller/anchor/placeholder/
        stderr/umask/gitignore/parent-symlink/headless/D13/dsh/D12/D6 wordings
        are corrected. L-1 (no tracker row) remains deferred by lead
        instruction. Rev 2 fixed the Rev 1 H-1/M-1..M-3/L-1..L-10.
Tracker: DESIGN_STATUS.md row 92 is NOT added by this revision (lead
        instruction: this task edits only the spec file). The shipped
        `tests/tools/check_design_status.py:199-201` fails for any `NN-*.md`
        without a row (verified for 92/93/94), so the orchestrator must add a
        `draft (Rev 3)` row before committing this file to `dev`; that row must
        state the same Status/Revision as this header. No tracker edit is made
        here.
```

## 1. Purpose, incident, and precedence

### 1.1 The report and the gap

The report is: "ymh crashed at work and there was not enough diagnostic
information to backtrace it." The daemon is a long-lived, detached process
(`04 sec 3.1`, `16 sec 1`). When it dies from a fatal signal, the shipped
process leaves only `host.log`; there is no memory image, no register state, and
no stack for a debugger. A user cannot answer "which thread, which address,
which allocation" from the log alone.

The operating system already has the mechanism: a fatal signal can write a core
file, subject to `RLIMIT_CORE` and `/proc/sys/kernel/core_pattern` (`core(5)`).
The daemon currently never touches `RLIMIT_CORE`. On a typical Linux desktop the
soft limit is 0 while the hard limit is unlimited, so the mechanism is present
but disabled for the process.

There is a second reality that the design must state: when `core_pattern`
starts with `|` (the systemd-coredump/apport form used by most current
distributions), the kernel **ignores `RLIMIT_CORE` for the piped dump**
(`core(5)`) and the user-space handler decides what to store. On such a host,
the crash may already be retrievable with `coredumpctl`, independent of this
switch. This spec still pins the limit raise (it is the correct action for a
file pattern), but it does not claim the raise is what causes a piped dump, and
the runbook tells the operator to check the handler first (92-D6, sec 12).

This errata pins an **optional** way to enable it for the daemon, correctly,
with diagnostics, and with a privacy boundary.

### 1.2 The rejected proposal (why the obvious patch is wrong)

A prior suggestion was to insert, after `::signal(SIGPIPE, SIG_IGN)`
(`src/host/workspace_host.cpp:822`), a default-on block that raises
`RLIMIT_CORE`. It has four independent defects:

1. **It is inverted.** The suggestion's condition, reproduced verbatim, was
   `if (cur <= soft && setrlimit(...) != 0)`, where `soft` bound the process's
   **current** `rlim_cur` (from `getrlimit`) and `cur` bound the **proposed**
   limit (`max(rlim_max, 64MB)`). With those bindings, a raise needs
   `cur > soft`; the left operand is then false, so `setrlimit` is skipped -
   the no-op case. When `cur <= soft` (no raise needed) the branch calls
   `setrlimit` anyway. The comparison is the wrong way round; the corrected
   predicate is `want > current_soft` (92-D2), and D13/T2 pin the intended
   behaviour as a regression.
2. **Its target can exceed the hard limit.** It uses
   `max(rlim_max, 64MB)` as the new soft limit. If `rlim_max` is, for example,
   32 MB, the requested soft value is 64 MB, which is greater than
   `rlim_max`; `setrlimit(2)` rejects that with `EINVAL`. The correct target is
   the observed hard limit, never a floor above it.
3. **It cites a non-existent decision.** The suggestion refers to `94-C1`;
   no `94-C1` decision exists in any spec in this tree. The correct decision
   anchor is 92-D2.
4. **It is default-on and dumps the whole address space.** A core is a full
   memory image. It can contain prompts, tool output, file contents, session
   data, and credentials held in memory (including a provider API key). Enabling
   that by default contradicts `00 sec 40` ("never dump full prompts or
   sensitive tool output into normal logs by default") and the logging rule in
   `AGENTS.md`.

Two further omissions are recorded because they shape the design: the proposal
never checks the return value of `getrlimit`/`setrlimit`, and it never considers
`core_pattern` (a pipe to `systemd-coredump`/`apport` may relocate or decline
the dump).

**This spec does not adopt the proposal.** It pins the corrected algorithm,
default-off opt-in, placement before the first thread, one bounded diagnostic
line, and a protected directory.

### 1.3 What this spec pins (in scope)

- **One switch:** `logging.core_dumps` (bool, default `false`) and the
  equivalent `YMH_CORE_DUMPS` environment override. The key is global-layer
  only (92-D1).
- **One pure decision function:** `plan_core_dump_raise(soft, hard)` with no
  syscalls, no environment, and no I/O (92-D2). It encodes:
  `want = hard` (which is `RLIM_INFINITY` when the hard limit is infinite);
  `apply` iff `hard != 0 && soft < hard`; `setrlimit({want, hard})`; never
  raise `rlim_max`; never lower anything.
- **One side-effecting startup helper:** `apply_core_dump_policy(enabled, root,
  diag)` that reads the limits, applies the plan, creates the protected
  directory, reads the core pattern, and emits one line (92-D3..92-D6).
- **Placement:** in `WorkspaceHost::Impl::startup()`, immediately after
  `::signal(SIGPIPE, SIG_IGN)` (`src/host/workspace_host.cpp:822`), before
  `executor_` (`:827`), before `WorkspaceRuntime::create` (`:871`), and before
  the owner-watchdog thread (`:1223`) or the transport runner thread
  (`src/transport/transport_server.cpp:312`) (92-D3).
- **One protected directory:** `<canonical-root>/.ymh/coredumps/`, a real
  (non-symlink) directory, mode 0700 when `dir_status=ok`, under the
  self-ignoring `.ymh/.gitignore` (`workspace_host.cpp:108-115`), plus
  `umask(0077)` on the opt-in path (92-D5).
- **One startup line** stating the effective soft/hard limits, the bounded
  `core_pattern`, and the recommended directory. No new output when the switch
  is off (92-D4).
- **A privacy warning** in the scaffold, in the startup line, and in the
  operator runbook (92-D7, sec 12).
- **Tests:** the pure helper's full truth table, config parsing, and a
  two-process daemon-start smoke test. No test may crash a process while dumps
  are enabled, so no test produces a core (92-D10, sec 11).

### 1.4 Non-goals and scope boundaries

- **No default behavior change.** With the switch absent/false, the daemon makes
  no `getrlimit`/`setrlimit` call, creates no directory, and prints no new line.
  "Off" means "ymh does not raise the limit", **not** "ymh forces cores off":
  if the launching shell or systemd unit already permits cores, the daemon may
  still be dumped. The operator must use `ulimit -c 0` or `LimitCORE=0` to
  suppress that; this spec deliberately does not override the host's policy
  (92-D7).
- **No supervisor or one-shot CLI coverage.** The supervisor TUI and
  `ymh list/show/replay/fork` keep their current limits. The daemon is the
  long-lived process that owns session data and the incident target. A future
  spec can call the same helper before the supervisor's first thread; that is
  OQ-92-1, not silently included here (92-D9).
- **No signal handler, no in-process core writer, no `gcore`, no upload.**
  ymh never creates or transmits a core file. The kernel or its configured
  pipe handler owns the actual artifact (92-D6).
- **No `core_pattern` mutation.** The sysctl is system-wide, root-only, and
  affects unrelated processes. ymh reads it and reports it; it never writes it
  (92-D6).
- **No protocol, event, database, TUI, or exit-code change.** The only new
  observable is one stderr line in `host.log` and one directory on disk
  (92-D12).
- **No core parsing or redaction.** A core is opaque bytes; ymh never reads,
  logs, redacts, or uploads its contents. The append-only session event log
  remains the authoritative trace (`01-session.md`); a core is a last-resort
  artifact (92-D7).

### 1.5 Precedence and relationship to 69

Spec 69 owns the failure-**reporting** channel: daemon stderr is redirected to
the log sink, and a bounded tail is surfaced when startup fails
(`69 sec 1`, 69-D2/69-D3). This spec adds a startup **diagnostic** line to the
same stderr channel; it does not create a new channel and does not change 69's
selection algorithm. If a daemon crashes during startup, 69's log-tail path can
still surface the core-dump line as context. This spec wins over 04 sec 3.3 only
for the new optional step 2.5; it does not change the order of the existing
steps.

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 92-A1 | 04 sec 3.3 startup order | `04-workspace-host-daemon.md:298-360`, shipped `src/host/workspace_host.cpp:822-827` | Insert optional step 2.5, "apply core-dump policy (opt-in)", immediately after the SIGPIPE disposition and before the first thread/runtime object. Non-fatal; no `HostExitCode`. |
| 92-A2 | 21 sec 3.3 / sec 7.6 `logging` schema | `include/ymh/config/config.hpp:216-221`, `src/config/config.cpp:572-587` | Add `core_dumps` (bool, default false, global-layer only) to `LoggingSettings`, `apply_logging`'s accepted-key list, and `apply_env_overrides` (`YMH_CORE_DUMPS`). A workspace-layer occurrence is a `ConfigError` (exit 2). |
| 92-A3 | 21 sec 8.2 scaffold text | `src/config/config.cpp:1509-1516` | Add one commented `// "core_dumps": false` line with the privacy warning. It stays commented so the generated file still loads to the built-in defaults (21-D7 / J15). |
| 92-A4 | 74 sec 3.1 global-layer-only logging convention | `74-llm-transcript-errata.md:92-118`, `src/config/config.cpp:579-582` | Extend the same global-layer-only rule and error shape to `logging.core_dumps`; the reason is the same (a repo-controlled layer must not opt a user into sensitive capture), with core-specific stakes (92-D7). |
| 92-A5 | 07 sec 5 / the Spawn seam (by reference) | `07-tools-execution.md` Spawn seam; `src/execution/process.cpp` | Declares that the raised limit is inherited by tool children and is intentional (92-D8). No change to 07's API; the consequence is documented and tested at the process level. |

## 3. Decision register

### 92-D1 - The switch is opt-in, default OFF, and global-layer only

`LoggingSettings` gains:

```cpp
bool core_dumps = false;  // 92-D1: opt-in, global-layer only, default OFF
```

Rules, in precedence order (last writer wins, matching 21 sec 3.4):

1. Built-in default: `false`.
2. Global `config.jsonc`: `logging.core_dumps` may be `true` or `false`.
3. Workspace `<root>/.ymh/config.jsonc`: the key is **rejected** with
   `ConfigError("... 'logging.core_dumps' is global-layer only")`. This mirrors
   the `llm_transcript` rule at `src/config/config.cpp:579-582`. A cloned
   repository must never be able to opt the user into dumping process memory.
4. Environment: `YMH_CORE_DUMPS` overrides the file (`apply_env_overrides`,
   `src/config/config.cpp:1868-1950`). The value uses the existing `truthy`
   helper (`:1455-1457`): `1`, `true`, `yes`, `on` mean on; every other
   non-empty value means off. An empty value is treated as unset because
   `env_value` returns `nullopt` for `""` (`:1682-1688`).

The env variable is process-global. The supervisor spawns the daemon with
`fork` + `execve` and inherits the environment (`04 sec 3.2`), so
`YMH_CORE_DUMPS=1 ymh` reaches the daemon as well; the daemon is also started with the
supervisor's effective `--config` path (`workspace_host.cpp:1393-1395`,
`:1546-1552`; 21-D15), so the file setting is consistent on both sides.

The scaffold gains one commented `core_dumps` key (with a short warning comment) in the `logging` object:

```jsonc
  "logging": {
    "level": "info",
    "log_prompts": false,
    // opt-in core dumps for crash backtraces. Global-layer only; OFF by
    // default. A core is a full memory image and may contain prompts, tool
    // output, file contents, session data, and API keys.
    // "core_dumps": false,
    "llm_transcript": false,
    "transcript_dir": ""
  },
```

Keeping it commented means the generated file still parses to the built-in
defaults (21-D7/J15) and no first-run behavior changes.

### 92-D2 - Correct limit math: target the observed hard limit, never a floor

The pure helper is:

```cpp
// include/ymh/host/core_dump.hpp
[[nodiscard]] CoreDumpPlan plan_core_dump_raise(rlim_t current_soft,
                                                rlim_t hard) noexcept;
```

The algorithm is the whole contract:

```text
if hard == 0:
    outcome = HardLimitZero; apply = false
elif current_soft >= hard:
    outcome = AlreadyAtCeiling; apply = false
else:
    outcome = RaiseToHard; apply = true; soft = hard
```

Properties that fix the rejected patch:

- **Target:** `want = hard`. When `hard == RLIM_INFINITY`, `want` is
  `RLIM_INFINITY`; no special case and no integer arithmetic are required. When
  `hard` is finite, `want == hard`.
- **No floor:** there is no `max(hard, 64MB)`. A 32 MB hard limit yields a
  32 MB soft target (`setrlimit` succeeds), not a 64 MB target (`EINVAL`).
- **No hard-limit raise:** the second argument of `setrlimit` is always the
  observed `rlim_max`. ymh never asks for a hard limit it did not already have;
  doing so would require `CAP_SYS_RESOURCE` and would be a privilege change.
- **No inversion:** `setrlimit` is called exactly when `apply` is true, which is
  exactly when the current soft limit is strictly below the hard limit. If the
  current soft is already at or above the hard limit, the helper is a no-op.
  The helper never lowers a limit.
- **Total:** every `rlim_t` pair maps to an outcome; no precondition can make
  it throw.

Truth table (all rows are unit-tested, sec 11):

| `current_soft` | `hard` | `apply` | `soft` target | Outcome |
|---|---|---|---|---|
| 0 | 0 | no | 0 | `HardLimitZero` |
| 0 | 32 MB | yes | 32 MB | `RaiseToHard` |
| 0 | 64 MB | yes | 64 MB | `RaiseToHard` |
| 0 | `RLIM_INFINITY` | yes | `RLIM_INFINITY` | `RaiseToHard` |
| 32 MB | 32 MB | no | 32 MB | `AlreadyAtCeiling` |
| 64 MB | 32 MB | no | 64 MB | `AlreadyAtCeiling` (never lower) |
| 1 MB | 32 MB | yes | 32 MB | `RaiseToHard` |
| `RLIM_INFINITY` | `RLIM_INFINITY` | no | `RLIM_INFINITY` | `AlreadyAtCeiling` |

The wrapper passes `rlim_t` to the plan unchanged. On the supported 64-bit
Linux targets `rlim_t` is an unsigned 64-bit type; no narrowing cast is used.

### 92-D3 - Call site and ordering: before any worker exists

The call site is in `WorkspaceHost::Impl::startup()`, immediately after the
existing SIGPIPE disposition:

```cpp
    ::signal(SIGPIPE, SIG_IGN);

    // 92-D3: raise RLIMIT_CORE before the first thread or runtime object.
    // Non-fatal: a failure never blocks daemon readiness.
    if (!config_.foreground) {
        (void)apply_core_dump_policy(config_.config.logging.core_dumps,
                                     canonical_root_, stderr);
    }

    boot_id_ = config_.boot_id.value_or(HostBootId{generate_uuid_v4()});
    pid_     = host_pid();

    executor_ = std::make_unique<AsioExecutor>(io_);
```

Why this exact position:

- `canonical_root_` is set and `chdir` has completed (`:805-817`), so the
  protected directory is rooted correctly and `.ymh` exists.
- `config_.config` has been loaded by `run_host_command` before
  `WorkspaceHost::create` (`src/cli/cli.cpp:291-326`).
- No thread exists yet: `executor_` is constructed on the next statement
  (`:827`); the owner watchdog starts later (`:1223`); the transport runner
  starts in `TransportServer::start` (`src/transport/transport_server.cpp:312`);
  `WorkspaceRuntime::create` is later (`:871`). `RLIMIT_CORE` is
  process-wide, but placing the call before the first thread removes any
  question about which thread observes the change and guarantees every
  descendant inherits it.
- It runs before the store open and `claimHost`, so a crash during the rest of
  startup is covered.
- It is non-fatal: `apply_core_dump_policy` reports every syscall/IO failure
  and returns an outcome; a diagnostic-path exception (including `bad_alloc`
  or path conversion) is caught and returned as `InternalError` with a fixed
  line; `startup()` ignores the return value. No new `HostExitCode`, no new
  `StartupRejected` site. Process-wide OOM outside this helper remains out of
  scope.

**`foreground` is a no-op.** `HostConfig::foreground` runs the host in the
caller's process for tests (`include/ymh/host/workspace_host.hpp:123-124`). The
in-process seam must not mutate the embedding process's resource limits, so the
policy is skipped entirely when `foreground == true`. The product daemon path
sets `foreground = false` (`src/cli/cli.cpp:328`); the enabled-path smoke test
uses the real two-process harness (sec 11), not the in-process seam.

### 92-D4 - One bounded startup line, and no line when off

The helper writes exactly one line to `diag` (the daemon passes `stderr`; a
supervisor-spawned daemon has that stderr redirected to `host.log` per
`04 sec 3.2`, while a directly launched `ymh --host` writes the line to its own
stderr) when, and only when:

- the switch is on (and not `foreground`), **and**
- the helper has finished its best-effort work (success or failure).

Disabled and `foreground` paths emit nothing. This preserves every existing
daemon-start log expectation and keeps the default byte-for-byte quiet.

Format (single line, ASCII, `\n`-terminated, at most 512 bytes):

```text
ymh --host: core dumps: enabled soft=<soft> hard=<hard> dir=<dir> dir_status=<ok|unavailable> pattern=<pattern> privacy=core-may-contain-prompts-tool-output-secrets
```

The non-`InternalError` variants share the prefix, the applicable field rules,
and the privacy token. The final `InternalError` variant is the fixed
`kCoreDumpInternalErrorLine` literal: it carries only the privacy token and
**no** `dir`/`dir_status`/`pattern` fields (M-1).

```text
ymh --host: core dumps: blocked (RLIMIT_CORE hard limit is 0; raise the launching shell/unit LimitCORE) soft=<soft> hard=<hard> dir=<dir> dir_status=<ok|unavailable> pattern=<pattern> privacy=core-may-contain-prompts-tool-output-secrets
ymh --host: core dumps: already at ceiling soft=<soft> hard=<hard> dir=<dir> dir_status=<ok|unavailable> pattern=<pattern> privacy=core-may-contain-prompts-tool-output-secrets
ymh --host: core dumps: setrlimit failed (<strerror>) soft=<soft> hard=<hard> dir=<dir> dir_status=<ok|unavailable> pattern=<pattern> privacy=core-may-contain-prompts-tool-output-secrets
ymh --host: core dumps: getrlimit failed (<strerror>); no limit change dir=<dir> dir_status=<ok|unavailable> pattern=<pattern> privacy=core-may-contain-prompts-tool-output-secrets
ymh --host: core dumps: internal error; no limit change privacy=core-may-contain-prompts-tool-output-secrets
```

Formatting rules:

- `<dir>` is a **display field**, not the path used for syscalls: it is
  `sanitize_diagnostic_field(dir.string(), 140)` - every byte outside printable
  ASCII `0x20..0x7e` becomes `?`, and a value longer than 140 bytes is cut to
  140 plus `...`. The real syscall path is the unsanitised
  `<canonical-root>/.ymh/coredumps`; the display form is bounded regardless of
  how the root was named.
- `dir_status=ok` means the leaf was a real directory (not a symlink), was
  created or already existed, was opened with `O_NOFOLLOW`, and `fchmod`'ed to
  0700;
  `dir_status=unavailable` means creation/opening/fchmod failed, or the
  leaf is a symlink/non-directory (92-D5). The limit result is independent of
  this status. The path in the line is only a recommendation when unavailable.
- `<soft>`/`<hard>` are the **observed** `rlim_cur`/`rlim_max` (the unchanged
  values on `setrlimit` failure, the effective values after a successful call,
  normally `0/0` in the blocked branch); `RLIM_INFINITY` is rendered as `unlimited`,
  otherwise decimal bytes. There is no literal `soft=<unchanged>` token; the
  testable contract is the numeric/`unlimited` rendering of the observed
  values (L-4/D4).
- `<pattern>` is the `core_pattern` text after: stripping a trailing newline;
  returning `unknown` if the result is empty; replacing every byte outside
  printable ASCII `0x20..0x7e` with `?`; and truncating to 100 bytes plus `...`
  if longer. The pattern is system configuration, not secret data, but it is
  still bounded.
- `<strerror>` is the saved `errno` text, passed through
  `sanitize_diagnostic_field(..., 60)` so a localised message cannot break the
  ASCII/length claim. The helper is called before any thread exists, so
  `std::strerror` is safe here; the code saves `errno` immediately after the
  failing call.
- The line never contains any core bytes, prompt text, tool output, or memory
  contents. Length is bounded by construction: every variable field is
  sanitised and capped (`<dir>` at most 143 bytes including `...`, `<pattern>`
  at most 103, the fixed text and numeric/error fields at most ~200), so the
  composed line is at most 512 bytes without relying on the socket-path guard
  (which caps a socket path, not the root). The `internal error` variant is the
  exact `kCoreDumpInternalErrorLine` constexpr literal and is used only when
  the diagnostic path itself throws (92-D11/L-4); it deliberately omits the
  sanitised fields because those allocations may be what failed, and carries
  only the privacy token (M-1).

The line is intentionally on the daemon's stderr (`host.log` on the spawned
path), not the TUI status bar: it is a startup diagnostic, not a user-facing
notification. If the daemon fails during
startup, 69's bounded log-tail path can surface it as context.

### 92-D5 - Protected output directory and umask

When the switch is on, the helper best-effort creates:

```text
<canonical-root>/.ymh/coredumps/     real directory, mode 0700 (dir_status=ok)
```

- `.ymh/` is created by `startup()` (`src/host/workspace_host.cpp:812-819`);
  its `chmod` there is best-effort, so an existing `.ymh` is not guaranteed
  `0700` by that pre-existing step. The daemon writes a self-ignoring
  `.gitignore` with `*` only when it is absent (`:108-115`) and never untracks
  a repo-shipped path, so a repo can ship a different `.gitignore` or a tracked
  path. The H-1 leaf contract is the protection for the new directory; a
  blanket "never tracked by git" is not claimed.
- **The leaf must be a real directory; a symlink is never followed (H-1).**
  The contract is:
  1. `symlink_status(dir, ec)`. If the leaf exists and is a symlink or is not
     a directory, set `dir_ok=false`, do not call `create_directories`, and do
     not call any path-based chmod.
  2. `create_directories(dir, ec)` best-effort (it may create the leaf).
  3. `open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)`. `ELOOP` or
     `ENOTDIR` means a symlink/non-directory was planted or raced in: set
     `dir_ok=false`; never fall back to a path-based chmod.
  4. Only `fchmod(fd, S_IRWXU)` on the opened fd; then close it. `chmod(path)`
     is forbidden for this leaf.
  A repo-shipped `.ymh/coredumps` symlink is therefore refused, its target is
  never chmod'ed, and the line reports `dir_status=unavailable`. The tree
  patterns for this are `src/registry/registry.cpp:1454` (symlink refusal), `src/execution/filesystem.cpp:50,103` (O_NOFOLLOW), and `src/registry/registry.cpp:728` (fchmod on the opened fd).
  The daemon does not unlink or repair the offending path; it only declines to
  use it.
- The pre-existing `.ymh` create/chmod shape at `src/host/workspace_host.cpp:815-819`
  is not changed or claimed as symlink-safe here; this contract applies to the
  new `coredumps` leaf. A symlinked `.ymh` is a pre-existing, separate issue
  (recorded for the reader, not silently made worse).
- A failure of any step above is reported in the one startup line as
  `dir_status=unavailable` and is non-fatal. The directory is attempted even
  when the hard limit is 0, so the operator has a known destination if they
  later raise the hard limit and restart.
- `::umask(0077)` is set on the opt-in path before the directory/limit work.
  This matches the launcher's existing `::umask(0077)` in the spawned child
  (`:1453`), so the daemon-spawned path is already owner-only; the call makes
  a directly launched `ymh --host` consistent and is idempotent. On a directly
  launched `--host`, it also makes files created by tool descendants
  owner-only, matching the normal spawn path; that hardening is intentional.
- When `dir_status=ok`, the real 0700 directory is the primary protection ymh
  can guarantee. The umask is defense-in-depth for files created on this path;
  do not rely on a particular kernel file mode. A pipe handler
  (`systemd-coredump`, `apport`) owns its own file mode and location; ymh
  cannot change that (92-D6). When `dir_status=unavailable`, no directory
  guarantee is made at all.
- ymh never creates, writes, reads, truncates, or deletes a core file itself.
  The directory is only a destination recommendation named in the startup
  line.

**The kernel owns the actual path.** `core_pattern` decides where a core goes.
If it names a relative path (for example `core`), the kernel writes relative to
the crashing process's cwd; the daemon's cwd is the workspace root (`chdir`,
`04 sec 3.3`), so the file can appear at `<workspace>/core`, not in
`coredumps/`. ymh cannot redirect it without changing the process cwd (forbidden
by H1/H2 and path safety) or the system sysctl (92-D6). The startup line names
both the recommended directory and the actual pattern so the operator is not
misled.

### 92-D6 - `core_pattern` is authoritative; ymh reads it and never writes it

The helper reads `/proc/sys/kernel/core_pattern` best-effort, bounded as in
92-D4. Cases and the required reporting:

| Literal first byte / form | Meaning | Required behavior |
|---|---|---|
| `|` (pipe) | The kernel pipes the dump to a user-space handler (`systemd-coredump`, `apport`, a container runtime). | Report `pattern=<pattern>`. State in the runbook that the kernel **ignores `RLIMIT_CORE`** for a piped dump (`core(5)`) and the handler decides whether and where to store it; `systemd-coredump` is retrieved with `coredumpctl`. ymh makes no claim that its raise caused the dump or that a file exists. |
| `/` (absolute path) | The kernel writes to that absolute path (with `%` substitutions). | Report it. If it is under the recommended directory, the protected-dir expectation holds; otherwise the system policy owns the location and mode. |
| other non-empty (relative) | The kernel writes relative to the crashing process's cwd. | Report it and warn in the runbook that a relative pattern lands in the workspace root for the daemon (and in each tool child's cwd for its descendants). Recommend an absolute path under `coredumps/` (root-owned sysctl) or a pipe handler. |
| empty or unreadable | Unknown/disabled pattern. | Report `pattern=unknown`; do not fail. |

The sysctl is never written. It is host-wide and root-only; ymh does not write
it regardless of privilege (a root daemon could, but must not), because it
would affect unrelated processes. The helper only reads bytes.

This table is **reporting guidance**, not a classification contract: no symbol
in this spec parses the pattern or branches on its form. The only first-byte
test is the diagnostic hint used by the runbook/F8, and it is the kernel's test
(the literal first byte; a leading space is a file pattern, not a pipe). The
helper's output is the sanitised text in D4.

Consequence for the incident: on a systemd host with a piped pattern, check
`coredumpctl list ymh` **before** enabling this switch; the dump may already be
there under the handler's policy. On a classic file pattern, the raised soft
limit is the mechanism that permits the dump. The startup line always reports
the observed limits and the pattern, so the operator can tell which case they
are in.

### 92-D7 - Privacy boundary: a core is a full memory image

A core is not a log line. It is the process's address space, and the daemon's
address space contains:

- system prompt and conversation text, including user prompts and model output;
- tool inputs and outputs, including file contents read by tools;
- session and persistence buffers;
- credential material held in memory (provider API keys, authorization header
  values, MCP tokens) even though the config stores only the *name* of the
  key's environment variable;
- MCP server data and any secrets they handle.

Therefore:

1. **OFF by default.** No core-dump limit is raised unless the user opts in.
2. **Global-layer only.** A repository cannot opt a user in (92-D1).
3. **The startup line carries a warning token** (`privacy=...`) and the
   scaffold comment states the risk (92-D1, 92-D4).
4. **No core contents are ever logged, parsed, or transmitted.** The event log
   remains the authoritative trace (`01-session.md`, `00 sec 40`).
5. **Off is not a suppression switch.** If the launching environment already
   permits cores (for example `ulimit -c unlimited`), a crash may still produce
   one. If `core_pattern` is a pipe, the kernel ignores `RLIMIT_CORE` and the
   handler may collect a dump even with the switch off. ymh deliberately does
   not lower a limit it did not raise, and it does not configure the system
   handler. To suppress, use the host's mechanism: `ulimit -c 0` /
   `LimitCORE=0` for a file pattern, or the piped handler's own configuration
   (`coredump.conf` `Storage=none`; a per-process `PR_SET_DUMPABLE` decision
   belongs to the process itself, not to an external operator).
6. **Delete after diagnosis.** The runbook says so (sec 12). A core under
   `.ymh/coredumps/` is owner-only but still sensitive.

### 92-D8 - Inheritance to tool children is intentional and documented

`RLIMIT_CORE` is process-wide on Linux; every thread and every `fork`/`exec`
descendant inherits the soft and hard limits. The daemon spawns shell,
terminal, and git children through the execution-environment seam
(`07-tools-execution.md`), so a crashing tool child can also produce a core.

This is accepted, not accidental:

- the incident target is the daemon, and the limit must be in place before any
  worker can crash;
- a tool-child core is often more useful than no core (the failing compiler or
  test runner is the crash);
- the privacy warning (92-D7) covers descendants explicitly.

If daemon-only dumps are ever required, the correct seam is the Spawn seam in
07: drop `RLIMIT_CORE` soft to 0 in the child between `fork` and `exec`. That
is a behavioural change to every tool and is **not** included here; it is
OQ-92-2.

### 92-D9 - Scope: the workspace daemon only

The policy is applied only by `WorkspaceHost::Impl::startup()`. The supervisor
TUI, `ymh run`/headless (which builds an in-process `WorkspaceRuntime` at
`src/cli/headless.cpp:151-180` and does not call `WorkspaceHost::startup`), and
the one-shot `list/show/replay/fork` commands keep their inherited limits.
Reasons:

- the daemon is the long-lived process that owns sessions and tool execution,
  and it is the process that crashed in the report;
- the supervisor is a UI process with a different crash profile; mutating its
  limits also mutates the user's shell child in a way that is harder to reason
  about;
- the in-process `foreground` test seam must remain side-effect-free (92-D3).

A future spec can call the same helper before the supervisor's first thread
(OQ-92-1). Because the helper is pure/I/O-local and has no dependency on daemon
state beyond the root path, that extension is additive.

### 92-D10 - Test policy: prove the math, prove the wiring, never produce a core

- The pure helper is exhaustively unit-tested; it performs no syscalls, so it
  cannot produce a core.
- The daemon-start smoke test uses the real two-process `HostHarness`
  (`tests/support/host_harness.hpp:94-137`), which starts `ymh --host` and
  stops it with `host.shutdown`. It never sends `SIGSEGV`/`SIGABRT`/`SIGQUIT`,
  and it never calls `crash_now` (`tests/support/crash_inject.hpp`) while dumps
  are enabled. Therefore no test produces a core in CI.
- The smoke test asserts a clean exit and reads `read_log()`
  (`host_harness.hpp:217-222`), which concatenates `host.log`, stdout, and
  stderr. It tolerates a host whose hard limit is 0 by accepting the `blocked`
  line; the `RaiseToHard` and `HardLimitZero` branches are proven by the pure
  tests, not by requiring CI to have a particular privilege.
- Tests that do not opt in assert the default: no `core dumps:` line and no
  `coredumps/` directory.

### 92-D11 - Config errors fail loud; runtime failures are best-effort

Two different classes, deliberately:

| Class | Example | Behavior |
|---|---|---|
| Configuration error | `"core_dumps": "yes"` (non-bool); the key in the workspace layer; an unknown sibling key | `ConfigError`, exit 2, before the daemon starts. This is a user mistake and must not be silently ignored. |
| Runtime capability failure | `getrlimit` fails; `hard == 0`; `setrlimit` fails with `EPERM`/`EINVAL`; `coredumps/` cannot be created or is a symlink; `core_pattern` is unreadable/empty | Exactly one bounded line; the daemon continues to readiness. The option is diagnostic; it must never be a new way to fail startup. |
| Diagnostic-path exception | A `std::filesystem_error`/path-conversion throw or `std::bad_alloc` inside the helper | The `noexcept` helper catches `...`, writes the fixed `internal error; no limit change` line (no allocation) to `diag`, and returns `CoreDumpOutcome::InternalError`; startup continues. OOM elsewhere is out of scope (92-D3). |

No new `HostExitCode` and no change to `StartupRejected == 16`.

### 92-D12 - Additive surface only

No new protocol method, event type, database column, registry row, TUI model
field, or exit code. The only new persistent filesystem object is the
`coredumps/` directory (state-lifetime table, sec 7); the startup line also
persists in `host.log` until the next spawn (state table). The only new log
output is the line in 92-D4, and it is off by default.

### 92-D13 - The prior proposal is rejected and its defects are pinned as tests

The four defects in sec 1.2 are recorded; the testable ones are pinned as regressions, and the citation defect is a documentation criterion (not a test):

- inverted condition -> `plan(0, 32MB).apply == true`;
- `max(hard, 64MB)` -> `plan(0, 32MB).soft == 32MB`, not 64MB;
- nonexistent `94-C1` -> the rejected citation is not used as a
  justification; the decision anchor is 92-D2;
- default-on -> the config default test asserts `false`, and the disabled
  daemon test asserts no line and no directory.

## 4. The crash-dump model

### 4.1 Startup flow

```text
WorkspaceHost::Impl::startup()
  ... validate root; canonicalize; chdir; create .ymh; write .gitignore ...
  ::signal(SIGPIPE, SIG_IGN)
  |
  +-- config_.foreground? -- yes --> skip, emit nothing
  |
  no
  |
  +-- enabled == false? --- yes --> return Disabled; no syscall, no output
  |
  no
  |
  +-- umask(077); symlink_status leaf; create real dir; open O_NOFOLLOW;
  |   fchmod(fd, 0700); close; dir_status=ok|unavailable
  +-- read /proc/sys/kernel/core_pattern; empty/unreadable -> "unknown";
  |   sanitise/bound (100) for the display field; sanitise/bound dir (140)
  +-- getrlimit(RLIMIT_CORE) -- fail --> one line; return GetLimitFailed
  +-- plan_core_dump_raise(soft, hard)
  |     hard == 0        --> one "blocked" line; return HardLimitZero
  |     soft >= hard     --> one "already at ceiling" line; return
  |     soft < hard      --> setrlimit({hard, hard})
  |                           fail --> one "setrlimit failed" line; return
  |                           ok   --> one "enabled" line; return RaiseToHard
  |   any throw          --> fixed "internal error" line; return InternalError
  |
  boot_id_ / pid_ / executor_ / runtime / threads ...
```

Every branch is non-fatal. No branch returns a `HostExitCode` or calls
`requestShutdown`.

### 4.2 What "enabled" means in the startup line

The word `enabled` means "ymh successfully raised the soft limit to the hard
limit (or found it already there)." It does **not** mean "a core file exists",
"a core will be written", or "the kernel used this limit." The actual outcome
depends on `core_pattern` (a pipe makes the kernel ignore `RLIMIT_CORE`
entirely), `core_uses_pid`, `suid_dumpable`, the signal (SIGKILL never dumps),
and any container/seccomp policy. The runbook (sec 12) tells the operator how
to verify, starting with `coredumpctl` when the pattern is a pipe.

### 4.3 Process-level scope

The limit is a property of the process, not of a session or a workspace row.
It survives `fork` and `exec`; it is reset only by another `setrlimit` or by a
new process tree. A daemon restart re-evaluates the config and re-applies the
policy from the supervisor's inherited limits. A config change takes effect on
the next daemon start, not live.

## 5. C++ interface sketches

```cpp
// include/ymh/host/core_dump.hpp (new)
#pragma once

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/resource.h>

namespace ymh {

// 92-D2: one opt-in switch, one directory name.
inline constexpr std::string_view kCoreDumpDirName = "coredumps";

// 92-I8/M-1: the ONLY line emitted by the InternalError path. It is a fixed
// literal (written with allocation-free fputs) and deliberately has no
// dir/pattern fields, because those allocations may be what failed.
inline constexpr char kCoreDumpInternalErrorLine[] =
    "ymh --host: core dumps: internal error; no limit change "
    "privacy=core-may-contain-prompts-tool-output-secrets\n";

enum class CoreDumpOutcome : std::uint8_t {
    Disabled,          // switch off or foreground: no syscall, no output
    AlreadyAtCeiling,  // soft >= hard; nothing to raise
    HardLimitZero,     // hard == 0; the kernel forbids a core
    RaiseToHard,       // setrlimit succeeded; soft == hard
    SetLimitFailed,    // setrlimit failed; the old limit is retained
    GetLimitFailed,    // getrlimit failed; no limit change
    InternalError,     // diagnostic-path exception; fixed line, no limit change
};

struct CoreDumpPlan {
    bool            apply = false;  // true iff setrlimit should be called
    rlim_t          soft = 0;       // requested rlim_cur when apply
    rlim_t          hard = 0;       // observed rlim_max; never raised
    CoreDumpOutcome outcome = CoreDumpOutcome::Disabled;
};

// Pure: no syscalls, no environment, no I/O, no allocation.
// Caller: apply_core_dump_policy (src/host/core_dump.cpp); unit tests.
[[nodiscard]] CoreDumpPlan plan_core_dump_raise(rlim_t current_soft,
                                                rlim_t hard) noexcept;

// Pure display-field formatter. Replaces bytes outside printable ASCII
// 0x20..0x7e with '?' and truncates to `max_bytes` plus "...". Caller:
// read_core_pattern (pattern, max 100) and apply_core_dump_policy (dir, max
// 140); unit tests.
[[nodiscard]] std::string sanitize_diagnostic_field(std::string_view text,
                                                    std::size_t max_bytes);

// I/O helper, not pure. Returns "unknown" for an unreadable or empty file;
// otherwise the sanitised, bounded pattern. `path` is injectable for tests.
// Caller: apply_core_dump_policy.
[[nodiscard]] std::string read_core_pattern(
    const std::filesystem::path& path =
        "/proc/sys/kernel/core_pattern");

// Side-effecting but non-fatal. Emits at most one bounded line to `diag`
// when `enabled` is true. `diag` must be a valid writable stream; the daemon
// passes stderr. The body is wrapped in try/catch: any allocation/path
// exception becomes InternalError plus the fixed internal-error line, never
// an escape from `noexcept`. Caller: WorkspaceHost::Impl::startup
// (src/host/workspace_host.cpp, immediately after ::signal(SIGPIPE, ...)).
[[nodiscard]] CoreDumpOutcome apply_core_dump_policy(
    bool enabled,
    const std::filesystem::path& workspace_root,
    std::FILE* diag) noexcept;

} // namespace ymh
```

```cpp
// src/host/core_dump.cpp (new; sketch of the body)
#include "ymh/host/core_dump.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ymh {
namespace {

std::string render_limit(rlim_t value) {
    return value == RLIM_INFINITY ? std::string{"unlimited"}
                                  : std::to_string(static_cast<std::uint64_t>(value));
}

} // namespace

std::string sanitize_diagnostic_field(std::string_view text, std::size_t max_bytes) {
    std::string out;
    const std::size_t limit = text.size() < max_bytes ? text.size() : max_bytes;
    out.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        const unsigned char u = static_cast<unsigned char>(text[i]);
        out.push_back((u >= 0x20 && u <= 0x7e) ? text[i] : '?');
    }
    if (text.size() > max_bytes) {
        out.append("...");
    }
    return out;
}

std::string read_core_pattern(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    std::string text;
    if (!input) {
        return "unknown";
    }
    std::getline(input, text);
    if (text.empty()) {
        return "unknown";  // M-1: empty file is not "pattern="
    }
    return sanitize_diagnostic_field(text, 100);
}

CoreDumpPlan plan_core_dump_raise(rlim_t current_soft, rlim_t hard) noexcept {
    if (hard == 0) {
        return CoreDumpPlan{false, current_soft, hard, CoreDumpOutcome::HardLimitZero};
    }
    if (current_soft >= hard) {
        return CoreDumpPlan{false, current_soft, hard, CoreDumpOutcome::AlreadyAtCeiling};
    }
    return CoreDumpPlan{true, hard, hard, CoreDumpOutcome::RaiseToHard};
}

CoreDumpOutcome apply_core_dump_policy(bool enabled,
                                       const std::filesystem::path& workspace_root,
                                       std::FILE* diag) noexcept {
    if (!enabled) {
        return CoreDumpOutcome::Disabled;
    }

    try {
        ::umask(0077);  // matches the spawned child at workspace_host.cpp:1453

        const std::filesystem::path dir = workspace_root / ".ymh" / kCoreDumpDirName;
        std::error_code link_error;
        const std::filesystem::file_status link_status =
            std::filesystem::symlink_status(dir, link_error);
        const bool leaf_exists =
            !link_error && std::filesystem::exists(link_status);
        const bool leaf_is_bad =
            leaf_exists && (std::filesystem::is_symlink(link_status) ||
                            !std::filesystem::is_directory(link_status));

        bool dir_ok = false;
        if (!leaf_is_bad) {
            std::error_code create_error;
            std::filesystem::create_directories(dir, create_error);
            if (!create_error) {
                // H-1: never follow the leaf. fchmod only on the opened fd.
                const int fd = ::open(
                    dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (fd >= 0) {
                    dir_ok = (::fchmod(fd, S_IRWXU) == 0);
                    ::close(fd);
                }
            }
        }
        const char* dir_status = dir_ok ? "ok" : "unavailable";
        const std::string dir_field =
            sanitize_diagnostic_field(dir.string(), 140);  // ASCII, <=143 bytes
        const std::string pattern = read_core_pattern();   // "unknown" if empty

        struct rlimit current {};
        if (::getrlimit(RLIMIT_CORE, &current) != 0) {
            const int saved = errno;
            const std::string error_text =
                sanitize_diagnostic_field(std::strerror(saved), 60);
            std::fprintf(diag,
                         "ymh --host: core dumps: getrlimit failed (%s); no limit change "
                         "dir=%s dir_status=%s pattern=%s "
                         "privacy=core-may-contain-prompts-tool-output-secrets\n",
                         error_text.c_str(), dir_field.c_str(), dir_status,
                         pattern.c_str());
            return CoreDumpOutcome::GetLimitFailed;
        }

        const CoreDumpPlan plan = plan_core_dump_raise(current.rlim_cur, current.rlim_max);

        if (plan.apply) {
            const struct rlimit next {plan.soft, plan.hard};
            if (::setrlimit(RLIMIT_CORE, &next) != 0) {
                const int saved = errno;
                const std::string error_text =
                    sanitize_diagnostic_field(std::strerror(saved), 60);
                std::fprintf(diag,
                             "ymh --host: core dumps: setrlimit failed (%s) soft=%s hard=%s "
                             "dir=%s dir_status=%s pattern=%s "
                             "privacy=core-may-contain-prompts-tool-output-secrets\n",
                             error_text.c_str(), render_limit(current.rlim_cur).c_str(),
                             render_limit(current.rlim_max).c_str(), dir_field.c_str(),
                             dir_status, pattern.c_str());
                return CoreDumpOutcome::SetLimitFailed;
            }
        }

        struct rlimit effective {};
        const bool have_effective = ::getrlimit(RLIMIT_CORE, &effective) == 0;
        const rlim_t soft = have_effective ? effective.rlim_cur : plan.soft;
        const rlim_t hard = have_effective ? effective.rlim_max : plan.hard;

        if (plan.outcome == CoreDumpOutcome::HardLimitZero) {
            std::fprintf(diag,
                         "ymh --host: core dumps: blocked (RLIMIT_CORE hard limit is 0; "
                         "raise the launching shell/unit LimitCORE) soft=%s hard=%s dir=%s "
                         "dir_status=%s pattern=%s "
                         "privacy=core-may-contain-prompts-tool-output-secrets\n",
                         render_limit(soft).c_str(), render_limit(hard).c_str(),
                         dir_field.c_str(), dir_status, pattern.c_str());
            return CoreDumpOutcome::HardLimitZero;
        }
        if (plan.outcome == CoreDumpOutcome::AlreadyAtCeiling) {
            std::fprintf(diag,
                         "ymh --host: core dumps: already at ceiling soft=%s hard=%s dir=%s "
                         "dir_status=%s pattern=%s "
                         "privacy=core-may-contain-prompts-tool-output-secrets\n",
                         render_limit(soft).c_str(), render_limit(hard).c_str(),
                         dir_field.c_str(), dir_status, pattern.c_str());
            return CoreDumpOutcome::AlreadyAtCeiling;
        }

        std::fprintf(diag,
                     "ymh --host: core dumps: enabled soft=%s hard=%s dir=%s dir_status=%s "
                     "pattern=%s privacy=core-may-contain-prompts-tool-output-secrets\n",
                     render_limit(soft).c_str(), render_limit(hard).c_str(),
                     dir_field.c_str(), dir_status, pattern.c_str());
        return CoreDumpOutcome::RaiseToHard;
    } catch (...) {
        // L-4: the diagnostic path itself failed (bad_alloc/path conversion).
        // fputs is allocation-free; the line is a fixed literal.
        if (diag != nullptr) {
            // kCoreDumpInternalErrorLine is a constexpr char[] (NUL-terminated,
            // no allocation); this is the M-1 fixed variant.
            std::fputs(kCoreDumpInternalErrorLine, diag);
        }
        return CoreDumpOutcome::InternalError;
    }
}

} // namespace ymh
```

Notes on the sketch:

- `plan_core_dump_raise` and `sanitize_diagnostic_field` are the pure,
  unit-test targets. `read_core_pattern` is a small injectable I/O helper.
  `apply_core_dump_policy` is deliberately small and linear so its failure
  branches are auditable.
- The whole body runs inside `try { ... } catch (...) { ... }`; the catch writes
  the fixed no-allocation `internal error` line and returns `InternalError`, so
  `noexcept` holds even if a path/string allocation throws (L-4).
- `umask`, the symlink check, creation, `open(O_NOFOLLOW)`, and `fchmod` happen
  before `getrlimit`; a failure is reported as `dir_status=unavailable` in the
  same line. It never changes the limit contract.
- Directory creation, `symlink_status`, `open(O_NOFOLLOW)`, and `fchmod` are
  best-effort; `std::error_code` avoids an exception on a read-only or
  permission-denied filesystem. A symlink/non-directory leaf is never followed
  and never path-chmod'ed (H-1).
- The code saves `errno` immediately after each failing syscall. It runs before
  any thread, so `std::strerror` is safe.
- The helper never calls `abort`, never raises a signal, and never reads a core.
- `coredumps/` is created even when `hard == 0`; this is why the blocked line
  still names a usable directory after the operator raises the hard limit.

## 6. New symbols and their callers

Every new symbol is normative: it has a body above and at least one concrete
caller in the tree it lands in (AGENTS.md).

| Symbol | Declared | Concrete caller(s) |
|---|---|---|
| `LoggingSettings::core_dumps` | `include/ymh/config/config.hpp:216-221` | Read by `apply_logging` (`src/config/config.cpp:572-587`), overridden by `apply_env_overrides` (`:1868-1950`), consumed by `WorkspaceHost::Impl::startup` (`src/host/workspace_host.cpp:822-830`). |
| `apply_env_overrides` `YMH_CORE_DUMPS` | `src/config/config.cpp:1868-1950` | `load_config` (`:1980-1987`); the daemon reaches it through `run_host_command` (`src/cli/cli.cpp:291-300`). |
| `kCoreDumpDirName` | `include/ymh/host/core_dump.hpp` | `apply_core_dump_policy` (`src/host/core_dump.cpp`). The smoke test asserts the literal `<root>/.ymh/coredumps` path; it is not a symbol caller. |
| `kCoreDumpInternalErrorLine` | `include/ymh/host/core_dump.hpp` | The `catch (...)` branch of `apply_core_dump_policy` (`src/host/core_dump.cpp`); `tests/unit/core_dump_test.cpp` T14 pins its exact text/fields. |
| `CoreDumpOutcome`, `CoreDumpPlan` | `include/ymh/host/core_dump.hpp` | `plan_core_dump_raise`, `apply_core_dump_policy`, unit tests. |
| `plan_core_dump_raise` | `include/ymh/host/core_dump.hpp` | `apply_core_dump_policy` (`src/host/core_dump.cpp`); `tests/unit/core_dump_test.cpp`. |
| `sanitize_diagnostic_field` | `include/ymh/host/core_dump.hpp` | `read_core_pattern` (pattern, max 100) and `apply_core_dump_policy` (dir, max 140); `tests/unit/core_dump_test.cpp` (UTF-8/control/truncation). |
| `read_core_pattern(path)` | `include/ymh/host/core_dump.hpp` (default `/proc/sys/kernel/core_pattern`) | `apply_core_dump_policy` (`src/host/core_dump.cpp`); `tests/unit/core_dump_test.cpp` (empty/unreadable/valid temp files). |
| `apply_core_dump_policy` | `include/ymh/host/core_dump.hpp` | `WorkspaceHost::Impl::startup` (`src/host/workspace_host.cpp`, call at `:824-828`); the two-process smoke test exercises it through `ymh --host`. |
| `src/host/core_dump.cpp` | `CMakeLists.txt` (add to the `ymh_host` target at `:686-690`) | Linked into `ymh::host`; no new dependency. |
| `tests/unit/core_dump_test.cpp` | `tests/CMakeLists.txt` (add near the host tests) | Unit truth table (sec 11). |
| `tests/unit/core_dump_host_smoke_test.cpp` | `tests/CMakeLists.txt` | Two-process `HostHarness` smoke test (sec 11). |

## 7. State-lifetime table

| State | Created | Stored | Lifetime | Destroyed / reset | Survives restart? | Crash path |
|---|---|---|---|---|---|---|
| `Config.logging.core_dumps` | `load_config` (`src/config/config.cpp:1980-1987`) | `Config` on the stack / in `HostConfig` | Until the daemon process exits | Process exit; re-read on the next start | No (re-read) | A crash after the read has no effect on the file or the config. |
| `CoreDumpPlan` | `apply_core_dump_policy` | Stack local | One call | Return | No | No state to lose. |
| `RLIMIT_CORE` soft/hard | `setrlimit` in `apply_core_dump_policy` | Kernel process credential/signal state | The process lifetime; inherited by `fork` and preserved by `exec` | Another `setrlimit`, a new process, or process exit | No (per-process; the next daemon re-applies from its inherited limits) | If the process dies after the raise, the kernel applies the current `core_pattern`; SIGKILL never dumps. |
| `umask` | `::umask(0077)` on the opt-in path | Kernel process state | The process lifetime; inherited | Process exit | No | No effect on an existing core; it affects files created after the call. |
| `<root>/.ymh/coredumps/` | `symlink_status` check + `create_directories` + `open(O_NOFOLLOW)` + `fchmod` when enabled | Filesystem | Persistent | Not deleted by ymh; the operator deletes it | Yes | A core written there by the kernel/handler is a regular file owned by the daemon uid; its mode is kernel/handler-defined, while the 0700 parent and the umask are the protections ymh controls. A symlink/non-directory leaf is refused and never chmod'ed. |
| A core file | The kernel or the `core_pattern` pipe handler | Filesystem, path determined by the sysctl | Until deleted | Operator | Yes | Not ymh state; ymh never reads or deletes it. Its mode is kernel/handler-defined; the 0700 parent and the umask are the protections ymh controls. |
| `host.log` startup line | `fprintf` to stderr | `<root>/.ymh/host.log` (supervisor-spawned, launcher `O_TRUNC`) or the direct-launched/foreground stderr | Until the next daemon spawn truncates the log (or the file is rotated) | Next spawn / operator | No (the log is per-spawn) | If the daemon crashes after the line, the line remains for 69's tail and for the operator. |

## 8. Invariants

| ID | Invariant |
|---|---|
| **92-I1** | With the switch off (or `foreground == true`), the daemon calls neither `getrlimit` nor `setrlimit`, creates no `coredumps/` directory, and emits no `core dumps:` line. "Off" does not force the limit to 0 (92-D1, 92-D7). |
| **92-I2** | The helper calls `getrlimit` before any `setrlimit`; it never calls `setrlimit` without an observed `rlim_max`. |
| **92-I3** | `setrlimit` is called iff `hard != 0 && current_soft < hard`; its arguments are exactly `{hard, hard}`. The requested soft value never exceeds the observed hard limit. |
| **92-I4** | `RLIM_INFINITY` is handled as an ordinary `rlim_t` value: `plan(0, RLIM_INFINITY)` applies `{RLIM_INFINITY, RLIM_INFINITY}`; `plan(RLIM_INFINITY, RLIM_INFINITY)` is a no-op. |
| **92-I5** | The helper never lowers a limit. A `current_soft >= hard` observation is a no-op, including the invalid `current_soft > hard` case. |
| **92-I6** | Every failure (`getrlimit`, `setrlimit`, symlink/creation/open/fchmod of the directory, pattern read, and a caught diagnostic-path exception) is non-fatal: startup continues, readiness is unaffected, and no `HostExitCode` changes. |
| **92-I7** | Exactly one `core dumps:` line is emitted when the switch is on and the process is not `foreground`; zero lines otherwise. The line is single-line, ASCII, `\n`-terminated, and at most 512 bytes: every variable field is sanitised to printable ASCII and bounded (`<dir>` <=143 bytes including `...`, `<pattern>` <=103; fixed text/limits/error <=~200). |
| **92-I8** | On every non-`InternalError` variant, the line names the recommended directory and its `dir_status`, the bounded `core_pattern`, and the privacy token; when `getrlimit` succeeds it also names the effective soft and hard limits, and on `getrlimit` failure it names the failure and says no limit change. The `InternalError` variant is exactly `kCoreDumpInternalErrorLine`: a fixed literal with only the privacy token, no `dir`/`dir_status`/`pattern`/limit fields (M-1). No variant claims a core file exists or that a piped `core_pattern` used the limit. |
| **92-I9** | The policy is applied before the first `std::thread`, before `WorkspaceRuntime::create`, and before the transport runner; every thread and `fork`/`exec` descendant inherits the resulting limit. |
| **92-I10** | `logging.core_dumps` in the workspace layer is a `ConfigError` (exit 2), not a silent ignore and not a warning. A non-bool value is also a `ConfigError`. |
| **92-I11** | The env override is last-writer; empty is unset; only `1/true/yes/on` enable; no other value enables. |
| **92-I12** | Provided the parent `.ymh` is a real directory (the normal path), the protected directory is exactly `<canonical-root>/.ymh/coredumps` under the self-ignoring `.ymh/.gitignore`. A pre-existing symlinked `.ymh` is the pre-existing residual recorded in D5; the leaf is then under that symlink target, not this exact path, and I12 makes no claim about it. The leaf must be a real directory: a symlink/non-directory is never followed and never path-chmod'ed; `dir_status=ok` only after a successful `symlink_status` check, `create_directories`, `open(O_NOFOLLOW)`, and `fchmod(fd, 0700)`. Any failure is reported as `dir_status=unavailable`, never silently claimed. ymh never creates a core file. |
| **92-I13** | `core_pattern` is read-only, sanitised, and bounded; an empty/unreadable pattern is rendered `unknown`, never `pattern=`. ymh never writes a sysctl and never includes core bytes, prompts, tool output, or credentials in the line. |
| **92-I14** | No test sends a dump-inducing signal while the switch is on, and every enabled-path test shuts the daemon down cleanly; no test produces a core in CI. |
| **92-I15** | The in-process `foreground` seam is side-effect-free with respect to the embedding process's `RLIMIT_CORE` and `umask`. |

## 9. Failure modes (`92-F#`)

| 92-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **92-F1** | `getrlimit(RLIMIT_CORE)` fails | Return value `< 0`, `errno` saved | One `getrlimit failed (<strerror>); no limit change` line; return `GetLimitFailed`; startup continues. |
| **92-F2** | Hard limit is 0 | `hard == 0` from `getrlimit` | No `setrlimit`; one `blocked (RLIMIT_CORE hard limit is 0; raise the launching shell/unit LimitCORE)` line; return `HardLimitZero`; startup continues. |
| **92-F3** | Soft already equals or exceeds hard | `current_soft >= hard` | No `setrlimit`; one `already at ceiling` line; return `AlreadyAtCeiling`; startup continues. |
| **92-F4** | Finite hard limit below 64 MB | `hard` is a finite value, e.g. 32 MB | Apply `{32 MB, 32 MB}` (not 64 MB); `setrlimit` succeeds; one `enabled soft=33554432 hard=33554432` line. This is the regression test for the rejected `max(hard, 64MB)` defect. |
| **92-F5** | `setrlimit` fails (`EPERM`, `EINVAL`, ...) | Return value `< 0`, `errno` saved | One `setrlimit failed (<strerror>)` line with the unchanged soft/hard; return `SetLimitFailed`; startup continues. |
| **92-F6** | Hard limit is `RLIM_INFINITY` and soft is 0 | `hard == RLIM_INFINITY`, `soft < hard` | Apply `{RLIM_INFINITY, RLIM_INFINITY}`; render `soft=unlimited hard=unlimited`; return `RaiseToHard`. |
| **92-F7** | Soft is greater than hard (invalid observation) | `current_soft > hard` | Treat as `AlreadyAtCeiling`; never lower; one line. |
| **92-F8** | `core_pattern` is a pipe | Literal first byte is `|` (the kernel's test; a leading space is a file pattern) | Report the sanitised/bounded pattern; the runbook explains the handler may relocate/suppress; no claim of a file. This is a reporting hint only, not a classifier symbol (D6). |
| **92-F9** | `core_pattern` is relative | Non-empty, does not start with `|` or `/` | Report it; the runbook warns it lands in the daemon's cwd (workspace root) and in each child's cwd; recommend an absolute path or pipe. |
| **92-F10** | `core_pattern` unreadable or empty | `ifstream` fails or the line is empty | Report `pattern=unknown`; do not fail. |
| **92-F11** | `coredumps/` cannot be made a real 0700 directory, or the leaf is a symlink/non-directory | `symlink_status` sees `is_symlink`/`!is_directory`; `create_directories` sets `error`; `open(O_NOFOLLOW)` returns `ELOOP`/`ENOTDIR`; or `fchmod` returns `< 0` | Continue; the one line reports `dir_status=unavailable` and still names the sanitised intended path; the leaf and any symlink target are untouched; no separate failure code. |
| **92-F12** | The workspace layer sets `logging.core_dumps` | `apply_logging(..., global_layer=false)` sees the key | `ConfigError: 'logging.core_dumps' is global-layer only`, exit 2 before daemon startup. |
| **92-F13** | `logging.core_dumps` is not a bool | `read_bool` type check | `ConfigError: invalid type for 'logging.core_dumps'`, exit 2. |
| **92-F14** | `YMH_CORE_DUMPS` has an unrecognized value | `truthy` returns false | Off; no line, no syscall; matches the existing env-bool behavior. |
| **92-F15** | A tool child crashes while the switch is on | A descendant inherits the raised limit | The child's crash may produce a core under the same `core_pattern`; this is accepted and documented (92-D8); it does not affect the daemon. |
| **92-F16** | The daemon is killed by `SIGKILL` | No core is written by the kernel | The raised limit has no effect; the runbook states that SIGKILL cannot be dumped. |
| **92-F17** | A test accidentally sends `SIGSEGV` with dumps on | Test code inspection / the CI policy | The test is a defect: no enabled-path test may call `crash_now`; the smoke test uses `host.shutdown` and asserts a clean exit (92-D10, 92-I14). |
| **92-F18** | The diagnostic path itself throws (`std::bad_alloc`, path conversion, `std::filesystem_error` despite `error_code`) | The `try`/`catch (...)` wrapper around `apply_core_dump_policy`'s body | `noexcept` holds: write the fixed `internal error; no limit change privacy=...` line with allocation-free `fputs`, return `InternalError`, leave the limit unchanged, and continue startup (92-D11, L-4). |

## 10. dsh (DeepSeek Harness) mapping

| dsh surface | ymh mirror | Justification |
|---|---|---|
| crash diagnostics for the runtime process | `logging.core_dumps` + `RLIMIT_CORE` raise + one startup line (92-D1..92-D4) | **Non-mirror: architectural difference.** dsh defines traceability through the append-only session log, not an OS core (`00-architecture.md:4862-4866`). ymh's runtime lives in a separate, supervisor-owned workspace daemon (`00 sec 54 D17/D18`, `04 sec 3.1`, `16 sec 1`), so an OS-level crash artifact is a ymh-process concern with no dsh counterpart. Anchor: `00-architecture.md:4804-4812` (D17/D18), `04-workspace-host-daemon.md:230-240`, `16-daemon-ownership.md` sec 1 (the daemon process). |
| system-wide core-dump policy | ymh reads `/proc/sys/kernel/core_pattern` and reports it; never writes it (92-D6) | **Non-mirror: deliberate authority boundary.** dsh has no core policy. ymh does not mutate a host-wide, root-only sysctl because that would affect unrelated processes and exceed the daemon's authority; its per-process resource caps are the `00 sec 9.11` / F8 scope. Anchor: `00-architecture.md:1392` (F8 resource caps), `04-workspace-host-daemon.md:242-296` (spawn/umask is the only process-policy seam). |
| core-file location | `<root>/.ymh/coredumps/` recommendation; actual path owned by the kernel/handler (92-D5/92-D6) | **Non-mirror: architectural absence.** ymh cannot relocate a kernel-written core without changing its cwd (forbidden by H1/H2 path safety) or the sysctl (92-D6). It can only create an owner-only recommended directory and report the real pattern. Anchor: `04-workspace-host-daemon.md:298-310` (single chdir, H1/H2), `02-persistence.md sec 4.3` (`.ymh` 0700). |
| sensitive crash artifacts | OFF by default, global-layer only, one-line privacy warning, no core parsing/upload (92-D7) | **Mirror of ymh's own logging rule, not a dsh feature.** dsh has no core dumps; ymh's rule is "never dump full prompts or sensitive tool output into normal logs by default" (`00 sec 40`, `AGENTS.md`). A core is stricter than a log, so the opt-in and global-only gate are required. Anchor: `00-architecture.md:4104`, `74-llm-transcript-errata.md:92-118`. |
| backtrace retrieval | operator runbook: `coredumpctl` or `gdb <binary> <core>`; no upload (sec 12) | **Non-mirror: deliberate scope decision.** dsh's traceability is the session event log; ymh keeps that log authoritative (`01-session.md`) and adds a last-resort OS artifact only for diagnosing a native crash. Automatic collection/upload is out of scope (no server, no consent UI). Anchor: `01-session.md:40` (the session is the durable source of truth), `00-architecture.md:4864` (dsh treats every run as traceable through the append-only session log). |

## 11. Test plan

Hermetic, no real LLM, no network, no core production. The two-process tests use
`tests/support/host_harness.hpp` and `tests/support/short_temp.hpp`; the pure
tests use no syscalls.

| ID | Test | Setup | Assertion |
|---|---|---|---|
| **92-T1** | Pure truth table | `tests/unit/core_dump_test.cpp`, `plan_core_dump_raise` | Every row of the 92-D2 table: `(0,0)` -> no apply/`HardLimitZero`; `(0,32MB)` -> apply/32MB; `(0,64MB)` -> apply/64MB; `(0,RLIM_INFINITY)` -> apply/infinity; `(32MB,32MB)` -> no apply; `(64MB,32MB)` -> no apply and no lower; `(1MB,32MB)` -> apply/32MB; `(inf,inf)` -> no apply. |
| **92-T2** | Regression for the rejected patch | `tests/unit/core_dump_test.cpp` | `plan(0, 32MB).soft == 32MB` (not `max(32MB,64MB)`); `plan(0, 32MB).apply == true` (the inverted condition is gone). |
| **92-T3** | Purity | `tests/unit/core_dump_test.cpp` | Calling the helper twice with the same arguments returns identical plans; it does not read env, open files, or change limits (the process's `getrlimit` before/after is unchanged). |
| **92-T4** | Config default, file, env | `tests/unit/config_test.cpp` (same helper style as `:884-917`) | `Config{}.logging.core_dumps == false`; global `true` parses; `YMH_CORE_DUMPS=1/true/yes/on` overrides a file `false`; `YMH_CORE_DUMPS=0` / an unknown value leaves it off; an empty value is unset. |
| **92-T5** | Workspace layer rejected | `tests/unit/config_test.cpp` | `<ws>/.ymh/config.jsonc` with `{"logging":{"core_dumps":true}}` throws `ConfigError` containing `global-layer only`; a non-bool value throws `invalid type`. |
| **92-T6** | Scaffold | `tests/unit/config_test.cpp` | The first-run scaffold contains the commented `"core_dumps": false` line and the privacy words; the generated file still loads to the defaults. |
| **92-T7** | Daemon start, enabled | `tests/unit/core_dump_host_smoke_test.cpp`; `ShortTempRoot root("ymh-e92-core"); HostHarnessOptions options; options.workspace_root = root.path(); options.env["YMH_CORE_DUMPS"] = "1"; HostHarness harness(options);` (sketch below) | `harness.start()`; `ASSERT_TRUE(harness.wait_ready()) << harness.read_log()`; `read_log()` contains `core dumps:`, `pattern=`, and `dir_status=ok`; it contains either `enabled`/`already at ceiling` or `blocked (RLIMIT_CORE hard limit is 0` (CI may have a 0 hard limit); `<root>/.ymh/coredumps` exists and `stat` reports mode 0700; `harness.stop()` returns `ok()`; no `core` file exists in the workspace root or `coredumps/`. |
| **92-T8** | Daemon start, default off | `tests/unit/core_dump_host_smoke_test.cpp`, config `{}`, `options.env["YMH_CORE_DUMPS"] = ""` to neutralize an inherited developer/CI value | `read_log()` does not contain `core dumps:`; `<root>/.ymh/coredumps` does not exist; the daemon still becomes ready and stops cleanly. |
| **92-T9** | Build/wiring | `tests/CMakeLists.txt`, `CMakeLists.txt` | `src/host/core_dump.cpp` is in `ymh_host`; both test files are in `ymh_tests`; `ctest` passes with warnings-as-errors. |
| **92-T10** | No-core CI guard | test code review + `92-T7`/`92-T8` | No test calls `crash_now`/`SIGSEGV`/`SIGABRT` with the switch on; the enabled test asserts a clean `host.shutdown` exit. A core file in the test workspace is a test failure. |
| **92-T11** | Symlinked leaf is refused (H-1) | `tests/unit/core_dump_host_smoke_test.cpp`; create `<root>/.ymh/coredumps` as a symlink to `<sibling>/sentinel` (mode 0755), then start with `YMH_CORE_DUMPS=1` | Daemon reaches readiness; `read_log()` contains `dir_status=unavailable`; `read_log()` does not contain `dir_status=ok`; `stat(sentinel).st_mode & 0777 == 0755` (not chmod'ed); the symlink still exists and was not unlinked; `harness.stop()` is clean. No core is produced. |
| **92-T12** | Empty pattern and field sanitisation (M-1/M-2) | `tests/unit/core_dump_test.cpp`; `read_core_pattern(temp_path)` and `sanitize_diagnostic_field(...)` | An empty file -> `"unknown"`; a missing/unreadable file -> `"unknown"`; `|/handler %P` preserved; UTF-8/control bytes -> `?`; a value longer than the cap -> cap+`...`; `sanitize_diagnostic_field(dir, 140).size() <= 143`; output contains only bytes `0x20..0x7e`. |
| **92-T13** | Line is ASCII and <=512 for a hostile root (M-2) | `tests/unit/core_dump_host_smoke_test.cpp`; a UTF-8/long workspace root under `ShortTempRoot` with an explicit short `options.socket_path` (so the root is not capped by the socket guard) | The `core dumps:` line is ASCII (every byte `0x20..0x7e`), its length is <=512, and its `dir=` field ends in `...` when the root exceeds the 140-byte cap; daemon becomes ready and stops cleanly. |
| **92-T14** | Internal-error variant is the fixed literal (M-1) | `tests/unit/core_dump_test.cpp`; `kCoreDumpInternalErrorLine` from the header | `std::string{kCoreDumpInternalErrorLine}` equals the pinned D4 text and ends in `\n`; it contains `privacy=` and does not contain `dir=`, `dir_status=`, `pattern=`, `soft=`, or `hard=`; length <=512. This pins I8's `InternalError`-variant exception so the test cannot require fields the catch deliberately omits. |

The T7 body (the only enabled-path test) is deliberately a start/assert/stop
sequence, never a crash:

```cpp
TEST(CoreDumpHost, EnabledStartupReportsAndDoesNotDump) {
    test::ShortTempRoot root("ymh-e92-core");
    test::HostHarnessOptions options;
    options.workspace_root = root.path();
    options.env["YMH_CORE_DUMPS"] = "1";

    test::HostHarness harness(options);
    harness.start();
    ASSERT_TRUE(harness.wait_ready()) << harness.read_log();

    const std::string log = harness.read_log();
    EXPECT_NE(log.find("core dumps:"), std::string::npos) << log;
    EXPECT_NE(log.find("pattern="), std::string::npos) << log;
    EXPECT_NE(log.find("dir_status=ok"), std::string::npos) << log;
    const bool reported = log.find("enabled") != std::string::npos ||
                          log.find("already at ceiling") != std::string::npos ||
                          log.find("blocked (RLIMIT_CORE hard limit is 0") != std::string::npos;
    EXPECT_TRUE(reported) << log;

    struct stat status {};
    const std::filesystem::path dir = root.path() / ".ymh" / "coredumps";
    ASSERT_EQ(::stat(dir.c_str(), &status), 0) << dir;
    EXPECT_EQ(status.st_mode & 0777, 0700);

    const test::ExitStatus stopped = harness.stop();
    EXPECT_TRUE(stopped.ok()) << stopped.code;
    EXPECT_FALSE(std::filesystem::exists(root.path() / "core"));
}
```

The default-off test uses the same body without `options.env`, and asserts
`log.find("core dumps:") == std::string::npos` and
`!std::filesystem::exists(root.path() / ".ymh" / "coredumps")`.

## 12. Operator runbook (normative documentation)

0. **Check the handler first.** Run `cat /proc/sys/kernel/core_pattern`. If it
   starts with `|` (systemd-coredump/apport), the kernel ignores `RLIMIT_CORE`
   for piped dumps and the handler may already have the crash: try
   `coredumpctl list ymh` before changing any config. This spec's switch is
   still useful for classic file patterns and for documenting the policy, but
   it is not what enables a piped dump.
1. **Enable.** In the global `config.jsonc`:
   `"logging": { "core_dumps": true }`, or export `YMH_CORE_DUMPS=1` before
   starting `ymh`. A workspace `config.jsonc` cannot enable it; that is a
   deliberate error (92-D1).
2. **Ensure the hard limit is not 0.** The daemon can raise only up to
   `rlim_max`; if the supervisor runs under systemd, set `LimitCORE=infinity`
   in the unit, or start it from a shell with `ulimit -Hc unlimited`. Check with
   `ulimit -Hc`.
3. **Verify.** After the daemon starts, read:
   `grep 'core dumps:' <workspace>/.ymh/host.log`.
   `enabled`/`already at ceiling` means the limit is in place; `blocked` names
   the hard-limit problem and the remedy. `dir_status=unavailable` means ymh
   did not use the directory (for example, the leaf is a repo-shipped symlink;
   ymh refuses it rather than chmod'ing its target). Inspect
   `<workspace>/.ymh/coredumps`; replace a symlink with a real directory if the
   protected destination is wanted.
4. **Reproduce the crash.** The core is produced by the kernel/handler, not by
   ymh. On a `systemd-coredump` host (the common Ubuntu/Fedora case) use
   `coredumpctl list ymh`, then `coredumpctl gdb <pid>` (or
   `coredumpctl dump <pid> --output <file>` followed by `gdb <binary> <file>`).
   For a plain file pattern, `gdb <path-to-ymh> <core>`.
   Build with `-DCMAKE_BUILD_TYPE=Debug` (or `RelWithDebInfo`) for symbols.
   `coredumpctl` works for a crash that happened before this switch was
   enabled, provided the handler stored it.
5. **If the pattern is relative** (`cat /proc/sys/kernel/core_pattern`), the
   core may appear in `<workspace>/core`, not in `.ymh/coredumps/`. Ask the
   system administrator to set an absolute pattern under the protected
   directory or to use the distro's pipe handler. ymh does not write the
   sysctl.
6. **Delete the core after diagnosis.** It is a full memory image and may
   contain prompts, tool output, source code, and credentials (92-D7). Do not
   attach it to an issue or send it anywhere.
7. **Disable.** Set `core_dumps` to `false` (or unset `YMH_CORE_DUMPS`) and
   restart the daemon. The limit is per-process; a running daemon keeps the
   limit it started with. To suppress file cores even when the environment
   allows them, run under `ulimit -c 0` or `LimitCORE=0`; for a piped handler,
   use the handler's own configuration (`coredump.conf` `Storage=none`),
   because the kernel ignores `RLIMIT_CORE` on that path. An external operator
   cannot set another process's `PR_SET_DUMPABLE`; do not present it as a
   lever.

## 13. Open questions

| ID | Question | Current disposition |
|---|---|---|
| **OQ-92-1** | Should the supervisor TUI apply the same policy before its first thread? | Deferred. It is a separate process with a different crash profile; the helper is reusable (92-D9). A follow-up can add it without changing this contract. |
| **OQ-92-2** | Should tool children drop `RLIMIT_CORE` to 0 so only the daemon dumps? | Deferred. The correct seam is 07's Spawn; it would change every tool child's behaviour, so it needs its own spec and tests (92-D8). |
| **OQ-92-3** | Should ymh detect `systemd-coredump` and print a `coredumpctl` hint at startup? | Deferred. It would add distro-specific text to the startup line; the runbook covers it without code. |
| **OQ-92-4** | Should `ymh doctor` report the effective limit and pattern? | Candidate follow-up. The startup line is enough for the incident; doctor would be additive. |

## 14. Revision log

| Rev | Date | Author | Change |
|---|---|---|---|
| 1 | 2026-10-20 | diag-author (draft) | Initial draft. Pins the opt-in `logging.core_dumps` / `YMH_CORE_DUMPS` switch (default off, global-layer only), the correct `want = hard` limit math, the pre-thread call site after `::signal(SIGPIPE, SIG_IGN)` (`workspace_host.cpp:822`), the single bounded effective-limit line, the 0700 `coredumps/` directory and umask, the read-only `core_pattern` caveat, the privacy boundary, the dsh mapping, the state-lifetime table, the pure-helper and two-process smoke tests, and the operator runbook. Records the rejected `94-C1` patch and its four defects (sec 1.2, 92-D13). Not independently reviewed; component gate open. |
| 2 | 2026-10-20 | diag-author (draft) | Applies gate-92 Rev 1 (1 HIGH / 3 MEDIUM / 10 LOW) without re-gating. H-1: symlink-safe leaf (`symlink_status` refusal, `open(O_NOFOLLOW)`, `fchmod(fd)`, no path chmod; D5/I12/F11, T11). M-1: empty pattern -> `unknown` (`read_core_pattern`, T12). M-2: sanitise/bound the `dir` and pattern fields (`sanitize_diagnostic_field`, 140/100 caps; D4/I7, T12/T13). M-3: defect-1 prose now defines `soft`/`cur` so the inversion is demonstrable. L-1: tracker note names the drift checker; no row added (lead instruction). L-2: privacy token on all variants. L-3: flow uses `RaiseToHard`. L-4: `noexcept` body wraps in try/catch and returns `InternalError` with a fixed line (D11/F18/I6). L-5: state table no longer claims mode 0600. L-6: F8/D6 use the literal first byte and state the table is reporting guidance. L-7: typo. L-8: runbook drops `PR_SET_DUMPABLE` as an operator lever. L-9: header says wire/DB schema. L-10: `ymh run`/headless wording. Still a draft; no re-gate. |
| 3 | 2026-10-20 | diag-author (draft) | Applies gate-92 Rev 2 (0 HIGH / 1 MEDIUM / 13 LOW) without re-gating. M-1: the `InternalError` variant is now the `kCoreDumpInternalErrorLine` constant with only the privacy token; D4's lead-in and internal-error paragraph and I8 are scoped to non-`InternalError` variants; T14 pins the literal (D4 378-428, I8 1025, T14 1088). LOWs: caller claim for `kCoreDumpDirName` corrected (991); fchmod anchor -> `registry.cpp:728` (465); `<soft>`/`<hard>` placeholders defined numerically (405-412); direct `ymh --host` stderr scoped (362-365, 1012); `.ymh` 0700/gitignore overclaims corrected (442-449); I12 parent-symlink scope (1029); headless range -> `:151-180` (587); D13 test/record wording (641); dsh weak `26` anchor replaced with `00:4862-4866` (1061); D12 "only persistent artifact" corrected (634-636); D6 privilege overclaim corrected (513-514). L-1 (no tracker row) remains deferred by lead instruction. Still a draft; no re-gate. |
