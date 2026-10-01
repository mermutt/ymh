# 67 — Session Rename Display & Edge-Case Errata (`/rename`)

```
Status: verified (Rev 3) · reviewer: see `DESIGN_STATUS.md` row 67 · gate: 0 HIGH / 0 MEDIUM
Revision: 3
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

**Gate (per `AGENTS.md`).** Rev 2 was marked `verified` by an independent Oracle
review (round 3 PASS, 0 HIGH / 0 MEDIUM; the round-1 LOWs were applied in Rev 2).
Rev 3 re-gated after 67-D7/67-A6: round 1 reopened with 0 HIGH / 1 MEDIUM (the
auto-title writer bypassed `normalize_title`), closed by 67-A6/67-I9; final
**PASS (0 HIGH / 0 MEDIUM)**.

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 67-A1 | 17 §6 / 10 §8.1 header | `src/ui/ui_render.cpp:1524-1583`, call `:2041` | truncate the right-aligned session title to the cells left of the workspace title |
| 67-A2 | 19 §6.1 rules | `src/ui/command_registry.cpp:195-209`, `src/ui/supervisor.cpp:2652-2674` | restate the binding `/rename` semantics and pin the observable outcomes |
| 67-A3 | 19 §12.5 | `tests/unit/errata67_ui_test.cpp` | supervisor-level coverage of spaces / empty / rejection / non-live |
| 67-A4 | 19 §6.1 validator / 67-D3 | `src/session/session.cpp` `normalize_title`, `src/ui/command_registry.cpp` `trim` | both use the shared `ymh::trim_unicode_whitespace` (`include/ymh/core/text.hpp`) so NBSP is whitespace (67-D7) |
| 67-A5 | 19 §12 validator test | `tests/unit/session_test.cpp` `SessionTitle.NormalizeTrimsUnicodeWhitespace` | assert NBSP-only is rejected and interior NBSP survives |
| 67-A6 | 19 §5.2 auto-title path | `src/session/session.cpp` `derive_auto_title` | apply `ymh::trim_unicode_whitespace` and return `std::nullopt` on an empty result, closing the second title writer (67-I9) |

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
  "Whitespace" is the Unicode `White_Space` property (67-D7), not ASCII only:
  `/rename` followed by a single U+00A0 must produce the notice, not reach the
  wire. On the `/rename` path `normalize_title` (67-D4) applies the same trim, so
  the registry and the validator agree. **Scoped (67-A6/67-I9):** the other title
  writer, the auto-title path (`derive_auto_title` → `Session::appendAutoRename`),
  does not go through `normalize_title`; it applies the same `trim_unicode_whitespace`
  itself (67-D7), so no first-turn-only-NBSP title can be stored there either.
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
* **67-I8** The empty/whitespace predicate is identical in the registry argument
  trim and `normalize_title`: both strip the same Unicode `White_Space` set via
  `ymh::trim_unicode_whitespace` (`include/ymh/core/text.hpp`). U+000A (LF) is
  **not** in the trim set and remains a rejected control character (19/RN8). A
  title consisting only of U+00A0 / U+2000–U+200A / U+3000 is rejected; an
  interior NBSP is preserved (67-D2).
* **67-I9** The auto-title writer (`derive_auto_title`, reached via
  `SessionManager::maybeAutoName` → `Session::appendAutoRename`) applies the same
  Unicode trim: a first user turn consisting only of U+00A0/U+3000 derives **no**
  title (`std::nullopt`, fail-soft per 19/RN8) and the session keeps its
  placeholder title. **Scope:** the two rename-adjacent writers governed here —
  `/rename` (`normalize_title`) and the auto-title path — cannot store a
  whitespace-only title. Creation-time titles are separate inputs owned by 19/25:
  `session.create`'s `title` param (`host_runtime.cpp`) and headless
  `ymh run`'s `first_line(task, 60)` (`headless.cpp`) are stored as given,
  including whitespace; they are **out of scope** for this errata's whitespace
  rule (recorded, not silently claimed closed).

## 6. Failure modes

* **67-F1 (F1 drop)** — a valid title is never silently shortened in storage;
  only its header rendering is ellipsized.
* **67-F2 (F7 wrong action)** — an empty argument never reaches the wire; it
  produces the usage notice only.
* **67-F3 (F1 drop)** — a rejected rename is never silent: the reply lambda
  routes it through the existing `ErrorOccurred` path.
* **67-F4 (F10 layout)** — a 120-byte title cannot overflow the header or
  displace the workspace title.
* **67-F5 (F7 wrong action)** — ASCII-only trimming would accept a whitespace-only
  NBSP title and store it, so a rename could blank the header. Forbidden by
  67-D3/67-D7/67-I8; 67-U8/U9 and `SessionTitle.NormalizeTrimsUnicodeWhitespace`
  pin the fix (they fail pre-fix: the NBSP argument reached the wire).
* **67-F6 (F7 wrong action)** — the auto-title path could derive an NBSP-only
  title from a first turn of only U+00A0, bypassing `normalize_title`. Forbidden
  by 67-D7/67-I9; `SessionTitle.DeriveAutoTitleFirstLineTrimCollapse` and
  `SessionManagerAutoName.NonPlaceholderAndEmptyPromptSuppress` pin the fix (they
  fail pre-fix: the derived title was the NBSP).

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
* **67-D7** The whitespace rule is the Unicode `White_Space` property, shared by
  the registry trim, `normalize_title`, **and the auto-title `derive_auto_title`**
  via `ymh::trim_unicode_whitespace`; LF is excluded so newline titles stay
  rejected. See §4.2/67-I8/67-I9.

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
* **67-U8** `Errata67Rename.NonBreakingSpaceOnlyArgIsUsageWithNoWireCall`
  (`tests/unit/errata67_ui_test.cpp`) — `/rename` + U+00A0 makes no wire call and
  shows `usage`. Fails pre-fix (ASCII trim leaves the NBSP, the call is sent).
* **67-U9** `Errata67Rename.NonBreakingSpaceAroundTitleIsTrimmed` — leading and
  trailing U+00A0 are stripped, an interior NBSP survives. Fails pre-fix (the
  transmitted title keeps the surrounding NBSP).
* `SessionTitle.NormalizeTrimsUnicodeWhitespace` (`tests/unit/session_test.cpp`,
  extends the former `NormalizeTrimsOnlyThePinnedSet`) — U+00A0-only and U+3000
  are rejected; leading/trailing NBSP and U+2003 are trimmed; interior NBSP is
  preserved; LF is still rejected. Fails pre-fix (NBSP-only does not throw).
* `SessionTitle.DeriveAutoTitleFirstLineTrimCollapse` +
  `SessionManagerAutoName.NonPlaceholderAndEmptyPromptSuppress` (extended) — a
  first turn of only U+00A0/U+3000 derives no auto title and leaves the
  placeholder; leading/trailing NBSP is trimmed, interior NBSP survives. Fails
  pre-fix (67-I9: the derived title was the NBSP).

## 10. Non-goals

* A modal rename editor (19 §11 rule 5 already rejects it).
* Renaming a workspace, project, or the binary (the user semantics are
  session-only; the project name `ymh` is frozen).
* Changing `kMaxSessionTitleBytes` (120), the storage rule, or any validator
  other than the shared whitespace trim (67-D7).

## 11. Revision log

| Date | Revision | Note |
|---|---|---|
| 2026-09-29 | Rev 1 (draft) | Header truncation pinned (67-D1); `/rename` edge-case semantics restated and covered end-to-end (67-D2..D5); tests U1–U6. |
| 2026-09-29 | Rev 2 (draft) | Oracle round 1 findings applied: L1 citations re-derived; L5 test-scope honesty (U1 width guard, U5 error-rendering not validation, U2 model-only); L6 cwd qualification in 67-I1/I2. |
| 2026-10-01 | Rev 3 (draft) | Independent verification found the whitespace rule was ASCII-only, so `/rename` + U+00A0 stored a whitespace-only title. 67-D7/67-I8/67-F5: the rule is now the Unicode `White_Space` property shared by `normalize_title` and the registry trim via `ymh::trim_unicode_whitespace`; LF stays excluded. Oracle gate round 1 (0 HIGH / 1 MEDIUM: the auto-title writer `derive_auto_title` bypasses `normalize_title`) closed by 67-A6/67-D7/67-I9/67-F6: that path now applies the same trim and returns no title for an NBSP-only first turn. Adds 67-A4/A5/A6 and tests 67-U8/U9 + `SessionTitle.NormalizeTrimsUnicodeWhitespace` + the derive/auto-name NBSP cases (fail pre-fix). |
