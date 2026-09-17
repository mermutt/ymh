# 21 — Config Format Errata: TOML → JSONC (RB-09)

```
Status: verified + implemented (RB-09, commit 9db17cd54) · Rev 5 (21-D11–21-D14) + Rev 6 (21-D15–21-D17) + Rev 7 + Rev 8 amendments pending re-gate
Component: 21 (errata) — amends 00 §37 and 08 §5.3 by reference; touches 15 §5.6,
           20 §3.3 and README.md as downstream references (README is a
           prerequisite of the code change, J-C11)
Depends on: 00-architecture.md §37/§53/§54, 08-llm-provider.md §5.3/§6.4,
            15-mcp-adapter.md §5.6, 16-daemon-ownership.md §1.4 :160 (deferral),
            20-skills.md §3.3 :360-361
Scope: switch the sole configuration file format from TOML to JSONC
       (`~/.config/ymh/config.jsonc`); retire TOML entirely. As of Rev 5
       (21-D11) ymh has **no TOML awareness**: it never stats, opens, parses,
       mentions, or warns about `config.toml`; a sibling `.toml` is invisible
Supersedes: 00 §37 :3941 ("Use TOML.") and §37 :3945-3968 (TOML example);
            08 §5.3 :695 ("Config is TOML") and §5.3 :701-717 (TOML example)
Amends:     15 §5.6 :1236-1237 (path) and §5.6 :1242-1274 (TOML example);
            20 §3.3 :360-361 (path); README.md :28 and :88-145 (J-C11);
            the config component interface (include/ymh/config/config.hpp),
            its CMake target, and the host-launcher interface
            (include/ymh/host/workspace_host.hpp, host_launcher.hpp) — 21-D15
Retained:   layering order, strict unknown-key rejection, secrets-never-in-config,
            `YMH_*` env overrides, CLI overrides, non-empty array replace
            wholesale (15 §5.6 :1296-1301; an empty `workspace_roots` is a no-op
            while a per-server empty array is equivalent to absent, J10), no hot
            reload (00 §53 :4725 — "hot reload" is an unshipped Phase 4
            experimental item)
```

This document is the design gate for `REQUIREMENTS_BACKLOG.md` **RB-09**. It is
**additive**: it pins new text, filenames, interface names, invariants, and
tests. It does not rewrite `00-architecture.md` or `08-llm-provider.md`; each
superseded clause is quoted with `file:line` and its replacement is given here.
The convention matches spec 16 (the superseding errata) and spec 17.

**Naming note.** Invariants local to this spec are **`J1`–`J22`**; failure modes
are **`J-F1`–`J-F16`**; decisions are **`21-D1`–`21-D17`**. The prefixes `H`, `T`,
`U`, `R`, `A`, `X`, `L`, `Q`, `P`, `S`, `C`, `M` are taken by specs 01–15, `O`
by 16, and `U-RB*` by 17. `J` is unused by every spec in `docs/design/`.

**Decisions already made by the user (2026-09-16), not re-opened here:**
**JSONC only; TOML is retired.** `~/.config/ymh/config.jsonc` is the sole config
file. There is **no dual-read and no precedence rule**. First-run scaffolding
writes `config.jsonc`. (Source: `docs/design/REQUIREMENTS_BACKLOG.md:250-254`.)

**Amendment decisions (user-approved 2026-09-17; Rev 5).** Four decisions extend
the original errata; they are pinned as `21-D11`–`21-D14` and recorded in §2 and
§14.3:

- **21-D11 — no TOML awareness at all.** ymh must never stat, open, parse,
  mention, or warn about `config.toml`. Every piece of legacy-TOML machinery
  (`kLegacyConfigFile`, `legacy_config_path`, `jsonc_target`, `scaffold_target`,
  `LegacyFile`, `collect_legacy`, `warn_legacy`, the `ymh config path` note) is
  **deleted**, not deprecated. A `config.toml` next to `config.jsonc` is simply
  invisible.
- **21-D12 — the global config layer is required.** `load_config` throws
  `ConfigError` when the global config path is empty or does not exist. The
  per-workspace layer (`<root>/.ymh/config.jsonc`) stays **optional**. Built-in
  defaults remain the base of the stack.
- **21-D13 — scaffolding bootstraps the conventional location only.** First-run
  scaffolding auto-creates `$XDG_CONFIG_HOME/ymh/config.jsonc` (else
  `$HOME/.config/ymh/config.jsonc`) when absent. With an explicit `--config
  <path>`, ymh must **not** create that file: a missing explicit path is the
  `21-D12` error. Scaffolding still ensures `<root>/.ymh/` in both cases.
- **21-D14 — top-level unknown-key message.** The top-level unknown-key error
  reads `unknown key 'auto_compact_enabled'` (no leading dot); nested-table
  messages keep the qualified form `unknown key 'agent.compaction.foo'`.

**Rev-6 amendment decisions (user-approved 2026-09-17; fixes the Oracle Rev-5
gate findings).** Three decisions, pinned as `21-D15`–`21-D17`:

- **21-D15 — the supervisor passes its effective global config path to the
  daemon.** When the supervisor supplies a path, the daemon loads exactly that
  file and never re-resolves the global path; the defaulted-empty case keeps the
  daemon's own resolution (see §5.5, J20). Fixes H1.
- **21-D16 — the required global layer must be a regular file.** An empty path,
  an absent path, or a path that exists but is not a regular file
  (directory/FIFO/socket/device) is a `ConfigError`. The optional workspace
  layer is lenient only about *absence*; a present non-regular file is rejected
  too (see §5.4, J21).
- **21-D17 — the config load is gated by the scaffolding switch.** `Tui`/`Run`/
  `List`/`Show`/`Replay`/`Fork` load config (and get config-derived logging);
  `Workspace`/`Config`/`Version` do not, and run with default logging. Only
  `Tui`/`Run` consume `Config` itself (see §6.3).

**Gate (per `AGENTS.md`).** Independent Oracle review must mark this `verified`
with no open HIGH/MEDIUM findings **before any code**. Nothing here is verified.

**Honesty note — recon vs. code.** Every `file:line` below was re-read against
the working tree. Where a claim could not be verified from code, it is listed in
§14.2 and marked **[UNVERIFIED]**. The nlohmann/json capability claims in §4.3
were verified by compiling and running probes against the **system** header
(`/usr/include/nlohmann/json.hpp`, v3.12.0), not by assumption.

---

## 1. Purpose, scope, and supersession map

### 1.1 The problem

