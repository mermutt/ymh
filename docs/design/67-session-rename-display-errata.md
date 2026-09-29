# 67 — Session Rename Display & Edge-Case Errata (`/rename`)

```
Status: verified · reviewer: see `DESIGN_STATUS.md` row 67 · gate: 0 HIGH / 0 MEDIUM
Revision: 2
Component: 67 (errata) — amends 19-session-rename-errata.md (§6.1/§6.3) and
           17-ui-transcript-errata.md (§6, RB-10) by reference; touches the
           header renderer and the supervisor-level `/rename` coverage
Depends on: 19-session-rename-errata.md (verified + implemented),
            17-ui-transcript-errata.md (verified; RB-10 right-aligned title),
            22-switcher-sessions-errata.md (verified; `/sessions` catalog),
            57-switcher-sessions-popup-errata.md (verified),
            45-ui-interaction-errata.md (verified; `/rename` dispatch),
            58-subagent-navigation-errata.md (verified)
Scope: the user-visible `/rename` contract: the header rendering of a long
       title, and the supervisor-level observable behaviour of the empty /
       whitespace / spaced / over-long / non-live cases
```

## 1. Purpose, scope, and precedence

User semantics (binding): *"`/rename` shall rename session (what displayed in
the upper right corner and what will be shown in history)."* It renames the
**session title** — never a path, project, or binary.

The mutation is already implemented and verified by `19` (`session.rename`
wire method, the durable `SessionRenamed` event, `SessionManager::renameSession`,
the `/rename` registry entry, and the `SessionTitleChanged` → `setCellTitle`
delivery). This errata does **not** add a second notion of title; it pins the
remaining display defect and the supervisor-level observable behaviour that
`19 §12.5` left to the registry unit tests.

**Precedence.** This errata wins over `17 §6` (RB-10) for the header's
right-slot overflow behaviour; `19` still owns the mutation, the wire method,
and the title validator. It changes no persistence, event, or wire shape.

**Gate (per `AGENTS.md`).** Independent Oracle review marked this `verified`
(round 3 PASS, 0 HIGH / 0 MEDIUM; the round-1 LOWs were applied in Rev 2).

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 67-A1 | 17 §6 / 10 §8.1 header | `src/ui/ui_render.cpp:1524-1583`, call `:2041` | truncate the right-aligned session title to the cells left of the workspace title |
| 67-A2 | 19 §6.1 rules | `src/ui/command_registry.cpp:195-209`, `src/ui/supervisor.cpp:2652-2674` | restate the binding `/rename` semantics and pin the observable outcomes |
| 67-A3 | 19 §12.5 | `tests/unit/errata67_ui_test.cpp` | supervisor-level coverage of spaces / empty / rejection / non-live |

## 3. Verified current state (before this errata)

* `render_header` selected the active session's cell title and rendered it with
  a bare `ftxui::text` after a `filler()`, with **no width budget**. A title up
  to `kMaxSessionTitleBytes = 120` (`include/ymh/session/session.hpp:91`) can
  exceed a narrow terminal, so the header overflowed: a golden probe at width 72
  with a 118-byte title no longer even contained the `ymh · /work` left slot
  (the right slot pushed it off).
* `/rename <title>` already: takes the whole trimmed argument (spaces allowed,
  `command_registry.cpp:130-137`); prints `usage: /rename <title>` with **no**
  wire call on an empty argument (`:202-205`); is validated only by the daemon
  (`host_runtime.cpp:733` → `normalize_title`,
  `session.cpp:191-217`), so an invalid/over-long title returns `InvalidParams`
  and is surfaced as a `Role::System` error entry
  (`supervisor.cpp:2693-2696`); and fails visibly when no supervisor connection
  exists (`submit_to`, `supervisor.cpp:2089-2106` → `"no supervisor connection"`
  at `:2106`).
* `/sessions` / the switcher render the persisted header title via
  `session_row_title` and the catalog (`session_catalog.cpp:62-73`);
  `SessionTitleChanged` updates the live cell (`ui_model.cpp:1303-1305`).

## 4. Design

### 4.1 Header truncation (decision 67-D1)

`render_header(model, theme, width)` (`ui_render.cpp:1555-1583`) computes the
inner width (`width - 2` for the frame) and the cells left after the left title
plus a one-cell gap, then `truncate_to_width` (`ui_render.cpp:1524-1553`) walks
whole UTF-8 glyphs (`glyph_at`) and appends a single `…`, so:

