# 68 — Terminal Colour Capability & `ui.color` Override Errata

```
Status: verified (Rev 3) · reviewer: see `DESIGN_STATUS.md` row 68 · gate: 0 HIGH / 0 MEDIUM
Revision: 3
Component: 68 (errata) — amends 48-ui-and-config-errata.md (TUI/config surface),
           10-supervisor-tui.md §8.3/§20.19 (theme wiring in `run_loop`),
           21-config-jsonc-errata.md §6 (the `[ui]` key set), and
           51-reasoning-fold-prompt-emphasis-and-tables-errata.md (§4.2 `Theme`)
Depends on: 48-ui-and-config-errata.md (verified),
            10-supervisor-tui.md (verified),
            21-config-jsonc-errata.md (verified + implemented),
            51-...-errata.md (verified + implemented; `make_theme`),
            66-bracketed-paste-errata.md (verified + implemented)
Scope: how the TUI decides whether to emit colour — `TerminalCapabilities`
       detection, the `NO_COLOR` convention, and the `ui.color` config override —
       plus the disposition of the (non-existent) `bracketedPaste` capability flag
```

## 1. Purpose, scope, and precedence

User-reported defect: *"ymh renders in COLOUR in my local terminal but is
completely monochrome when I PuTTY into Ubuntu — while nvim and other apps DO
have colour over that same PuTTY session."*

Root cause (established, re-verified in §3): `TerminalLayer::capabilities()`
recognised colour only via `COLORTERM=truecolor|24bit` or `TERM=*256color*`.
PuTTY sends `TERM=xterm` with no `COLORTERM`, matching neither, so `trueColor`
and `color256` were both `false`; the theme gate (`make_theme`) is fed
`caps.trueColor`, so the whole transcript rendered monochrome. `nvim` still shows
colour because it consults **terminfo** (xterm declares 8/16 colours), which ymh
never did.

This errata makes the colour decision honest for an 8/16-colour terminal, honours
`NO_COLOR`, and adds a `ui.color = auto|always|never` override so the user has an
explicit escape hatch for exactly the PuTTY case. It changes no wire, event,
persistence, or renderer shape.

**Out of scope (68-D8).** This errata governs the **TUI** colour gate only. The
non-TUI stderr logger still chooses colour from `isatty(STDERR_FILENO)` alone
(`src/cli/cli.cpp`, `src/core/logging.cpp`), so `ui.color = never` does not
suppress colour on a tty logger. That is a recorded disposition, not an open
question: the logging path is a separate surface with a different (global,
pre-config) lifetime and is not part of the reported PuTTY TUI defect.

**Precedence.** For the colour gate this errata wins over the implicit
"truecolor-only" reading of 48/51. `make_theme(bool color, ...)`
(`include/ymh/ui/theme.hpp:45-54`) is unchanged: `color` remains the sole colour
gate (51-D2.1). `10 §8.3/§20.19` owns `TerminalLayer` as the low-level terminal
seam; this errata only replaces the body of `capabilities()` and adds pure
helpers. `21` owns the `[ui]` config table; this errata adds one key and its
validator.

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 68-A1 | 48/51 colour gate; `10 §8.3` | `src/ui/terminal_layer.cpp:171-175`, `include/ymh/ui/terminal_layer.hpp:25-34` | `capabilities()` becomes a conservative, explicit detection: any non-empty non-`dumb` `TERM` is at least 8-colour; `trueColor`/`color256` keep the higher-tier conventions |
| 68-A2 | `48`/`51` theme construction | `src/ui/supervisor.cpp:3900-3901` | feed `make_theme` the resolved `caps.color` (not `caps.trueColor`) |
| 68-A3 | `21 §6` `[ui]` table | `include/ymh/config/config.hpp:55-89`, `src/config/config.cpp:326-339`, `src/config/config.cpp:1429-1434` | new `ui.color = "auto"|"always"|"never"` key, parsed + validated; scaffold updated |
| 68-A4 | `21 §6`/48 config surface | `src/ui/terminal_layer.{hpp,cpp}` | `NO_COLOR` (non-empty) disables colour in `auto` mode |

## 3. Verified current state (before this errata)

* `TerminalLayer::capabilities()` (`src/ui/terminal_layer.cpp:171-175` pre-errata
  body was `:132-138`) sets
  `caps.trueColor = COLORTERM ∈ {truecolor,24bit}` and
  `caps.color256 = trueColor || TERM ∋ 256color`; it never sets any basic-colour
  flag.
* `TerminalCapabilities` (`include/ymh/ui/terminal_layer.hpp:25-34`) declared
  `trueColor = true; color256 = true;` as *defaults*, but `capabilities()`
  overwrote both on every call, so the defaults were dead and the conservative
  branch silently won.
