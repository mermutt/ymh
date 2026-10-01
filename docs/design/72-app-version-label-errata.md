# 72 — App Version Label Errata

```
Status: verified (Rev 2) · reviewer: Oracle gate (round 2) · gate: 0 HIGH / 0 MEDIUM
Verification status: Oracle gate round 2 PASS — 0 HIGH / 0 MEDIUM; round 1 (2 MEDIUM) fixed in Rev 1; Rev 2 is a post-gate addendum (version 0.001→0.002 + MCP clientInfo single-sourcing) carrying the Rev 1 gate
Revision: 2
Component: 72 (errata) — amends 10-supervisor-tui.md §8.1 (the component
           hierarchy's header row), 67-session-rename-display-errata.md (67-D1
           truncation math), 17-ui-transcript-errata.md (RB-10 right-aligned
           title), 45-ui-interaction-errata.md §9 (45-D7, the `/status` version
           line), and the build-version plumbing (`ymh --version`)
Depends on: 67-session-rename-display-errata.md (verified + implemented),
            68-terminal-color-capability-errata.md (implemented),
            10-supervisor-tui.md (verified),
            17-ui-transcript-errata.md (verified),
            45-ui-interaction-errata.md (verified; the `/status` version line, 45-D7)
Scope: the app version shown in the TUI header's top-left product label, and the
       single compile-time source of that version
```

## 1. Purpose, scope, and precedence

User request (binding): the top-left label that currently renders as
`ymh · <cwd>` must become `ymh (v<version>) · <cwd>`, where `<version>` is the
one build version the project already defines, rendered exactly as `v0.002`
(Rev 2; it was `v0.001` at Rev 1).

This errata changes **presentation only**: the header's left slot, and the CMake
plumbing that makes the version visible to the renderer. It changes no wire
method, event, persistence row, session/title semantics, or keyboard handling.