* the header line never exceeds the terminal width (67-I1);
* the left `ymh · <cwd>` slot stays visible for any valid title (67-I2; the left
  slot's own cwd text is bounded only by the terminal, unchanged RB-10);
* a title that fits is rendered unchanged, preserving RB-10's exact
  right-alignment (67-I3).

`truncate_to_width` returns `{}` for a non-positive budget and `"…"` for a
one-cell budget, so a very narrow terminal degrades to no title / an ellipsis
rather than overflowing.

### 4.2 Binding `/rename` semantics (decisions 67-D2..67-D4)

These are already implemented; this errata makes them observable and explicit:

* **67-D2 (spaces)** — the argument is the whole trimmed remainder, so
  `/rename my new title` renames to `my new title`.
* **67-D3 (empty / whitespace)** — a local `usage:` notice, **no wire call**.
* **67-D4 (long / invalid)** — the client never pre-validates; the daemon is the
  single validator (19 §6.1 rule 4) and the reply lambda surfaces
  `rename failed: <error>`. A title longer than 120 bytes is rejected, not
  silently truncated, at the mutation layer; the *display* layer truncates for
  the header (67-D1). The two are deliberately different: the stored title is
  exact and durable, the rendered title is bounded.
* **67-D5 (not live)** — a rename in a workspace with no supervisor connection
  fails visibly (`rename failed: no supervisor connection`); it is never a
  silent no-op. A non-focused session is not addressable by `/rename` (the
  command always targets the active session), so the only "not live" path is
  the connection-less one.

## 5. Invariants

* **67-I1** The rendered header line width never exceeds the terminal width
  (FTXUI clipping bounds the left cwd; the right slot is explicitly budgeted).
* **67-I2** For any valid session title, the left `ymh` / cwd slot is visible.
  The left slot's own width is bounded by the terminal, not by this errata: a
  cwd longer than the terminal is clipped by FTXUI exactly as before.
* **67-I3** A title that fits is rendered byte-identical to the pre-errata
  right-aligned behaviour (no ellipsis).
* **67-I4** `/rename <spaced title>` transmits the full trimmed title verbatim.
* **67-I5** `/rename` with an empty/whitespace argument makes no
  `session.rename` call.
* **67-I6** A rejected rename appends exactly one `Role::System` error entry;
  a successful rename appends none (success is the streamed title change).
* **67-I7** The stored title is the exact validated title; only the header
  render is truncated.

## 6. Failure modes

* **67-F1 (F1 drop)** — a valid title is never silently shortened in storage;
  only its header rendering is ellipsized.
* **67-F2 (F7 wrong action)** — an empty argument never reaches the wire; it
  produces the usage notice only.
* **67-F3 (F1 drop)** — a rejected rename is never silent: the reply lambda
  routes it through the existing `ErrorOccurred` path.
* **67-F4 (F10 layout)** — a 120-byte title cannot overflow the header or
  displace the workspace title.

## 7. dsh mapping

| dsh concept | ymh | Justification |
|---|---|---|
| session title mutation | unchanged (`session.rename`, spec 19) | **mirrored**; this errata adds no mutation path. Anchor: 19 §5.4/§6.1. |
| title display | header right-slot truncation (67-D1) | **non-mirror**: dsh's host UI bounds its own title slot; ymh's renderer owns that bound because the spec-19 cap (120 bytes) is intentionally larger than a narrow terminal. Anchor: 67-D1 / `kMaxSessionTitleBytes`. |
| rename error surfacing | existing reply-lambda `ErrorOccurred` | **mirrored**; reuses 19's pinned mechanism. Anchor: 19 §6.1 (M1). |

## 8. Decisions

* **67-D1** Header right-slot is truncated UTF-8-safely to the cells left of the
  workspace title, with a single ellipsis. See §4.1.
* **67-D2** Spaces are significant; the whole trimmed argument is the title.
* **67-D3** Empty/whitespace → usage, no wire call.
* **67-D4** The daemon validates; the client does not pre-validate; storage is
  exact, display is bounded.
* **67-D5** The connection-less rename fails visibly; a non-focused session is
  not addressable via `/rename`.
* **67-D6 (rejected)** A second "title" notion (e.g. a UI-only label): reuses
  the existing session title instead (per the binding user semantics).

## 9. Test plan

* **67-U1** `UiRenderGolden.HeaderTruncatesLongSessionTitle`
  (`tests/unit/ui_render_golden_test.cpp`) — a 118-byte title at width 72 keeps
  `ymh · /work`, adds `…`, and drops the full title. Fails pre-fix (the left
  slot is pushed off). The width-≤-72 assertion is a guard, not the failer:
  FTXUI clips to the screen, so the left-slot/ellipsis/absent-title assertions
  carry the proof.
* **67-U2** `UiRenderGolden.SwitcherShowsRenamedSessionTitle` — a renamed title
  appears in the switcher/`/sessions` row. Pins the renderer on a title already
  in the model (the disk→catalog read is owned by spec 19); recorded honestly.
* **67-U3** `Errata67Rename.SpacesReachTheWireAsTheWholeTrimmedTitle`
  (`tests/unit/errata67_ui_test.cpp`) — `session.rename` params carry
  `my new title`. Pins existing behaviour.
* **67-U4** `Errata67Rename.EmptyOrWhitespaceArgMakesNoWireCallAndShowsUsage` —
  no wire call, usage notice. Pins existing behaviour.
* **67-U5** `Errata67Rename.RejectionIsVisible` — a canned `InvalidParams`
  reply yields a visible `rename failed: InvalidParams` entry. It tests the
  *error-rendering* terminal, not daemon validation; the validator itself is
  covered by `session_test.cpp`'s `normalize_title` cases and spec 19 §12.
* **67-U6** `Errata67Rename.NoLiveConnectionFailsVisibly` — no connection →
  `rename failed: no supervisor connection`. Pins existing behaviour.
* **67-U7** existing `PersistenceRename.*` / `SessionManagerRename.*` /
  `UiModel.SessionTitleChangedUpdatesCellAndMarks` — persistence and model
  delivery are owned by spec 19; cited, not re-implemented.

## 10. Non-goals

* A modal rename editor (19 §11 rule 5 already rejects it).
* Renaming a workspace, project, or the binary (the user semantics are
  session-only; the project name `ymh` is frozen).
* Changing `kMaxSessionTitleBytes` (120) or `normalize_title`.

## 11. Revision log

| Date | Revision | Note |
|---|---|---|
| 2026-09-29 | Rev 1 (draft) | Header truncation pinned (67-D1); `/rename` edge-case semantics restated and covered end-to-end (67-D2..D5); tests U1–U6. |
| 2026-09-29 | Rev 2 (draft) | Oracle round 1 findings applied: L1 citations re-derived; L5 test-scope honesty (U1 width guard, U5 error-rendering not validation, U2 model-only); L6 cwd qualification in 67-I1/I2. |