* The supervisor builds the theme from `caps.trueColor`
  (`src/ui/supervisor.cpp:3900-3901`), so an 8/16-colour terminal (PuTTY
  `TERM=xterm`) is rendered monochrome even though it can render colour.
* `ui.theme` and `ui.side_panel` are parsed (`src/config/config.cpp:326-332`) but
  not validated; there was no `ui.color` key and no `NO_COLOR` handling.
* `bracketedPaste` is **not** a `TerminalCapabilities` field: spec 66-D2 pinned
  bracketed paste as an unconditional runtime emission
  (`TerminalLayer::enterBracketedPaste` / `bracketedPasteActive`,
  `include/ymh/ui/terminal_layer.hpp:90-94`, sequences at `:68-69`) because
  DECSET 2004 is not introspectable. There is no flag to reconcile.

## 4. Design

### 4.1 Detection (decisions 68-D1, 68-D2, 68-D5)

`detect_capabilities(TerminalEnv)` is a **pure** function (no `getenv`, no
subprocess) over `{term, colorterm, no_color}`. Rules, in order:

1. **`NO_COLOR`** — any non-empty value returns an all-false capability set
   (68-D4).
2. **`TERM` empty or `dumb`** — monochrome. `dumb` also beats a contradictory
   `COLORTERM` (68-D2): a `dumb` terminal cannot interpret escapes.
3. **`COLORTERM` ∈ {`truecolor`,`24bit`}** — `color = color256 = trueColor =
   true`.
4. **any other non-empty `TERM`** — `color = true` (≥8 colours); `color256` is
   additionally true only when `TERM` contains `256color`.

`caps.color` is the new `make_theme` gate; the tier relation
`trueColor ⇒ color256 ⇒ color` holds (68-I8). `mouse`/`unicode` are unchanged
from the shipped struct.

### 4.2 Terminfo (decision 68-D6)

Terminfo was evaluated and **rejected**:

* `setupterm`/`tigetnum("colors")` requires linking libtinfo/ncurses, which is
  **not** in `AGENTS.md`'s declared dependency set; adding it would be a new link
  dependency and is forbidden by this errata's mandate.
* A `tput colors` subprocess would add no *link* dependency, but it introduces a
  fork/exec into the TUI startup path (unsafe to fork from the multithreaded
  supervisor), is non-hermetic (absent in minimal images, can block on a broken
  terminfo DB), and buys nothing for the reported case: the terminfo answer for
  `TERM=xterm` is 8 colours, which the heuristic already produces.

The heuristic therefore is the shipped detection, and `ui.color = "always"` is
the explicit escape hatch (68-D3, 68-I4). If a future change adds a declared
terminfo dependency, `detect_capabilities` takes an optional `terminfo_colors`
input without changing callers' rules.

### 4.3 `ui.color` override (decision 68-D3)

`resolve_color(caps, mode)`:

| `ui.color` | Result |
|---|---|
| `auto` (default) | `caps.color` (detection; `NO_COLOR` already applied) |
| `always` | `true`, regardless of `TERM`/`COLORTERM`/`NO_COLOR` |
| `never` | `false`, regardless of `TERM`/`COLORTERM` |

`always` is a deliberate user override and therefore **wins over `NO_COLOR`**
(68-I4); `NO_COLOR` is a convention, an explicit config key is an instruction.
The value is validated at load time: an unknown string is a `ConfigError`
(68-I7). The default is `"auto"` and the scaffolded global file documents it.

### 4.4 State lifetime

AGENTS.md requires every introduced piece of state to tabulate its lifetime.

| State | Created | Destroyed / evicted | Owner | Restart / reconnect | Crash path |
|---|---|---|---|---|---|
| `UiConfig::color` (`include/ymh/config/config.hpp:88`) | when `load_config` builds the `Config` (`src/config/config.cpp:326-339`) | with the `Config` (supervisor process lifetime) | the `Config` object | re-read from `config.jsonc` on every process start (the file, not ymh, is the durable copy); a supervisor reconnect creates no new `Config` | the in-memory value is lost with the process; the user's file is untouched, so the next start reloads the same value |
| `TerminalCapabilities` (incl. `color`, `trueColor`, `color256`) (`include/ymh/ui/terminal_layer.hpp:25-34`) | per `TerminalLayer::capabilities()` call (`src/ui/terminal_layer.cpp:171-175`) | at the end of the calling expression; never stored | the caller's stack temporary | recomputed at each `run_loop` start (`src/ui/supervisor.cpp:3900`); no persistence | pure function of the environment + config, so a crash leaves nothing behind |

## 5. Invariants