`ymh` originally read configuration from a TOML file (`config.toml`) using
toml++ (`src/config/config.cpp:13`). The user requires the file format of
`~/.config/ymh/config.jsonc` — JSON with comments (JSONC). The format is pinned
at the architecture level (`00-architecture.md:3941` "Use TOML.") and at the
component level (`08-llm-provider.md:695` "Config is TOML and layered exactly as
`§37`"). Changing it is therefore a design-gate change: it alters a frozen
config surface and needs an explicit errata (RB-09's own gate text,
`REQUIREMENTS_BACKLOG.md:275-276`).

Spec 16 already recorded this work as deferred: `16-daemon-ownership.md:160`
lists `config.jsonc` among the items "triaged separately in the backlog". This
errata is that triage.

### 1.2 What this spec changes, in one sentence

The config component reads `config.jsonc` (JSONC) instead of `config.toml`
(TOML), using the already-present `nlohmann/json` with `ignore_comments=true`;
all layering, strictness, keys, defaults, env overrides, and CLI overrides are
unchanged. As of Rev 5 there is **no TOML awareness at all** (21-D11): no code
path stats, opens, parses, mentions, or warns about `config.toml`; the global
layer is **required** (21-D12) and the workspace layer is **optional**.

### 1.3 Supersession map

#### 1.3.1 Superseded (text is replaced here)

| Clause | Verified text today | Replacement |
|---|---|---|
| `00-architecture.md:3941` | "Use TOML." | "Use JSONC (JSON with `//` and `/* */` comments)." |
| `00-architecture.md:3945-3968` | TOML example (`[ui]`, `[agent]`, …) | the faithful JSONC rendering in §1.3.1a |
| `08-llm-provider.md:695` | "Config is TOML and layered exactly as `§37`:" | "Config is JSONC and layered exactly as `§37`:" |
| `08-llm-provider.md:701-717` | TOML `[llm.default]` / `[llm.default.retry]` example | the JSONC equivalent in §7.7 |

**§1.3.1a — Faithful JSONC rendering of the superseded `00 §37` example.** Keys,
values, key order, and the one inline comment are preserved exactly from
`00-architecture.md:3945-3968`; only the syntax changes (`[section]` headers
become nested objects, the `#` comment becomes `//`). This is a faithful
rendering of *that* example, not the commented first-run scaffold of §8.2 (which
is a superset carrying every key and its default):

```jsonc
{
  "ui": {
    "theme": "default",
    "show_activity": true,
    "side_panel": "auto"
  },
  "agent": {
    "model": "...",
    "max_steps": 100
  },
  "workspace": {
    "root": ".",
    "workspace_roots": ["~/prjs"]   // first-run bootstrap discovery roots (§9.10)
  },
  "permissions": {
    "shell": "ask",
    "write": "ask",
    "read": "allow"
  },
  "llm": {
    "default": {
      "provider": "openai-compatible",
      "base_url": "http://localhost:8000/v1",
      "model": "..."
    }
  }
}
```

#### 1.3.2 Amended (additive: path/example references)

| Clause | Verified text today | Amendment |
|---|---|---|
| `15-mcp-adapter.md:1236-1237` | "global `~/.config/ymh/config.toml` → project `<root>/.ymh/config.toml`" | `config.jsonc` in both slots |
| `15-mcp-adapter.md:1242-1274` | TOML `[mcp]` / `[[mcp.server]]` example | JSONC equivalent in §7.8 |
| `20-skills.md:360-361` | "…the config loader reading `$XDG_CONFIG_HOME/ymh/config.toml` outside the workspace" | `config.jsonc` |
| `include/ymh/config/config.hpp:3-16,179-223` | TOML filename + `apply_toml_file` | §5.4 interface sketch (historical; landed in RB-09) |
| `src/config/config.cpp:21,488-540,647-668` | `config.toml`, `kDefaultConfigToml`, `apply_toml_file` | §5/§8 (historical; landed in RB-09) |
| `CMakeLists.txt:84-92,435-439` | toml++ FetchContent + `tomlplusplus::tomlplusplus` link | drop toml++; `nlohmann_json::nlohmann_json` (already found at `CMakeLists.txt:130`) |

The `20-skills.md` path is the only §3.3 occurrence of the config filename, but
the same stale string also appears elsewhere in that spec — `20-skills.md:305`
("last fallback is the **relative** `.config/ymh/config.toml`") and
`20-skills.md:708` ("is `.config/ymh/config.toml`") — and in `README.md` (see
OQ-6). Because J3 retires the basename globally, the rename applies to those
occurrences by reference; they are recorded here so the code-landing change does
not silently leave a stale filename behind.

**Additional stale TOML / toml++ references (full sweep, re-derived
2026-09-16).** Beyond the config-filename occurrences above, the following
references to the retired format or the retired `toml++` dependency remain in the
tree and are part of J-C11's prerequisite sweep:

| File:line | Stale content |
|---|---|
| `00-architecture.md:1598` | a `toml`-tagged code fence for the local-inference `[llm.default]` example (superseded by §7.7) |
| `00-architecture.md:4065` | `toml++` in the "Potential libraries" list |
| `00-architecture.md:4438` | `toml++` under "## Configuration" |
| `00-architecture.md:4486` | `toml++` in the final dependency list |
| `AGENTS.md:65` | `toml++` in the FetchContent dependency list |
| `AGENTS.md:113` | `toml++` in the core-deps list |
| `09-permissions.md:361-383` | a `toml`-tagged code fence for the `[permissions]` example: the `shell`/`write`/`read` shorthand is live and rendered in §7.5; its `default`/`rule` keys are spec-09's proposed extension, not in the live `Config` (`apply_permissions` allows only `shell`,`write`,`read`, `config.cpp:195`) |
| `13-context-compaction.md:304` | "`[agent.compaction]` TOML table" |
| `15-mcp-adapter.md:509` | "unknown TOML keys already throw" |
| `20-skills.md:492-496` | `toml++` "already available" for TOML skill frontmatter |
| `20-skills.md:2031` | "(20-D3) … TOML (already available) and general YAML are rejected" — the "already available" clause is stale once J-C10 drops toml++ |
| `REQUIREMENTS_BACKLOG.md:255-267` | RB-09's own "Current state" bullets (`config.toml`, toml++, `apply_toml_file` `:619-640`); the requirement text itself, to be updated or closed when RB-09 lands |

Because J3 retires the format and J-C10 drops the dependency, every entry above
must be swept in the same change. This errata does not edit those files
(design-only, §1.4); it records them so the sweep is not forgotten.

**Out of scope — `permissions.local.toml` (pinned).** The workspace-local
permission-grants file named `permissions.local.toml` in
`09-permissions.md:216,518,887,1092,1238,1295` is **not** the layered config file
and is **not** read by the config component: `grep` finds no
`permissions.local`/`local.toml` reference in `src/` or `include/` — it is a
distinct, still-unimplemented persistence artifact owned by the permission
subsystem (spec 09), not a `ConfigPaths` slot. RB-09 retires the config
*format*; it does not rename this file. The one genuine config example in that
spec is the `toml` fence at `:361-383`, which is in the sweep above. Whether the
grants file should also become JSONC is a spec-09 decision and is deliberately
not pre-empted here.

#### 1.3.3 Retained (recorded, not changed)

- **Layer order** — defaults → global → workspace → `YMH_*` env → CLI
  (`include/ymh/config/config.hpp:5-16`, `src/config/config.cpp:742-755`).
- **Strict unknown keys** — an unknown key is a `ConfigError`, not a silent
  default (`src/config/config.cpp:27-45`; `08-llm-provider.md:721-722`).
- **Secrets never in `Config`** — `llm.api_key_env` names an env var
  (`include/ymh/config/config.hpp:14-16`, `08-llm-provider.md:706`).
- **Array replace, not merge** (`15-mcp-adapter.md:1296-1301`).
- **No hot reload** (`00-architecture.md:4725`, §53 "Phase 4 — Experimental":
  "hot reload" is listed as a not-yet-pursued item; `15-mcp-adapter.md:1302`
  states the consequence — a config change requires a daemon restart).
- **`[llm]` flat *or* `[llm.default]` nested** — both accepted today
  (`src/config/config.cpp:230-249`); both accepted in JSONC (§7.7).
- **Best-effort, never-throwing scaffolding** (`scaffold_config`, `src/config/config.cpp` ~:805-830).

#### 1.3.4 Rev-5 removals (this amendment)

| Removed symbol / text | Where it lived | Replacement |
|---|---|---|
| `kLegacyConfigFile` | `src/config/config.cpp` (const) | **deleted** — no TOML basename exists (21-D11) |
| `legacy_config_path` | `src/config/config.cpp` + `include/ymh/config/config.hpp` | **deleted** (21-D11) |
| `jsonc_target` | `src/config/config.cpp` + `include/ymh/config/config.hpp` | **deleted** (21-D11) |
| `scaffold_target` | `src/config/config.cpp` + `include/ymh/config/config.hpp` | **deleted**; replaced by the 21-D13 rule in `scaffold_for_invocation` (§8.1) |
| `LegacyFile`, `collect_legacy`, `warn_legacy` | `src/config/config.cpp` | **deleted** (21-D11) |
| `ymh config path` legacy note (J-C9) | `src/cli/cli.cpp` (`run_config_command`) | **deleted** (21-D11) |
| §6.1–§6.8 (detection, warning text, once-per-run, both-files table, `config path` note) | this spec | replaced by §6 (No TOML awareness) |

The full code/doc impact of the removals is inventoried in §14.4.

### 1.4 Scope boundaries

**In scope:** the config file format, filename, path resolution, parser,
strictness parity, the required-global rule (21-D12) and its regular-file
tightening (21-D16), the scaffold target rule (21-D13), the unknown-key message
(21-D14), the removal of all TOML awareness (21-D11), the supervisor→daemon
config handoff (21-D15), the command-gated config load (21-D17), the scaffold
text, and the affected tests/docs. **This includes the
`README.md` config section as a mandatory prerequisite of the code-landing
change** (J-C11, OQ-6) — this errata does not edit it, but no code may land with
the README still documenting TOML.

**Out of scope:** changing any key name, default, or layering rule; adding new
config keys; adding a config-watching/hot-reload mechanism; workspace-level
config *content* (the workspace file simply changes extension); the `ymh config`
subcommand set (`ymh config path` is unchanged — no legacy note, 21-D11).

**Deliberate tightening (21-D16, recorded).** Rejecting a present non-regular
file at either layer is a user-visible behaviour change: a pre-existing
`mkdir .ymh/config.jsonc` (or `~/.config/ymh/config.jsonc`) that previously
silently no-op'd now fails loud. This is intentional — the silent-`{}` outcome
is exactly the "ran on defaults without saying so" hazard the required-global
rule exists to remove. Recorded here and in J21/J-F16/J-T47.

---

## 2. Amendment register

**Rev-5 note (2026-09-17).** This register is the original RB-09 register. The
Rev-5 amendment (`21-D11`–`21-D14`) is folded into the rows below where it
changes them and added as `J-C13`–`J-C16`. Where a row is marked **Rev 5** the
original behaviour is superseded.

**Rev-6 note (2026-09-17).** The Rev-6 amendment (`21-D15`–`21-D17`, fixing the
Oracle Rev-5 gate findings H1 and M1/M5) is added as `J-C17`–`J-C19`. No prior
row changes under Rev 6 except `J-C9`'s wording (L4).

**Rev-7 note (2026-09-17).** Rev 7 resolves the second re-gate (M4 residual,
the J20/§5.5 absolute, and four LOWs). It adds `J-C20` (fixture + the six
`HostLifecycle` sites) and `J-C21` (the `config_path` contract clarifications:
supplied-only fidelity, no ctor validation, relative path allowed, J-F15 trigger
scoped). No new decision IDs; `21-D15`–`21-D17` are unchanged.

**Rev-8 note (2026-09-17).** Rev 8 applies the four non-blocking LOWs from the
final gate. It is **test-plan/inventory wording only**: no decision, invariant,
failure mode, or contract changes. Fixes: J-T48 uses a dedicated empty dir
independent of the shared fixture; J-T45 pins `/proc/<pid>/cmdline` inspection
for the daemon argv; the `ui_supervisor_pty_test.cpp` row states that no fixture
change is needed (the Tui binary self-scaffolds); the §14.4 `run_cli` row adds
the config-derived `init_logging` gating already pinned by J-C19/§6.3/J22.

| ID | Clause | Verified code anchors | New behaviour |
|---|---|---|---|
| **J-C1** | `config.cpp:21` | `kConfigFile = "config.toml"` | `kConfigFile = "config.jsonc"`; **Rev 5 (21-D11): do NOT add `kLegacyConfigFile`** |
| **J-C2** | `config.cpp:607-615` | `default_global_config_path()` | returns `…/ymh/config.jsonc`; `XDG_CONFIG_HOME` unset handling unchanged |
| **J-C3** | `config.cpp:617-619` | `workspace_config_path()` | returns `<root>/.ymh/config.jsonc` |
| **J-C4** | `config.cpp:13,647-668`; `config.hpp:223` | toml++ parse in `apply_toml_file` | `apply_jsonc_file` using `nlohmann::json::parse(…, ignore_comments=true)`; toml++ removed |
| **J-C5** | `config.cpp:488-540` | `kDefaultConfigToml` | `kDefaultConfigJsonc` (§8); valid JSONC |
| **J-C6** | `config.cpp:742-755`; `config.hpp:215-219` | `load_config` | **Rev 5 (21-D12): `Logger*` removed; global layer required, workspace optional; no warning** |
| **J-C7** | `config.cpp:27-99` | `reject_unknown` / `read_value` / `read_string_array` | JSON equivalents with identical strictness (§4.2); integers must be ≥ 0 |
| **J-C8** | `cli.cpp:61-68,215-219` | `load_config(paths)` call sites | pass `&category_logger(LogCategory::Filesystem)` |
| **J-C9** | `cli.cpp:124-134` | `ymh config path` output | **Rev 5 (21-D11): legacy-sibling note DELETED; primary output unchanged (path + `(exists)`/`(missing)`); no secondary note remains** |
| **J-C10** | `CMakeLists.txt:84-92,435-439` | toml++ dep | drop toml++; link `nlohmann_json::nlohmann_json` (already found at `:130`) |
| **J-C11** | `README.md:28,88-145`; `AGENTS.md:65,113`; `00-architecture.md:1598,4065,4438,4486`; `09-permissions.md:361-383`; `13-context-compaction.md:304`; `15-mcp-adapter.md:509`; `20-skills.md:305,492-496,708,2031`; `REQUIREMENTS_BACKLOG.md:255-267` | docs still name TOML / toml++ | rewrite the config docs to `config.jsonc` JSONC examples and drop toml++ from every dependency list; update RB-09's own current-state bullets. **Mandatory in the same change as code** (prerequisite, §14.1 OQ-6), not deferred. Full inventory in §1.3.2 |
| **J-C12** | `config.cpp` (`scaffold_config`); `cli.cpp` (`scaffold_for_invocation`) | `scaffold_config` has no empty-path guard: `global_config == ""` reaches `write_default_config("")`, which warns `cannot write ''` and sets `ok = false` | add the empty-target branch (§8.1): `!global_config.empty()` guards the global directory + write; an empty target skips both, emits no warning, keeps `ok == true`, and still runs the `<root>/.ymh/` step. **Rev 5 (21-D13): `scaffold_for_invocation` passes `default_global_config_path()` when there is no `--config`, else the empty target.** |
| **J-C13** | `config.cpp` (`kLegacyConfigFile`, `legacy_config_path`, `jsonc_target`, `scaffold_target`, `LegacyFile`, `collect_legacy`, `warn_legacy`); `config.hpp` (three decls) | legacy-TOML machinery | **DELETE all of it (21-D11).** No symbol that probes a `.toml` path remains |
| **J-C14** | `config.cpp` (`load_config`, `apply_jsonc_file`) | global layer silently skipped when absent; `Logger*` existed only to feed `warn_legacy` | `apply_jsonc_file(config, path, bool required = false)`; `load_config` calls it with `required=true` for `paths.global` and `required=false` for `paths.workspace`; `Logger*` removed (21-D12) |
| **J-C15** | `cli.cpp` (`scaffold_for_invocation`) | passes `scaffold_target(effective_global_config(invocation))` | explicit `--config` ⇒ empty target (no scaffold); otherwise `default_global_config_path()` (21-D13). J-C12 empty-target branch retained |
| **J-C16** | `config.cpp` (`reject_unknown`), caller `apply_document` | the message builder always concatenates `table_name + "." + name`, so the top-level call `reject_unknown(table, "", …)` yields `unknown key '.auto_compact_enabled'` | build the qualified name as `table_name.empty() ? name : table_name + "." + name`, yielding `unknown key 'auto_compact_enabled'` at top level and `unknown key 'agent.compaction.foo'` nested (21-D14) |
| **J-C17** | `workspace_host.hpp` (`HostConfig`); `workspace_host.cpp` (`ForkExecLauncher::build_argv`, `HostLifecycle::configFor`); `host_launcher.hpp` (`HostLifecycle`); `cli.cpp` (`run_supervisor_entry`, `run_via_daemon`, `run_cli`) | the daemon never received the supervisor's `--config`; it re-resolved `default_global_config_path()` | additive `HostConfig::config_path`; `build_argv` appends `--config <path>` when set; `HostLifecycle` ctor gains a defaulted `config_path` stored as `config_path_` and `configFor` sets it; `run_supervisor_entry`/`run_via_daemon` gain a config-path parameter and `run_cli` passes `effective_global_config(invocation)` (21-D15, §5.5) |
| **J-C18** | `config.cpp` (`apply_jsonc_file`) | `exists` + `ifstream` accepts a directory (opens OK, reads 0 bytes → blank → `{}` no-op) | empty + required ⇒ `ConfigError` (special-cased message, not `fail`); then `is_regular_file`: absent + required ⇒ `ConfigError`; present but not a regular file ⇒ `ConfigError` for **either** layer (21-D16, J21, §5.4) |
| **J-C19** | `cli.cpp` (`run_cli`) | `load_invocation_config` runs for every command except `Version`/`Config`, including `Workspace` | gate the load **and** the config-derived `init_logging` by the same switch as `scaffold_for_invocation`; `Workspace`/`Config`/`Version` load nothing and use default logging (21-D17, §6.3) |
| **J-C20** | `tests/support/host_harness.hpp`; `tests/integration_two_process_test.cpp` (`configure_workspace`, six `HostLifecycle` sites) | harness sets `XDG_CONFIG_HOME` and writes no global config; `configure_workspace` likewise, yet six `HostLifecycle`-spawned daemons run through it (only some preceded by a harness) | write `<root>/.config/ymh/config.jsonc` in the shared fixture (and/or set `HostHarnessOptions::config_path`), ordering-independent; direct `--host` spawn passes `--config` or writes the conventional file (M4, §12.2) |
| **J-C21** | `workspace_host.hpp`/`workspace_host.cpp`/`host_launcher.hpp`/`cli.cpp` | `config_path` defaulted-empty; J20/J-F15 stated as absolutes | clarify: fidelity holds **when the supervisor supplies a path**; the defaulted-empty case keeps self-resolution; no `HostLifecycle` ctor validation; relative path is allowed (resolved pre-`chdir` in both processes); J-F15 triggers on absent/non-regular, not empty (Rev 7, §5.5, J20/J-F15) |

---

## 3. Historical baseline (pre-RB-09 verified state)

All line numbers are against the **pre-RB-09** working tree as read on
2026-09-16 (authoring time); they are hints, not live anchors.

> **Drift note (Rev 6).** This section is **historical** — it is not the live
> contract and its title no longer claims to be "current". RB-09 landed in
> `9db17cd54`, so `kConfigFile` is `"config.jsonc"`, `apply_toml_file` is
> `apply_jsonc_file`, and `kDefaultConfigToml` is `kDefaultConfigJsonc`; the
> scaffolding and parser anchors here have moved (e.g. `scaffold_config` is at
> `src/config/config.cpp` ~:805-830, not ~:621-645; `ScaffoldResult` is at
> `include/ymh/config/config.hpp` ~:211-220, not ~:190-199). Where §3 names TOML
> symbols it describes the state the errata superseded. The live contract is
> §5–§10.

### 3.1 Filename constant and path resolution

| Fact | Evidence |
|---|---|
| Config basename is `config.toml` | `src/config/config.cpp:21` — `constexpr std::string_view kConfigFile = "config.toml";` |
| Config dir basename is `ymh` | `src/config/config.cpp:20` — `constexpr std::string_view kGlobalDir = "ymh";` |
| Global path: `$XDG_CONFIG_HOME/ymh/config.toml`, else `$HOME/.config/ymh/config.toml`, else `./.config/ymh/config.toml` | `default_global_config_path`, `src/config/config.cpp:607-615` |
| Workspace path: `<root>/.ymh/config.toml` | `workspace_config_path`, `src/config/config.cpp:617-619` |
| Header documents the TOML filenames | `include/ymh/config/config.hpp:8-9,179,182` |

### 3.2 Parser entry point and strictness

| Fact | Evidence |
|---|---|
| Parser is toml++ | `src/config/config.cpp:13` — `#include <toml++/toml.h>` |
| Entry point `apply_toml_file` | `src/config/config.cpp:647-668`; declared `include/ymh/config/config.hpp:223` |
| Parse call | `src/config/config.cpp:658` — `toml::parse_file(path.string())` |
| Missing file is a no-op | `src/config/config.cpp:651-654` (pre-RB-09; Rev 5 makes the **global** layer required — 21-D12, §6) |
| Parse error → `ConfigError` with line | `src/config/config.cpp:659-665` |
| Unknown key → `ConfigError` | `src/config/config.cpp:27-45` (`reject_unknown`), called by every `apply_*` |
| Type mismatch → `ConfigError` | `src/config/config.cpp:47-61` (`read_value`), `:63-75` (`read_optional_string`), `:77-99` (`read_string_array`) |
| Top-level allowed keys | `src/config/config.cpp:426-428` — `ui, agent, workspace, permissions, logging, llm, mcp, skills` |
| Section dispatch | `src/config/config.cpp:425-466` (`apply_document`) |

### 3.3 Every config key read today

The complete key set is enumerated from the `reject_unknown` allow-lists and the
`read_*` calls. `file:line` for each section:

| Section | Key | Type read | Default | Evidence |
|---|---|---|---|---|
| `ui` | `theme` | string | `"default"` | `config.cpp:102-103` |
| `ui` | `show_activity` | bool | `true` | `config.cpp:104-105` |
| `ui` | `side_panel` | string | `"auto"` | `config.cpp:106-107` |
| `agent` | `model` | string | `""` | `config.cpp:161-162` |
| `agent` | `max_steps` | int64→size_t | `100` | `config.cpp:163-165` |
| `agent` | `reasoning_effort` | optional string | absent | `config.cpp:166-167` |
| `agent` | `system_prompt` | string | `""` | `config.cpp:168-169` |
| `agent` | `compaction_threshold_tokens` | int64→size_t | `0` | `config.cpp:170-172` |
| `agent` | `compaction` | table | — | `config.cpp:173-179` |
| `agent.compaction` | `enabled` | bool | `false` | `config.cpp:120-121` |
| `agent.compaction` | `threshold_tokens` | int64→size_t | `0` | `config.cpp:122-124` |
| `agent.compaction` | `threshold_ratio` | double | `0.80` | `config.cpp:125-127` |
| `agent.compaction` | `context_window_tokens` | int64→size_t | `0` | `config.cpp:128-130` |
| `agent.compaction` | `reserve_output_tokens` | int64→size_t | `4096` | `config.cpp:131-133` |
| `agent.compaction` | `keep_recent_turns` | int64→size_t | `2` | `config.cpp:134-136` |
| `agent.compaction` | `min_prefix_messages` | int64→size_t | `4` | `config.cpp:137-139` |
| `agent.compaction` | `max_summary_tokens` | int64→size_t | `1024` | `config.cpp:140-142` |
| `agent.compaction` | `max_summary_bytes` | int64→size_t | `262144` | `config.cpp:143-145` |
| `agent.compaction` | `summarizer_model` | string | `""` | `config.cpp:146-147` |
| `agent.compaction` | `max_compactions_per_turn` | int64→size_t | `1` | `config.cpp:148-150` |
| `agent.compaction` | `retry_on_context_length` | bool | `true` | `config.cpp:151-153` |
| `workspace` | `root` | string | `"."` | `config.cpp:183-185` |
| `workspace` | `workspace_roots` | array<string> | `[]` | `config.cpp:186-189` (empty array is a no-op, §7.4) |
| `permissions` | `shell` | string | `"ask"` | `config.cpp:195-197` |
| `permissions` | `write` | string | `"ask"` | `config.cpp:198-199` |
| `permissions` | `read` | string | `"allow"` | `config.cpp:200-201` |
| `logging` | `level` | string | `"info"` | `config.cpp:205-207` |
| `logging` | `log_prompts` | bool | `false` | `config.cpp:208-209` |
| `llm` / `llm.default` | `provider` | string | `"openai-compatible"` | `config.cpp:251-252` |
| `llm` / `llm.default` | `base_url` | string | `"https://api.deepseek.com/v1"` | `config.cpp:253-254` |
| `llm` / `llm.default` | `model` | string | `"deepseek-flash"` | `config.cpp:255-256` |
| `llm` / `llm.default` | `api_key_env` | string | `"DEEPSEEK_API_KEY"` | `config.cpp:257-258` |
| `llm` / `llm.default` | `reasoning_effort` | optional string | absent | `config.cpp:259-260` |
| `llm` / `llm.default` | `max_concurrency` | int64→size_t | `4` | `config.cpp:261-263` |
| `llm` / `llm.default` | `connect_timeout_ms` | int64→ms | `10000` | `config.cpp:264-265` |
| `llm` / `llm.default` | `idle_timeout_ms` | int64→ms | `60000` | `config.cpp:266-267` |
| `llm` / `llm.default` | `request_timeout_ms` | int64→ms | `120000` | `config.cpp:268-269` |
| `llm` / `llm.default` | `retry` | table | — | `config.cpp:271-277` |
| `llm.default.retry` | `max_attempts` | int64→uint32 | `3` | `config.cpp:217-219` |
| `llm.default.retry` | `base_delay_ms` | int64→ms | `500` | `config.cpp:220-221` |
| `llm.default.retry` | `max_delay_ms` | int64→ms | `30000` | `config.cpp:222-223` |
| `llm.default.retry` | `jitter` | double | `0.25` | `config.cpp:224-225` |
| `llm.default.retry` | `honor_retry_after` | bool | `true` | `config.cpp:226-227` |
| `mcp` | `enabled` | bool | `true` | `config.cpp:344` |
| `mcp` | `max_servers` | int64→size_t | `8` | `config.cpp:345-346` |
| `mcp` | `max_inflight_calls_per_server` | int64→size_t | `4` | `config.cpp:347-350` |
| `mcp` | `startup_deadline_ms` | int64 | `5000` | `config.cpp:351-352` |
| `mcp` | `handshake_timeout_ms` | int64 | `10000` | `config.cpp:353-354` |
| `mcp` | `list_timeout_ms` | int64 | `5000` | `config.cpp:355-356` |
| `mcp` | `list_max_pages` | int64→size_t | `64` | `config.cpp:357-359` |
| `mcp` | `reconnect_max_attempts` | int64→uint32 | `5` | `config.cpp:360-362` |
| `mcp` | `reconnect_initial_backoff_ms` | int64 | `500` | `config.cpp:363-365` |
| `mcp` | `reconnect_max_backoff_ms` | int64 | `30000` | `config.cpp:366-367` |
| `mcp` | `reconnect_jitter` | double | `0.25` | `config.cpp:368-369` |
| `mcp` | `reconnect_stable_window_ms` | int64 | `30000` | `config.cpp:370-371` |
| `mcp` | `ping_interval_ms` | int64 | `15000` | `config.cpp:372-373` |
| `mcp` | `shutdown_grace_ms` | int64 | `2000` | `config.cpp:374-375` |
| `mcp` | `max_frame_bytes` | int64→size_t | `8388608` | `config.cpp:376-378` |
| `mcp` | `allow_network_servers` | bool | `false` | `config.cpp:379-380` |
| `mcp` | `server` (array of tables) | array<table> | `[]` | `config.cpp:382-397` |
| `mcp.server` | `id` | string | `""` | `config.cpp:289-290` |
| `mcp.server` | `enabled` | bool | `true` | `config.cpp:291-292` |
| `mcp.server` | `required` | bool | `false` | `config.cpp:293-294` |
| `mcp.server` | `transport` | string | `"stdio"` | `config.cpp:295-296` |
| `mcp.server` | `command` | string | `""` | `config.cpp:297-298` |
| `mcp.server` | `args` | array<string> | `[]` | `config.cpp:311-314` |
| `mcp.server` | `env` | array<string> | `[]` | `config.cpp:315-318` |
| `mcp.server` | `cwd` | string | `""` | `config.cpp:299` |
| `mcp.server` | `url` | string | `""` | `config.cpp:300` |
| `mcp.server` | `header_env` | array<string> | `[]` | `config.cpp:319-322` |
| `mcp.server` | `protocol_version` | string | `""` | `config.cpp:301-302` |
| `mcp.server` | `allowed_tools` | array<string> | `[]` | `config.cpp:323-326` |
| `mcp.server` | `denied_tools` | array<string> | `[]` | `config.cpp:327-330` |
| `mcp.server` | `default_verdict` | string | `"ask"` | `config.cpp:303-304` |
| `mcp.server` | `call_timeout_ms` | int64 | `60000` | `config.cpp:305-306` |
| `mcp.server` | `max_result_bytes` | int64→size_t | `1048576` | `config.cpp:307-309` |
| `skills` | `enabled` | bool | `true` | `config.cpp:406` |
| `skills` | `expose_workspace` | bool | `false` | `config.cpp:407-408` |
| `skills` | `max_skills` | int64→size_t | `256` | `config.cpp:409-410` |
| `skills` | `max_skill_bytes` | int64→size_t | `65536` | `config.cpp:411-413` |
| `skills` | `max_description_bytes` | int64→size_t | `512` | `config.cpp:414-416` |
| `skills` | `max_index_bytes` | int64→size_t | `8192` | `config.cpp:417-419` |
| `skills` | `max_frontmatter_bytes` | int64→size_t | `4096` | `config.cpp:420-422` |

Struct defaults are pinned in `include/ymh/config/config.hpp:36-160`.

**Empty-array semantics differ by key (pinned, real code).** Every
`array<string>` key is read through `read_string_array`
(`src/config/config.cpp:77-99`) and then guarded by `!values.empty()` before
assignment. The guard's observable effect depends on whether the assignment
target survives the layer:

- **`workspace_roots` (and only this key): an empty array is a genuine no-op.**
  `apply_workspace` guards the assignment on the *same* `Config` object the
  earlier layer populated (`!roots.empty()`, `src/config/config.cpp:186-189`),
  so `"workspace_roots": []` leaves the earlier layer's list intact. It cannot
  clear a global list from the workspace layer.
- **Per-server arrays (`args`, `env`, `header_env`, `allowed_tools`,
  `denied_tools`): an empty array is equivalent to *absent*, not "preserve the
  earlier layer".** `apply_mcp` first runs `mcp.servers.clear()` whenever the
  `mcp.server` key is present, then constructs a **fresh** `McpServerSettings`
  per entry (`src/config/config.cpp:382-397`) and calls `apply_mcp_server` on
  it. Each fresh server starts at its struct defaults, so the `!values.empty()`
  guard at `src/config/config.cpp:311-330` only protects an already-empty
  default: `"args": []` yields `[]`, exactly as if `args` had been omitted.
  Because the whole list was cleared first, no per-server value from an earlier
  layer can survive; an empty per-server array does **not** preserve it.

The `mcp.server` table-array itself is cleared and rebuilt whenever the key is
present, so `"server": []` **does** clear the list (as does any present `server`
array). All three behaviours are pinned in §7.4 and §7.8 and tested (J-T21,
J-T22, J-T26).

### 3.4 Global-vs-workspace precedence and layer order

| Fact | Evidence |
|---|---|
| Loader applies global then workspace then env | `load_config`, `src/config/config.cpp:742-748` |
| Later layer wins per key (scalars) | `include/ymh/config/config.hpp:5-16`; tests `tests/unit/config_test.cpp:111-133` |
| `agent.model` overrides `llm.model` via `effective_model` | `effective_model`, `src/config/config.cpp:757-762`; `tests/unit/config_test.cpp:132` |
| Env overrides (`YMH_*`) applied last | `apply_env_overrides`, `src/config/config.cpp:670-740` |
| CLI overrides applied by the CLI layer | `load_invocation_config`, `src/cli/cli.cpp:70-88` |
| Array fields replace wholesale when non-empty. An empty array is a **no-op only for `workspace_roots`**; for per-server arrays an empty array is equivalent to absent (each server is rebuilt fresh), and the `mcp.server` table-array clears/replaces whenever present, including `[]` | `15-mcp-adapter.md:1296-1301`; `src/config/config.cpp:186-189` (`apply_workspace`), `:311-330` (`apply_mcp_server`), `:382-397` (`apply_mcp`) |

### 3.5 Scaffolding

| Fact | Evidence |
|---|---|
| Default TOML text | `kDefaultConfigToml`, `src/config/config.cpp:488-540` |
| Warn helper (null-safe) | `scaffold_warn`, `src/config/config.cpp:542-546` |
| Directory creation (best-effort) | `ensure_directory`, `src/config/config.cpp:548-569` |
| Write-if-absent, never overwrite | `write_default_config`, `src/config/config.cpp:571-595` |
| `scaffold_config` orchestration | `src/config/config.cpp:621-645` |
| No empty-target guard in `scaffold_config` (the gap J-C12 closes): `ensure_directory("")` is a no-op returning `true`, then `write_default_config("")` warns `config: cannot write ''` and returns `false`, so `ok` becomes `false` | `src/config/config.cpp:621-641` (`scaffold_config`), `:548-551` (`ensure_directory` empty short-circuit), `:582-586` (`write_default_config` open failure) |
| `ScaffoldResult` fields | `include/ymh/config/config.hpp:190-199` |
| Scaffolding invoked before load on every run command | `scaffold_for_invocation`, `src/cli/cli.cpp:105-122`, called at `:653` |

**Rev 5 (21-D13).** The empty-target branch (J-C12) is retained and is now the
mechanism by which an explicit `--config` suppresses global scaffolding (§8.1).

### 3.6 Consumers / readers

Every reader of `Config` (so every place the format switch is invisible, but the
interface change must not break them). Rows are anchored by the function that
reads the struct, with `file:line` as a secondary hint (the symbol-anchored
convention of §14.3):

| Consumer (symbol) | Evidence |
|---|---|
| `to_provider_config` — `[llm]` mapping | `src/cli/wiring.cpp:54-69` |
| `to_permission_config` — `[permissions]` mapping | `src/cli/wiring.cpp:71-97` |
| `to_mcp_config` — `[mcp]` mapping | `src/cli/wiring.cpp:99-153` |
| `to_skill_catalog_config` — `[skills]` mapping | `src/cli/wiring.cpp:155-165` |
| `to_agent_config` — `[agent]` defaults | `src/cli/wiring.cpp:167-180` |
| `to_compaction_policy` — `[agent.compaction]` | `src/cli/wiring.cpp:182-203` |
| `effective_model` — `agent.model` else `llm.model` | `src/config/config.cpp:757-762`; callers `wiring.cpp:58,169`, `ui_application.cpp:475,493` |
| `load_invocation_config` — TUI/`run`/`list`/… load | `src/cli/cli.cpp:61-68` |
| `run_host_command` — daemon load (`--host`) | `src/cli/cli.cpp:215-219` |
| TUI model / session options | `src/ui/ui_application.cpp:475,493` |
| `run_config_command` — `ymh config path` | `src/cli/cli.cpp:124-134` |
| Downstream consumers of the wiring adapters (read `Config` indirectly) | `src/agent/workspace_runtime.cpp:37,48,103,128,154,242`; `src/host/workspace_host.cpp:523` |

The two rows absent from the round-2 table — `to_skill_catalog_config`
(`wiring.cpp:155-165`, the `[skills]` reader added by RB-05) and
`to_compaction_policy` (`wiring.cpp:182-203`, the `[agent.compaction]` reader
added by spec 13) — are the same class of drift as the omitted `skills` field
(§5.4): a schema/reader inventory that lags the code. Both are re-derived here
from the current tree (`grep -n "const Config&" include/ymh/cli/wiring.hpp`
yields exactly the six `to_*` readers listed above).

### 3.7 Build wiring

| Fact | Evidence |
|---|---|
| toml++ fetched from git | `CMakeLists.txt:84-92` |
| `ymh_config` links toml++ | `CMakeLists.txt:435-439` |
| nlohmann/json already found | `CMakeLists.txt:130` — `find_package(nlohmann_json REQUIRED)` |
| nlohmann/json linked to most targets | `CMakeLists.txt:159,184,213,245,271,293,323,354,384,416,539,566,597` |

---

## 4. The JSONC decision

### 4.1 What "JSONC" means in `ymh` (pinned)

**`ymh` JSONC = RFC 8259 JSON plus `//` line comments and `/* … */` block
comments, and *no* trailing commas.** This is the precise subset the chosen
parser accepts. The definition is pinned in J1–J3:

- **J1** — Comments: `// …` to end of line and `/* … */` are ignored anywhere a
  token may appear. Verified accepted by nlohmann 3.12.0 (§4.3, probe `comments`,
  `block-comment`).
- **J2** — No trailing commas: a `,` before `}` or `]` is a syntax error.
  Verified rejected by nlohmann 3.12.0 (§4.3, probe `trailing-comma`). The
  user-visible consequence is pinned in §4.4.
- **J3** — No `#` comments: a `#` is a syntax error, not a comment (probe
  `hash-comment`). A hand-converted TOML file that leaves `#` fails loud.

This is a **deliberate, documented subset**, not full "jsonc" as some editors
define it. §4.4 states the consequence and the rejected remediation.

### 4.2 Parser configuration (exact)

The loader uses the already-present `nlohmann::json`, with comment tolerance
enabled. No new dependency is added (satisfying `AGENTS.md`'s "no giant umbrella
dependency"; nlohmann/json is already a transitive dependency of the project at
`CMakeLists.txt:130`).

```cpp
// src/config/config.cpp (new include, replacing <toml++/toml.h> at :13)
#include <nlohmann/json.hpp>

namespace ymh {
namespace {

using Json = nlohmann::json;

// True iff `text` contains only whitespace and `//` / `/* */` comments (no JSON
// token at all), ignoring a single optional leading UTF-8 BOM. This needs no
// string-literal handling: any non-whitespace byte that is not a comment opener
// makes it return false before any string is entered. Used so an empty,
// comments-only, or BOM-only config is a no-op `{}`, matching the retired TOML
// loader where an empty file parsed to an empty table (J-L2).
bool blank_or_comments_only(std::string_view text) {
    std::size_t i = 0;
    // A leading UTF-8 BOM (EF BB BF) is not a JSON token: skip it so BOM-only,
    // BOM+whitespace and BOM+comments files are blank (the "BOM accepted" rule,
    // §4.3). Only at byte 0; a BOM anywhere else is left to `parse`, which
    // rejects it (§4.2 boundary table).
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        i = 3;
    }
    while (i < text.size()) {
        const char c = text[i];
        // RFC 8259 whitespace only: space, tab, LF, CR. `\f` (0x0C) and `\v`
        // (0x0B) are NOT JSON whitespace, so a file containing only them is not
        // blank and reaches `parse`, which rejects it (J-F1) instead of being
        // silently no-op'd. A literal `{}` is likewise not blank: `{` is not
        // whitespace, so it is parsed normally (see §4.2, `{}` note).
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            i += 2;
            while (i < text.size() && text[i] != '\n') { ++i; }
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) { ++i; }
            if (i + 1 >= text.size()) {
                return false;   // unterminated block comment is malformed, not blank
            }
            i += 2;
        } else {
            return false;
        }
    }
    return true;
}