**Precedence.** `10 §8.1` owns the header row in the component hierarchy (the
`render_header` widget); `67-D1` owns the right-slot
truncation arithmetic; this errata only extends the *left* slot's fixed prefix
and is therefore subordinate to both (the version is part of `title` and so is
already counted by `67-D1`'s `string_width(title)` budget — see 72-I3).

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 72-A1 | 10 §8.1 header / 67-D1 | `src/ui/ui_render.cpp:1621-1652` | the header left slot is `ymh (v<YMH_VERSION>)`, then ` · <cwd>` when a cwd is set |
| 72-A2 | build-version plumbing; `ymh --version`/`/status` output | `CMakeLists.txt:3-8`, `:773`, `:816`, `:828`, new `ymh_ui` define | sets the top-level `project(ymh VERSION 0.001)` — changed from `0.1.0`, so `ymh --version` and `/status` now read `0.001` — as the single source; `YMH_VERSION` is now also compiled into `ymh_ui` (the header renderer's target) |
| 72-A3 | `src/cli/cli.cpp:55-57` | `include/ymh/core/version.hpp` | the `#ifndef YMH_VERSION` fallback is centralised in one header and included by both consumers |
| 72-A4 | `tests/unit/ui_render_golden_test.cpp` | `:286`, `:877`, `:2109`, new `HeaderShowsVersionLabel` | pin the new label text and prove the test fails pre-change |
| 72-A5 | 45-ui-interaction-errata.md §9.1/§9.2 (45-D7) | `docs/design/45-ui-interaction-errata.md:809-814`, `:848` | records the version-value change (`0.1.0` → `0.001`) and the single access point (`include/ymh/core/version.hpp`), and corrects the `/status` version account (the supervisor now reads the version) |
| 72-A6 | version value + `src/mcp/mcp_client.cpp:130` | `CMakeLists.txt:5`, new `ymh_mcp` define, `include/ymh/core/version.hpp` | advances the single-sourced version `0.001` → `0.002` and threads `YMH_VERSION` into `ymh_mcp` so the MCP `clientInfo.version` (previously the stray literal `"0.1.0"`) reports the same build version (72-D4) |

## 3. Version source of truth (decisions 72-D1, 72-D2, 72-D4)

**72-D1 — one source.** The version lives in exactly one place: the top-level
`project(ymh VERSION 0.002 ...)` (`CMakeLists.txt:3-8`). CMake derives
`PROJECT_VERSION == "0.002"` and it is compiled in as the preprocessor string
`YMH_VERSION`:

```cmake
target_compile_definitions(ymh_cli PRIVATE YMH_VERSION="${PROJECT_VERSION}")
target_compile_definitions(ymh      PRIVATE YMH_VERSION="${PROJECT_VERSION}")
target_compile_definitions(ymh_ui   PRIVATE YMH_VERSION="${PROJECT_VERSION}")  # 72-A2
target_compile_definitions(ymh_mcp  PRIVATE YMH_VERSION="${PROJECT_VERSION}")  # 72-A6
```

`ymh_ui` is the target that compiles `src/ui/ui_render.cpp`, so before this
errata the renderer had no `YMH_VERSION` in scope. `ymh_mcp` is the target that
compiles `src/mcp/mcp_client.cpp`; it gets the same define so the MCP client
reports the build version (72-D4).

**72-D2 — one access point.** `include/ymh/core/version.hpp` is the single
header a translation unit includes to read the macro, with the fallback for
non-CMake/editor builds:

```cpp
#pragma once
#ifndef YMH_VERSION
#define YMH_VERSION "0.0.0"   // never the shipped version; CMake always supplies the macro
#endif
```

Body and callers (56-D6 / "new symbols are normative"): the macro has no
runtime symbol; its concrete callers are `src/ui/ui_render.cpp` (label text,
72-A1), `src/cli/cli.cpp` (`options.version`, `ymh --version`; `:554`,
`:1328`, `:1462`), which previously carried a duplicated local `#ifndef`, and
`src/mcp/mcp_client.cpp:130` (the MCP `clientInfo.version` field, 72-D4).

**72-D4 — the MCP client is not a second source (Rev 2).** The MCP client's
`initialize` `clientInfo.version` previously sent a hardcoded `"0.1.0"`, which
drifted from the app version and bypassed `YMH_VERSION`; it now sends
`YMH_VERSION` via `include/ymh/core/version.hpp`, and `ymh_mcp` carries the
`target_compile_definitions(... YMH_VERSION="${PROJECT_VERSION}")` define. The
`"0.1.0"` literal is retained **only** in `docs/` history and in unrelated
`tests/unit/status_format_test.cpp` inputs (surrogate values for the generic
`/status` formatter, not the shipped app version). The version is thus
single-sourced from `CMakeLists.txt` `PROJECT_VERSION` across the CLI, the TUI
header, `/status`, and the MCP handshake.

**Non-hardcoding.** The literal `0.002` appears **only** in `CMakeLists.txt`
(72-D1) and in the test assertion that pins the shipped string (72-U1). No
source file hardcodes it.

## 4. Exact rendering (decision 72-D3)

`render_header` (`src/ui/ui_render.cpp:1621`) builds the left slot as:

```cpp
std::string title = std::string{"ymh (v"} + YMH_VERSION + ")";   // 72-D3
if (workspace != model.workspaces.end() && !workspace->second.cwd.empty()) {
    title += " · " + workspace->second.cwd;
}
```

Label grammar (the header's top-left product mark):

```
left  := "ymh (v" VERSION ")" [ " · " CWD ]
right := <active session title, 67-D1-truncated to the cells right of `left`>
```

The version is not a separate widget: it is a prefix of the existing `title`
string, so the `67-D1` computation
`available = inner - ftxui::string_width(title) - 1` sees the longer prefix and
truncates the right slot correspondingly. `left` is painted cyan+bold exactly as
before (`src/ui/ui_render.cpp:1652`).

## 5. Terminal title (finding: none exists)

The request also asked for "any terminal-title escape ymh sets". There is none.
A repo-wide search for OSC introducers (`ESC ]`, `]0;`, `]2;`, octal `\033]`)
finds no writer in `src/`, `include/`, or `tests/`, and FTXUI v6.1.9 has no
window-title API (its only OSC handling is the *input* parser and the OSC-8
hyperlink renderer). ymh therefore sets no terminal title, and this errata adds
none (out of scope). Anchor: repo search of `src/ include/ tests/`, this spec §5.

## 6. Invariants

| ID | Invariant |
|---|---|
| 72-I1 | The header's left slot begins with exactly `ymh (v` + `YMH_VERSION` + `)`. |
| 72-I2 | `YMH_VERSION` has exactly one textual source: `PROJECT_VERSION` (72-D1); no source file contains the literal `0.002`. |
| 72-I3 | The `67-D1` truncation is unchanged: the right slot is ellipsized against `inner - string_width(title) - 1` with the version included in `title`; the header never exceeds the terminal width. |
| 72-I4 | `ymh --version`, the `/status` version line, and the header label all report the same `YMH_VERSION`. |

## 7. Failure modes

| ID | Condition | Behaviour |
|---|---|---|
| 72-F1 | `YMH_VERSION` not defined (non-CMake TU build) | `version.hpp` yields `"0.0.0"`; the label reads `ymh (v0.0.0)`. Never shipped. |
| 72-F2 | `YMH_VERSION` empty | The label reads `ymh (v)`. Not reachable via CMake (`PROJECT_VERSION` is non-empty once `VERSION` is set); no guard added. |
| 72-F3 | Narrow terminal | `67-D1` still truncates the right slot; the left slot (now 9 cells wider) can never push the frame over width because `left` is bounded and the right slot is ellipsized. |

## 8. dsh mapping

| dsh concept | ymh | Justification |
|---|---|---|
| build/version string | `YMH_VERSION` from `PROJECT_VERSION` (72-D1) | **mirrored**: both derive the app version from the build system, not a hardcoded runtime literal. Anchor: 72-D1, `CMakeLists.txt:5`. |
| version in the product header | `ymh (v<YMH_VERSION>)` left slot (72-D3) | **non-mirror, presentation**: dsh carries its version in CLI/`--version` output, not its TUI header; ymh additionally surfaces it top-left at the user's request. This is a deliberate ymh-local presentation choice with no architectural seam. Anchor: 72-D3. |
| terminal window title | none (72 §5) | **mirror, absence**: neither dsh nor ymh sets a terminal window title in this codebase; adding one would require an OSC writer that no spec defines. Anchor: `src/ui/ui_render.cpp:1621-1652` (header render emits no OSC), 72-D3. |

## 9. Test plan

| ID | Test | Assertion |
|---|---|---|
| 72-U1 | `UiRenderGolden.HeaderShowsVersionLabel` (`tests/unit/ui_render_golden_test.cpp`) | the first header row contains `ymh (v0.002)`; the full golden snapshot's header row is updated to the same text. **Fails pre-change** (pre-change code renders `ymh · /work`). |
| 72-U2 | `UiRenderGolden.HeaderTruncatesLongSessionTitle` (updated) | `67-D1` still holds with the longer left slot: header contains `ymh (v0.002) · /work`, contains `…`, excludes the full long title, and `string_width(header) <= 72`. |
| 72-U3 | `UiRenderGolden.ConversationSnapshot` (`kGolden`) | the golden header row is byte-exact `ymh (v0.002) · /work`; a regression in either the label or the filler width fails the snapshot. |

## 10. Non-goals

- Adding a terminal window title (OSC 0/2) — §5.
- Any change to `67`'s right-slot truncation semantics.
- The MCP protocol revision (`protocolVersion`) — 72-D4 only single-sources the
  MCP `clientInfo.version`, not the negotiated protocol revision.

## 11. Revision log

- Rev 1 — initial errata: version label + single-source plumbing + test.
- Rev 2 — post-gate addendum: advance the single-sourced version `0.001` →
  `0.002` and thread `YMH_VERSION` into `ymh_mcp` so the MCP `clientInfo.version`
  (was the stray literal `"0.1.0"`) reports the same build version (72-A6,
  72-D4). No re-gate; carries the Rev 1 Oracle gate (0 HIGH / 0 MEDIUM).