* **68-I1** `TERM=xterm` with no `COLORTERM`/`NO_COLOR` ⇒ `caps.color == true`.
* **68-I2** `TERM` empty or `dumb` (and no overriding `ui.color`) ⇒
  `caps.color == false`.
* **68-I3** A non-empty `NO_COLOR` in `auto` mode ⇒ `caps.color == false` even
  when `COLORTERM=truecolor`.
* **68-I4** `ui.color == always` ⇒ the theme gate is `true` for every `TERM`
  and every `NO_COLOR` value.
* **68-I5** `ui.color == never` ⇒ the theme gate is `false` even for
  `COLORTERM=truecolor`.
* **68-I6** `detect_capabilities` is pure: it never reads the process
  environment and never spawns a process, so every rule is unit-testable.
* **68-I7** An unknown `ui.color` value is a `ConfigError`; it is never silently
  ignored.
* **68-I8** Capability tiers are monotone: `trueColor ⇒ color256 ⇒ color`.
* **68-I9** `TerminalCapabilities`' in-class defaults are conservative
  (`trueColor = color256 = color = false`) and agree with the detection, which
  sets each field explicitly (68-D5).
* **68-I10** The CLI/stderr logging colour choice is independent of
  `TerminalCapabilities`/`ui.color` and remains `isatty(STDERR_FILENO)`-based;
  this is a deliberate scope boundary (68-D8), not an invariant violated by this
  errata. Any future change that routes the logger through `resolve_color` must
  be its own errata.

## 6. Failure modes

* **68-F1 (F7 wrong action)** — an 8/16-colour terminal rendered monochrome
  because `trueColor` was the only gate. Fixed by 68-D1/68-A2.
* **68-F2 (F1 drop)** — `ui.color = always` still renders monochrome (overridden
  by env). Forbidden by 68-I4.
* **68-F3 (F7 wrong action)** — a typo (`ui.color = "alwys"`) silently falls back
  to `auto`. Forbidden by 68-I7.
* **68-F4 (F7 wrong action)** — `NO_COLOR` ignored on a truecolor terminal.
  Forbidden by 68-I3.
* **68-F5 (F10 layout)** — detection forking/blocking the TUI at startup.
  Avoided by 68-I6 / 68-D6.
* **68-F6 (F1 drop)** — the conservative defaults (`false`) being treated as
  "colour on". Forbidden by 68-I9.

## 7. dsh mapping

| dsh concept | ymh | Justification |
|---|---|---|
| terminal capability probe | heuristic in `detect_capabilities` (68-D1/68-D2) | **non-mirror**: dsh links a terminal-info library; ymh's dependency set (`AGENTS.md`) declares none, and a `tput` fork is rejected on fork-safety/hermeticity grounds (68-D6). Anchor: 68-D6. |
| colour opt-out | `NO_COLOR` honoured in `auto` (68-D4) | **mirrored** as the standard convention; no-colour.org semantics. Anchor: 68-D4. |
| force-colour escape hatch | `ui.color = always` (68-D3) | **mirror**: the explicit user override for terminals that advertise nothing. Anchor: 68-D3. |
| bracketed paste | runtime emission, no capability flag (spec 66-D2) | **mirrored**: dsh also emits DECSET 2004 unconditionally (not introspectable). No `bracketedPaste` caps field exists to reconcile. Anchor: 66-D2, `include/ymh/ui/terminal_layer.hpp:68-94`. |
| colour tiers (`trueColor`/`color256`) | informational reports (68-D1/68-I8) | **non-mirror, reporting-only**: only `caps.color` gates rendering (`src/ui/supervisor.cpp:3900`); the higher tiers are retained for diagnostics/tests and a future renderer that wants to degrade RGB (51-D2.1 keeps `color` the sole gate). Anchor: 68-I8, `src/ui/supervisor.cpp:3900-3901`. |

## 8. Decisions

* **68-D1** Any non-empty `TERM` other than `dumb` is at least an 8-colour
  terminal (`caps.color = true`).
* **68-D2** `TERM` empty or `dumb` is monochrome; `dumb` beats a contradictory
  `COLORTERM`.
* **68-D3** `ui.color = auto|always|never`, default `auto`; `always` forces the
  colour gate on and wins over all environment signals; `never` forces it off.
* **68-D4** `NO_COLOR` (any non-empty value) disables colour in `auto` mode.
* **68-D5** `TerminalCapabilities` defaults are conservative and agree with a
  detection that assigns every colour field explicitly.
* **68-D6** Terminfo is not linked and no `tput` subprocess is spawned; the
  heuristic is authoritative (see §4.2).
* **68-D7** There is no `bracketedPaste` capability flag; bracketed paste stays a
  runtime emission (66-D2). Nothing is removed because nothing existed.