// Byte offset -> 1-based line, for the error message. Forward-declared here and
// defined below `parse_jsonc`: the sketch is a single translation unit, and
// `parse_jsonc`'s catch block calls it, so it must be declared before use.
std::size_t line_for_byte(std::string_view text, std::size_t byte);

// Parse a JSONC document with comments tolerated. Exceptions are enabled so the
// caller can report line/column (parity with the TOML path, config.cpp:659-665).
Json parse_jsonc(const std::filesystem::path& path, std::string_view text) {
    if (blank_or_comments_only(text)) {
        return Json::object();          // empty / comments-only => no-op (J-L2)
    }
    try {
        return Json::parse(text,
                           /*cb=*/nullptr,
                           /*allow_exceptions=*/true,
                           /*ignore_comments=*/true);
    } catch (const Json::parse_error& error) {
        std::ostringstream message;
        message << error.what() << " (line " << line_for_byte(text, error.byte) << ')';
        fail(path, message.str());          // fail() is [[noreturn]] (config.cpp:23)
    } catch (const std::exception& other) {
        fail(path, other.what());
    }
}

} // namespace
} // namespace ymh
```

The **exact** call is:

```cpp
nlohmann::json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/true,
                      /*ignore_comments=*/true)
```

- `cb = nullptr` — no parse callback; the default DOM parse is used.
- `allow_exceptions = true` — on syntax error, `nlohmann::json::parse_error` is
  thrown (it carries `.byte`, `exceptions.hpp:204`); we translate it to
  `ConfigError`, preserving the existing "config `<path>`: `<detail>`" shape
  (`config.cpp:23-25`).
- `ignore_comments = true` — the JSONC flag. It enables both `//` and `/* */`
  comment forms.

**Empty, comments-only, and BOM-only files (pinned, J-L2).** Raw `parse` rejects
`""`, whitespace-only, comments-only, and BOM-only input with `parse_error.101`
(§4.3 probes). That would be a **behaviour regression**: the retired TOML loader
treated an empty (or comments-only) file as a no-op empty table, i.e. equivalent
to `{}` (toml++ parses no tokens and returns an empty table). To keep parity, the
loader short-circuits `blank_or_comments_only(text)` to `Json::object()`
**before** calling `parse`. `blank_or_comments_only` skips one optional **leading**
UTF-8 BOM (EF BB BF) so that a BOM-only or BOM+comments file is blank too; the
BOM is otherwise left to `parse`, which skips it (§4.3 probe `bom`). Consequences:
an empty, comments-only, or leading-BOM-only `config.jsonc` is a valid no-op
(defaults apply); it is **not** a `ConfigError`. Only a file with a real token
that is malformed still fails loud (J-F1). This is the only blank-input special
case, and it is pinned so it is not lost during implementation.

**What a literal `{}` means (pinned).** A file whose entire content is `{}` is
**not** the blank-input special case above — `{` is not whitespace, so
`blank_or_comments_only` returns false and the text goes through `parse`
normally. It is valid JSON, parses to an empty object, and is likewise a valid
**no-op** (every layer stays at built-in defaults): the two paths agree on the
observable result, but by different routes. A literal `{}` is never a
`ConfigError`. The blank-input set is exactly "zero JSON tokens, after an
optional single leading UTF-8 BOM", and the whitespace that may separate them is
exactly RFC 8259 whitespace — space, tab, LF, CR — so `\f`/`\v` are **not**
accepted and a file containing them is a parse error, not a silent no-op.

**Boundary inputs (pinned — one outcome per input).** Every input where the two
rules above ("BOM accepted" and "blank set = zero JSON tokens") could disagree is
enumerated, with the single outcome this design pins. All were executed against
the system nlohmann 3.12.0 header (§4.3); the "route" column is the loader path.

| Input | Outcome | Route |
|---|---|---|
| `""` (empty) | no-op `{}` | blank short-circuit |
| whitespace-only (space/tab/LF/CR) | no-op `{}` | blank short-circuit |
| `// …` only | no-op `{}` | blank short-circuit |
| `/* … */` only | no-op `{}` | blank short-circuit |
| single leading BOM only | no-op `{}` | blank short-circuit (BOM skipped) |
| BOM + whitespace only | no-op `{}` | blank short-circuit (BOM skipped) |
| BOM + comments only | no-op `{}` | blank short-circuit (BOM skipped) |
| `{}` | no-op `{}` | `parse` |
| `{ /* c */ }` | no-op `{}` | `parse` |
| BOM + `{}` (or BOM + ws + `{}`) | no-op `{}` | `parse` (nlohmann skips the BOM) |
| BOM after whitespace (`"  \xEF\xBB\xBF{}"`) | `ConfigError` | `parse` (BOM recognised only at byte 0) |
| two BOMs (`"\xEF\xBB\xBF\xEF\xBB\xBF{}"`) | `ConfigError` | `parse` |
| `\f` (0x0C) or `\v` (0x0B) only | `ConfigError` | `parse` |
| unterminated `/* …` | `ConfigError` | `parse` |

So the only inputs that are a silent no-op are the blank set (optionally after a
single leading BOM) and a syntactically valid empty object; every other
non-object or malformed token fails loud.

A small helper converts the byte offset to a 1-based line for the error message:

```cpp
std::size_t line_for_byte(std::string_view text, std::size_t byte) {
    const std::size_t end = std::min(byte, text.size());
    return 1 + static_cast<std::size_t>(std::count(text.begin(), text.begin() + end, '\n'));
}
```

`text` is read once into a `std::string` (binary mode) so the same buffer is used
for parsing and for the line computation.

**JSON read helpers (sketch, J-C7).** The TOML `read_value` /
`read_optional_string` / `read_string_array` helpers
(`src/config/config.cpp:47-99`) are replaced by explicit per-type functions. Each
returns `fallback` when the key is absent and throws `ConfigError` on a type
mismatch, exactly like today; `read_int64` additionally rejects negative values
(J13, J-F10). These names are the ones referenced by §7.9 and J-F10:

```cpp
// src/config/config.cpp (namespace { … })
[[nodiscard]] std::string read_string(const Json& obj, std::string_view key,
                                      std::string_view table, std::string fallback,
                                      const std::filesystem::path& source);
[[nodiscard]] bool read_bool(const Json& obj, std::string_view key,
                             std::string_view table, bool fallback,
                             const std::filesystem::path& source);
[[nodiscard]] double read_double(const Json& obj, std::string_view key,
                                 std::string_view table, double fallback,
                                 const std::filesystem::path& source);
// Non-negative only: a negative integer is a ConfigError (J13, J-F10).
[[nodiscard]] std::int64_t read_int64(const Json& obj, std::string_view key,
                                      std::string_view table, std::int64_t fallback,
                                      const std::filesystem::path& source);
[[nodiscard]] std::optional<std::string> read_optional_string(
    const Json& obj, std::string_view key, std::string_view table,
    const std::filesystem::path& source);
[[nodiscard]] std::vector<std::string> read_string_array(
    const Json& obj, std::string_view key, std::string_view table,
    const std::filesystem::path& source);
```

### 4.3 Vendored nlohmann/json capability evidence

**Which nlohmann/json?** The **system** header, not a FetchContent copy:

- `find_package(nlohmann_json REQUIRED)` — `CMakeLists.txt:130`.
- Installed package: `nlohmann-json 3.12.0-2` (system package manager).
- Version macros: `NLOHMANN_JSON_VERSION_MAJOR 3`, `…_MINOR 12`, `…_PATCH 0` —
  `/usr/include/nlohmann/detail/abi_macros.hpp:21-23`.
- Header path: `/usr/include/nlohmann/json.hpp`.

**Evidence (compiled + executed, not assumed).** A probe compiled with
`g++ -std=c++23` against the system header produced:

| Input | Result |
|---|---|
| `{ // line\n /* block */ "ui": { "theme": "default" } }` with `ignore_comments=true` | **accepted** → `{"ui":{"theme":"default"}}` |
| `{ "a": 1, }` with `ignore_comments=true` | **rejected**: `parse_error.101 … unexpected '}'; expected string literal` |
| `{ // ok\n "a":1 # bad\n}` | **rejected**: `parse_error.101` (the `#` is not a comment) |
| `{ "a":1 } // no newline` | **accepted** (trailing comment at EOF) |
| `"// not a comment"` inside a string | **accepted** as a literal string |
| UTF-8 BOM before `{` | **accepted** (BOM is skipped) |
| UTF-8 BOM only (no other byte) | **rejected** by `parse` (`parse_error.101`, "unexpected end of input") → the loader short-circuits a leading BOM before `blank_or_comments_only` decides blank, so it is a no-op `{}` (J-L2, §4.2) |
| BOM + whitespace, or BOM + comments only | **rejected** by `parse` → same leading-BOM blank short-circuit to `{}` (J-L2) |
| BOM after whitespace (`"  \xEF\xBB\xBF{}"`) or a second BOM | **rejected** (`parse_error.101`, "invalid literal") → a BOM is recognised only at byte 0; the loader passes it to `parse` and it fails loud (§4.2) |
| top-level `[1,2,3]` | **accepted by the parser** → the loader must reject non-objects (§7.1) |
| duplicate keys `{"a":1,"a":2}` | **accepted, last wins** silently → documented divergence (§7.10) |
| `NaN`, `+1` | **rejected** (`parse_error.101`) |
| `""` (empty), `"   \n\t "` (whitespace) | **rejected** by `parse` (`parse_error.101`, "attempting to parse an empty input" / "unexpected end of input") → the loader short-circuits these to `{}` before calling `parse` (J-L2) |
| `// hi\n` and `/* hi */` (comments only) | **rejected** by `parse` (`parse_error.101`, "unexpected end of input") → same short-circuit to `{}` (J-L2) |

The `ignore_comments` overload signature is at `/usr/include/nlohmann/json.hpp:136-141`
and `:4045-4049`; `parse_error::byte` at `/usr/include/nlohmann/detail/exceptions.hpp:204`.

### 4.4 Trailing commas: unsupported — user-visible consequence

**nlohmann/json 3.12.0 does not support trailing commas.** There is no parser
flag for it, and `ignore_comments=true` does not change this (probe above).

**User-visible consequence (pinned).** A `config.jsonc` with a trailing comma
after the last member of an object or array fails to load with a
`ConfigError` naming the file and line, e.g.:

```
ymh: config /home/u/.config/ymh/config.jsonc: [json.exception.parse_error.101]
     parse error at line 12, column 3: syntax error while parsing object key -
     unexpected '}'; expected string literal (line 12)
```

This is **fail loud**, consistent with strictness (J8). It means the phrase
"JSONC" in `ymh` is narrower than VS Code's jsonc: **comments yes, trailing
commas no**.

**Rejected remediation (recorded).** A string-aware trailing-comma stripper
pre-pass was considered and **rejected for this errata**:

- A correct stripper must be a mini JSON lexer that tracks string state, escape
  sequences, and comment state — otherwise it will corrupt a comma inside a
  string value (e.g. `"base_url": "http://x/a,b"`). That is real parser code
  with real risk, to paper over a cosmetic convenience.
- A naive regex/`std::regex` replacement is unsafe for exactly that reason and
  is rejected outright.
- The low-risk alternative is user discipline: the scaffold (§8) is
  comma-correct, and the error message is explicit.

**Deferred enhancement (pinned for a follow-up, not implemented here).** If
trailing-comma tolerance is later required, add a single `strip_trailing_commas`
lexer immediately before `parse_jsonc`, and unit-test it with a comma inside a
string, a comma inside a comment, and a comma before `}`/`]`. That is a
self-contained follow-up with its own gate; it is **not** part of RB-09.

### 4.5 Rejected parser alternatives

| Alternative | Why rejected |
|---|---|
| Keep toml++ and add a JSONC reader (dual-read) | Explicitly forbidden by the user's decision (`REQUIREMENTS_BACKLOG.md:250-254`); doubles the schema surface and precedence rules. |
| Use a third-party JSONC library (e.g. jsoncpp, RapidJSON with comments) | Adds a dependency for no capability nlohmann lacks except trailing commas; violates "do not build a giant umbrella dependency" (`AGENTS.md`). |
| Hand-written JSONC parser | Reimplements a well-tested parser; large risk for zero benefit. |
| `nlohmann::ordered_json` | Order is irrelevant to a config loader; no reason to diverge from `nlohmann::json`. |
| JSON5 (unquoted keys, trailing commas) | Far larger dialect; the user asked for JSONC, and no parser is present. |

---

## 5. Filename + resolution

### 5.1 Filename constants (pinned)

```cpp
// src/config/config.cpp
constexpr std::string_view kGlobalDir  = "ymh";           // unchanged
constexpr std::string_view kConfigFile = "config.jsonc";  // was config.toml
// Rev 5 (21-D11): kLegacyConfigFile is DELETED. No TOML basename exists.
```

### 5.2 Path resolution (pinned, unchanged logic)

`default_global_config_path()` (`src/config/config.cpp:607-615`) keeps its exact
branch order; only `kConfigFile` changes:

1. `$XDG_CONFIG_HOME` set and non-empty → `$XDG_CONFIG_HOME/ymh/config.jsonc`.
2. else `$HOME` set and non-empty → `$HOME/.config/ymh/config.jsonc`.
3. else `./.config/ymh/config.jsonc` (relative fallback, unchanged).

`workspace_config_path(root)` (`src/config/config.cpp:617-619`) →
`<root>/.ymh/config.jsonc`.

`XDG_CONFIG_HOME` unset: unchanged from today (`src/config/config.cpp:611-614`) —
the `$HOME/.config` fallback is used. `env_value`
(`src/config/config.cpp:599-605`) treats an empty variable as unset, so
`XDG_CONFIG_HOME=""` falls through to `$HOME`.

### 5.3 Does the workspace-level config also become JSONC?

**Yes.** There is one format and one basename; the workspace file is
`<workspace>/.ymh/config.jsonc` (`src/config/config.cpp:617-619`). The user's
requirement names the global path, but the loader uses the same `kConfigFile` for
both slots (`src/config/config.cpp:609,612,614,618`); keeping TOML at the
workspace level would require a second parser and a second strictness path —
rejected (J10, J11).

### 5.4 Interface sketch

```cpp
// include/ymh/config/config.hpp (amendments only; unchanged lines elided)
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ymh {

class ConfigError : public std::runtime_error { /* unchanged :31-34 */ };

struct Config {
    UiConfig           ui;
    AgentDefaults      agent;
    WorkspaceSettings  workspace;
    PermissionDefaults permissions;
    LoggingSettings    logging;
    LlmSettings        llm;
    McpSettings        mcp;
    SkillsSettings     skills;   // [skills] (20 §5.6; added by RB-05)
};

// Explicit layer sources. `global` is required (21-D12); `workspace` may be empty (optional).
struct ConfigPaths {
    std::filesystem::path global;      // global config.jsonc
    std::filesystem::path workspace;   // <root>/.ymh/config.jsonc
};

// `$XDG_CONFIG_HOME/ymh/config.jsonc`, else `$HOME/.config/ymh/config.jsonc`.
[[nodiscard]] std::filesystem::path default_global_config_path();

// `<root>/.ymh/config.jsonc`.
[[nodiscard]] std::filesystem::path workspace_config_path(
    const std::filesystem::path& workspace_root);

// Rev 5 (21-D11): legacy_config_path, jsonc_target and scaffold_target are
// DELETED. Nothing in this header probes a `.toml` path.
//
// Rev 5 (21-D12): the global layer is REQUIRED. `load_config` throws
// `ConfigError` when `paths.global` is empty or does not exist. `Logger*` is
// removed: it existed only to carry the deleted legacy warning.
[[nodiscard]] Config load_config(const ConfigPaths& paths);
[[nodiscard]] Config load_config(const std::filesystem::path& workspace_root);

// Applies one JSONC document over `config`. `required == false` (the optional
// workspace layer): an empty path or an absent file contributes nothing.
// `required == true` (the global layer): an empty path or an absent file throws
// `ConfigError` (21-D12). A present path that is not a regular file
// (directory/FIFO/socket/device) is a `ConfigError` for EITHER layer (21-D16).
// The empty-path error is constructed directly as
// `ConfigError("config: required global config path is empty")` -- it must NOT
// go through `fail(path, ...)`, which would render `config : ...` (21-D16, L6).
// A parse failure or unknown key always throws.
void apply_jsonc_file(Config& config, const std::filesystem::path& path,
                      bool required = false);

// Unchanged.
void apply_env_overrides(Config& config);
[[nodiscard]] std::string effective_model(const Config& config);
[[nodiscard]] std::optional<std::string> env_value(std::string_view name);

} // namespace ymh
```

**Source compatibility (Rev 5).** Removing the `Logger*` parameter from
`load_config` is a **breaking** signature change: the CLI call sites
(`load_invocation_config`, `run_host_command`) and every test that passed a
`CapturingLogger` must drop the argument. `scaffold_config`'s signature is
unchanged and keeps its `Logger*`; its body keeps the empty-target branch
(J-C12, §8.1) — now reached when the user passes an explicit `--config`
(21-D13) — so the explicit path is a deliberate no-op rather than a
`cannot write ''` failure. `apply_jsonc_file` gains a defaulted `bool required`
parameter, so existing callers keep compiling.

### 5.5 Supervisor → daemon config handoff (pinned, 21-D15)

**Problem (H1).** A Tui/Run supervisor resolves its global config from
`--config` when present. A workspace daemon is a **separate process** spawned by
`ForkExecLauncher` and re-resolves the global path itself. Before Rev 6 the
launcher's argv carried no `--config`
(`ForkExecLauncher::build_argv`, `src/host/workspace_host.cpp`) and
`HostLifecycle::configFor` carried no config path, so a daemon spawned by
`ymh --config <path>` re-loaded `default_global_config_path()`. With 21-D12 that
is a hard error: the daemon exits 2 and `HostLifecycle::spawnAndAttach` fails
(it detects the dead daemon on its `!isAlive` check, then spends up to ~5 s in
the winner-attach window). Rev 6 fixes the handoff. This also fixes a **latent
pre-RB-09 divergence** — the daemon previously ignored `--config` entirely and
silently ran on defaults.

**Rule (scoped, Rev 7).** When the supervisor supplies a path (Tui/`run`
always do), the daemon loads **exactly** that file; it never re-resolves the
global path independently. The field is additive and defaulted, so the
*unsupplied* case — direct `HostConfig{}`/test construction, or a hand-invoked
`ymh --host` without `--config` — keeps the pre-Rev-6 behaviour: the daemon
resolves `default_global_config_path()` itself. J20 is scoped to match.

**Interface deltas (pinned).**

1. `HostConfig` (`include/ymh/host/workspace_host.hpp`) gains an **additive**
   field:
   ```cpp
   std::filesystem::path config_path;   // global config the daemon must load
   ```
2. `ForkExecLauncher::build_argv` (`src/host/workspace_host.cpp`) appends
   `--config <path>` when `config.config_path` is non-empty.
3. `HostLifecycle` (`include/ymh/host/host_launcher.hpp`) gains a constructor
   parameter `std::filesystem::path config_path = {}`, stored as `config_path_`;
   `HostLifecycle::configFor` (`src/host/workspace_host.cpp`) sets
   `config.config_path = config_path_`.
4. `run_supervisor_entry` and `run_via_daemon` (`src/cli/cli.cpp`) each gain a
   config-path parameter; `run_cli` passes
   `effective_global_config(invocation)`.

`config_path` is additive and defaulted, so every existing `HostConfig{}` /
`HostLifecycle{launcher, registry}` construction keeps compiling and keeps the
old behaviour (empty ⇒ the daemon resolves the conventional path). The Tui and
`run` paths always set it.