* **68-D8** The CLI/stderr logging colour path (`isatty(stderr)`-based) is
  explicitly **out of scope**: `ui.color` governs the TUI gate only. Recorded as
  a disposition in §1, not deferred (see 68-I10).

## 9. Test plan

All hermetic; `detect_capabilities` is pure so no process environment is touched
by the rule tests. `tests/unit/errata68_ui_test.cpp`:

* **68-U1** `Errata68Color.TermXtermWithoutColortermIsAtLeastEightColours`
  (`{"xterm","",""}`) — `color` true, `trueColor` false. Fails pre-fix (the old
  detection requires `COLORTERM`/`256color`).
* **68-U2** `Errata68Color.TermXterm256colorIs256` — `color256` true,
  `trueColor` false. Fails pre-fix (`color` field absent / not set).
* **68-U3** `Errata68Color.ColortermTruecolorIs24bit` — `trueColor` and
  `color256` true.
* **68-U4** `Errata68Color.DumbTerminalIsMonochrome` — `color` false. Guard.
* **68-U5** `Errata68Color.DumbTerminalBeatsContradictoryColorterm` — `color`
  false even with `COLORTERM=truecolor`. Guard for 68-D2.
* **68-U6** `Errata68Color.NoColorDisablesEvenTruecolor` — all colour fields
  false with `NO_COLOR=1`. Fails pre-fix (`trueColor` was true).
* **68-U7** `Errata68Color.EmptyNoColorDoesNotDisable` — an empty `NO_COLOR`
  leaves `color` true.
* **68-U8** `Errata68Color.UnsetTerminalIsMonochrome` — `{"","",""}` false.
* **68-U9** `Errata68Color.AlwaysForcesColourOnDumbTerminal` —
  `resolve_color(detect({"dumb"}), Always)` true. Fails pre-fix (no override).
* **68-U10** `Errata68Color.NeverForcesMonochromeOnTruecolor` —
  `resolve_color(detect({"xterm","truecolor"}), Never)` false. Fails pre-fix.
* **68-U11** `Errata68Color.AutoFollowsDetection` — `auto` returns `caps.color`.
* **68-U12** `Errata68Color.DefaultCapabilityFieldsAreConservative` — a
  default-constructed `TerminalCapabilities` has `trueColor/color256/color`
  false (pins 68-D5/68-I9).
* **68-U13** `Errata68Color.CapabilitiesReadsTheProcessEnvironment` —
  `setenv(TERM=xterm)`, unset `COLORTERM`/`NO_COLOR`, then
  `TerminalLayer(0).capabilities().color == true`, with a scoped env guard.
* **68-U14** `Errata68Color.ConfigParsesColorModesAndRejectsUnknown` —
  `load_config` accepts `auto|always|never`; `"alwys"` throws `ConfigError`; the
  default is `Auto`.
* **68-U15** `Errata68Color.ScaffoldedDefaultConfigLoadsWithColorAuto` — the
  scaffolded global file is valid and closes as `ui.color == Auto`.
* **68-U16** `Errata68Color.AlwaysOverridesNoColor` — `always` also wins over a
  non-empty `NO_COLOR` (68-I4). Fails pre-fix (no override existed).

Existing coverage cited, not re-implemented: `Config.*` (`tests/unit/config_test.cpp`),
spec 66's `Errata66Ui.*`, and `make_theme` (51-D2).

## 10. Non-goals

* Linking ncurses/terminfo or spawning `tput` (68-D6).
* Light/dark theme auto-selection (51-D2.8 / OQ-51-2 stay deferred).
* Changing `Theme` fields or the meaning of `make_theme`'s `color` parameter.
* A capability probe for bracketed paste (66-D2 stands).
* The CLI/stderr logging colour path — dispositioned by 68-D8/68-I10.

## 11. Revision log

| Date | Revision | Note |
|---|---|---|
| 2026-09-29 | Rev 1 (draft) | initial errata: heuristic detection, `NO_COLOR`, `ui.color`, flag disposition |
| 2026-09-29 | Rev 2 (draft) | Oracle gate round 1: 0 HIGH / 1 MEDIUM (state-lifetime table added, §4.4) + LOWs (anchor refresh, reporting-only `trueColor`/`color256` row, 68-U16 `always`+`NO_COLOR`, `same_config` field) |
| 2026-10-01 | Rev 3 (draft) | Independent verification noted the CLI/stderr logger still colours from `isatty(stderr)` alone, so `ui.color = never` can colourise tty logs. Recorded as an explicit out-of-scope disposition (68-D8/68-I10/§1) rather than a deferred feature; no behaviour change. |