**No ctor validation (pinned, Rev 7).** `HostLifecycle` does **not** validate
`config_path` in its constructor. The supervisor has already loaded the file
successfully before spawning, so a ctor check is redundant and would break the
intentional defaulted-empty/test construction. The only remaining gap is the
load→spawn race (the file could disappear between the supervisor's load and the
daemon's load), which J-F15 covers.

**Path form (pinned, Rev 7).** `HostConfig::config_path` may be **relative or
absolute**. The supervisor resolves it in `load_invocation_config` and the
daemon resolves it in `run_host_command`, both **before** any `chdir` (the
daemon's `chdir` to the workspace root happens later, in `WorkspaceHost::run`);
the forked child inherits the supervisor's cwd (the launcher does not change
it), so a relative path resolves to the same file in both processes. Absolute
is preferred but not required.

**Failure mode.** J-F15: the daemon cannot load the supplied config (it exits 2
before binding its socket), so `spawnAndAttach` detects the dead daemon and
fails. Pinned by J-T44/J-T45.

---

## 6. No TOML awareness (pinned, 21-D11)

> **Renumbering note (Rev 5).** The original §6.1–§6.8 (detection rule, warning
> text/channel, once-per-run semantics, "what the user is told", both-files
> table, detection/warning sketch, and the `ymh config path` legacy note) are
> **deleted**. §6 is reduced to a short pinned section. No content of the old §6 survives
> except the `scaffold_config` empty-target branch, which moved to §8.1 (J-C12)
> and is now reached via 21-D13.

### 6.1 The rule (pinned)

ymh has **no TOML awareness**. Concretely, no code path in `ymh` may:

1. **stat** a `.toml` path (`std::filesystem::exists`/`status` on any
   `config.toml`);
2. **open** a `.toml` file (`std::ifstream`/`ofstream`/`toml::parse_file`);
3. **parse** TOML;
4. **mention** `config.toml` in any user-visible output (warning, note, error,
   `ymh config path` line); or
5. **warn** about a `.toml` sibling, migration, or conversion.

A `config.toml` sitting next to a `config.jsonc` is **invisible**: it is not
read, not parsed, not renamed, not deleted, not warned about, and not converted.
It contributes nothing to `Config`. There is **no migration warning** and **no
conversion** (21-D11).

This deletes the entire legacy-TOML machinery: `kLegacyConfigFile`,
`legacy_config_path`, `jsonc_target`, `scaffold_target`, `LegacyFile`,
`collect_legacy`, `warn_legacy` (`src/config/config.cpp`), their three header
declarations (`include/ymh/config/config.hpp`), and the `ymh config path` legacy
note (`run_config_command`, `src/cli/cli.cpp`). See §14.4 for the full impact
inventory.

### 6.2 What replaced the migration story

| Old §6 topic | Rev-5 replacement |
|---|---|
| Detection of a legacy sibling (`legacy_config_path`) | Deleted. Nothing is probed (21-D11). |
| Warning text / channel / once-per-run | Deleted. No warning is ever emitted for a `.toml`. |
| "Both files present" precedence table | Deleted. Only `config.jsonc` is ever considered. |
| Legacy-named `--config …/config.toml` substitution rules | Deleted. The slot is parsed as JSONC; if the file is absent, the required-global rule fires (21-D12). |
| `ymh config path` legacy note | Deleted. The command is unchanged. |
| Automatic conversion / migration command | Not offered (was 21-D5; now vacuous because there is no detection). |

The only surviving legacy-adjacent behaviour is **strictness**: a file that
happens to contain TOML but is named `config.jsonc` (or passed to `--config`)
fails loud as a JSONC parse error (`#` is not a comment, `[section]` is not a
JSON object member) — see J-F1/J-F7/J-F8. Nothing is silently swallowed.

### 6.3 Commands that load config (pinned, 21-D17)

The load set is pinned to equal `scaffold_for_invocation`'s set exactly:

| Command | Scaffolds | Loads config | Uses `Config` |
|---|---|---|---|
| `Tui`, `Run` | yes | yes | yes (passed to the supervisor / `HeadlessOptions`) |
| `List`, `Show`, `Replay`, `Fork` | yes | yes | no — `session_*` handlers take only `root` |
| `Workspace`, `Config`, `Version` | no | no | no |

The rationale is **not** "commands that consume `Config`" — `List`/`Show`/
`Replay`/`Fork` do not take a `Config` (`session_list`/`session_show`/
`session_replay`/`session_fork`, `src/cli/cli.cpp` ~:723-733). It is: (a) `Tui`/
`Run` need the full `Config`; (b) the other four are workspace commands that
already load and use the config-derived logging options; and (c) the load set
must equal the scaffold set so the scaffold/loader agreement (J19) holds. Before
Rev 6, `run_cli` scaffolded only the scaffold set but loaded config for every
command except `Version`/`Config`, so `ymh workspace …` on a fresh machine (no
config file) aborted with the J-F5 error even though the command never reads
`Config`. After 21-D17 the load is gated by the same switch as the scaffold.

**Logging is gated too (pinned, Rev 7).** `init_logging` is currently driven by
`config.logging` (`run_cli`, `src/cli/cli.cpp` ~:679-683) *after* the load.
Under 21-D17 the config-derived logging init is gated by the same switch: for
`Workspace`/`Config`/`Version` neither `load_config` nor the config-derived
`init_logging` runs, so `Workspace` executes with the **default** logging
options (exactly like `Config`/`Version`, which already return before
`init_logging`).

`run_workspace_command` (`src/registry/workspace_cli.cpp`) touches only the
registry (`WorkspaceRegistry::open` / `openReadOnly`): no daemon spawn and no
`Config`, so `ymh workspace add|list` must work with no config file present.
`ymh config path` never loads either (it prints the effective path only). The
`ymh --host` daemon entry always loads (it is the daemon) and receives the
supervisor's path via 21-D15.

## 7. Key-by-key JSONC schema

The JSONC document is a single top-level object. Section names are identical to
the TOML table names; the mapping is `[a.b]` → `"a": { "b": { … } }` and
`[[a.b]]` → `"a": { "b": [ { … }, … ] }`.

### 7.1 Top level

```jsonc
{
  "ui":          { /* §7.2 */ },
  "agent":       { /* §7.3 */ },
  "workspace":   { /* §7.4 */ },
  "permissions": { /* §7.5 */ },
  "logging":     { /* §7.6 */ },
  "llm":         { /* §7.7 */ },
  "mcp":         { /* §7.8 */ },
  "skills":      { /* §7.11 */ }
}
```

- Unknown top-level keys are rejected (`apply_document` calls
  `reject_unknown(table, "", …)`). The message is `unknown key
  'auto_compact_enabled'` — **no leading dot** (21-D14, §7.10).
- The top-level value **must be an object**. nlohmann accepts a top-level array
  (probe `top-array`), so the loader must explicitly reject non-objects (J9,
  J-F9).

### 7.2 `ui`

```jsonc
"ui": {
  "theme": "default",       // string
  "show_activity": true,    // bool
  "side_panel": "auto"      // string; "auto" | "always" | "never"
}
```

No value validation today (`config.cpp:101-108`); the comment enumerates the
intended values. Retained.

### 7.3 `agent` and `agent.compaction`

```jsonc
"agent": {
  "model": "",                       // string; empty => use llm.default.model
  "max_steps": 100,                  // integer >= 0
  // "reasoning_effort": "low",      // optional: "low" | "medium" | "high"
  // "system_prompt": "",            // optional: empty => built-in default
  "compaction_threshold_tokens": 0,  // legacy alias, integer >= 0
  "compaction": {
    "enabled": false,
    "threshold_tokens": 0,
    "threshold_ratio": 0.80,
    "context_window_tokens": 0,
    "reserve_output_tokens": 4096,
    "keep_recent_turns": 2,
    "min_prefix_messages": 4,
    "max_summary_tokens": 1024,
    "max_summary_bytes": 262144,
    "summarizer_model": "",
    "max_compactions_per_turn": 1,
    "retry_on_context_length": true
  }
}
```

`compaction_threshold_tokens` is a pre-existing alias for
`agent.compaction.threshold_tokens` (`config.cpp:170-172`); retained verbatim.

### 7.4 `workspace`

```jsonc
"workspace": {
  "root": ".",
  // "workspace_roots": []           // array<string>; first-run discovery roots
}
```

**Pinned: an empty `workspace_roots` array does not override (21-D9).** The
reader is `read_string_array` (`src/config/config.cpp:77-99`); the assignment is
guarded by `!roots.empty()` (`apply_workspace`, `src/config/config.cpp:186-189`).
Consequently:

- `"workspace_roots": ["a", "b"]` replaces the earlier layer's list wholesale
  (array replace, `15-mcp-adapter.md:1296-1301`).
- `"workspace_roots": []` is a **no-op**: it leaves whatever the earlier layer
  (or the built-in default `[]`) set. It cannot be used to *clear* a global list
  from the workspace layer.

This is the real current behaviour and is preserved verbatim (J10). **This no-op
property is specific to `workspace_roots`.** The per-server
`args`/`env`/`header_env`/`allowed_tools`/`denied_tools` arrays also carry the
`!values.empty()` guard, but because `mcp.server` is cleared and each server
rebuilt from defaults (§7.8), an empty per-server array behaves as *absent*, not
as a preserved earlier layer. The `mcp.server` list itself is cleared by any
present value, including `[]`. Tested by J-T21, J-T22, and J-T26 (§12.1).

### 7.5 `permissions`

```jsonc
"permissions": {
  "shell": "ask",   // "allow" | "ask" | "deny"
  "write": "ask",
  "read": "allow"
}
```

Value validation is downstream in `parse_verdict` (`wiring.cpp:10-22`), which
throws `ConfigError` for anything else. Retained.

### 7.6 `logging`

```jsonc
"logging": {
  "level": "info",     // "debug" | "info" | "warn" | "error"
  "log_prompts": false // never enable implicitly; prompt bodies are redacted
}
```

### 7.7 `llm`, `llm.default`, and `llm.default.retry`

Both shapes are accepted, exactly as today (`config.cpp:230-249`):

```jsonc
// Preferred (nested):
"llm": {
  "default": {
    "provider": "openai-compatible",
    "base_url": "https://api.deepseek.com/v1",
    "model": "deepseek-flash",
    "api_key_env": "DEEPSEEK_API_KEY",   // the NAME; the secret lives in the env
    // "reasoning_effort": "low",
    "max_concurrency": 4,
    "connect_timeout_ms": 10000,
    "idle_timeout_ms": 60000,
    "request_timeout_ms": 120000,
    "retry": {
      "max_attempts": 3,
      "base_delay_ms": 500,
      "max_delay_ms": 30000,
      "jitter": 0.25,
      "honor_retry_after": true
    }
  }
}
```

```jsonc
// Also accepted (flat; parity with config.cpp:230-278):
"llm": {
  "provider": "openai-compatible",
  "base_url": "https://api.deepseek.com/v1",
  "model": "deepseek-flash"
}
```

**Pinned quirk (retained, not fixed).** `apply_llm` (`config.cpp:237-249`) sets
`section = table["default"]` when `"default"` exists and then reads only from
`*section`. Therefore, when both a `"default"` object and flat keys are present,
the flat keys are accepted by `reject_unknown` but **ignored**. This is
pre-existing behaviour; the format switch preserves it verbatim (J12). Fixing it
is a separate semantic change and is recorded as an open item (§14.1).

`max_concurrency` is advisory to `LLMPool` (`08-llm-provider.md:719-720`);
unchanged.

### 7.8 `mcp` and `mcp.server`

The array-of-tables key is **`server`** (singular), matching
`config.cpp:382` (`table["server"]`), not `servers`:

```jsonc
"mcp": {
  "enabled": true,
  "max_servers": 8,
  "max_inflight_calls_per_server": 4,
  "startup_deadline_ms": 5000,
  "handshake_timeout_ms": 10000,
  "list_timeout_ms": 5000,
  "list_max_pages": 64,
  "reconnect_max_attempts": 5,
  "reconnect_initial_backoff_ms": 500,
  "reconnect_max_backoff_ms": 30000,
  "reconnect_jitter": 0.25,
  "reconnect_stable_window_ms": 30000,
  "ping_interval_ms": 15000,
  "shutdown_grace_ms": 2000,
  "max_frame_bytes": 8388608,
  "allow_network_servers": false,
  "server": [
    {
      "id": "github",
      "transport": "stdio",
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": ["GITHUB_PERSONAL_ACCESS_TOKEN=${GITHUB_TOKEN}"],
      "required": false,
      "default_verdict": "ask",
      "call_timeout_ms": 60000
    },
    {
      "id": "fs",
      "transport": "stdio",
      "command": "mcp-server-filesystem",
      "args": ["."],
      "cwd": ".",
      "allowed_tools": ["read_*", "list_*"],
      "denied_tools": ["write_*", "delete_*"],
      "default_verdict": "ask"
    }
  ]
}
```

Per-server `default_verdict` is validated downstream by `parse_verdict`
(`wiring.cpp:92`); `max_frame_bytes` is validated against
`protocol::TransportLimits` (`wiring.cpp:120-123`). Retained.

The example above is **illustrative, not exhaustive**: §3.3 is the normative key
inventory. Per-server keys not shown — `enabled`, `url`, `header_env`,
`protocol_version`, and `max_result_bytes` (the `enabled` in the example is
`mcp.enabled`, not the per-server one) — are accepted with the §3.3 defaults.

**Pinned: array semantics inside `mcp.server` (parity with today).** Two distinct
rules apply and are preserved verbatim (J10):

- The per-server string arrays `args`, `env`, `header_env`, `allowed_tools`, and
  `denied_tools` are read with the `!values.empty()` guard
  (`apply_mcp_server`, `src/config/config.cpp:311-330`). Because `apply_mcp`
  clears the list and constructs a fresh `McpServerSettings` for every entry
  (`src/config/config.cpp:382-397`), the guard only protects a fresh struct's
  default: `"args": []` yields `[]`, identical to omitting `args`. It does
  **not** preserve an earlier layer's per-server list (empty ≡ absent). Contrast
  `"workspace_roots": []`, which is a genuine no-op (§7.4).
- The `mcp.server` table-array itself is handled by `if (node) { mcp.servers.clear();
  … }` (`apply_mcp`, `src/config/config.cpp:382-397`): the key being **present**
  clears and rebuilds the list, so `"server": []` **does** clear a global server
  list. (An absent `server` key leaves the earlier list untouched.)

Tested by J-T21, J-T22, and J-T26 (§12.1).

### 7.9 Types, defaults, validation (pinned)

| JSON type | Accepted for | Rule |
|---|---|---|
| string | all string keys | `is_string()`; else `ConfigError` |
| boolean | all bool keys | `is_boolean()`; else `ConfigError` |
| integer | all count/size/duration keys | `is_number_integer()` or `is_number_unsigned()`; must fit `int64`; **must be ≥ 0** (21-D6) |
| number | `threshold_ratio`, `jitter`, `reconnect_jitter` | `is_number()` (int or float); read as `double` |
| array<string> | `workspace_roots`, `args`, `env`, `header_env`, `allowed_tools`, `denied_tools` | `is_array()`, every element `is_string()`; else `ConfigError` |
| object | every object-valued position: `ui`, `agent`, `agent.compaction`, `workspace`, `permissions`, `logging`, `llm`, `llm.default`, `llm.default.retry`, `mcp`, `mcp.server[i]` (each array element), `skills` | `is_object()`; else `ConfigError` |

**New validation introduced by this errata (21-D6).** Integer keys must be
**non-negative**. Today the TOML reader casts `int64_t` to `size_t`
unconditionally (`config.cpp:122-124` etc.), so a negative value silently wraps
to a huge `size_t`. JSONC is the moment to reject it (J13, J-F10). This is the
only semantic tightening in this errata; it rejects input that was already
nonsensical.

**Not added (recorded as open items, §14.1):** range checks for `threshold_ratio`
/ `jitter` / `reconnect_jitter` (today none), enum checks for `side_panel` /
`theme` / `logging.level` (today none or downstream), and duplicate-key
detection (§7.10).

### 7.10 Malformed and unknown keys (fail loud, pinned)

| Condition | Handling |
|---|---|
| Malformed JSONC (syntax) | `ConfigError` with file + line, run aborts (exit 2 via `run_cli`'s `ConfigError` catch). **Fail loud.** (J8) |
| Empty, comments-only, or leading-BOM-only file | **No-op `{}`** — parity with the retired TOML loader, where an empty/comment-only file parsed to an empty table. One optional leading UTF-8 BOM is skipped before the blank test, so a BOM-only file is a no-op too. Not an error (J-L2, §4.2). |
| Unknown key at any level | `ConfigError` (parity with `reject_unknown`). **Fail loud.** (J7) |
| Wrong value type | `ConfigError` (parity with `read_value`, `config.cpp:47-61`). **Fail loud.** |
| Negative integer | `ConfigError` (new, 21-D6). **Fail loud.** |
| Duplicate key | **Last wins, silently** (nlohmann default; probe `dup-keys`). Documented divergence from the retired TOML loader (the toml++ comparison is withdrawn, §14.2). Accepted as JSON semantics; a SAX-based duplicate detector is a deferred open item (§14.1). (J14) |
| UTF-8 BOM | Accepted. A **leading** BOM (byte 0 only) is skipped by `blank_or_comments_only` for the blank test and by `parse` for the token stream; a BOM after whitespace or a second BOM is a `ConfigError` (probes `bom`, §4.2 boundary table). |
| `#` comment (TOML habit) | Syntax error → `ConfigError`. No migration text exists; the error itself names the line (21-D11). (J-F8) |

**Exact unknown-key message (pinned, 21-D14).** `reject_unknown` builds the
qualified key as `table_name.empty() ? name : table_name + "." + name`. The
top-level caller `apply_document` passes an empty `table_name`
(`reject_unknown(table, "", {…})`), so the message is:

```
config <path>: unknown key 'auto_compact_enabled'
```

There is **no leading dot**. Nested tables keep the qualified form, e.g.
`config <path>: unknown key 'agent.compaction.foo'`. Pinned by J18 and tested by
J-T40/J-T41.

Fail-loud is chosen because it is the project's explicit policy: "The loader is
strict: an unknown key in a JSONC file is a `ConfigError`, so a typo can never
be silently ignored" (`config.hpp:13-15`; `08-llm-provider.md:721-722`). The
errata preserves that policy.

### 7.11 `skills`

```jsonc
"skills": {
  "enabled": true,
  "expose_workspace": false,       // DANGER: exposes repo-authored skills
  "max_skills": 256,
  "max_skill_bytes": 65536,
  "max_description_bytes": 512,
  "max_index_bytes": 8192,
  "max_frontmatter_bytes": 4096
}
```

Defaults are `SkillsSettings` (`include/ymh/config/config.hpp:151-160`); the keys
are read by `apply_skills` (`src/config/config.cpp:400-422`).
`expose_workspace` is a deliberate opt-in (default `false`). This section is
**additive relative to the errata's first draft**: it was introduced by the
skills subsystem (spec 20, RB-05) after this errata was authored, and is recorded
here so J12 ("no key is renamed, added, or removed") stays true. No key in this
section is an `array<string>`, so the §3.3 empty-array rules do not apply.

---

## 8. Scaffolding

### 8.1 What first run writes (pinned, 21-D13)

`scaffold_config` (`src/config/config.cpp`, ~:805-830; J-C12) keeps its existing
control flow with **one added branch**: best-effort, never throws, creates the
target's parent directory, writes the default JSONC text only if the target does
not exist, creates `<root>/.ymh/`, and reports via `ScaffoldResult`
(`include/ymh/config/config.hpp`, ~:211-220). Only the written **filename** and
**content** change, plus the empty-target branch pinned below.

**Scaffold target (pinned, 21-D13).** `scaffold_for_invocation`
(`src/cli/cli.cpp`) passes a target to `scaffold_config`:

```
scaffold_target(invocation) =
    invocation.config_path.empty() ? default_global_config_path() : {}   // explicit --config: no scaffold
```

- **No `--config`:** the target is the conventional
  `default_global_config_path()` = `$XDG_CONFIG_HOME/ymh/config.jsonc`, else
  `$HOME/.config/ymh/config.jsonc`. If absent it is created; the loader then
  requires the same path (21-D12) and reads it. Scaffold and loader agree.
- **Explicit `--config <path>`:** the target is **empty**. Scaffolding writes
  **no** global directory and **no** file — it must not create the user's
  explicit path. A missing explicit path is the `21-D12` hard error, never an
  auto-created default. The loader requires exactly `<path>`.
- **Either way**, scaffolding still ensures `<root>/.ymh/` exists.

`effective_global_config` (`src/cli/cli.cpp`) is retained for the loader path
and `ymh config path`; the scaffold no longer calls it. The old
`scaffold_target(slot)` helper (which inspected `kLegacyConfigFile`) is deleted
(21-D11).

**Empty-target branch (pinned, J-C12 — retained).** The existing
`scaffold_config(workspace_root, global_config, logger)` has **no** empty-path
guard: `global_config == ""` would call `ensure_directory("")` (a no-op that
returns `true` with `created == false`) and then `write_default_config("")`,
which opens `""` — an `ofstream` that fails — emits `config: cannot write ''`,
and sets `ScaffoldResult::ok = false`. That is the wrong outcome for a
**deliberate** skip, so the control flow is amended by exactly one branch:

```cpp
ScaffoldResult scaffold_config(const std::filesystem::path& workspace_root,
                               const std::filesystem::path& global_config,
                               Logger* logger) {
    ScaffoldResult result;
    result.global_config = global_config;
    result.workspace_dir = workspace_root / ".ymh";

    if (!global_config.empty()) {                       // NEW (J-C12)
        if (ensure_directory(global_config.parent_path(), logger,
                             result.global_dir_created)) {
            if (!write_default_config(global_config, logger,
                                      result.global_config_created)) {
                result.ok = false;
            }
        } else {
            result.ok = false;
        }
    }
    // Empty target: skip the global directory + write entirely. Emit no warning;
    // `ok` stays true; `global_dir_created`/`global_config_created` stay false.

    if (!ensure_directory(result.workspace_dir, logger, result.workspace_dir_created)) {
        result.ok = false;
    }
    return result;
}
```

Consequences of the branch (pinned): for `global_config == ""` the scaffold
performs **no** global directory creation, **no** file write, emits **no**
warning, and returns `ok == true` with `global_dir_created == false` and
`global_config_created == false`; the `<root>/.ymh/` step still runs. With
21-D13 this branch is reached exactly when the user passes an explicit
`--config`; it is no longer tied to a legacy basename.

**Scaffold/loader agreement (pinned, J19).** Whenever the scaffold writes a
config file it is exactly the file `load_config` subsequently requires; when the
target is empty the scaffold writes nothing and the loader requires the user's
explicit path (and errors if it is absent). Tested by J-T38/J-T39.

**Interaction with the required global layer (pinned, L5).** Scaffolding is
best-effort, but the global layer is required (21-D12). If the conventional file
cannot be created (J-F12), the load that follows in the same run fails with
J-F5 (`required global config not found`) after the scaffold warning. A
pre-existing `config.jsonc` that is not a regular file is **not** overwritten
(`write_default_config` sees `exists`) and is then rejected by J-F16. Both are
hard failures, never silent defaults.

### 8.2 The default JSONC text (pinned, 21-D7)

It must be **valid JSONC** (comments allowed, **no trailing commas**) and must
load to exactly the built-in defaults — mirroring the invariant the current TOML
scaffold satisfies (`tests/unit/config_test.cpp:228-242`).

```jsonc
// ymh configuration — created on first run.
//
// Layering (last writer wins):
//   built-in defaults -> this global file -> <workspace>/.ymh/config.jsonc
//   -> YMH_* environment variables -> command-line flags.
//
// Format: JSON with // and /* */ comments (JSONC). Trailing commas are NOT
// allowed. The loader is strict: an unknown key is an error, so add only real
// keys. Secrets are never stored here; "api_key_env" names the environment
// variable that holds the key.

{
  "ui": {
    "theme": "default",          // color theme name
    "show_activity": true,       // show the activity indicator
    "side_panel": "auto"         // "auto" | "always" | "never"
  },
  "agent": {
    "model": "",                 // empty => use llm.default.model
    "max_steps": 100             // max tool-calling steps per task
    // optional: "reasoning_effort": "low"   // "low" | "medium" | "high"
    // optional: "system_prompt": ""         // empty => built-in default
  },
  "workspace": {
    "root": "."                  // workspace root (relative to the process cwd)
    // optional: "workspace_roots": []       // discovery roots for workspace selection
  },
  "permissions": {
    "shell": "ask",              // "allow" | "ask" | "deny"
    "write": "ask",
    "read": "allow"
  },
  "logging": {
    "level": "info",             // "debug" | "info" | "warn" | "error"
    "log_prompts": false         // never enable implicitly; prompt bodies are redacted
  },
  "llm": {
    "default": {
      "provider": "openai-compatible",
      "base_url": "https://api.deepseek.com/v1",
      "model": "deepseek-flash",
      "api_key_env": "DEEPSEEK_API_KEY",
      // optional: "reasoning_effort": "low"   // "low" | "medium" | "high"
      "max_concurrency": 4,
      "connect_timeout_ms": 10000,
      "idle_timeout_ms": 60000,
      "request_timeout_ms": 120000,
      "retry": {
        "max_attempts": 3,
        "base_delay_ms": 500,
        "max_delay_ms": 30000,
        "jitter": 0.25,
        "honor_retry_after": true
      }
    }
  }
}
```

### 8.3 Comma-safety rules for the scaffold (pinned)

Because trailing commas are rejected (J2), the scaffold obeys:

1. Every commented-out optional key is placed **after** the last live key of its
   object, with **no comma** on that last live key (e.g. `"max_steps": 100`
   followed by comment lines then `}`). (J15)
2. No live key is followed only by commented keys with a comma left behind.
3. The text contains no `#` comments (only `//`).
4. The text is verified by a unit test that writes it, loads it, and asserts the
   result equals built-in defaults after env overrides (§12, J-T7).

### 8.4 Scaffolding is idempotent and non-destructive

`write_default_config` (`src/config/config.cpp`) checks `exists` before writing
and uses `std::ios::trunc` only on a file it just found absent. Retained: the
conventional `config.jsonc` is never overwritten. With an explicit `--config
<path>` the global file write is skipped entirely by the `scaffold_config`
empty-target branch (J-C12, §8.1, 21-D13), so the user's explicit path is
**never created** (and never overwritten); the `<root>/.ymh/` directory step
still runs and `ScaffoldResult::ok` stays `true`. A race between two first runs
writes identical bytes (J16, J-F11).

---

## 9. Invariants

| ID | Invariant | Where |
|---|---|---|
| **J1** | **Comments are `//` and `/* */` only.** `#` is never a comment. | §4.1, §4.3 |
| **J2** | **No trailing commas.** A comma before `}`/`]` is a syntax error; the error is loud and names the line. | §4.1, §4.4 |
| **J3** | **One format, one basename.** `config.jsonc` is the sole config file for both the global and workspace layers; TOML is retired. | §5.1-§5.3 |
| **J4** | **Path resolution is unchanged.** `$XDG_CONFIG_HOME` → `$HOME/.config` → `./.config`; the workspace file is `<root>/.ymh/config.jsonc`. | §5.2 |
| **J5** | **No TOML awareness.** No code path stats, opens, parses, mentions, or warns about `config.toml`. A sibling `.toml` is invisible and contributes nothing. | §6, 21-D11 |
| **J6** | **Global layer required; workspace layer optional.** `load_config` throws `ConfigError` when `paths.global` is empty or does not exist; a missing `paths.workspace` contributes nothing. | §5.4, §6, 21-D12 |
| **J7** | **Strictness parity.** Unknown keys and wrong types are `ConfigError`, exactly as the loader did before the format switch. | §7.10 |
| **J8** | **Malformed JSONC fails loud.** Parse errors abort the run with a `ConfigError` naming file and line. | §7.10 |
| **J9** | **The document root is an object.** A non-object top level is `ConfigError`. | §7.1 |
| **J10** | **Layering is unchanged.** defaults → global → workspace → env → CLI, last writer wins per key. A **non-empty** array replaces wholesale. An **empty `workspace_roots`** is a **no-op** and does not clear an earlier layer. For per-server `args`/`env`/`header_env`/`allowed_tools`/`denied_tools`, an empty array is equivalent to **absent** (the whole `mcp.server` list is cleared and each server rebuilt fresh, so nothing from an earlier layer survives). The `mcp.server` table-array is cleared/replaced whenever the key is present, including `[]`. | §3.4, §3.3, §7.4, §7.8 |
| **J11** | **No dual-read, no precedence rule.** No code path prefers TOML over JSONC or vice versa; there is no TOML path at all. | §6, 21-D11 |
| **J12** | **Schema is byte-for-byte equivalent to today's key set.** No key is renamed, added, or removed; the `llm` flat/nested quirk is preserved. | §7 |
| **J13** | **Integer keys are non-negative.** A negative count/size/duration is `ConfigError`. | §7.9, 21-D6 |
| **J14** | **Duplicate keys are last-wins and documented.** No silent divergence is left unstated. | §7.10 |
| **J15** | **Scaffold output is valid JSONC and loads to built-in defaults.** No trailing comma, no `#`. | §8.2-§8.3 |
| **J16** | **Scaffolding never overwrites.** An existing `config.jsonc` is untouched; concurrent first runs write identical bytes. | §8.4 |
| **J17** | **An explicit `--config` is never scaffolded.** Scaffolding writes only the conventional global path; with `--config <path>` it writes no global file (and still ensures `<root>/.ymh/`). | §8.1, 21-D13 |
| **J18** | **Top-level unknown-key message has no leading dot.** `unknown key 'auto_compact_enabled'` at top level; `unknown key 'agent.compaction.foo'` nested. | §7.1, §7.10, 21-D14 |
| **J19** | **Scaffold target == loader path.** When the scaffold writes a config file it is exactly the file `load_config` requires; when the target is empty the loader requires the user's explicit path. | §8.1, 21-D13 |
| **J20** | **Daemon config fidelity (when supplied).** When the supervisor supplies a path (Tui/`run` always do), the daemon loads exactly that file and never re-resolves `default_global_config_path()`. The defaulted-empty path (direct `HostConfig{}`/test construction, or a hand-invoked `ymh --host` without `--config`) keeps the pre-Rev-6 self-resolution behaviour. | §5.5, 21-D15 |
| **J21** | **A present config path must be a regular file.** A directory/FIFO/socket/device at `paths.global` is a `ConfigError`; a non-regular file at `paths.workspace` is also a `ConfigError` (absence alone is the only lenient case). | §5.4, §10 J-F16, 21-D16 |
| **J22** | **The config-load (and config-derived logging) set equals the scaffold set.** `Workspace`/`Config`/`Version` load nothing and use default logging; `Tui`/`Run`/`List`/`Show`/`Replay`/`Fork` load (only `Tui`/`Run` consume `Config`; the rest use config-derived logging and keep the set equal to the scaffold set). | §6.3, 21-D17 |

---

## 10. Failure modes (`J-F1`–`J-F16`)

Convention per `00-architecture.md:4832-4856` (§54 F1–F12 reference). Each mode
names detection and handling.

| ID | Failure | Detection | Handling |
|---|---|---|---|
| **J-F1** | Malformed JSONC (syntax error, including trailing comma J2, `#` comment J1, and non-JSON whitespace such as `\f`/`\v`). **Excludes** an empty, comments-only, or leading-BOM-only file (no JSON token after an optional leading BOM) and a literal `{}`, all of which are valid no-ops (§4.2, J-L2). | `nlohmann::json::parse_error` caught in `parse_jsonc` | `ConfigError` with file + line; run aborts (exit 2 via `run_cli`'s `ConfigError` catch, `src/cli/cli.cpp` ~:672-677; the `--host` daemon has its own catch at ~:236-238); no partial config is applied. |
| **J-F2** | Unknown key (typo) | `reject_unknown` JSON equivalent | `ConfigError` naming the qualified key; run aborts. Parity with `config.cpp:27-45`. |
| **J-F3** | Wrong value type (e.g. string where bool expected) | `is_boolean()` / `is_string()` / `is_number()` / `is_array()` checks | `ConfigError`; run aborts. Parity with `config.cpp:47-99`. |
| **J-F4** | Unreadable file (exists but `open` fails: permissions, EACCES) | `std::ifstream` fails to open | `ConfigError "cannot open <path>"`; run aborts. NOTE: an *absent* **workspace** file is a no-op (optional layer); an absent **global** file is J-F5, not a no-op (21-D12). |
| **J-F5** | **Required global config absent or empty.** `paths.global` is empty, or names a file that does not exist | `apply_jsonc_file(..., required=true)` checks `path.empty()` then `std::filesystem::exists` | `ConfigError`; run aborts (exit 2 via `run_cli`'s `ConfigError` catch, `src/cli/cli.cpp`). Message: `config <path>: required global config not found` (missing) or `config: required global config path is empty` (empty). (21-D12, J6) |
| **J-F6** | **Explicit `--config <path>` names a missing file.** The user's own path is absent | same as J-F5 (the explicit path is `paths.global`) | Same hard error as J-F5; scaffolding must **not** create `<path>` (21-D13, J17); `<root>/.ymh/` is still ensured. (21-D12, 21-D13) |
| **J-F7** | Trailing comma in user config | `parse_error.101` (`unexpected '}'`) | `ConfigError` naming line; user-visible consequence pinned in §4.4. |
| **J-F8** | `#` comment (TOML habit) in JSONC | `parse_error.101` (invalid literal) | `ConfigError` naming the line. There is no migration text (21-D11). |
| **J-F9** | Top-level value is not an object (array/scalar) | `is_object()` check after parse | `ConfigError "top-level value must be an object"`. (J9) |
| **J-F10** | Negative integer | `read_int64` sign check (§4.2 sketch) | `ConfigError "value must be non-negative for '<key>'"`. New tightening (21-D6). |
| **J-F11** | Two processes scaffold concurrently | none (no lock) | Benign: both write byte-identical defaults; last writer wins; `write_default_config` never overwrites an existing file. (J16) |
| **J-F12** | Scaffold target unwritable / directory not creatable | `error_code` from `exists`/`create_directories`/`ofstream` | Best-effort: `scaffold_warn` to the logger, `ScaffoldResult::ok = false`, never throws. Parity with `config.cpp:548-595`. An **empty** target is not this failure: it is the deliberate J-C12 skip (no warning, `ok == true`). |
| **J-F13** | **A `config.toml` sibling exists.** | none — the path is never stat'ed or probed (21-D11) | **Non-event.** The file is invisible: no read, no parse, no warning, no rename, no delete. It contributes nothing. (21-D11, J5) |
| **J-F14** | **Optional workspace layer absent.** `paths.workspace` is empty or names a missing file. | `apply_jsonc_file(..., required=false)` returns without effect | **Not an error.** That layer contributes nothing; the global layer and env still apply. (21-D12, J6) |
| **J-F15** | **Daemon cannot load the supervisor's config.** The supplied `--config <path>` (21-D15) is absent or not a regular file, so the host process exits 2 before binding its socket. (An **empty** `config_path` is *not* this failure: the daemon then self-resolves the conventional path, which succeeds iff that file exists — J20 is scoped to the supplied case.) | `run_host_command`'s `ConfigError` catch (`src/cli/cli.cpp`, ~:236-238); the supervisor sees no ready claim | `HostLifecycle::spawnAndAttach` detects the dead daemon on its `!isAlive` check (`src/host/workspace_host.cpp`, ~:1086-1088), then spends up to ~5 s in the winner-attach window (~:1096-1108) before failing; the supervisor reports the attach failure. Prevented by 21-D15 + J20: the supervisor passes a path it already loaded successfully. (H1, J20) |
| **J-F16** | **Config path exists but is not a regular file.** A directory, FIFO, socket, or device sits at `paths.global` (required) or `paths.workspace` (optional). | `std::filesystem::is_regular_file` after `exists` in `apply_jsonc_file` | `ConfigError`; run aborts. Message: `config <path>: config path is not a regular file`. Closes the silent-defaults hole where a directory named `config.jsonc` opened as an empty `ifstream` and parsed to `{}` (M1, 21-D16, J21). |

---

## 11. dsh mapping

| dsh concept | `ymh` mapping | Note |
|---|---|---|
| Cordis plugin config (`cordis.yml`) | `Config` loaded from `config.jsonc` | Same layering role; JSONC replaces TOML as the on-disk encoding. |
| Schema validation / strict config | `reject_unknown` + type checks → `ConfigError` | dsh validates plugin options; `ymh` keeps a strict, fail-loud loader. |
| Hot reload of config | none (`00 §53 :4725` — unshipped experimental item) | Unchanged; a config change requires a daemon restart (`15 §5.6:1302`). |
| Secrets via environment | `llm.api_key_env` names the env var; `mcp.server.env`/`header_env` are `${VAR}` references | Unchanged (`config.hpp:14-16`, `08 §6.4`). |
| Layered composition (global → project) | global `config.jsonc` → `<root>/.ymh/config.jsonc` → env → CLI | Unchanged order (J10). |

The format choice is an encoding detail, not an architectural change: no dsh
seam moves. This is why the errata is additive and touches only the config
component plus doc references.

---

## 12. Test plan

### 12.1 Unit tests (extend `tests/unit/config_test.cpp`)

| ID | Test | Concrete assertion |
|---|---|---|
| J-T1 | `JsoncCommentsAccepted` | Write `config.jsonc` containing `//` and `/* */`; `load_config` returns the configured value. |
| J-T2 | `JsoncTrailingCommaRejected` | Write `{ "agent": { "max_steps": 5, } }`; `EXPECT_THROW(load_config, ConfigError)`. |
| J-T3 | `JsoncHashCommentRejected` | Write `{ "agent": { "max_steps": 5 # five } }`; `EXPECT_THROW(load_config, ConfigError)`. |
| J-T4 | `JsoncUnknownKeyRejected` | Write `{ "agent": { "modle": "typo" } }`; `EXPECT_THROW(load_config, ConfigError)`. Mirrors `config_test.cpp:153-160`. |
| J-T5 | `JsoncWrongTypeRejected` | Write `{ "agent": { "max_steps": "many" } }`; `EXPECT_THROW(load_config, ConfigError)`. |
| J-T6 | `JsoncNegativeIntegerRejected` | Write `{ "agent": { "max_steps": -1 } }`; `EXPECT_THROW(load_config, ConfigError)`. (J13) |
| J-T7 | `ScaffoldOutputReparsesToDefaults` | Call `scaffold_config`, then `load_config`; assert `same_config(loaded, expected)` after `apply_env_overrides`. Mirrors `config_test.cpp:228-242`. **Also** assert the scaffold contains no `#` and that parsing it with `ignore_comments=true` succeeds. (J15) |
| J-T8 | `ScaffoldWritesJsoncName` | `scaffold_config(ws, ws/"xdg"/"ymh"/"config.jsonc")`; assert the file exists and `default_global_config_path()` ends in `config.jsonc`. |
| ~~J-T9~~–~~J-T12~~ | **RETIRED (Rev 5).** `LegacyTomlWarnsOnceAndIgnores`, `LegacyWarningOncePerCall`, `BothFilesPrefersJsoncAndWarns`, `LegacyOnlyContinuesOnDefaults` are deleted with the legacy machinery (21-D11). |
| J-T34 | `TomlSiblingIsInvisible` | Valid global; valid `.ymh/config.jsonc` (`max_steps = 3`) **and** `.ymh/config.toml` (`[agent] max_steps = 7`) present; assert `max_steps == 3u` — the `.toml` had no effect (21-D11, J5, J-F13). Behaviour-only: Rev 5 removed `load_config`'s `Logger*`, so "no warning" is **structural** (no loader channel exists), not asserted. |
| J-T35 | `TomlOnlyWorkspaceContributesNothing` | Only `.ymh/config.toml` exists (global valid); assert no `ConfigError` and the workspace layer contributes nothing (21-D12, J-F14). Same structural note: no warning channel exists. |
| J-T36 | `MissingGlobalConfigThrows` | `ConfigPaths{}` (global empty) → `EXPECT_THROW(load_config(paths), ConfigError)`; message is `config: required global config path is empty` (21-D12, J-F5). |
| J-T37 | `MissingGlobalFileThrows` | `paths.global = <dir>/absent.jsonc` → `EXPECT_THROW`; message is `config <path>: required global config not found` (21-D12, J-F5). |
| J-T38 | `ExplicitConfigMissingIsNotCreated` | CLI: `--config <dir>/missing.jsonc` absent → non-zero exit, stderr `required global config not found`, `<dir>/missing.jsonc` **not created**, `<root>/.ymh/` created (21-D12/21-D13, J17, J19). |
| J-T39 | `ConventionalMissingIsScaffoldedThenLoads` | CLI: no `--config`, empty `XDG_CONFIG_HOME`/`HOME` temp dir → `default_global_config_path()` created, valid JSONC, loads to defaults; `<root>/.ymh/` created (21-D13, J19). |
| J-T40 | `TopLevelUnknownKeyMessage` | `{ "auto_compact_enabled": true }` → `ConfigError` whose `what()` contains `unknown key 'auto_compact_enabled'` and **not** `'.auto_compact_enabled` (21-D14, J18). |
| J-T41 | `NestedUnknownKeyMessage` | `{ "agent": { "compaction": { "foo": 1 } } }` → `what()` contains `unknown key 'agent.compaction.foo'` (21-D14, J18). |
| J-T42 | `WorkspaceLayerOptional` | Valid global; `paths.workspace = <dir>/absent.jsonc`; load succeeds with global values (21-D12, J-F14). |
| J-T13 | `TopLevelNonObjectRejected` | Write `[1,2,3]`; `EXPECT_THROW(load_config, ConfigError)`. (J9) |
| J-T25 | `EmptyAndCommentsOnlyAreNoOp` | Write `""`, `"   \n"`, `"// only\n"`, and `"/* only */"` as `config.jsonc` in turn; assert each `load_config` returns built-in defaults (plus env) and does **not** throw. Mirrors the retired TOML loader's empty-file no-op. (J-L2, §4.2) |
| J-T30 | `NonJsonWhitespaceRejected` | Write a file whose only content is `"\f"` (0x0C), and separately `"\v"` (0x0B); `EXPECT_THROW(load_config, ConfigError)` — these are **not** RFC 8259 whitespace, so they are a parse error, never a silent no-op. (V3-8, §4.2) |
| J-T31 | `EmptyObjectIsValidNoOp` | Write `"{}"` and separately `"{ /* c */ }"`; assert `load_config` returns built-in defaults (plus env) and does not throw — a literal empty object is a valid no-op, not a `ConfigError`. (V3-8, §4.2) |
| J-T32 | `LeadingBomBoundary` | (a) Write `"\xEF\xBB\xBF"` (BOM only), separately `"\xEF\xBB\xBF  \n"` and `"\xEF\xBB\xBF// c\n"`; assert each `load_config` returns built-in defaults (plus env) and does not throw — a leading BOM is not a JSON token, so these are blank no-ops (V4-3, §4.2). (b) Write `"\xEF\xBB\xBF{}"`; assert it is a no-op (nlohmann skips the BOM). (c) Write `"  \xEF\xBB\xBF{}"` (BOM after whitespace) and `"\xEF\xBB\xBF\xEF\xBB\xBF{}"` (two BOMs); `EXPECT_THROW(load_config, ConfigError)` — a BOM is recognised only at byte 0. |
| J-T14 | `JsoncLlmNestedAndFlatParity` | Write nested `llm.default` in one file and flat `llm` in another; assert both yield the same `config.llm`. (J12) |
| J-T15 | `JsoncMcpServerArrayParses` | Write `mcp.server` as a JSON array with two entries; assert `config.mcp.servers.size() == 2u` and fields map. Mirrors `mcp_manager_test.cpp:420-448`. |
| J-T16 | `WorkspaceOverridesGlobalJsonc` | Global sets `max_steps = 5`; workspace sets `model`; assert per-key last-wins. Mirrors `config_test.cpp:111-133`. |
| J-T21 | `EmptyWorkspaceRootsDoesNotOverride` | Global `config.jsonc` sets `"workspace_roots": ["a"]`; workspace `config.jsonc` sets `"workspace_roots": []`; assert `config.workspace.workspace_roots == {"a"}` (the empty array is a no-op, not a clear). Mirrors the real guard at `config.cpp:186-189`. (J10) |
| J-T22 | `EmptyMcpServerArrayClears` | Global sets `mcp.server` with two entries; workspace sets `"server": []`; assert `config.mcp.servers.empty()` (the table-array key present clears and rebuilds; `apply_mcp`, `config.cpp:382-397`). (J10) |
| J-T26 | `PerServerEmptyArrayEqualsAbsent` | Global `mcp.server` = `[{ "id": "s", "args": ["--x"], "env": ["A=1"] }]`; workspace `mcp.server` = `[{ "id": "s", "args": [], "env": [] }]`; assert `config.mcp.servers.size() == 1u` and `servers[0].args.empty() && servers[0].env.empty()` — the workspace layer clears and rebuilds the list, so an empty per-server array yields the struct default, exactly as if the key were omitted (empty ≡ absent; contrast J-T21). (`apply_mcp`, `config.cpp:382-397`; `apply_mcp_server`, `:311-330`.) (J10, §3.3/§7.8) |
| J-T44 | `DaemonArgvCarriesConfig` | `ForkExecLauncher::build_argv` with `HostConfig::config_path = "/x/my.jsonc"` emits `--config /x/my.jsonc`; with an empty `config_path` it emits no `--config`. (21-D15, J20) |
| J-T45 | `TuiWithExplicitConfigStartsDaemon` | Integration: run Tui/`run` with `--config <path>` where `<path>` exists and the conventional global file does **not**; assert the supervisor loads `<path>` and `spawnAndAttach` succeeds. The daemon argv is observed by reading `/proc/<pid>/cmdline` for the spawned host pid (the repo already parses this NUL-separated blob via the `host_processes`/`is_ymh_host_process` helpers, `tests/support/pty_child.hpp`); assert it contains `--config <path>`. (H1, 21-D15, J20, J-F15) |
| J-T46 | `GlobalConfigDirectoryRejected` | `paths.global` is a directory → `EXPECT_THROW(load_config(paths), ConfigError)`; message `config <path>: config path is not a regular file`. (M1, 21-D16, J21, J-F16) |
| J-T47 | `WorkspaceNonRegularRejected` | Valid global; `paths.workspace` is a directory → `EXPECT_THROW` with the same message. Absence stays lenient (J-T42). (21-D16, J21, J-F16) |
| J-T48 | `WorkspaceCommandNeedsNoConfig` | CLI: `ymh workspace list` with `XDG_CONFIG_HOME`/`HOME` pointed at a **dedicated empty dir independent of the shared fixture** — do **not** reuse `ShortTempRoot`, which the M4 fix may make pre-create a global config — so the test still distinguishes gated from ungated loading → exit 0, no `ConfigError`. (M5, 21-D17, J22) |
| J-T49 | `ScaffoldFailureThenRequiredError` | Conventional global dir unwritable (J-F12): a run scaffolds (warns) and then throws `config <path>: required global config not found`; never silently defaults. (L5, 21-D12, J-F5) |

### 12.2 Integration / CLI tests

**Current tree (post-RB-09).** The TOML→JSONC fixture migration already landed:
every fixture now writes `.ymh/config.jsonc` (`tests/integration_two_process_test.cpp`,
`tests/integration_ownership_two_supervisor_test.cpp`,
`tests/integration_live_e2e_test.cpp`, `tests/unit/ownership_crash_injection_test.cpp`,
`tests/unit/mcp_manager_test.cpp`, `tests/unit/ui_supervisor_pty_test.cpp`). The
Rev-5 change adds the required-global rule and the explicit-`--config`
no-scaffold rule, so fixtures and CLI tests must be adjusted as follows.

**Fixtures that construct `ConfigPaths` with an empty `global`** (all in
`tests/unit/config_test.cpp`, 27 sites; `tests/unit/mcp_manager_test.cpp` uses
`ConfigPaths{{}, config_path}`) must now supply an existing global path (or wrap
the call in `EXPECT_THROW` where the test's purpose is the missing-global case).
The same applies to `load_config(workspace_root)` callers: the conventional
global file must exist (scaffold it, or point `XDG_CONFIG_HOME` at a temp dir
with a written `config.jsonc`).

**Delete the legacy CLI tests:**

- `Cli.LegacyTomlWarningOnStderr` (`tests/unit/cli_test.cpp`) — the warning no
  longer exists.
- `Cli.ConfigPathLegacyNote` (`tests/unit/cli_test.cpp`) — the note is deleted.

**Update the explicit-`--config` CLI tests:**

- `Cli.RunListEmptyWorkspace` currently passes `--config <ws>/missing.toml` and
  expects exit `0`. Under 21-D12 a missing explicit `--config` is a hard error;
  the test must either write the file first or expect a non-zero exit and the
  `required global config not found` message.
- `Cli.ConfigPathPrintsEffectivePath` uses `--config /tmp/ymh-test-config.toml`
  with `config path` (no load). It stays valid — `ymh config path` never loads —
  but the filename is misleading; rename to `.jsonc` for clarity.

**Retained/rewritten CLI test:**

- **J-T28 `ScaffoldTargetMatchesLoader`** — (a) no `--config`: the scaffold
  writes `default_global_config_path()` and a subsequent `load_config` reads it;
  (b) `--config <dir>/other.jsonc` (absent): assert the scaffold creates **no**
  `<dir>/other.jsonc` and the load fails with the required-global error;
  (c) `--config <dir>/other.jsonc` (present): assert the loader reads it and the
  scaffold does not modify it. Replaces the old legacy-named branch.
  (21-D13, J17, J19)

**Add CLI tests J-T38/J-T39** (defined in §12.1).

**Host/daemon fixtures need a global config (M4; residual fixed in Rev 7).**
Two fixture families spawn daemons, and **both** must provide a global config.
The fix belongs in the shared fixture that owns the per-test `XDG_CONFIG_HOME`
(`configure_workspace` and/or `ShortTempRoot`), so it is ordering-independent —
**do not** rely on "a harness ran first".

- `tests/support/host_harness.hpp` (~:93-96) sets `XDG_CONFIG_HOME` to
  `<root>/.config` and writes no global config, so every harness-started daemon
  fails J-F5. Fix: write a valid `<root>/.config/ymh/config.jsonc`, or set the
  existing `HostHarnessOptions::config_path` field (already forwarded to
  `--config` by the harness's `build_argv`, ~:213-215); otherwise
  `wait_ready()` times out.
- `tests/integration_two_process_test.cpp` `configure_workspace` (~:503-508)
  sets `XDG_CONFIG_HOME` and writes no global config, yet **six**
  `HostLifecycle`-spawned daemons run through it (`HostLifecycle lifecycle(launcher,
  *registry)` at ~:590, ~:794, ~:865, ~:994, ~:1335, ~:1368). Only some are
  preceded by a `HostHarness`; `TwoSpawnsExactlyOneDaemon` (~:779, spawn at
  ~:794) starts **no** harness and would fail/crash. The shared fixture must
  write `<root>/.config/ymh/config.jsonc` so **every** `HostLifecycle`-spawned
  daemon finds a global config regardless of test ordering.
- `tests/integration_two_process_test.cpp` direct `--host` spawn (~:826-834) —
  pass `--config <existing>` or point `XDG_CONFIG_HOME` at a dir containing
  `config.jsonc`; otherwise the host exits 2.
- `tests/integration_host_harness_test.cpp` (~:279-291) — the harness start +
  `run_cli` path needs the global config present.
- `tests/unit/ui_supervisor_pty_test.cpp` — **no fixture change needed.** This
  test drives the Tui binary, which self-scaffolds the conventional global config
  on first run, so the daemon finds it without a fixture write. Do not add a
  redundant global-config write.

### 12.3 Parse-level unit tests for the helpers

| ID | Test | Assertion |
|---|---|---|
| J-T17 | `LineForByte` | `line_for_byte("{}\n{}", 4) == 2`; offset at EOF clamps. |
| J-T18 | `ReadStringArrayRejectsNonString` | `{ "workspace": { "workspace_roots": ["a", 1] } }` throws `ConfigError`. |
| J-T19 | `ReadSizeRejectsUint64Overflow` | `18446744073709551615` (> `int64` max) throws `ConfigError`. |
| J-T43 | `ApplyJsoncRequiredFlag` | `apply_jsonc_file(config, "", true)` and `apply_jsonc_file(config, <absent>, true)` throw `ConfigError`; the same with `required=false` are no-ops. Pins 21-D12. |
| ~~J-T20~~, ~~J-T23~~, ~~J-T24~~, ~~J-T29~~, ~~J-T33~~ | **RETIRED (Rev 5).** The `legacy_config_path`/`jsonc_target`/`scaffold_target` unit tests and the legacy-warning-target tests are deleted with the helpers (21-D11). |

### 12.4 Not tested (explicit non-goals)

- Trailing-comma tolerance (not implemented; §4.4).
- Duplicate-key rejection (not implemented; §7.10).
- Range validation of ratios/jitters (not implemented; §14.1).
- TOML detection, sibling probing, migration warning, substitution, and conversion (all deleted; 21-D11).

---

## 13. Rejected alternatives (consolidated)

1. **Keep TOML, add JSONC (dual-read).** Forbidden by the user decision;
   doubles the strictness surface and needs a precedence rule.
2. **Accept `.jsonc` alongside `.toml` with TOML preferred.** Same as (1);
   contradicts "no precedence rule".
3. **Third-party JSONC/JSON5 parser.** New dependency for no capability
   nlohmann lacks except trailing commas; violates the no-umbrella-dependency
   rule.
4. **Hand-written parser.** High risk, no benefit.
5. **Trailing-comma pre-pass (naive).** Corrupts commas inside strings; unsafe.
6. **Trailing-comma pre-pass (correct mini-lexer).** Correct but is real parser
   code; deferred to a separate gated follow-up (§4.4).
7. **Once-ever warning with a persistent marker file.** *(Retired — vacuous.)*
   There is no warning at all under 21-D11.
8. **Automatic TOML→JSONC conversion.** Rewrites user files; a converter bug is
   silent data loss. **Rejected and now moot:** with no TOML awareness there is
   nothing to convert (21-D11).
9. **Workspace-level config stays TOML.** Two formats and two parsers; rejected
   (J3).
10. **Warn-and-default for unknown keys (lenient JSONC).** Contradicts the
    project's explicit strictness policy (`config.hpp:13-15`); rejected (J7).
11. **`nlohmann::ordered_json`.** Irrelevant ordering; needless divergence.
12. **Emit the legacy warning after `init_logging`.** *(Retired — vacuous; there
    is no warning under 21-D11.)*
13. **Keep a TOML sibling advisory "just in case".** Rejected: the user
    explicitly wants the advisory gone; a sibling `.toml` is invisible (21-D11).
14. **Silently default when the global config is missing.** Rejected: the user
    expects a plain error when the config they expect is absent; the global layer
    is required (21-D12).
15. **Auto-create an explicit `--config <path>` when missing.** Rejected: it
    would fabricate a file at a user-named path and mask a typo; a missing
    explicit path is a hard error (21-D12, 21-D13).
16. **TOML fallback / dual-read for a `.toml` sibling.** Forbidden in every form;
    no detection, no precedence, no conversion (21-D11).
17. **Emit the top-level unknown-key message with a leading dot** (`'.key'`).
    Rejected: it is a display defect; the message is built without the leading
    dot (21-D14).

---

## 14. Open items and unverified claims

### 14.1 Open items (deferred, each needs its own gate)

**Exception:** OQ-6 below is **not deferred** — it is a mandatory prerequisite of
the code-landing change (J-C11). Everything else in this subsection is deferred.

- **OQ-1 — duplicate-key rejection.** nlohmann silently last-wins. Detecting
  duplicates needs a SAX parse or a custom callback; decide whether strictness
  policy requires it. (§7.10)
- **OQ-2 — trailing-comma tolerance.** A correct string-aware stripper could be
  added later; §4.4 pins the approach and its tests.
- **OQ-3 — range validation.** `threshold_ratio` ∈ [0,1], `jitter` ∈ [0,1],
  `reconnect_jitter` ∈ [0,1], and enum checks for `side_panel` / `theme` /
  `logging.level` are absent today and remain absent. This errata deliberately
  does not add them.
- **OQ-4 — `llm` flat-vs-nested quirk.** Flat keys are ignored when `"default"`
  is present (`config.cpp:237-249`); preserved for parity. A follow-up may
  reject the mix or merge it.
- **OQ-5 — automatic converter.** *Withdrawn (Rev 5, 21-D11).* With no TOML
  awareness there is nothing to detect or convert.
- **OQ-6 — docs (PREREQUISITE, not deferred; still outstanding at Rev 5).**
  `README.md:28` still lists `toml++` and `README.md:88-147` still documents TOML
  (path block `:90-91`, `toml` examples `:99-117` and `:142-147`).
  `AGENTS.md:65,113` still list `toml++`. The code-landing change **must** update
  these in the same commit (J-C11); shipping JSONC while the README documents TOML
  leaves the documented format wrong. This errata does not edit them (design-only).
  The same commit must also sweep the non-README stale references in §1.3.2 —
  `00-architecture.md:1598,3941,3945-3968,4065,4438,4486`;
  `08-llm-provider.md:695,701`; `09-permissions.md:361-383`;
  `13-context-compaction.md:304`; `15-mcp-adapter.md:509,1236-1237,1242`;
  `20-skills.md:305,361,492-496,708,2031,2147`;
  `REQUIREMENTS_BACKLOG.md:252-267`. The `permissions.local.toml` grants-file
  references are explicitly out of scope (§1.3.2).

### 14.2 Unverified claims (explicit)

- **[WITHDRAWN, Rev 5] toml++ duplicate-key behaviour.** toml++ is gone (J-C10);
  the claim is irrelevant.
- **[WITHDRAWN, Rev 5] toml++ handling of the existing `config.toml` today.**
  Irrelevant: ymh never reads `config.toml` (21-D11).
- **[UNVERIFIED] nlohmann behaviour under `NDEBUG` / other build types.** The
  probes were compiled with `-std=c++23` (no `-DNDEBUG`). `ignore_comments` and
  trailing-comma rejection are parser features, not assertions, so build type is
  not expected to matter.
- **[UNVERIFIED] `parse_error.byte` line mapping for multi-byte UTF-8.** The
  line computation counts `'\n'` bytes, which is correct for UTF-8 (newline is
  single-byte); no probe with multi-byte characters was run.
- **All `file:line` references** were re-read against the working tree at
  authoring time; they will drift as code changes.

### 14.3 Citation audit (2026-09-16)

Every `file:line` in this errata was re-read against the working tree on
2026-09-16 (after unrelated `/export` work shifted `src/ui/supervisor.cpp`; none
of this errata's anchors live in that file). Round-3 fixes (§15 Rev 3) were
re-derived the same day against the post-RB-03/RB-05 tree; the round-3 bullet
below lists them. Corrections and confirmations:

- **Corrected (J-H1).** The `20-skills.md` config-filename quote is at
  `20-skills.md:360-361`, not `:326`. Verified text at `:361`:
  "`$XDG_CONFIG_HOME/ymh/config.toml` outside the workspace" (the sentence
  begins on `:360`). §3.3 of that spec starts at `:345`. The old `:326` was in
  §3.2 and does not contain the string.
- **Corrected (J-H2).** "No hot reload" is **not** §54 D11 —
  `00-architecture.md:4780-4782` reads "Do not make dynamic plugins a
  prerequisite for modularity." The correct anchor is `00-architecture.md:4725`
  under §53 "Phase 4 — Experimental" (heading `:4719`), which lists "hot reload"
  as a not-yet-pursued item. The previously cited `:4830` is D23 ("A workspace
  daemon survives TUI detach"). Note: `15-mcp-adapter.md:1302` repeats the same
  stale `§54 D11` parenthetical; that is an upstream doc defect this errata
  cannot edit, and only the substantive statement there ("Config changes require
  a daemon restart") is relied on.
- **Confirmed unchanged (docs and tests).** `00-architecture.md:3941` ("Use
  TOML."), `:3945-3968` (TOML example), `:4832-4856` (F1–F12 reference);
  `08-llm-provider.md:695,701-717,706,719-720,721-722`;
  `15-mcp-adapter.md:1236-1237,1242-1274,1296-1301,1302`;
  `16-daemon-ownership.md:160`; `REQUIREMENTS_BACKLOG.md:250-254,275-276`;
  `src/cli/wiring.cpp:54-69,71-97,99-153,155-165,167-180,182-203` (all six
  `to_*` `Config` readers — the round-3 range was truncated at `167-175` and
  omitted `to_skill_catalog_config`/`to_compaction_policy`; corrected in V4-4);
  `:10-22` (`parse_verdict`), `:92`, `:120-123`;
  `src/ui/ui_application.cpp:475,493`; `src/core/logging.*`; and the `tests/*`
  ranges in §12.2. `src/config/config.cpp` anchors at or below `:397` are
  unchanged; those above were corrected (next bullet).
- **Additional stale TOML strings found (not defects in this errata).**
  `20-skills.md:305` and `:708` also name `.config/ymh/config.toml`; recorded in
  §1.3.2. The full sweep added by J-R2-3 — `00-architecture.md:1598,4065,4438,4486`,
  `AGENTS.md:65,113`, `13-context-compaction.md:304`, `15-mcp-adapter.md:509`,
  `20-skills.md:492-496` — is tabulated in §1.3.2 and folded into J-C11.
  `README.md` is covered by OQ-6 / J-C11.
- **Behaviour re-checked, not just line numbers.** `config.cpp:186-189` (empty
  `workspace_roots` is a no-op) and `:311-330` / `:382-397` (mcp array
  semantics) were read, not inferred (J-M1). The old §6.7 target expression was
  evaluated by hand and found to print `toml -> toml` for the workspace slot;
  replaced with the per-slot `jsonc_target` (J-M3).
- **New probe (J-L2).** nlohmann 3.12.0 rejects `""`, whitespace-only, and
  comments-only input with `parse_error.101`; the loader now short-circuits these
  to `{}` to preserve TOML parity.
- **Round-2 re-derivation (2026-09-16) — citation convention.** Two unrelated
  features landed while this errata was being revised: RB-03 (`162fa8641`,
  session auto-name) and RB-05 (`6761e62ef`, the `/skills` subsystem). RB-05
  added the `skills` config section to `src/config/config.cpp` (the ~25 lines
  after `apply_mcp`), shifted every `config.cpp` anchor above it, and grew
  `config.hpp` and `CMakeLists.txt`. Every `file:line` in this errata was
  therefore re-derived against the current working tree. **Convention adopted:**
  name the source symbol (`apply_mcp`, `run_config_command`,
  `scaffold_for_invocation`, …) and give `file:line` only as a secondary hint —
  a symbol anchor cannot go stale the way a bare line number can. Anchors at or
  below `config.cpp:397` were unaffected; anchors above it were corrected
  (`kDefaultConfigToml` `460-512` → `488-540`, `scaffold_config` `593-617` →
  `621-645`, `apply_toml_file` `619-640` → `647-668`, `load_config` `691-704` →
  `742-755`, `effective_model` `706-711` → `757-762`, `CMakeLists.txt` toml++
  link `407-411` → `435-439`). The schema now documents the `skills` section
  (§7.11), which RB-05 added after the errata was first drafted, so J12 ("no key
  added or removed") stays true.
- **Resolved (J-R2-1).** `src/config/config.cpp:382-397` (`apply_mcp`) was read:
  `mcp.servers.clear()` runs whenever `mcp.server` is present and each entry is
  built as a fresh `McpServerSettings`, so a per-server empty array is *absent*,
  not a preserved earlier layer. The §3.3/§3.4/§7.4/§7.8/J10 claims and J-T22
  were corrected; J-T26 was added.
- **Resolved (J-R2-2).** `scaffold_for_invocation` (`src/cli/cli.cpp:105-122`)
  passes `effective_global_config` to `scaffold_config`; with `--config
  …/config.toml` that wrote a JSONC file the loader then ignored. Pinned as
  21-D10 (§8.1): the scaffold target is the effective global path, skipped when
  it is legacy-named; J-T28 added.
- **Corrected (J-R2-6).** `scaffold_for_invocation` begins at
  `src/cli/cli.cpp:105` in the current tree (it has begun there since
  `e6ce1e8676`, 2026-09-15; the round-2 report's `:104` does not match the tree);
  it is now cited by symbol.
- **Round-3 re-derivation (2026-09-16) — verification findings V3-1…V3-8.**
  Re-read against the current tree after RB-03 (`162fa8641`) and RB-05
  (`6761e62ef`). **V3-1**: the live `Config` struct is exactly `ui, agent,
  workspace, permissions, logging, llm, mcp, skills` (`config.hpp:162-171`); the
  §5.4 sketch had omitted `skills`, now added. No other field is missing — every
  field of all nine settings structs matches §3.3/§7 (checked field by field).
  **V3-2**: the sweep now includes `09-permissions.md:361-383`,
  `20-skills.md:2031`, and `REQUIREMENTS_BACKLOG.md:255-267`; the
  `permissions.local.toml` grants-file references are declared out of scope
  (§1.3.2) because no `src/`/`include/` code references that file — it is a
  spec-09 artifact, not a `ConfigPaths` slot. **V3-3**: §3.6 now lists all six
  `to_*` `Config` readers; the two it lacked were `to_skill_catalog_config`
  (`wiring.cpp:155-165`, RB-05) and `to_compaction_policy`
  (`wiring.cpp:182-203`). Ranges were re-derived and corrected
  (`to_provider_config` 54-69, `to_mcp_config` 99-153, `to_agent_config`
  167-180), and the adapter consumers in `workspace_runtime.cpp` /
  `workspace_host.cpp` are recorded. **V3-4**: §1.3.1a gives the faithful JSONC
  rendering of the superseded `00 §37` example instead of pointing at the §8
  scaffold. **V3-5**: `line_for_byte` is forward-declared before `parse_jsonc`.
  **V3-6**: `legacy_config_path` now returns empty for a custom basename, so
  `--config /x/other.{jsonc,toml}` never probes a `/x/config.toml` sibling;
  pinned by J-T20/J-T29. **V3-7**: §6.4 states the warning is per-process and the
  TUI-spawned daemon's copy goes to `<root>/.ymh/host.log`
  (`workspace_host.cpp:983-985`), not the terminal. **V3-8**:
  `blank_or_comments_only` accepts only RFC 8259 whitespace (space/tab/LF/CR),
  and §4.2 states a literal `{}` is a valid no-op; pinned by J-T30/J-T31.
- **Round-4 re-derivation (2026-09-16) — verification findings V4-1…V4-5 and a
  full defect-class sweep.** Every `file:line` in this errata was re-read against
  the working tree at HEAD `6761e62ef` (RB-05), including the ranges this round
  corrected. **V4-1**: the legacy warning text is now case-correct for both
  emitting paths — a conventional `config.jsonc` slot and an explicit
  legacy-named `--config …/config.toml` slot — with a per-line
  `(explicit --config slot: update --config)` marker and a closing sentence that
  no longer claims the sibling is loaded when it is not (§6.2/§6.3/§6.5/§6.7);
  §8.1 no longer claims the warning mentions `--config` for the sibling case, and
  §6.8's `ymh config path` note gains the repoint clause for the self-named slot.
  Pinned by the extended J-T27/J-T28. **V4-2**: the `scaffold_config`
  empty-target branch is pinned as J-C12 and in §8.1/§8.4, replacing the
  contradicting "exact control flow" claim; J-T28(c) now asserts `ok == true` and
  no scaffold warning. **V4-3**: `blank_or_comments_only` skips a single leading
  UTF-8 BOM, and §4.2 pins one outcome for every boundary input (empty,
  whitespace-only, comments-only, BOM-only, BOM+comments, `{}`, BOM after
  whitespace, double BOM, `\f`/`\v`, unterminated comment); probes were re-run
  against nlohmann 3.12.0 and J-T32 added. **V4-4**: `apply_toml_file`'s header
  anchor is `config.hpp:223` (was `:211`) and the §14.3 `wiring.cpp` reader ranges
  are re-derived to all six `to_*` functions. **V4-5**: the §7.9 object row now
  enumerates every object-valued position (`ui`, `workspace`, `permissions`,
  `logging`, `agent.compaction`, `llm.default.retry`, …). The sweep also checked
  every other enumeration in the spec against the tree: the §3.3 key inventory
  (all `reject_unknown` allow-lists and `read_*` calls), the §5.4 `Config`
  fields, the §7.1 top-level keys, the §3.6 reader list, the §7.8
  "keys not shown" list, the §1.3.2 stale-reference inventory, the §12.2 fixture
  inventory, and the §12 test-ID sequence; all were complete. No residual open
  finding is known.

- **Round-5 amendment (2026-09-17) — D-new-1…D-new-4 → 21-D11…21-D14.**
  Re-derived against HEAD after RB-09 landed (`9db17cd54`). Verified in the
  current tree: `kConfigFile = "config.jsonc"` (`config.cpp:29`),
  `kLegacyConfigFile = "config.toml"` (`config.cpp:30`), `legacy_config_path`
  (`config.cpp:713`), `jsonc_target` (`:723`), `scaffold_target` (`:727`),
  `LegacyFile`/`collect_legacy` (`:733-757`), `warn_legacy` (`:759-779`), the
  `load_config` warn call + `loadable` filter (`:929-941`), `apply_jsonc_file`'s
  silent missing-file return (`:833-840`), `apply_document`'s top-level
  `reject_unknown(table, "", …)` (`:534`), `reject_unknown`'s message builder
  (`:123`, which concatenates `table_name + "." + name`), `scaffold_config`'s
  empty-target branch (`:812-820`), and `run_config_command`'s legacy note
  (`cli.cpp:134-148`).
  **Defect classes swept:** (a) every sentence implying a `.toml` is detected,
  warned about, substituted, or converted; (b) every claim that a missing global
  config silently yields defaults; (c) every reference to the three deleted
  helpers, `kLegacyConfigFile`, `collect_legacy`, `warn_legacy`, `J-C9`, the old
  §6.x, `V3-6`, `V3-7`, and the retired tests
  J-T9–J-T12/J-T20/J-T23/J-T24/J-T27/J-T29/J-T33; (d) every top-level unknown-key
  message example with a leading dot. The repo-wide impact inventory is §14.4.
  The four decisions are recorded in §2 (J-C13–J-C16) and §15 Rev 5.

- **Round-6 amendment (2026-09-17) — Oracle Rev-5 gate FAIL (H1 + 5 MEDIUM +
  6 LOW) → 21-D15/21-D16/21-D17.** Re-derived against HEAD and read the host
  spawn path directly: `HostConfig` (`workspace_host.hpp` ~:105`),
  `ForkExecLauncher::build_argv` (`workspace_host.cpp` ~:933-948`, which emitted
  no `--config`), `HostLifecycle` ctor (`host_launcher.hpp` ~:98`) and
  `configFor` (`workspace_host.cpp` ~:1030-1042`), `run_supervisor_entry`
  (`cli.cpp` ~:366`) / `run_via_daemon` (`cli.cpp` ~:426`) and their `run_cli`
  call sites, `run_workspace_command` (`src/registry/workspace_cli.cpp`, registry
  only), `apply_jsonc_file` (`config.cpp` ~:833-855`, `exists` + `ifstream`),
  `config.hpp:14` ("unknown key in a JSONC file"), and the harness fixture
  (`tests/support/host_harness.hpp` ~:95`). **Defect classes swept:** (a) a
  config source that diverges between supervisor and daemon; (b) path kinds
  (directory/FIFO) that defeat the required-global rule; (c) commands that load
  config they do not consume; (d) stale citations (`scaffold_config`,
  `ScaffoldResult`, the exit-2 catch, the §7.10 quote); (e) test fixtures that
  assume config is optional. §14.4 now lists the host/harness files.

- **Round-7 amendment (2026-09-17) — second re-gate FAIL (M4 residual + the
  J20/§5.5 absolute + 4 LOWs).** Verified against the tree:
  `configure_workspace` (`tests/integration_two_process_test.cpp` ~:503-508`,
  sets `XDG_CONFIG_HOME`, writes no global config) and the six `HostLifecycle`
  sites (~:590,794,865,994,1335,1368`); `TwoSpawnsExactlyOneDaemon` (~:779`)
  starts no harness; `HostHarnessOptions::config_path` (~:38`) is forwarded by
  the harness `build_argv` (~:213-215`); `spawnAndAttach`'s `!isAlive` break
  (`workspace_host.cpp` ~:1086-1088`) and ~5 s winner-attach window
  (~:1096-1108`); `session_list`/`session_show`/`session_replay`/`session_fork`
  take no `Config` (`cli.cpp` ~:723-733`); `init_logging` after the load
  (`cli.cpp` ~:679-683`); `run_host_command` loads before the daemon's `chdir`
  (`workspace_host.cpp` ~:464`). **Defect classes swept:** (a) an inventory that
  lists some but not all fixture spawn sites; (b) an absolute invariant falsified
  by its own defaulted path; (c) a failure-mode trigger listing a non-failure;
  (d) a rationale ("commands that consume it") not matching the code; (e) a
  load-gating restructure that would leave logging un-gated.

- **Round-8 amendment (2026-09-17) — final gate PASS, four non-blocking LOWs.**
  Test-plan/inventory wording only. Verified against the tree: the repo already
  parses `/proc/<pid>/cmdline` for daemon argv (`tests/support/pty_child.hpp`
  ~:239-243`; `host_processes`/`is_ymh_host_process`), so J-T45 pins that
  inspection rather than an unobservable public-API assertion; `ShortTempRoot`
  (~:50`) may pre-create a global config under the M4 fix, so J-T48 must use a
  dedicated empty dir; `ui_supervisor_pty_test.cpp` drives the Tui binary, which
  self-scaffolds, so it needs no fixture change; `run_cli` initialises logging
  from `config.logging` (~:679-683`), now recorded in the §14.4 row. No contract
  change.

Residual risk: the system nlohmann version is pinned by the OS package
(3.12.0-2); if a different major version is resolved, the `ignore_comments`
overload signature must be re-checked (§4.3).

### 14.4 Implementation impact inventory (Rev 5–7)

Every file:line the implementation phase must touch. Line numbers are hints;
the symbol anchor is authoritative (this repo has been burned by stale line
numbers).

**src/**

| File | Symbol / line | Change |
|---|---|---|
| `src/config/config.cpp` | `kLegacyConfigFile` (~:30) | **delete** |
| `src/config/config.cpp` | `reject_unknown` (~:109-126) | build the qualified name without a leading dot when `table_name` is empty (21-D14) |
| `src/config/config.cpp` | `apply_document` (~:534) | caller unchanged (`reject_unknown(table, "", …)`); message now correct |
| `src/config/config.cpp` | `legacy_config_path` (~:713) | **delete** |
| `src/config/config.cpp` | `jsonc_target` (~:723) | **delete** |
| `src/config/config.cpp` | `scaffold_target` (~:727) | **delete** |
| `src/config/config.cpp` | `LegacyFile`/`collect_legacy`/`warn_legacy` (~:733-779) | **delete** |
| `src/config/config.cpp` | `apply_jsonc_file` (~:833) | add `bool required = false`; empty/absent + `required` ⇒ `ConfigError` (21-D12); reject a present non-regular file (`is_regular_file`) for either layer (21-D16, J-C18, J21, J-F16) |
| `src/config/config.cpp` | `load_config` (~:929) | drop `Logger*`; call `apply_jsonc_file(config, paths.global, true)` then `…, paths.workspace, false)`; delete `warn_legacy`/`loadable` (21-D12) |
| `src/config/config.cpp` | `load_config(workspace_root)` overload (~:946) | drop `Logger*` |
| `src/config/config.cpp` | `scaffold_config` (~:805) | **keep** the empty-target branch (J-C12); now reached via 21-D13 |
| `src/cli/cli.cpp` | `load_invocation_config` (~:68) | `load_config(paths)` (drop logger) |
| `src/cli/cli.cpp` | `scaffold_for_invocation` (~:105-122) | explicit `--config` ⇒ empty target; else `default_global_config_path()` (21-D13) |
| `src/cli/cli.cpp` | `run_config_command` (~:124-150) | **delete** the legacy note (~:134-148); primary output unchanged (L4) |
| `src/cli/cli.cpp` | `run_host_command` (~:235) | `load_config(paths)` (drop logger) |
| `src/cli/cli.cpp` | `run_cli` (~:669-702) | pass `effective_global_config(invocation)` to `run_supervisor_entry`/`run_via_daemon`; gate the load **and** the config-derived `init_logging` (~:679-683) by 21-D17 |
| `src/cli/cli.cpp` | `run_supervisor_entry` (~:366) | gain a config-path parameter (21-D15) |
| `src/cli/cli.cpp` | `run_via_daemon` (~:426) | gain a config-path parameter (21-D15) |
| `src/host/workspace_host.cpp` | `ForkExecLauncher::build_argv` (~:933-948) | append `--config <path>` when `HostConfig::config_path` is non-empty (21-D15) |
| `src/host/workspace_host.cpp` | `HostLifecycle::configFor` (~:1030-1042) | set `config.config_path = config_path_` (21-D15) |
| `src/host/workspace_host.cpp` | `HostLifecycle` ctor | forward the new `config_path` ctor parameter |

**include/**

| File | Symbol / line | Change |
|---|---|---|
| `include/ymh/config/config.hpp` | `legacy_config_path` decl (~:187-193) | **delete** |
| `include/ymh/config/config.hpp` | `jsonc_target` decl (~:195-198) | **delete** |
| `include/ymh/config/config.hpp` | `scaffold_target` decl (~:200-203) | **delete** |
| `include/ymh/config/config.hpp` | `ConfigPaths` comment (~:174) | note global required / workspace optional |
| `include/ymh/config/config.hpp` | `load_config` decls (~:235-242) | drop `Logger*` |
| `include/ymh/config/config.hpp` | `apply_jsonc_file` decl (~:244) | add `bool required = false`; reject non-regular files (21-D16) |
| `include/ymh/host/workspace_host.hpp` | `HostConfig` (~:105) | add `std::filesystem::path config_path;` (21-D15) |
| `include/ymh/host/host_launcher.hpp` | `HostLifecycle` ctor (~:98) | add `std::filesystem::path config_path = {}`, store `config_path_` (21-D15) |

**tests/**

| File | Symbol / line | Change |
|---|---|---|
| `tests/unit/config_test.cpp` | `LegacyTomlWarnsOnceAndIgnores` (~:489) | **delete** |
| `tests/unit/config_test.cpp` | `LegacyWarningOncePerCall` (~:501) | **delete** |
| `tests/unit/config_test.cpp` | `BothFilesPrefersJsoncAndWarns` (~:512) | **delete** |
| `tests/unit/config_test.cpp` | `LegacyOnlyContinuesOnDefaults` (~:524) | **delete** |
| `tests/unit/config_test.cpp` | `LegacyWarningTargetsBothSlots` (~:537) | **delete** |
| `tests/unit/config_test.cpp` | `NoSiblingProbeForCustomBasename` (~:555) | **delete** |
| `tests/unit/config_test.cpp` | `LegacyPathFromJsonc` (~:583) | **delete** |
| `tests/unit/config_test.cpp` | `JsoncTargetPerSlot` (~:591) | **delete** |
| `tests/unit/config_test.cpp` | `ScaffoldTargetPerSlot` (~:596) | **delete** |
| `tests/unit/config_test.cpp` | 27 × `ConfigPaths paths;` with empty `global` | supply an existing global (or expect `ConfigError`) |
| `tests/unit/cli_test.cpp` | `Cli.LegacyTomlWarningOnStderr` (~:251) | **delete** |
| `tests/unit/cli_test.cpp` | `Cli.ConfigPathLegacyNote` (~:268) | **delete** |
| `tests/unit/cli_test.cpp` | `Cli.RunListEmptyWorkspace` (~:131) | missing `--config` now errors; rewrite |
| `tests/unit/cli_test.cpp` | `Cli.ConfigPathPrintsEffectivePath` (~:153) | rename `.toml` → `.jsonc` (cosmetic) |
| `tests/unit/mcp_manager_test.cpp` | `ConfigPaths{{}, config_path}` (~:480,488,493) | supply a global or expect `ConfigError` |
| `tests/support/host_harness.hpp` | `HostHarnessOptions` / env setup (~:93-96) | write a global `config.jsonc` (or pass `--config`); else the daemon fails J-F5 (M4) |
| `tests/integration_host_harness_test.cpp` | `HostIntegration.RunRoutesThroughLiveDaemon` (~:279-291) | ensure the global config exists before `harness.start()` (M4) |
| `tests/integration_two_process_test.cpp` | `configure_workspace` (~:503-508); six `HostLifecycle` sites (~:590,794,865,994,1335,1368); direct `--host` spawn (~:826-834) | write `<root>/.config/ymh/config.jsonc` in the shared fixture so every `HostLifecycle`-spawned daemon finds it (ordering-independent); the direct spawn likewise (M4) |
| `tests/unit/ui_supervisor_pty_test.cpp` | fixture | **no change** — the Tui binary self-scaffolds the conventional global config (M4) |
| new | J-T34–J-T49 | see §12.1–§12.3 |

**docs / metadata (OQ-6, J-C11 — same commit as code)**

| File:line | Stale content |
|---|---|
| `README.md:28` | `toml++` in the dependency list |
| `README.md:90-91` | TOML config paths |
| `README.md:99-117` | `toml` example |
| `README.md:142-147` | `toml` workspace example |
| `AGENTS.md:65,113` | `toml++` in dependency lists |
| `docs/design/00-architecture.md:1598` | `toml` fence |
| `docs/design/00-architecture.md:3941,3945-3968` | "Use TOML." + TOML example (superseded by §1.3.1) |
| `docs/design/00-architecture.md:4065,4438,4486` | `toml++` |
| `docs/design/08-llm-provider.md:695,701-717` | "Config is TOML" + TOML example (superseded by §1.3.1) |
| `docs/design/09-permissions.md:361-383` | `toml` fence (the `permissions.local.toml` grants-file refs at `:216,518,887,1092,1238,1295` are out of scope) |
| `docs/design/13-context-compaction.md:304` | "[agent.compaction] TOML table" |
| `docs/design/15-mcp-adapter.md:509` | "unknown TOML keys already throw" |
| `docs/design/15-mcp-adapter.md:1236-1237,1242-1274` | TOML paths/example |
| `docs/design/20-skills.md:305,361,708` | `.config/ymh/config.toml` |
| `docs/design/20-skills.md:492-496,2031,2147` | `toml++` / TOML-frontmatter rationale |
| `docs/design/REQUIREMENTS_BACKLOG.md:252-267` | RB-09's own pre-change bullets |
| `HANDOFF.md` | no `toml`/`config.toml` matches in the current tree (nothing to do) |
| `CMakeLists.txt` | no `toml` matches in the current tree (toml++ already dropped by J-C10) |

---

## 15. Revision log

| Rev | Change |
|---|---|
| 0 | Initial errata. Supersedes `00 §37` and `08 §5.3`; pins the JSONC subset (comments only, no trailing commas), the exact `nlohmann::json::parse(..., ignore_comments=true)` configuration, the once-per-run legacy warning, the full key schema, scaffolding text, `J1–J16`, and `J-F1–J-F12`. |
| 1 | Gate round 1 fixes (2026-09-16): **J-H1** re-cite `20-skills.md:360-361`; **J-H2** re-cite hot reload to `00 §53 :4725` (D11 is dynamic plugins; `:4830` is D23); **J-M1** pin empty-array no-override (J10, §3.3/§3.4/§7.4/§7.8) + tests J-T21/J-T22; **J-M2** case-neutral warning text aligned across §6.2/§6.3/§6.5/§6.6 (21-D8); **J-M3** per-slot `jsonc_target`, fixed sketch, explicit-`.toml` no-target case + tests J-T23/J-T24; **J-L1** drop undefined `legacy_config_present`; **J-L2** empty/comments-only = `{}` (parity, §4.2/§7.10/J-F1) + test J-T25; **J-L3** README update promoted to prerequisite (J-C11/OQ-6). Adds §14.3 citation audit. |
| 2 | Gate round 2 fixes (2026-09-16): **J-R2-1** restrict empty-array no-op to `workspace_roots`; per-server empty ≡ absent (§3.3/§3.4/§7.4/§7.8/J10), rewrite J-T22, add J-T26; **J-R2-2** pin scaffold target (21-D10, §8.1), correct §8.4, add J-T28; **J-R2-3** add the full stale TOML/toml++ inventory to §1.3.2 and fold it into J-C11; **J-R2-4** pin the `ymh config path` legacy note (§6.8) + J-T27; **J-R2-5** add fixture constant-definition lines to §12.2; **J-R2-6** symbol-anchor `scaffold_for_invocation`; **J-R2-7** sketch the JSON `read_*` helpers (§4.2) so `read_int64` is defined; **J-R2-8** declare §3.3 the normative key inventory for the §7.8 example. Re-derived every anchor against the post-RB-03/RB-05 tree and adopted symbol-first citations (§14.3). Documents the RB-05 `skills` section (§7.11). |
| 3 | Gate round 3 fixes (2026-09-16): **V3-1** add the live `skills` field to the §5.4 `Config` sketch (RB-05); **V3-2** extend the §1.3.2/J-C11 sweep with `09-permissions.md:361-383`, `20-skills.md:2031`, `REQUIREMENTS_BACKLOG.md:255-267`, and declare `permissions.local.toml` out of scope; **V3-3** add `to_skill_catalog_config` + `to_compaction_policy` to §3.6 and re-derive the reader ranges; **V3-4** add §1.3.1a, a faithful JSONC rendering of the superseded `00 §37` example; **V3-5** forward-declare `line_for_byte`; **V3-6** restrict legacy sibling detection to conventional basenames (§6.1/§6.7/§6.8) + J-T20/J-T29; **V3-7** pin per-process warning semantics and the daemon `host.log` sink (§6.4, J6); **V3-8** restrict blank-input whitespace to RFC 8259 and state literal-`{}` semantics (§4.2, J-F1) + J-T30/J-T31. |
| 4 | Gate round 4 fixes (2026-09-16) plus a full defect-class sweep. **V4-1** case-correct the legacy warning for the explicit legacy-named `--config` slot (per-line marker + closing sentence, §6.2/§6.3/§6.5/§6.7), fix §8.1's false "warning says update `--config`" claim, add the repoint clause to the §6.8 note, extend J-T27/J-T28. **V4-2** pin the `scaffold_config` empty-target branch as **J-C12** (§5.4/§8.1/§8.4), replacing "exact control flow"; J-T28(c) asserts `ok == true` and no scaffold warning. **V4-3** skip a single leading UTF-8 BOM in `blank_or_comments_only`, add the §4.2 boundary-input table (one outcome per input), update §4.3/§7.10/J-F1, add J-T32. **V4-4** fix `apply_toml_file` header anchor `:211`→`:223` and re-derive the §14.3 `wiring.cpp` ranges to all six `to_*` readers. **V4-5** complete the §7.9 object-valued enumeration. Swept all five defect classes (prose-vs-behaviour, invariant-vs-control-flow, boundary inputs, stale citations, incomplete enumerations) over the whole spec; §14.3 records the audit. |
| 5 | **Rev-5 amendment (2026-09-17), user-approved D-new-1…D-new-4.** Adds **21-D11** (no TOML awareness at all; delete `kLegacyConfigFile`, `legacy_config_path`, `jsonc_target`, `scaffold_target`, `LegacyFile`, `collect_legacy`, `warn_legacy`, the `ymh config path` note, and §6.1–§6.8), **21-D12** (global config layer required — `ConfigError` on empty/absent; workspace optional; `Logger*` removed from `load_config`), **21-D13** (scaffolding bootstraps only the conventional path; explicit `--config` is never created; J-C12 empty-target branch retained), **21-D14** (top-level unknown-key message `unknown key 'auto_compact_enabled'`, no leading dot). New invariants J17–J19; failure modes J-F5/J-F6 rewritten, J-F13/J-F14 added; tests J-T9–J-T12/J-T20/J-T23/J-T24/J-T27/J-T29/J-T33 retired, J-T34–J-T43 added; §6 rewritten; §14.4 impact inventory added. Defect classes swept: stale TOML detection/warning/substitution/conversion claims, "missing global silently defaults" claims, and leading-dot message examples. |
| 6 | **Rev-6 amendment (2026-09-17), user-approved, fixes the Oracle Rev-5 gate FAIL (H1 + 5 MEDIUM + 6 LOW).** Adds **21-D15** (supervisor→daemon config handoff: additive `HostConfig::config_path`; `build_argv` appends `--config`; `HostLifecycle` ctor `config_path`; `run_supervisor_entry`/`run_via_daemon` config-path parameter; daemon loads exactly the supervisor's file — fixes H1 and a latent pre-RB-09 `--config`-ignored divergence), **21-D16** (required global layer must be a regular file; empty/absent/non-regular ⇒ `ConfigError`; optional workspace rejects non-regular too — fixes M1), **21-D17** (config loaded only for Tui/Run/List/Show/Replay/Fork; `Workspace`/`Config`/`Version` never load — fixes M5). New invariants J20–J22; new failure modes J-F15 (daemon cannot load the supervisor's config) and J-F16 (config path not a regular file); tests J-T44–J-T49 added. Also fixes M2 (`scaffold_config`/`ScaffoldResult` citations), M3 (J-T34/J-T35 no longer reference the removed `Logger*`; "no warning" is structural), M4 (host/harness fixtures and §14.4 rows added), L1 (§7.10 quote now "JSONC file"), L2 (§3 retitled historical + drift note), L3 (exit-2 catch citations), L4 (`config path` primary output), L5 (scaffold-failure ↔ required-global interaction), L6 (empty-path message constructed without `fail`). §5.5, §6.3, §14.3 round-6 entry, and §14.4 rows added. |
| 7 | **Rev-7 amendment (2026-09-17), user-approved, fixes the second re-gate FAIL (M4 residual + the J20/§5.5 absolute + 4 LOWs).** **M4 residual:** §12.2 and §14.4 now pin that the shared fixture (`configure_workspace` and/or `ShortTempRoot`, whichever owns the per-test `XDG_CONFIG_HOME`) writes `<root>/.config/ymh/config.jsonc`, and enumerate the six `HostLifecycle` sites (`integration_two_process_test.cpp` ~:590,794,865,994,1335,1368) so the fix is ordering-independent; `TwoSpawnsExactlyOneDaemon` starts no harness and is named explicitly. **J20/§5.5:** the "never re-resolves" rule is scoped to "when the supervisor supplies a path"; the defaulted-empty case keeps self-resolution; J-F15's trigger is now "absent/non-regular" (empty dropped). **Pinned clarifications:** no `HostLifecycle` ctor validation; `config_path` may be relative or absolute (resolved pre-`chdir` in both processes). **LOWs:** §14.4 `apply_jsonc_file` row gains the `is_regular_file` change; J-F15 timing corrected to the `!isAlive` break + ~5 s winner-attach window; J-T34 gains "global valid"; J22/§6.3 rationale corrected (the set mirrors `scaffold_for_invocation`; `List`/`Show`/`Replay`/`Fork` do not take a `Config`) and the config-derived `init_logging` is pinned as gated (Workspace runs with default logging). §1.4 records the 21-D16 tightening as deliberate. Register `J-C20`/`J-C21`, §14.3 round-7 entry added. |
| 8 | **Rev-8 amendment (2026-09-17), final gate PASS — four non-blocking LOWs.** Test-plan/inventory wording only; no decision, invariant, failure mode, or contract change. **J-T48** now uses a **dedicated empty dir independent of the shared fixture** (not `ShortTempRoot`, which the M4 fix may make pre-create a global config) so gated-vs-ungated loading stays distinguishable. **J-T45** pins `/proc/<pid>/cmdline` inspection (via the existing `host_processes`/`is_ymh_host_process` helpers, `tests/support/pty_child.hpp`) to observe the daemon's `--config <path>` argv. **`ui_supervisor_pty_test.cpp`** row now states **no fixture change is needed** (the Tui binary self-scaffolds the conventional global config). **§14.4 `run_cli` row** adds the config-derived `init_logging` gating already pinned by J-C19/§6.3/J22. Register Rev-8 note and §14.3 round-8 entry added. |
