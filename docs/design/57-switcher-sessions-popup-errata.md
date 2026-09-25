# 57 — Switcher & Sessions Popup Errata (Ctrl-S / `/sessions`)

```
Status: verified (Rev 1) · GATED — Oracle's fifth and final independent pass
        returned **PASS** with zero open HIGH/MEDIUM and states the spec may be
        marked `verified`; the residual LOWs are non-blocking and recorded (§15).
        This is an errata; coding follows AGENTS.md "The rule".
Component: 57 (errata) — amends 22-switcher-sessions-errata.md (verified) and
           51-reasoning-fold-prompt-emphasis-and-tables-errata.md (verified) by
           reference only; it does not rewrite them.
Depends on: 00-architecture.md §20.24/§54 (F1–F12 :4832-4856), 10-supervisor-tui.md
            §7, 16-daemon-ownership.md (verified), 22-switcher-sessions-errata.md
            (verified), 51-reasoning-fold-prompt-emphasis-and-tables-errata.md
            (verified), 48-ui-and-config-errata.md (EscArm, verified),
            UI_SURFACE_INVENTORY.md Surface 3.
Scope: five user-requested changes on the two shared switcher overlays
       (`SwitcherSource::Live` = Ctrl-S, `SwitcherSource::History` = `/sessions`):
       (A) drop the frozen "captured Ns ago" footer segment; (B) a persistent
       Ctrl+D delete hint; (C) render the two-press confirmation inline on the
       target's own row (red background) instead of appending a row; (D) pin
       popup key ownership so Ctrl+Q inside a popup never exits; (E) order the
       effective-root workspace first, then the rest by last usage.
Supersedes: 22-D1's **workspace-ordering** half — `22-switcher-sessions-errata.md`
            §3.7 :646-649 ("Workspace entries (both sources). `title` ascending,
            case-insensitive … `canonical_path` ascending"), §4.3 :938-940
            ("Workspace entries in both sources are ordered by `title` ascending
            … tie-broken by `canonical_path` ascending (22-D1)"), and SW17
            :1587 ("Both switcher sources order workspace entries by `title`
            ascending (case-insensitive), tie-break `canonical_path`
            ascending"); the docs summary `AGENTS.md` :111-112 ("ordered by title
            ascending, case-insensitive, `canonical_path` tie-break") and
            `UI_SURFACE_INVENTORY.md` :67-68/:85-86. 22-D1's **session-ordering**
            half is RETAINED (Live = daemon `session.list` order; History =
            `updated_at` desc, `id` asc). Also supersedes the History footer text
            at `22` §4.3 :936-937 ("`stored sessions · captured <relative> ago
            [· partial] · …`") and **SW19** (`22-switcher-sessions-errata.md`
            :877-880 prose, `:1589` table): the `captured <relative> ago` segment
            is removed and its carrier `capturedAtMs` is deleted (57-D1). SW19 is
            **narrowed** to the `partial` marker — the "stale snapshot displayed
            with a footer marker" clause now holds only when `complete == false`
            (`22` :936-937, retained for `partial`). Also declares the
            **transient** deviations 57-D3 makes while a row is armed: the
            History session-leaf metadata (`· kind · model · age · fork←…`) at
            `22` §4.3 :933-934, the **Live** session-leaf metadata (the
            `[<title> <state glyph>[!]]` format, `22` §4.3 :934-935,
            `ui_render.cpp:960-964`) and the workspace row tail at `22` §4.3
            :929-932 are suppressed on the armed target's own row for the arm
            window only and return on disarm (57-I7). Also
            supersedes the stale comment at
            `include/ymh/ui/session_catalog.hpp:57` ("the renderer sorts by title
            (22-D1)") — ordering is model-only (57-I13) and the rule is 57-D5.
Amends:     `51` §6.3 point 3 (`docs/design/51-…:838-848`, which pins the
            appended hint `- one more Ctrl+D to delete` and the arm semantics)
            and its golden `UI51_D4_SwitcherDeleteHintGolden`
            (`tests/unit/ui_render_golden_test.cpp:2379-2414`, `51-…:1153`): the
            two-press arm/confirm SEMANTICS are retained verbatim; only the
            confirmation RENDERING moves from an appended row to an inline
            same-line red badge (57-D3), and the footer key hint gains `Ctrl+D
            delete` (57-D2). `51-I21` (`51-…:1010`) pins arm state, not
            rendering, so it is **not** amended (Oracle-§6). Amends the exact
            workspace-delete hint text ("workspace and its N sessions", shipped
            at `src/ui/ui_render.cpp:997-998`) to the inline `(N sessions)`
            suffix on the same row (57-D3). The 51 clause it belongs to is `51`
            §6.3 **point 5** (`docs/design/51-…:881-894`; the phrase `delete
            workspace "alpha" and its 3 sessions?` is at `:891-892`); `51-D4.5`
            (`51-…:985`) is only the code comment naming the cascade and is
            retained. The junction count source (`51-D4.5`/`51-L3`,
            `51-…:985-987`) is retained. Amends the stale `ExitConfirmState`
            comment at `include/ymh/ui/ui_model.hpp:523-524` ("reachable through
            an explicit exit action (Ctrl-D / `/exit`)") — exit is Ctrl+Q per
            `51-D4`. Amends the stale `SessionCatalogModel::nowMs` doc comment at
            `include/ymh/ui/ui_model.hpp:551-554`, whose first clause still says
            `nowMs` exists "so the pure renderer can derive `captured <relative>
            ago`" — that label is removed by 57-D1; the replacement comment keeps
            only the per-session relative-age role (`nowMs` itself is retained,
            57-I2).
Retained:   the single-overlay design (`SwitcherOverlayModel`, `22-D3`), the
            live-only Live predicate (22 §3.1), the History disk catalog (22 §4),
            the `delete_arm`/`delete_target`/`delete_target_session_count` fields
            and the `kEscArmTimeout` (3 s) window (`src/ui/supervisor.cpp:54`,
            `51-D4.3`), the per-session relative-age leaf
            (`src/ui/ui_render.cpp:887-888`), and `begin_exit(true)` as the Ctrl+Q
            global binding in Conversation mode (`51-D4`).
```

**Naming.** Invariants local to this spec are **`57-I1`–`57-I20`**; failure modes
are **`57-F1`–`57-F14`** (tagged with the `00` §54 F1–F12 findings; **`—`** where
no §54 finding applies, as `51` does at `51-…:386-387`/`:712-719`); decisions are
**`57-D1`–`57-D7`** (`57-D7` records a user decision). Local IDs `57-F10`/`57-F11`
share a number with the real `00` §54 `F10`/`F11` but are tagged `—` and are
scoped by the `57-` prefix (same scheme as `51`); this is readability only
(Oracle-N8). Prefixes `SW` (22), `O` (16), `U-RB*` (17), and the 45/46/48/49
local prefixes are taken; `57-` is unused.

**No new RPC, no new persistence (pinned).** All five changes are supervisor-local
model + renderer work. `/sessions` keeps reading disk through the existing catalog
(`SessionCatalogReader`); no wire method, no registry schema change, no session-store
write is introduced.

---

## 1. Purpose

Five user-reported defects/requests on the two shared overlays. This spec verifies
each against the shipped code with `file:line`, records the finding, and pins the
replacement behaviour, interfaces, invariants, failure modes, and tests. It is an
**errata**: it names every superseded/amended clause with a `file:line` anchor and
gives its replacement here; a clause is quoted verbatim only where the replacement
must preserve exact text (the arm semantics and the user's badge string).

---

## 2. Current behaviour (verified, not trusted from the brief)

### 2.1 The two overlays are one model + one renderer

The Ctrl-S (Live) and `/sessions` (History) surfaces are the **same**
`SwitcherOverlayModel` distinguished by `SwitcherSource` (`include/ymh/ui/ui_model.hpp:381-384`,
`:415-449`), opened by `UiModel::openSwitcher()` (`src/ui/ui_model.cpp:1186-1191`,
`source = Live`) and `SupervisorApp::open_sessions()` (`src/ui/supervisor.cpp:964-973`,
`source = History`), and rendered by the single `render_switcher`
(`src/ui/ui_render.cpp:896-1005`). Confirmed against `22-D3` and
`UI_SURFACE_INVENTORY.md:76-79`.

### 2.2 (A) The "captured Ns ago" segment is dead

The History footer is built at `src/ui/ui_render.cpp:977-988`; the segment is
`:979-983`:

```cpp
if (model.catalog.capturedAtMs > 0 && model.catalog.nowMs > 0) {
    footer += " · captured " +
              relative_age_label(model.catalog.nowMs - model.catalog.capturedAtMs) +
              " ago";
}
```

- `catalog.capturedAtMs` is set on the reader thread at snapshot build time
  (`src/ui/session_catalog.cpp:222`).
- `catalog.nowMs` is reassigned on **every** catalog delivery to the UI-thread
  wall clock (`src/ui/supervisor.cpp:950`); it is **not** "set once". The reader
  rebuilds on its cadence and on `refreshNow()` (`session_catalog.cpp:238-252`,
  default 15 s, `22` :873), and `on_catalog_snapshot` runs on each delivery while
  the popup is visible (`supervisor.cpp:926-931`, `:942-960`). It is read at
  `src/ui/ui_render.cpp:887` (per-session leaf) and was read at `:979` (the
  removed segment).

**Finding:** both timestamps are reassigned from the **same** snapshot at each
delivery, so the rendered delta is the reader→delivery queue latency, never the
snapshot's staleness. In practice it is `0s` (occasionally 1–2 s on a busy UI
thread) and it **never advances** while the popup is idle: a hung reader
suppresses delivery entirely (`supervisor.cpp:926` only enqueues when the key
changes or the popup is visible; the loop blocks in `build()`,
`session_catalog.cpp:238-252`), so the label cannot tick. This matches the
user's "it appears doing nothing anyway". It is **not** the snapshot's staleness,
and it is **not** duplicated elsewhere (the per-session `· <relative>` leaf at
`:887-888` is a different quantity — a session's last-update age). It conveys no
information. Disposition: **remove it and delete its carrier** (57-D1). The
information it *intended* to convey (staleness) cannot be shown without a ticking
model clock — the **renderer** reads no clock and derives every relative age from
the model's frozen `nowMs` (`ui_render.cpp:887-888`;
`include/ymh/ui/ui_model.hpp:552-553`, `10` U3/D16, `F12`). That is recorded as
an explicit non-goal, not a silent drop.

### 2.3 (B) No delete hint in the popup footer

The two footers are `src/ui/ui_render.cpp:987` (History:
`stored sessions · r refresh · Enter resume · Esc close`) and `:990` (Live:
`j/k move · Tab expand · Enter focus · Esc close`). Neither mentions Ctrl+D, so
the delete key is undiscoverable. The only Ctrl+D text is the transient armed hint
(`:995`), which is gone until a first press.

### 2.4 (C) The confirmation is an appended row

The two-press arm already exists (`51-D4.2/D4.3`): `handle_switcher` routes Ctrl+D
to `handle_switcher_delete` (`src/ui/supervisor.cpp:2765-2769`, `:2817-2849`); the
arm state lives in `SwitcherOverlayModel` (`include/ymh/ui/ui_model.hpp:423-433`);
disarm is `ui_model.cpp:1423-1428`; timeout is the 3 s tick
(`supervisor.cpp:54`, `:1920-1926`). But the confirmation is rendered by **pushing
an extra row** onto the window:

```cpp
if (switcher.delete_arm == EscArm::Armed) {                 // ui_render.cpp:994
    std::string hint = "  - one more Ctrl+D to delete";      // :995
    ...
    rows.push_back(ftxui::text(hint) | ftxui::dim);          // :1000
}
```

Every session row is a single `ftxui::text(...)` element pushed once
(`src/ui/ui_render.cpp:951-974`, leaf built at `:957`/`:965`, pushed at `:973`),
so a row is one line high. Adding a `rows.push_back` grows the `ftxui::window`
(`:1002-1004`) by one line → the user's "additional line added to popup window".
The golden `UI51_D4_SwitcherDeleteHintGolden`
(`tests/unit/ui_render_golden_test.cpp:2379-2414`) asserts the appended-row string.

### 2.5 (D) Key-dispatch order — the popup already owns Ctrl+Q

`SupervisorApp::handle_event_inner` (`src/ui/supervisor.cpp:3390-3462`) dispatches
in this exact order:

| # | Condition | Handler | Line |
|---|---|---|---|
| 1 | `Event::Custom` | `drain()` | `:3391` |
| 2 | `model_.exitConfirm.open` | `handle_exit_confirm` | `:3395` |
| 3 | `model_.dialog.open` | `handle_dialog` | `:3398` |
| 4 | `mode == Context && context.open` | `handle_context` | `:3402` |
| 5 | `mode == Notice && message.open` | `handle_notice` | `:3405` |
| 6 | `mode == Switcher` | `handle_switcher` | `:3408` |
| 7 | `mode == ModelPicker && visible` | `handle_model_picker` | `:3411` |
| 8 | `CtrlS`/`CtrlP` | `openSwitcher()` | `:3414` |
| 9 | `CtrlQ` | `begin_exit(true)` | `:3422` |
| 10 | `CtrlC` | `cancelActive()` | `:3426` |
| 11 | `CtrlN` | `new_session()` | `:3430` |
| 12 | `CtrlO` | `toggle_folds()` | `:3434` |
| 13 | `PageUp`/`PageDown` | `scroll_by(…)` | `:3438`/`:3442` |
| 14 | `Ctrl+Home`/`Ctrl+End` | `scroll_to_top/bottom()` | `:3446`/`:3449` |
| 15 | `Shift+Up`/`Shift+Down` | `scroll_by(true,false)` / `scroll_by(false,false)` | `:3454`/`:3458` |
| 16 | (tail fallback) | `handle_input` | `:3462` |

`handle_switcher` (`:2765-2814`) consumes **every** event and returns `true`
unconditionally (`:2814`). Ctrl+Q matches none of its branches — CtrlD `:2767`,
Escape/CtrlC `:2775`, ArrowDown/j `:2781`, ArrowUp/k `:2785`, Tab `:2789`, `r`
`:2793`, Return `:2800`. Every other popup handler also returns `true`
unconditionally (`handle_notice` `:1069`, `handle_dialog` `:2674` (return
`:2719`), `handle_model_picker` `:1225`, `handle_context` `:3376`,
`handle_exit_confirm` `:773-801`, return `:800`).

**Finding (verified):** an open popup is dispatched **before** the global Ctrl+Q
binding, and it consumes Ctrl+Q. On this HEAD, Ctrl+Q inside the Ctrl-S or
`/sessions` popup **cannot** call `begin_exit` and **cannot** tear down the
application. The user's symptom ("Ctrl+Q exits without popup window") is **not
reproducible on this HEAD** through the switcher path.

**Disposition (57-OQ1 closed by user decision, 2026-09-25).** I could not
reproduce the report in code; the only production Ctrl+Q handler is
`src/ui/supervisor.cpp:3422` (`grep -rn CtrlQ src/ include/` matches only it and
the test-harness dispatch at `:3856`), and the popup-first dispatch is real
(`handle_event_inner` `supervisor.cpp:3390-3422`). The user has confirmed that the
code is correct and that the invariant plus regression test lock it in, so 57-OQ1
is **closed** and no dispatch change is made. 57-I10 pins the behaviour and
57-H1/57-H4 add the missing regression coverage. Plausible explanations of the
original symptom, none confirmed: an older build predating the overlay-first
dispatch; the popup was actually the exit-confirm modal (whose Ctrl+Q is also
swallowed, `:3395`); the popup had already closed on a prior key; or the user was
in Conversation mode and Ctrl+Q exited **directly with no prompt** because the
orphaning set was empty — `begin_exit` calls `confirm_exit` without a prompt when
`orphaning.empty()` (`supervisor.cpp:550-553`, `51-L1`, `51` :829-834). That last
path is **by design** (`16` §7.6), not a bug. The correct disposition remains to
**pin the invariant and add the missing regression test**, not to change the
dispatch (57-D4).

### 2.6 (E) Current ordering, and the last-usage source

Both sorters order **workspace groups by `title` asc (lowercased), then path asc**:

- Live: `src/ui/ui_model.cpp:1306-1322` (`left_title < right_title`, then
  `WorkspaceModel::cwd`).
- History: `src/ui/ui_model.cpp:1403-1411` (`lower_ascii(title)`, then
  `WorkspaceHistory::canonicalPath`).

This is `22-D1` (`docs/design/22-switcher-sessions-errata.md:641-676`, `:938-946`,
SW17 `:1587`), summarised in `AGENTS.md:111-112` and
`UI_SURFACE_INVENTORY.md:67-68/:85-86`.

**Last-usage source — verified, with the gap recorded.** There is **no dedicated
`last_used_at` column.**

- The registry `workspaces.updated_at` exists (`src/registry/registry.cpp:49`,
  exposed as `WorkspaceRecord::updatedAt`, `include/ymh/registry/registry.hpp:69`).
  It is bumped by `touch_workspace` (`src/registry/registry.cpp:398-403`) on
  session-junction mutations only (`:1248` register, `:1279` remove, `:1298`
  archive, `:1332` reorder), and separately by a direct `UPDATE … updated_at` in
  `setDisplayTitle` (rename, `registry.cpp:1045-1053`). It is **not** a
  user-usage timestamp (attaching, focusing, or prompting a workspace does not
  touch it).
- The session store `sessions.updated_at` exists (`src/session/session_persistence.cpp:40`)
  and is bumped on every appended event (`src/session/session.cpp:651`, `:716`).
  The catalog already materialises it per session (`SessionHistoryEntry::updatedAt`,
  `include/ymh/ui/session_catalog.hpp:39`) and sorts History sessions by it
  (`src/ui/session_catalog.cpp:76-83`, `src/ui/ui_model.cpp:1393-1399`).

**Disposition (57-D6):** define a workspace's last usage as
`max(session.updatedAt)` over the workspace's **materialized** sessions (i.e. after
the `isUnprompted` filter at `src/ui/session_catalog.cpp:141-144`, so the value
matches the rows the popup actually shows), materialised once by the catalog
reader (`read_workspace_history`, `src/ui/session_catalog.cpp:97`) as a new
`WorkspaceHistory::lastUsedAt`. This needs no schema change. The gaps, all
recorded:

- For a workspace with zero materialized sessions or an unreadable/absent DB,
  `lastUsedAt == 0` (unknown, sorts last — 57-I12).
- The Live source's value is only as fresh as the last catalog snapshot (cadence
  `catalog_refresh_interval`, `include/ymh/ui/supervisor.hpp:60`; Ctrl-S forces a
  refresh at `src/ui/supervisor.cpp:3417-3419`). The first-open settle is 57-F11.
- **Over-ranking (57-F10).** `sessions.updated_at` bumps on *every* appended event
  (`src/session/session.cpp:651`, `:716`), including background-agent events in
  another live daemon, so a busy background workspace can outrank the one the user
  is actually in. **Accepted**, because the effective-root-first rule (57-D5 step 1,
  57-I11) guarantees the user's own workspace is always first regardless, so the
  residual over-ranking affects only the tail. The seam is a new
  `workspaces.last_used_at` written on attach/focus (no such column today,
  `registry.cpp:44-59`) — explicitly **out of scope**; recorded at 57-OQ2.

**Effective-root source — verified (with the `--resume` override stated and
approved).** The "current directory" this spec orders first is the supervisor's
**effective root** = `SupervisorRunOptions::initial_workspace`
(`include/ymh/ui/supervisor.hpp:32`). It is *not* literally `getcwd()`:
`resolve_workspace()` (`src/cli/cli.cpp:154-157`) returns
`std::filesystem::current_path()` only when `--workspace` is absent, and
`run_supervisor_entry` then **overrides** the root with the resumed session's own
workspace when `--resume <id>` is passed
(`resolved_root = resolved->canonicalPath`, `cli.cpp:485-493`); `canonicalize`
(`:393-400`) and `:554` assign it. So under `ymh --resume S` from `/projA`, the
effective root is S's workspace (`/projB`) and 57-I11 pins **projB** first, not the
shell cwd; likewise `--workspace X` (`cli.cpp:107`) pins X. This is deliberate:
the effective root is where the user asked to operate and the path the daemon
targets (path-safety: the supervisor never uses `getcwd()` as a resolution base).
The consequence is stated in 57-D5, and the user has **approved** effective-root
precedence under `--resume` (decision **57-D7**, 2026-09-25): requirement 5 holds
verbatim for bare and `--workspace` launches; under `--resume` the effective root
deliberately takes precedence over the literal shell cwd. It is distinct from
`UiModel::activeWorkspaceId`, which follows focus (`src/ui/supervisor.cpp:417-425`
only *seeds* it at startup). The Live node's path is `WorkspaceModel::cwd`
(canonical; `ui_model.hpp:319`, set at `supervisor.cpp:822`/`:1514`); the History
node's is `WorkspaceHistory::canonicalPath` (`session_catalog.hpp:50`). So a
canonical-path match is available in both sources.

---

## 3. Decisions

### 57-D1 (A) — Remove the "captured Ns ago" footer segment

Remove `src/ui/ui_render.cpp:979-983` from the History footer. The footer becomes:

```
stored sessions · [partial ·] r refresh · Ctrl+D delete · Enter resume · Esc close
```

- **Delete the `capturedAtMs` carrier** (R4-F2: once the renderer stops reading
  it, it is write-only dead state). Remove the field from `SessionCatalogModel`
  (`include/ymh/ui/ui_model.hpp:559`) and from `SessionCatalogSnapshot`
  (`include/ymh/ui/session_catalog.hpp:62`), its two production writes
  (`src/ui/session_catalog.cpp:222`, `src/ui/supervisor.cpp:949`), and its test
  fixtures/assertions (`tests/unit/ui_render_golden_test.cpp:1377`, `:1503`;
  `tests/unit/supervisor_harness_test.cpp:1318`;
  `tests/unit/session_catalog_test.cpp:640`). No dangling **state**; the stale
  `nowMs` doc comment that still names the removed label is amended separately
  (`include/ymh/ui/ui_model.hpp:551-554`, §8).
- `SessionCatalogModel::nowMs` is **retained** and still read by the per-session
  age leaf (`ui_render.cpp:887-888`), which is out of scope (57-I2).
- **Rationale:** the segment is frozen and meaningless (§2.2). Showing a true
  snapshot age would require a ticking model clock (`F12`); that is a separate
  feature, not a silent omission. This is a deliberate drop of a dead label, with
  the intent recorded here.

### 57-D2 (B) — Persistent Ctrl+D delete hint

Append `Ctrl+D delete` to both footers (`ui_render.cpp:987`, `:990`). Exact rendered
segments:

- History: `stored sessions · [partial ·] r refresh · Ctrl+D delete · Enter resume · Esc close`
- Live: `j/k move · Tab expand · Ctrl+D delete · Enter focus · Esc close`

The label is `Ctrl+D delete` (the popup's existing key-label casing, `51-D4`); the
user's lowercase `ctrl+d to delete` denotes the same affordance. The badge text
(57-D3) keeps the user's **verbatim** `press Ctrl+d again to confirm` including its
lowercase `d`; the footer uses the repo's `Ctrl+D`. The two labels denote the same
key — this is a **deliberate literal deviation** (the footer uses the repo's
`Ctrl+D`; the user string is preserved verbatim on the badge), reasoned and
intentional, not an inconsistency (Oracle-N10). The hint is present whether or not
a row is armed.

- **Live scope note (`51-I23`).** In the Live source, Ctrl+D on a **workspace**
  target is always refused (a workspace delete is reachable only for a non-running
  workspace, `51-I23`, `51-…:1012`); only session targets delete. The hint is still
  correct because every Live row is a live workspace whose session targets delete
  normally; the refusal path surfaces a notice (`51-I25`, `51-…:1014`). The footer
  advertises the key, not a guarantee for the workspace node.

### 57-D3 (C) — Inline same-line red confirmation

Replace the appended row (`ui_render.cpp:992-1001`) with an inline badge on the
armed target's **own** row:

- **Session target** (`delete_target->session.has_value()`): the row renders
  `    [<name>` followed by the red badge; the normal trailing metadata
  (`· kind · model · age · fork←…` for History; the state glyph/`!` for Live) is
  **suppressed while armed** so the line stays one row and its width stays bounded
  (the transient `22` §4.3 :933-935 deviation declared in the Supersedes header;
  it returns on disarm).
- **Workspace target** (no session): the workspace row renders its shipped prefix
  (`collapsed ? "+ " : "- "`, `ui_render.cpp:922`/`:924`) plus `<title>`, followed
  by the red badge, then a dim `(N sessions)` suffix carrying
  `delete_target_session_count`. The normal tail (`<glyph> [<mark>]` for Live;
  `<glyph> [owned]`/`· [history]` for History, `22` §4.3 :929-932) is **suppressed
  while armed** and returns on disarm. This amends the exact text "workspace and
  its N sessions" (shipped at `src/ui/ui_render.cpp:997-998`) to the inline
  `(N sessions)` suffix; the junction-count source is unchanged.
- **Badge text (exact):** `press Ctrl+d again to confirm`.
- **Badge style and the colour gate (R1-L1).** The badge obeys `theme.color`; it
  does **not** hardcode ANSI. When `theme.color == true` it is
  `ftxui::bgcolor(ftxui::Color::Red) | ftxui::color(ftxui::Color::White) |
  ftxui::bold`; when `theme.color == false` it falls back to
  `ftxui::inverted | ftxui::bold` (reverse video, no colour) so it is still
  distinguishable. It deliberately does **not** go through `paint_bg`
  (`ui_render.cpp:40-45`), whose `theme.user_block` check is unrelated to the
  popup. The badge is a **separate `hbox` child** and is **never** passed through
  `ftxui::inverted` by the row highlight — the highlight is applied to the
  name/prefix child only (next bullet), so White-on-Red survives.
- **The row highlight applies to the name text only (R1-H2).** `inverted` is
  reverse video (`ftxui::dom/inverted.cpp:20-26`, `screen/screen.cpp:104-106`);
  applying it to the whole `hbox` would swap the badge's explicit fg/bg and turn
  White-on-Red into Red-on-White. The armed row is therefore built as
  `hbox({ name_element, text(" "), badge })`, where `name_element` (not the hbox)
  carries the highlight: `| ftxui::inverted` for a session target
  (`ui_render.cpp:967-969`) and `paint(…, Color::Cyan) | bold` for a workspace
  target (`ui_render.cpp:928-929`). The badge is never the decorated element.
- **Width and height are both invariant (R1-H1/Oracle-H2/N2/N3).** The badge is
  an `hbox` child of the existing row, so the `ftxui::window` (`:1002-1004`) gains
  **no row** (57-I5). The window is **also** width-invariant: `render_switcher`
  pins the window **content** with
  `window(title, vbox(rows) | ftxui::size(ftxui::WIDTH, EQUAL, w)) | clear_under |
  center`, where `w` is computed once from the **disarmed** rows plus a **fixed,
  state-independent** reserve:

  ```
  rows                   = switcher_disarmed_rows(model)        // one builder, §4
  footer                 = the Footer-kind row of rows
  max_disarmed_row_width = max string_width(row.text) over the non-Footer rows
  computed               = max(string_width(footer), max_disarmed_row_width,
                               kDeleteBadgeReserve)
  kDeleteBadgeReserve    = 5 + 1 + kDeleteBadgeWidth + 1 + kDeleteSuffixReserve
                           // 5 == string_width("    ["); both target shapes
  kDeleteBadgeWidth      = 29    // string_width("press Ctrl+d again to confirm")
  kDeleteSuffixReserve   = 17    // string_width(" (99999 sessions)")
  ```

  **The `size` decorator is applied to the window content, not to the window.**
  This is load-bearing and was settled by measurement (round-3 re-check, §13):
  `window(...) | size(WIDTH, EQUAL, w) | clear_under | center` — the shape this
  spec previously pinned — makes the **centered** `center` decorator
  (`composite_decorator.cpp:15-32`: `hbox(filler(), vbox(filler(), child,
  filler()), filler())`) hand the `Size` node a box only `w` wide, so the border
  eats the 2 columns and the **total** is `w` while the **content** is `w - 2`.
  Applying the decorator to the content instead lets the window auto-size to the
  content and makes the **content** `w` and the **total** `w + 2`. Measured on
  FTXUI: `w=20` ⇒ content 20 / total 22; `w=70` ⇒ content 70 / total 72 (the
  70-col History footer then fits exactly). The opposite reading holds only for the
  bare `window | size` form without `center`, which is not the shipped pipeline.

  `kDeleteBadgeReserve` is a **fixed runtime `const int`** (not `constexpr`:
  `ftxui::string_width` is a runtime function, `ftxui/screen/string.hpp:19`); it is
  **not** derived from `delete_arm`, the cursor, or the armed target's junction
  count. Because `w` never depends on `delete_arm`, the centered window (`:1004`)
  cannot widen or shift on arm (57-I16). This is chosen over plumbing every
  workspace's junction count into the model: the count is read only for the armed
  target (`arm_workspace_delete`, `supervisor.cpp:2855-2866`,
  `registry->listSessions` `:2861`) and is absent from
  `WorkspaceModel`/`WorkspaceHistory`; making every row's armed width knowable
  would need a new registry read per workspace and would change the count's source
  (51-L3 pins the junction set, not the model's session list). The fixed reserve
  keeps the change renderer-only. Counter-example prevented: workspace A with 2
  junctions and B with 1000 arm to the same `w`, because neither count feeds `w`.
  Without this pin the auto-sized window (`ftxui/dom/border.cpp:507`) grows when the
  armed row is the widest and re-centers.
- **Badge min-width / ellipsize and the width clamp (R1-M2/N4, R4-N2, R2-M1,
  R4-M1/M2).** The badge has a pinned width `kDeleteBadgeWidth = 29` and is
  **never** ellipsized. Every other armed-row segment is ellipsized with the
  existing `ellipsize_text` (`ui_render.cpp:517-545`) to its budget. Two terms
  must be named separately (R3-LOW-3): the **`fixed_prefix`** is the row marker
  that is *never* ellipsized — `"    ["` (5 cells) for a session target
  (`ui_render.cpp:957`) or `"- "`/`"+ "` (2 cells) for a workspace target
  (`ui_render.cpp:922`/`:924`) — while the **`name_element`** is the whole
  caller-built `fixed_prefix + <name>` (§4). Only the *name* after the
  `fixed_prefix` is ellipsized, to the budget `w - (string_width(fixed_prefix) +
  1 + kDeleteBadgeWidth)` for a session; for a workspace the budget **subtracts**
  the suffix too —
  `w - (string_width(fixed_prefix) + 1 + kDeleteBadgeWidth + 1 + suffix_width)` —
  where `suffix_width` is `kDeleteSuffixReserve` **only while the suffix is
  rendered** (`w >= kDeleteBadgeReserve`, 57-I6) and is the suffix's own
  ellipsized width below that threshold, so the 17-cell reserve is **not** charged
  once the suffix has ellipsized to `{}` (otherwise the name is over-truncated for
  `35 <= w < 53`; R4-LOW-2). The clamp accounts for the 2
  border columns: the content is pinned to `w` and the total is `w + 2`, so the
  width actually applied is **`w = max(0, min(computed, available_width - 2))`**;
  the total therefore never exceeds `available_width` **except when the window
  title forces the border wider (the ≤ 10-col exception below)**. Whenever
  `w >= kDeleteBadgeReserve` the fixed parts (`fixed_prefix` + badge [+ suffix])
  fit and only the *name* may still be ellipsized to its remainder (R3-LOW-1) —
  never the badge (57-I20). On a **narrower** terminal the name is ellipsized to
  its remainder (at the bound, to `{}`) and then the workspace suffix ellipsizes
  (to `{}` below `kDeleteBadgeReserve`,
  `ellipsize_text(..., max_width<=0)`, `ui_render.cpp:521-523`); because the
  `fixed_prefix` is **excluded from the name budget** and survives at the bound,
  the badge is fully visible only for
  `w >= string_width(fixed_prefix) + 1 + kDeleteBadgeWidth` — **35** for a session
  (`5 + 1 + 29`) and **32** for a workspace (`2 + 1 + 29`; by `w = 32` the
  `(N sessions)` suffix has already ellipsized to `{}`, so the row is
  `fixed_prefix + " " + badge + " " + {}` = 33 cells and the single overflow cell
  is absorbed by the **trailing separator** that follows the badge — FTXUI's
  `ComputeShrinkHard` shrink, `box_helper.cpp:46-61`; there is no right-edge clip —
  R4 probe B measured the workspace badge full at `w = 32`), where the row is
  `fixed_prefix + " " + badge` (session) /
  `fixed_prefix + " " + badge + " " + <suffix>` (workspace). Below that the badge
  is **not guaranteed fully visible**: FTXUI's `hbox` hard-shrinks *every* text
  child when over-constrained rather than right-clipping, so the `fixed_prefix` is
  not preserved below the bound — the badge is partially clipped for `3 <= w <`
  that bound (session `3..34`, workspace `3..31`) and fully clipped only for
  `w <= 2` (both; measured: session `w=6 → 4/29` badge cells, `w=3 → 1/29`,
  `w=2 → 0/29`)
  (R2-MEDIUM-1, R4-N2, R1-NEW-4). This is a documented narrow-terminal bound, not
  a violation of the pinned width (57-F13). The fixed `w` guarantees arming adds
  no new clipping. (The window title, ≤ 10 cols, can force the border wider than
  `w + 2` only on terminals narrower than the title; part of the same bound.)
- **Disarm conditions (all already implemented; pinned as invariants):** any key
  other than Ctrl+D (`supervisor.cpp:2770-2774`); a cursor move (same path, plus
  the `delete_target == cursor` confirm guard `:2828-2829`); popup close
  (`SwitcherOverlayModel::close` → `disarm_delete`, `ui_model.cpp:1416-1421`);
  reopen (`open`/`openHistory` → `disarm_delete`, `:1241`/`:1329`); timeout
  (`kEscArmTimeout` = 3 s, `supervisor.cpp:54`, `:1920-1926`); a delete that
  actually confirms (disarm first, `:2831`). A terminal resize is **not** a disarm
  trigger: the arm survives a resize until the 3 s tick (`supervisor.cpp:1920-1926`),
  which is benign (the target stays valid) and is stated here as a deliberate
  omission from the disarm list (R1-L2).
- **Rationale:** the user was explicit that the confirmation must not be an extra
  line and must sit on the session-name line with a red background; this is a
  renderer-only change that preserves the existing arm state machine.

### 57-D4 (D) — Popup key ownership (Ctrl+Q inside a popup is a consumed no-op)

**Verified truth:** popups already own their keys; `handle_switcher`
(`supervisor.cpp:2765-2814`) consumes Ctrl+Q before the global binding
(`:3408-3409` vs `:3422-3424`). Pin the invariant (57-I10) and add the missing
regression test; **change no dispatch code.**

- **Exact per-modal guards (R3-L1).** Ctrl+Q is consumed iff one of these
  predicates holds, evaluated in order in `handle_event_inner`
  (`supervisor.cpp:3395-3411`):

  | # | Guard | Line | Handler |
  |---|---|---|---|
  | 1 | `model_.exitConfirm.open` | `:3395` | `handle_exit_confirm` |
  | 2 | `model_.dialog.open` | `:3398` | `handle_dialog` |
  | 3 | `mode == UiMode::Context && model_.context.open` | `:3402` | `handle_context` |
  | 4 | `mode == UiMode::Notice && model_.message.open` | `:3405` | `handle_notice` |
  | 5 | `mode == UiMode::Switcher` (**mode only** — the overlay has no `open` flag; its open state IS the mode, `ui_render.cpp:1446`) | `:3408` | `handle_switcher` |
  | 6 | `mode == UiMode::ModelPicker && model_.model_picker.visible` | `:3411` | `handle_model_picker` |

  While a guard is true, Ctrl+Q is **consumed and is a no-op**: it does not exit
  and does not close the popup. Every guard's handler returns `true`
  unconditionally (`:2814`, `:1069`, `:1225`, `:3376`, exit-confirm `:773-801`,
  return `:800`).
  Esc/Ctrl+C remain the close keys (`handle_switcher` `:2775-2780`;
  `handle_notice` `:1063-1068`; etc.). A stale `mode == UiMode::Switcher` with no
  rendered overlay would also swallow Ctrl+Q (guard 5 is mode-only); no path
  creates that today — every open/close pairs mode and overlay (`:967`, `:2777`,
  `:2810`, `:2996`, `ui_model.cpp:1189`) — so it is latent only.
- When **no** guard is true (Conversation), Ctrl+Q keeps `begin_exit(true)`
  (`supervisor.cpp:3422-3424`, `51-D4`, `51-I19`). This binding is not touched.
- **Other globals are swallowed too (57-I15).** The guards precede the global
  bindings, so Ctrl+S/P (`:3414`), Ctrl+Q (`:3422`), Ctrl+N (`:3430`), Ctrl+O
  (`:3434`), PageUp/Down (`:3438-3445`) and Ctrl+Home/End (`:3446-3453`) are all
  unreachable while a popup is up. 57-I15 pins this.
- **Rationale:** satisfies the user's requirement (popup must not tear the app
  down) without weakening the global exit binding when no popup is open. Adding
  Ctrl+Q as a popup *close* key is rejected — it is not requested and would
  duplicate Esc/Ctrl+C.
- **Amendment:** fix the stale comment at `ui_model.hpp:523-524` (`Ctrl-D` →
  `Ctrl-Q`) so the documented exit key matches `51-D4`.

### 57-D5 (E) — Order: effective root first, then last usage desc

Replace the workspace-group comparator in **both** `open` and `openHistory`
(§2.6) with:

1. **Effective-root workspace first.** The node whose canonical path equals
   `UiModel::cwdWorkspacePath` (new field, 57-D6) sorts before all others.
   For Live compare `WorkspaceModel::cwd`; for History compare
   `WorkspaceHistory::canonicalPath`. The rule fires only when
   `!cwdWorkspacePath.empty()` **and** a node's path is non-empty and equal
   (the `!=` form: `if (left_cwd != right_cwd) return left_cwd;`), so a stray
   empty-path node is never pinned first (R2-LOW-1). If the effective-root
   workspace is absent (not registered, filtered out, or not live) the rule
   simply does not fire.
2. **Then `lastUsedAt` descending (most recently used first).**
   **Direction and justification:** descending matches the existing History
   session order (`updated_at` desc, `ui_model.cpp:1393-1399`, SW17) and the
   "resume what I touched last" intent; a `lastUsedAt == 0` (unknown) node sorts
   after every dated node because 0 is the smallest key.
3. **Tie-break (shared timestamp):** `title` ascending, case-insensitive
   (`lower_ascii`), then `canonical_path` ascending — i.e. the retained half of
   `22-D1` (`ui_model.cpp:1300-1305` `lower`; `:1403-1411`).

- **"Current directory" means the effective root (user-approved, 57-D7).** Per
  §2.6, `cwdWorkspacePath` is `options_.initial_workspace`, which is the
  canonicalised `--workspace` root or the resumed session's workspace, **not** the
  shell cwd. Consequence: `ymh --resume S` from `/projA` (S in `/projB`) renders
  `/projB` first; `ymh --workspace X` renders X first. This is intended (the
  effective root is where the user asked to operate and the path the daemon
  targets) and the user has explicitly chosen effective-root precedence under
  `--resume` (**57-D7**): requirement 5 holds verbatim for bare and `--workspace`
  launches; under `--resume` the effective root takes precedence over the literal
  shell cwd, and this **is** a behaviour change from the literal reading, now
  approved. The rejected alternative (source the field from
  `std::filesystem::canonical(std::filesystem::current_path())` to always follow
  the shell cwd) is recorded here as considered and **not** chosen.
- **Where applied:** in the **model** (`SwitcherOverlayModel::open` /
  `openHistory`), the single source of truth, exactly as today. The renderer
  (`render_switcher`) stays order-free and pure (57-I13).
- **Session order is unchanged:** Live keeps the daemon `session.list` order;
  History keeps `updated_at` desc / `id` asc (22-D1's session half is retained,
  57-I14).
- **Rationale:** the user wants their current directory always reachable first
  and recent work near the top; this is a deterministic total order.
- **Test impact (see §9.5):** SW-U15 (`ui_model_test.cpp:1138`) and SW-U6
  (`:1260`) encode SW17's workspace title order and must be extended to cover
  57-D5; SW-G2 (`ui_render_golden_test.cpp:1386`) is updated by 57-G1.

### 57-D6 — Last-usage and effective-root data (additive fields)

Add fields; no schema change, no wire change:

- `WorkspaceHistory::lastUsedAt` (`session_catalog.hpp:47-54`), epoch ms, set by
  the catalog reader to `max(session.updatedAt)` over the workspace's
  **materialized** (post-`isUnprompted`-filter, `session_catalog.cpp:141-144`)
  sessions (0 if none). The computation is placed **after** the try/catch in
  `read_workspace_history` (not inside the `try` before `sort_sessions`): a
  mid-loop `isUnprompted` throw (`session_catalog.cpp:142`) then leaves a degraded
  `note` with `lastUsedAt == 0`, never a partial value (R2-LOW-3). The degraded
  `note_history` (`session_catalog.cpp:85-93`) sets `lastUsedAt == 0`.
- `WorkspaceNode::lastUsedAt` (`ui_model.hpp:386-406`), copied from the matching
  `WorkspaceHistory` in `open` (`ui_model.cpp:1263-1268`) and in `openHistory`
  (`:1350-1400`).
- `UiModel::cwdWorkspacePath` (`ui_model.hpp`), the canonical path of
  `options_.initial_workspace`, assigned once in `SupervisorApp::run()` near
  `supervisor.cpp:410-426`. Empty when `initial_workspace` is empty (the rule then
  does not fire). It is **not** `activeWorkspaceId` (focus-following). No
  canonicalisation happens at assignment: `cli.cpp:554` assigns `*canonical`, which
  `canonicalize` (`:393-400`) already produced (R2-LOW-4).
- **Live async settle (Oracle-MEDIUM).** Live copies `lastUsedAt` from the current
  `model.catalog` snapshot at `open` (`ui_model.cpp:1263-1268`); Ctrl-S opens
  first (`supervisor.cpp:3415`) then requests `refreshNow()` (`:3417-3419`), and
  the snapshot lands later (`on_catalog_snapshot`, `:942-960`), which calls
  `resnapshot_switcher()` (`:955-957`). If no snapshot has been delivered yet,
  the first Live render is all-zero → title order, then **re-sorts on each
  subsequent catalog snapshot** while the popup is open (`on_catalog_snapshot`
  re-opens/re-snapshots on every delivery, `supervisor.cpp:952-958`), not once.
  This is **not** prevented, deliberately: blocking the UI thread on the reader
  would violate the async-catalog design (`22` §4.2, `SW7`/`SW12`). It is bounded
  (no feedback loop: the re-snapshot never triggers a new reader pass) and never
  loops (57-I19, 57-F11); the effective-root rule keeps the user's own row pinned
  so it never moves.

### 57-D7 (E, user decision) — Effective-root-first under `--resume` is approved

**Decision (user, 2026-09-25).** Requirement 5 — "always show the current
directory first, the rest sorted on last usage date" — is interpreted as
**effective-root first**: the workspace the supervisor is actually operating in
(`options_.initial_workspace`, 57-D6) is pinned first. For a bare launch this is
the shell cwd; for `ymh --workspace X` it is X; for `ymh --resume S` it is S's
own workspace (`cli.cpp:485-493`, `:554`), **not** the literal shell cwd. The user
has approved this precedence explicitly.

- **Behaviour change, stated plainly.** Under `--resume` (and `--workspace`) the
  literal shell cwd is *not* necessarily first. This is a deliberate change from
  the literal reading of requirement 5, approved by the user — not a preservation
  of it. Requirement 5 holds verbatim for bare launches and for `--workspace`
  launches (where the flag names the root the user chose).
- **Rationale.** The effective root is where the user asked to operate and the
  path the daemon targets; the supervisor never uses `getcwd()` as a resolution
  base (path-safety, `00` §18). Pinning the resumed session's workspace first
  matches the resume intent.
- **Rejected alternative.** Source `cwdWorkspacePath` from
  `std::filesystem::canonical(std::filesystem::current_path())` so the shell cwd
  is always first. Rejected: it would put a workspace the user is not operating in
  ahead of the resumed one, and it reintroduces a `getcwd()`-derived base.
- **No open question.** This closes the point Oracle-N1 raised; 57-OQ1 is
  unrelated and also closed (57-OQ1).

---

## 4. C++ interface sketches (signatures only)

```cpp
// include/ymh/ui/session_catalog.hpp
struct WorkspaceHistory {
    // ... existing fields (id, title, canonicalPath, live, note, sessions) ...
    // 57-D6: max(session.updatedAt) over the materialized `sessions`
    // (post-isUnprompted filter); 0 == none/unknown. Set after the try/catch.
    std::int64_t lastUsedAt = 0;
};
// 57-D1: `SessionCatalogSnapshot::capturedAtMs` (session_catalog.hpp:62) is
// DELETED, along with `SessionCatalogModel::capturedAtMs` (ui_model.hpp:559).

// include/ymh/ui/ui_model.hpp
struct WorkspaceNode {
    // ... existing fields ...
    // 57-D5/57-D6: epoch ms; 0 == unknown (sorts last).
    std::int64_t lastUsedAt = 0;
};

struct UiModel {
    // ... existing fields ...
    // 57-D5/57-D6: canonical path of options_.initial_workspace (the effective
    // root, not getcwd); empty == no match.
    std::string cwdWorkspacePath;
};

// src/ui/ui_model.cpp (anonymous namespace) — 57-D5
// Effective-root first, then lastUsedAt desc, then title asc (case-insensitive),
// then canonical path asc.
[[nodiscard]] bool switcher_workspace_less(const WorkspaceNode& left,
                                           const WorkspaceNode& right,
                                           const std::string& cwd_path,
                                           const std::string& left_path,
                                           const std::string& right_path);

// src/ui/ui_render.cpp (anonymous namespace) — 57-D3
// The inline badge; text == "press Ctrl+d again to confirm"; fixed width
// kDeleteBadgeWidth; obeys `theme.color` (Red/White, or inverted|bold when
// colour is disabled). Never ellipsized, never passed through `inverted`.
[[nodiscard]] ftxui::Element delete_confirm_badge(const Theme& theme);

// 57-D3 (Oracle MED-1, R4-MED-2): the single source of truth for the popup's
// disarmed rows. render_switcher renders each row from THIS struct and
// switcher_content_width measures each row's string_width from this exact
// vector, so neither the pinned width nor the per-row styling can drift from the
// model. `text` is the pre-ellipsize string; Separator rows carry an empty text.
// The footer is the row with kind == Footer.
//
// The identity/style fields are the exact per-node inputs the shipped renderer
// reads, so render_switcher styles the vector directly and never re-walks
// `model.switcher.workspaces` by index (the parallel index walk is what let row
// order and styling drift apart — R4-MED-2). A Leaf row sets at most one of
// `workspace`/`session`; `cursor` mirrors `on_workspace`/`on_session`
// (`ui_render.cpp:911-912`,`:952-954`), `attention` mirrors `session.attention`
// (`ui_render.cpp:961-962`,`:970-971`), and `from_disk` mirrors
// `session.fromDisk` (`ui_render.cpp:956-957`). Note/Separator/Footer rows leave
// all five unset/false.
enum class SwitcherRowKind { Leaf, Note, Separator, Footer };
struct SwitcherRow {
    std::string                text;
    SwitcherRowKind            kind;
    std::optional<WorkspaceId> workspace;          // set on a workspace Leaf
    std::optional<SessionId>   session;            // set on a session Leaf
    bool                       cursor    = false;  // row is the cursor row
    bool                       attention = false;  // session needs attention
    bool                       from_disk = false;  // history-sourced session leaf
};
[[nodiscard]] std::vector<SwitcherRow> switcher_disarmed_rows(const UiModel& model);

// 57-D3: build the armed target's one-line row as an hbox of
// { name_element, text(" "), badge [, text(" "), dim(suffix)] }.
// `name_element` is the caller-built, already-ellipsized name text. Its
// `fixed_prefix` — "    [" for a session (its trailing `]` and metadata
// suppressed), "<collapsed? '+ ' : '- '>" for a workspace (shipped markers
// `ui_render.cpp:957`, `:922`/`:924`) — is **never** ellipsized; only the
// `<name>`/`<title>` after it is, to the budget `w - (string_width(fixed_prefix)
// + 1 + kDeleteBadgeWidth [ + 1 + suffix_width])` (57-D3). `name_element`
// (NOT the returned hbox) carries the row highlight. `suffix` is the workspace's
// `(N sessions)` text **already ellipsized by the caller**: `render_switcher`
// knows `w` (it receives `available_width`) and ellipsizes `suffix` to
// `kDeleteSuffixReserve`, or to `{}` below `kDeleteBadgeReserve` (57-I6, 57-D3).
// `with_delete_confirm` only wraps it in `dim()` and must not build or ellipsize
// it — it has no width. A session row passes an empty `suffix` (no suffix
// element). Returns the hbox; never pushes a row.
[[nodiscard]] ftxui::Element with_delete_confirm(ftxui::Element name_element,
                                                 std::string suffix,
                                                 const Theme& theme);

// 57-D3: the pinned window CONTENT width (57-I16), computed once per render as
// computed = max(string_width(footer), max_disarmed_row_width, kDeleteBadgeReserve)
// over switcher_disarmed_rows(model) (one builder, above) with the fixed,
// state-independent kDeleteBadgeReserve (a runtime const int, see 57-D3), then
// clamped for the 2 border columns: w = max(0, min(computed, available_width - 2)).
// Applied to the CONTENT so the window auto-sizes to total w + 2:
//   window(title, vbox(rows) | size(WIDTH, EQUAL, w)) | clear_under | center
// (Not `window(...) | size(...) | clear_under | center`, which pins the TOTAL to
// w and the content to w - 2 under `center`; measured, §13.)
[[nodiscard]] int switcher_content_width(const UiModel& model, int available_width);

// 57-D3: `render_switcher` gains the terminal width so the pinned window can be
// clamped to it and the name/suffix ellipsized to keep the badge on-screen
// (57-I20). It renders exactly `switcher_disarmed_rows(model)` (one builder,
// above) plus the armed row, styling each row from its own
// `workspace`/`session`/`cursor`/`attention` fields — never by re-walking
// `model.switcher.workspaces` by index (R4-MED-2) — and applies
// `size(WIDTH, EQUAL, w)` to the content.
// `build_ui` already has the size (`supervisor.cpp:3475`, `ui_render.cpp:1447`).
[[nodiscard]] ftxui::Element render_switcher(const UiModel& model, const Theme& theme,
                                             int available_width);

// src/ui/session_catalog.cpp — 57-D6: set lastUsedAt after the try/catch (and
// after sort_sessions) in read_workspace_history(); note_history() sets 0.
```

No other signature changes. `SwitcherOverlayModel::open/openHistory/close/
disarm_delete/moveDown/moveUp` keep their signatures; only the comparator body and
the `lastUsedAt` copy change. `render_switcher` and `build_ui` gain the
already-available terminal width (above).

---

## 5. Invariants

| ID | Invariant |
|---|---|
| 57-I1 | The History footer contains no `captured` segment (57-D1). |
| 57-I2 | `SessionCatalogModel::nowMs` (reassigned each delivery, `supervisor.cpp:950`) is still the source for the per-session `· <relative>` leaf (`ui_render.cpp:887-888`); 57-D1 removes only `capturedAtMs`. |
| 57-I3 | Both footers contain the literal `Ctrl+D delete` whenever the popup is open (57-D2). |
| 57-I4 | While `delete_arm == Armed`, the exact text `press Ctrl+d again to confirm` is rendered on the armed target's own row as part of an `hbox` — never as an appended row — with a red background when `theme.color` is true (57-D3). |
| 57-I5 | Arming does not change the number of rendered lines of the popup (`ftxui::window` height is identical disarmed vs armed) (57-D3). |
| 57-I6 | A session target's confirmation is on the session-name row; a workspace target's is on the workspace row and states `delete_target_session_count` in a dim `(N sessions)` suffix ellipsized to `kDeleteSuffixReserve` — and, on a terminal narrower than `kDeleteBadgeReserve`, ellipsized further (to `{}`) so the never-ellipsized badge stays visible (the suffix may shrink; 57-D3, 57-I20). |
| 57-I7 | The arm is disarmed by any non-Ctrl+D key, a cursor move, close, reopen, the 3 s timeout, or a confirming delete; a terminal resize does NOT disarm (57-D3). |
| 57-I8 | A single Ctrl+D never deletes; only a second Ctrl+D with `delete_target == cursor` confirms (retained 51-D4.3). |
| 57-I9 | **In the switcher overlay**, Ctrl+D deletes the highlighted session, else the workspace (retained 51-D4.2). |
| 57-I10 | While any popup guard is true — `exitConfirm.open`, `dialog.open`, `Context && context.open`, `Notice && message.open`, `mode == Switcher` (mode-only), `ModelPicker && visible` (`supervisor.cpp:3395-3411`) — Ctrl+Q is consumed and does not call `begin_exit`; with no guard true, Ctrl+Q calls `begin_exit(true)` (57-D4, `F6`). |
| 57-I11 | Both switcher sources put the **effective-root** workspace (`UiModel::cwdWorkspacePath` = `options_.initial_workspace`) first when present and non-empty; under `--resume`/`--workspace` that is the target workspace, not the shell cwd. Requirement 5 holds verbatim for bare/`--workspace` launches; under `--resume` the effective root deliberately takes precedence over the shell cwd, **approved** (57-D5, 57-D7). |
| 57-I12 | After the effective-root workspace, nodes are `lastUsedAt` desc; equal `lastUsedAt` falls back to title asc (case-insensitive), then canonical path asc (57-D5). |
| 57-I13 | Ordering is applied in `SwitcherOverlayModel::open`/`openHistory`; `render_switcher` never sorts (57-D5). |
| 57-I14 | Session order is unchanged: Live = daemon order; History = `updated_at` desc, `id` asc (retains 22-D1's session half). |
| 57-I15 | While any popup guard is true, no global binding reaches its global handler — Ctrl+S/P (`supervisor.cpp:3414`), Ctrl+Q (`:3422`), Ctrl+C (`:3426`; popups deliberately repurpose it as a close key), Ctrl+N (`:3430`), Ctrl+O (`:3434`), PageUp/Down (`:3438-3445`), Ctrl+Home/End (`:3446-3453`), Shift+Up/Shift+Down (`:3454-3460`) — because the guards precede them and each popup handler returns `true` unconditionally (57-D4). |
| 57-I16 | Arming does not change the popup's **width**: `render_switcher` pins the window **content** to `size(WIDTH, EQUAL, w)`, applied to the content inside the window (57-D3, §13: `window(title, content-sized-to-w)` then `clear_under` then `center`), where `w = max(0, min(computed, available_width - 2))` and `computed = max(string_width(footer), max_disarmed_row_width, kDeleteBadgeReserve)` is computed once from `switcher_disarmed_rows(model)` plus a **fixed, state-independent** armed-row reserve `kDeleteBadgeReserve`; the content is `w` and the total is `w + 2` **except when the window title forces the border wider on terminals narrower than the title** (57-D3), and `w` does not depend on `delete_arm`, the cursor, or the armed target (57-D3, §13). |
| 57-I17 | The row highlight (`inverted` for a session target, cyan/bold for a workspace target) is applied to the name/prefix element only, never to the `hbox` or the badge, so the badge's explicit Red/White (or `inverted` fallback) is never reverse-videoed away (57-D3). |
| 57-I18 | The badge obeys `theme.color`: with colour it is `bgcolor(Red)` + `color(White)` + `bold`; without colour it is `inverted` + `bold` and emits no ANSI colour. In the no-colour fallback the badge and the session-name highlight are both `inverted`, so the confirmation is distinguished by `bold` and by the badge text itself; in colour mode the badge's Red background further distinguishes it (57-D3, 57-G6). |
| 57-I19 | Live `lastUsedAt` is copied from the current catalog snapshot; when no snapshot exists the first render is title-ordered and **re-sorts on each catalog snapshot delivered while the popup is open** (`on_catalog_snapshot` → `resnapshot_switcher()`, `supervisor.cpp:952-958`) — bounded, never looping (57-D6). |
| 57-I20 | The badge is never ellipsized; only the *name* (and, for a workspace, the `(N sessions)` suffix) is ellipsized (`ellipsize_text`) to its budget, so within the pinned content width **whenever `w >= kDeleteBadgeReserve`** the fixed parts (`fixed_prefix` + badge [+ suffix]) fit and only the name may still be ellipsized to its remainder (R3-LOW-1). On a narrower terminal the name is ellipsized to its remainder (at the bound, to `{}`) and then the workspace suffix ellipsizes (to `{}` below `kDeleteBadgeReserve`, `ellipsize_text(..., max_width<=0)`, `ui_render.cpp:521-523`); because the **`fixed_prefix` is never ellipsized** (it is excluded from the name budget, `57-D3`), the badge stays **fully visible** only for `w >= string_width(fixed_prefix) + 1 + kDeleteBadgeWidth` — **35** for a session (`"    ["` = 5) and **32** for a workspace (`"- "` = 2; by then the suffix has already ellipsized to `{}`, so the row is `fixed_prefix + " " + badge + " " + {}` and the lone overflow cell falls on the trailing separator that follows the badge — there is no right-edge clip) — where the row is `fixed_prefix + " " + badge` (session) / `fixed_prefix + " " + badge + " " + <suffix>` (workspace). Below that the badge is **not guaranteed fully visible** — partially clipped for `3 <= w <` the bound above (session `3..34`, workspace `3..31`) and fully clipped only for `w <= 2` (both; FTXUI's `hbox` hard-shrinks every text child rather than right-clipping — measured session `w=6 → 4/29`, `w=3 → 1/29`, `w=2 → 0/29`) — the documented narrow-terminal bound (57-D3, 57-F13). |

---

## 6. Failure modes (F1–F12 tagged)

| ID | Tag | Failure | Disposition |
|---|---|---|---|
| 57-F1 | F12 | A future edit re-adds a clock-derived "age" to the footer without a ticking model clock, re-introducing a frozen label. | 57-I1; the only relative age is the per-session leaf, derived from the model's frozen `nowMs` (57-I2). |
| 57-F2 | — | Arming renders a second line, or widens/shifts the centered popup. | 57-I4/57-I5/57-I16; goldens assert line-count and width equality. |
| 57-F3 | — | A red badge on the cursor row is inverted away by the row highlight, hiding the confirmation. | 57-I17: the highlight is applied to the name child only; the badge is a separate `hbox` child with explicit fg/bg. |
| 57-F4 | F6 | A stale `delete_target` redirects a confirm onto a different row after the cursor moved. | 57-I7/I8; the `delete_target == cursor` guard (`supervisor.cpp:2828-2829`) is retained. |
| 57-F5 | F6 | Ctrl+Q in a popup reaches `begin_exit` and tears down daemons. | 57-I10; popup dispatch precedes `:3422`; regression test added. |
| 57-F6 | F6 | The Ctrl+Q fix accidentally unbinds exit when no popup is open. | 57-I10 second half; `errata51_ui_test.cpp:212-232` retained. |
| 57-F7 | — | The effective-root node is identified by `activeWorkspaceId`, so focusing another workspace changes "current directory". | 57-D6: a dedicated `cwdWorkspacePath`, never focus-derived. (`F6` is keybinding-focus ownership, not focus-derived paths; `—` is honest — R3-LOW-1.) |
| 57-F8 | — | `lastUsedAt == 0` (no materialized sessions / unreadable DB) sorts unpredictably. | 57-I12: 0 is the smallest key → last, with the title/path tie-break. |
| 57-F9 | — | Ordering is duplicated in the renderer and drifts from the model. | 57-I13: model-only. |
| 57-F10 | — | The `max(sessions.updated_at)` proxy over-ranks a background agent's activity over the user's workspace. | Accepted: effective-root-first (57-I11) pins the user's own workspace; residual tail only. Seam: `workspaces.last_used_at` (`registry.cpp:44-59`, 57-OQ2). |
| 57-F11 | — | Live's async `lastUsedAt` makes the first render title-ordered, then re-sorts on each subsequent catalog snapshot. | 57-I19; bounded (re-sorts per snapshot while the popup is open, `supervisor.cpp:952-958`), never loops; the effective root stays pinned. |
| 57-F12 | F6 | A global binding (Ctrl+S/P/N/O, Ctrl+C, PageUp/Down, Ctrl+Home/End) reaches its handler while a popup is open. | 57-I15; the guards precede the bindings (`supervisor.cpp:3395-3411`); popups consume Ctrl+C as a close key. |
| 57-F13 | — | The badge is not guaranteed fully visible on a narrow terminal, so an armed row may show only a partial confirmation. | 57-I20: fixed `kDeleteBadgeWidth`; while `w >= kDeleteBadgeReserve` the name/suffix are ellipsized so the fixed parts (`fixed_prefix` + badge [+ suffix]) fit. For `string_width(fixed_prefix) + 1 + kDeleteBadgeWidth <= w < kDeleteBadgeReserve` the badge is still fully visible (the name is ellipsized to its remainder — at the lower boundary, to `{}` — and the `fixed_prefix` is excluded from the name budget, so the row is `fixed_prefix + " " + badge`; session `w >= 35`, workspace `w >= 32` — by then the suffix has ellipsized to `{}`, and the lone overflow cell at `w = 32` falls on the trailing separator, not the badge). Below that bound the badge is partially clipped down to `w = 3` (session `3..34`, workspace `3..31`) and fully clipped only at `w <= 2` (both; FTXUI's `hbox` hard-shrinks every text child rather than right-clipping — the `fixed_prefix` is not preserved — measured session `w=6 → 4/29`, `w=3 → 1/29`, `w=2 → 0/29`, `ui_render.cpp:521-523`) — an accepted, documented bound. |
| 57-F14 | — | The badge bypasses `theme.color` and emits ANSI on a colour-disabled terminal. | 57-I18: the badge uses the `theme.color` gate with an `inverted`+`bold` fallback. |

---

## 7. dsh (DeepSeek Harness) mapping

dsh is a headless harness whose UI is a plugin (`00-architecture.md:4864`), so there
is no dsh switcher/popup contract to mirror. Non-mirror cells carry an anchor per
the AGENTS.md rule.

| dsh concept | ymh realization (57) | Anchor / non-mirror justification |
|---|---|---|
| Session/workspace listing order | effective root first, then last-usage desc, then title/path | **Non-mirror, deliberate scope.** dsh exposes no interactive switcher ordering; ymh's overlay is a UI-only projection (`22-D3`, `docs/design/22-switcher-sessions-errata.md:896`). Ordering is a ymh presentation decision, replaced by 57-D5. |
| Keybinding focus (F6) | Exactly one key owner; popups consume keys before globals | Mirrors the `F6` principle (`docs/design/00-architecture.md:4850`); no divergence. |
| Delete confirmation | UI-only two-press arm | **Non-mirror.** dsh has no interactive delete affordance; the arm is ymh-specific (`51-D4`, `docs/design/51-reasoning-fold-prompt-emphasis-and-tables-errata.md:131`). 57 changes only its rendering. |
| Relative snapshot age | Removed | **Non-mirror.** dsh renders no snapshot-age label; the ymh footer was a UI-only artifact (`22` §4.3 `:936-937`) whose value was frozen (57-D1). |
| Session last-usage timestamp | `max(sessions.updated_at)` from the store | **Non-mirror, deliberate proxy.** dsh exposes no per-workspace last-usage surface; ymh derives it from the event store's last event time (`src/session/session.cpp:651`, `:716`). The proxy over-ranks background activity (57-F10); the seam is a `workspaces.last_used_at` column (`registry.cpp:44-59`, 57-OQ2). |

---

## 8. Supersedes / amends (explicit)

- **Supersedes** `22-D1`'s workspace-order rule at
  `docs/design/22-switcher-sessions-errata.md:646-649`, `:938-940`, and SW17
  `:1587`; `AGENTS.md:111-112`; `UI_SURFACE_INVENTORY.md:67-68` and `:85-86`. The
  replacement is 57-D5. `22-D1`'s session-order half is **retained** (57-I14).
- **Supersedes** the History footer text at
  `docs/design/22-switcher-sessions-errata.md:936-937`; replacement 57-D1.
- **Supersedes / narrows SW19** (`docs/design/22-switcher-sessions-errata.md:877-880`
  prose, `:1589` table): "supersedes" is scoped to the `captured <relative> ago`
  segment and its `capturedAtMs` carrier, both removed; the rest of SW19 is
  **narrowed, not superseded** — the staleness marker becomes the `partial` marker
  (`complete == false`), which is retained. Replacement 57-D1. (The History footer
  clause at `22` :936-937 is the same line that carries `[· partial]`, so the
  supersede of `:936-937` above is likewise scoped to its captured segment.)
- **Supersedes the stale ordering comment** at
  `include/ymh/ui/session_catalog.hpp:57` ("the renderer sorts by title (22-D1)");
  ordering is model-only (57-I13) and the rule is 57-D5.
- **Declares the transient suppression** of the `22` §4.3 :933-934 (History
  session-leaf metadata), `:934-935` (the **Live** session-leaf
  `[<title> <state glyph>[!]]` format, `ui_render.cpp:960-964`) and `:929-932`
  (workspace row tail) formats while a row is armed; all return on disarm
  (57-D3/57-I7). This is a declared transient deviation, not a permanent
  supersede.
- **Amends** `51` §6.3 point 3
  (`docs/design/51-reasoning-fold-prompt-emphasis-and-tables-errata.md:838-848`)
  and its golden `UI51_D4_SwitcherDeleteHintGolden`
  (`tests/unit/ui_render_golden_test.cpp:2379-2414`, `51-…:1153`): arm semantics
  retained; confirmation rendering replaced by 57-D3.
- **Does not amend** `51-I21` (`51-…:1010`) — it pins arm state, not rendering
  (Oracle-§6).
- **Amends** the exact workspace-delete hint text "workspace and its N sessions"
  (shipped at `src/ui/ui_render.cpp:997-998`) to the inline `(N sessions)` suffix
  (57-D3). The clause it belongs to is `51` §6.3 **point 5**
  (`51-…:881-894`; the phrase `delete workspace "alpha" and its 3 sessions?` is at
  `:891-892`); `51-D4.5` (`51-…:985`) is only the code comment.
- **Amends** the stale exit-key comment at `include/ymh/ui/ui_model.hpp:523-524`
  (57-D4).
- **Amends** the stale `SessionCatalogModel::nowMs` doc comment at
  `include/ymh/ui/ui_model.hpp:551-554` (it still names the removed `captured
  <relative> ago` label); the retained `nowMs` keeps only the per-session
  relative-age role (57-D1/57-I2).
- **Amends** `UI_SURFACE_INVENTORY.md` Surface 3 (`:67-68`, `:72-75`, `:85-86`)
  to the new ordering and hint; the doc-sync pass updates it, 57 does not edit it.

---

## 9. Test plan

ID scheme: `57-U*` unit, `57-G*` golden render, `57-H*` harness, `57-P*` PTY.
The implementation phase writes the tests; this spec names files and assertions.

### 9.1 Unit — model (`tests/unit/ui_model_test.cpp`)

- **57-U1** `SwitcherOrdersEffectiveRootFirstThenLastUsed` — `openHistory` with
  four catalog workspaces (effective root = `/beta`, lastUsed 100; `/alpha`
  lastUsed 5000; `/gamma` lastUsed 3000; `/delta` lastUsed 3000). Assert order
  `[root /beta, /alpha, then /gamma|/delta by title/path]`.
- **57-U2** `SwitcherOrderTieBreak` — two workspaces with equal `lastUsedAt` and
  equal titles but paths `/a` < `/b`; assert `/a` first; a third with a
  lexicographically smaller title sorts before both.
- **57-U3** `SwitcherOrderZeroLastUsedLast` — one workspace with `lastUsedAt == 0`
  sorts after all dated ones.
- **57-U4** `SwitcherOrderEffectiveRootAbsentOrFiltered` — `cwdWorkspacePath`
  matching no node: pure lastUsed order, no crash; with a filter that excludes the
  effective-root node, the effective-root rule does not fire.
- **57-U5** `SwitcherOrderLiveSource` — the same assertions through
  `SwitcherOverlayModel::open` with `WorkspaceModel::cwd` / catalog-matched
  `lastUsedAt`.
- **57-U6** `SwitcherLastUsedFromReader` — `read_workspace_history` sets
  `lastUsedAt == max(sessions.updatedAt)` over the materialized sessions; a
  degraded `note_history` has `lastUsedAt == 0`. Extends the catalog-reader
  fixtures **SW-U4** (`tests/unit/session_catalog_test.cpp:309`), **SW-U5**
  (`:623`) and **SW-U20** (`:549`). SW-U19 is a *supervisor* test
  (`22-switcher-sessions-errata.md:1686`) and is **not** the fixture here
  (R5-MEDIUM-2).
- **57-U7** `PerSessionLeafStillRendersAfterD1` — after 57-D1 the per-session
  `· <relative>` leaf still renders from `catalog.nowMs`
  (`ui_render.cpp:887-888`) (57-I2; Oracle-§6 missing test).
- **57-U8** `SwitcherEffectiveRootRuleWhenCatalogUnloaded` — Live `open` before
  any catalog snapshot: with `cwdWorkspacePath` set and `catalog.loaded == false`,
  the effective-root node still sorts first (using `WorkspaceModel::cwd`) and the
  rest tie-break by title (57-I11/57-I19; Oracle-§6 missing test).

### 9.2 Golden render (`tests/unit/ui_render_golden_test.cpp`)

- **57-G1** `HistoryFooterHasNoCapturedSegment` — `history_model()` rendered; assert
  `rendered.find("captured") == npos`; `stored sessions`, `partial`, and
  `Ctrl+D delete` present. **Updates** `HistoryOverlayGroupsAndLeaves`
  (`:1386-1413`), which currently asserts `captured 5s ago` at `:1411`.
- **57-G2** `SwitcherFooterDeleteHint` — both sources' footers contain
  `Ctrl+D delete`.
- **57-G3** `SwitcherDeleteConfirmInline` — arm a session target; **render and
  measure** (the §4 helpers are in an anonymous namespace and cannot be linked
  from a cross-TU test; R5-N4): assert the rendered row contains `press Ctrl+d
  again to confirm` on the **same line** as the session name (the substring
  `beta-session` and the badge occur on one normalized line), that no line
  contains only `one more Ctrl+D to delete`, that the red background is applied
  when `Theme{true}` (assert the ANSI SGR red-bg escape precedes the badge text in
  the un-normalized render), and that the badge is **not** inverted (its SGR run
  contains the red-bg sequence, not `\x1b[7m`) (57-I17). **Updates**
  `UI51_D4_SwitcherDeleteHintGolden` (`:2379-2414`).
- **57-G4** `SwitcherDeleteConfirmInvariants` — render disarmed and armed and
  **measure the rendered `Screen`** in **cells** — `ftxui::string_width`,
  `Screen::dimx()`/`Screen::PixelAt` — **never** `Screen::ToString().size()` (each
  box-drawing border glyph is 3 UTF-8 bytes; §13) (the anonymous-namespace
  `switcher_content_width` is not linkable from a cross-TU test; R5-N4). Take the
  pinned `w` from the **disarmed** render's measured **content** width; do **not**
  re-derive it from a duplicated `max(footer, max_disarmed_row,
  kDeleteBadgeReserve)` formula, which would reintroduce the drift the shared
  builder (57-D3) removed (R4-MED-2). Assert `normalize(...)` line counts are equal
  (57-I5) **and** the window border columns are identical (each rendered line's
  left/right border column index is unchanged), and that the armed render's window
  **content** width is that same measured `w` and its **total** is `w + 2`
  (57-I16). Do **not** assert "every rendered line's width is equal": rendered
  lines are padded to the screen width, so that disjunct is vacuous (R1-NEW-3).
  Assert the workspace target still shows `N sessions` on its row (57-I6) at the
  pinned test width, chosen `>= kDeleteBadgeReserve` so the suffix is rendered
  rather than ellipsized to `{}` (R2-LOW-2).
- **57-G5** `SwitcherEffectiveRootFirstGolden` — a History model whose
  effective-root workspace is last by title/lastUsed renders first.
- **57-G6** `SwitcherDeleteConfirmColourGate` — `Theme{false}`: the badge emits
  no ANSI colour, renders `inverted | bold`, and its SGR run carries a bold
  attribute that the session-name highlight's SGR run does not (only `bold`
  distinguishes them; Oracle-N7). `Theme{true}`: the badge's SGR run carries the
  red bg and no `\x1b[7m`, while the highlighted name's does (57-I18; R1-L1).
- **57-G7** `SwitcherRowStyleAlignment` — a History/Live model with a cursor
  workspace followed by one of its sessions renders the workspace row cyan/bold
  and the session row `inverted`, and an `attention` session row red, all selected
  from each row's own `workspace`/`session`/`cursor`/`attention` fields in
  `switcher_disarmed_rows(model)` — not from a parallel index walk over
  `model.switcher.workspaces` (57-D3, R4-MED-2). Assert the SGR runs on the
  rendered `Screen` rows (cells, not bytes) so a re-walk that misaligns row order
  fails; the fixture includes a collapsed workspace, a Note leaf and a second
  workspace so an index walk would style the wrong row (Oracle-LOW-3). **This is
  a directive, not a mechanical proof:** a re-walk that happens to align passes,
  so G7 catches drift only; the "never re-walks by index" requirement (`57-D3`,
  §4) is enforced by review (R1-LOW-3, R2-LOW-1, R3-LOW-1).

### 9.3 Harness (`tests/unit/errata57_ui_test.cpp`)

- **57-H1** `CtrlQInSwitcherDoesNotExit` — seed `D4Fixture`
  (`tests/unit/errata51_ui_test.cpp:149`) so `open_switcher()` opens the
  **Switcher**, not the Notice fallback (`supervisor.cpp:1049-1056`); record the
  pre-state, then `EXPECT_TRUE(dispatch_key("ctrl-q"))`; assert
  `quit_requested() == false`, `model().exitConfirm.open == false`,
  `model().mode == UiMode::Switcher`, and the switcher still has nodes (57-I10).
  Repeat for the History source via `open_sessions` (R3-LOW-2; Oracle-§4).
- **57-H2** `CtrlQNoPopupStillExits` — Conversation, `dispatch_key("ctrl-q")`;
  assert `quit_requested() == true` (retains `errata51_ui_test.cpp:212-232`).
- **57-H3** `ArmInlineDisarmTable` — reuse the `D4Fixture`
  (`tests/unit/errata51_ui_test.cpp:149`): Ctrl+D arms; `down` disarms; a second
  Ctrl+D on the moved cursor does not delete; Ctrl+D twice on the same target
  deletes; the 3 s tick disarms (extend
  `UI51_D4_ArmThenConfirm`/`ArmExpiresOnTick`/`DisarmOnOtherKey`).
- **57-H4** `CtrlQSwallowedByEveryPopup` — extend coverage to the other **five**
  guards (57-I10/57-F5; Oracle-§4, R3-MEDIUM-2): with a Notice
  (`mode==Notice && message.open`), a Context (`mode==Context && context.open`), a
  ModelPicker (`mode==ModelPicker && visible`), an `exitConfirm.open` state and a
  `dialog.open` state, `dispatch_key("ctrl-q")` returns true and
  `quit_requested() == false`. For the Notice/Context/ModelPicker cases
  additionally assert `exitConfirm.open == false` (proving Ctrl+Q did not *open*
  the exit prompt). For the `exitConfirm.open`/`dialog.open` cases assert the flag
  is **unchanged** (pre-state flag equals post-state flag) — asserting
  `exitConfirm.open == false` there would contradict "unchanged" whenever the
  pre-state is true, so that assertion is made only for the non-dialog cases.

### 9.4 PTY (`tests/unit/ui_supervisor_pty_test.cpp`)

- **57-P1** drive the real binary: open Ctrl-S, send `C-q`, assert the process is
  still alive and the popup frame is present; send `C-d` twice, assert the row is
  deleted. **Not** gated on `YMH_LIVE_LLM`: it invokes no LLM, and the PTY suite
  has no such gate (`tests/unit/ui_supervisor_pty_test.cpp` contains no
  `YMH_LIVE_LLM`/`SKIP`). It runs in the normal suite (R3-LOW-6).

### 9.5 Mandatory updates to existing tests

- `tests/unit/ui_render_golden_test.cpp` `HistoryOverlayGroupsAndLeaves`
  (`:1386-1413`): the `captured 5s ago` assertion at `:1411` fails (57-G1 updates
  it); remove the `capturedAtMs` fixture line `:1377`.
- `tests/unit/ui_render_golden_test.cpp:1503` sets `capturedAtMs` (delete);
  `tests/unit/supervisor_harness_test.cpp:1318` sets `snapshot.capturedAtMs`
  (delete); `tests/unit/session_catalog_test.cpp:640` asserts
  `EXPECT_GT(first.capturedAtMs, 0)` (delete).
- `tests/unit/ui_render_golden_test.cpp` `UI51_D4_SwitcherDeleteHintGolden`
  (`:2379-2414`): the appended-row assertions at `:2403`/`:2412` fail (57-G3
  updates it to the inline badge).
- `tests/unit/ui_model_test.cpp` `SwitcherOrderingAndSessionOrder` (SW-U15,
  `:1138`) and `SwitcherOpenHistoryBuildsFromCatalog` (SW-U6, `:1260`) encode
  SW17's workspace title order; they must be extended to set
  `cwdWorkspacePath`/`lastUsedAt` and assert the 57-D5 rule. (Their existing
  assertions coincide with the new rule only because both default to empty/zero,
  so they are not coverage of 57-D5 — see the review-rounds record, R2/Oracle-H3
  nuance.)

---

## 10. Verification status and open questions

- **Status: `verified` (Rev 1).** Gated — Oracle's fifth and final independent
  pass returned PASS with zero open HIGH/MEDIUM; residual LOWs non-blocking (§15).
- **57-OQ1 (D): CLOSED (user decision, 2026-09-25).** The user's Ctrl+Q report is
  not reproducible on HEAD (§2.5); the popup-first dispatch is real
  (`supervisor.cpp:3390-3422`) and correct, so the code is right as shipped. The
  user has accepted that the invariant (57-I10) plus the regression tests
  (57-H1/57-H4) lock the behaviour in; no dispatch change is made. The
  empty-orphaning-set path (`begin_exit` → `confirm_exit` with no prompt,
  `supervisor.cpp:550-553`, `51-L1`) is by design and remains the most likely
  explanation of the original symptom. No open question remains here.
- **57-OQ2 (E):** `max(sessions.updated_at)` over the materialized sessions is a
  proxy for "last usage", not a true per-workspace timestamp. It over-ranks
  background activity (57-F10); accepted because effective-root-first (57-I11)
  pins the user's own workspace. If the proxy is judged too coarse (e.g. a
  workspace opened but not prompted shows stale), the seam is a new
  `workspaces.last_used_at` column (`src/registry/registry.cpp:44-59`) — deferred.
- **57-OQ3 (C):** suppressing the armed row's metadata keeps the line bounded but
  hides context for the 3 s arm window. If that is unacceptable, retain metadata
  and accept width growth; **height** invariance (57-I5) holds either way, and
  width invariance (57-I16) must then be re-derived.

---

## 11. Review rounds (consolidated fix pass)

Six independent reviews (R1–R5 + Oracle) were applied in one pass. Every finding
is listed with its resolution; disagreements are recorded explicitly. The spec
stayed `draft` at this round (see §15).

### R1 — visual/UX (`/tmp/opencode/spec57-gate-R1.md`)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| H1 | HIGH | Arming can widen the auto-sized window and re-center it | 57-I16 (width invariance), the pinned `size(WIDTH, EQUAL, w)` in 57-D3, the `switcher_content_width`/`render_switcher` signatures in §4, and the width assertion in 57-G4. |
| H2 | HIGH | `inverted` swaps the badge's Red/White to Red-on-White | 57-D3 pins the highlight on the name child only; 57-I17; 57-F3 re-reasoned; 57-G3 asserts the badge is not inverted. |
| M1 | MED | `with_delete_confirm(Element row)` cannot express `    [<name` | §4 signature changed to `with_delete_confirm(Element name_element, int session_count, const Theme&)`; the caller builds the prefix (metadata/`]` suppressed). Round 5 replaced `session_count` with the caller-ellipsized `suffix` string (§15). |
| M2 | MED | Badge clipped on narrow terminals, no min-width/ellipsize | 57-I20 + `kDeleteBadgeWidth`; window width clamped to the terminal via the new `available_width` parameter; name ellipsized with `ellipsize_text`. |
| L1 | LOW | Badge bypasses `theme.color`/`paint_bg` | 57-I18 + 57-G6: badge obeys `theme.color` with an `inverted`+`bold` fallback; deliberately not `paint_bg` (its `user_block` check is unrelated). |
| L2 | LOW | Resize is not a disarm trigger | Stated as a deliberate omission in 57-D3/57-I7. |

### R2 — ordering/data (`/tmp/opencode/spec57-gate-R2.md`)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| MEDIUM-1 | MED | `initial_workspace` is not the shell cwd under `--resume` | §2.6 and 57-D5 renamed the rule to **effective root first** and state the `--resume`/`--workspace` consequence (`cli.cpp:485-493`, `:107`); 57-I11 reworded. |
| LOW-1 | LOW | empty-cwd guard | 57-D5 step 1 pins `!cwdWorkspacePath.empty()` and the `!=` form. |
| LOW-2 | LOW | filtered-vs-raw max | §2.6/57-D6 pin `max` over the **materialized** (post-`isUnprompted`) sessions. |
| LOW-3 | LOW | degraded-path placement | 57-D6 pins the computation **after** the try/catch. |
| LOW-4 | LOW | stale comment `session_catalog.hpp:57` | Added to the Supersedes header + §8. |
| LOW-5 | LOW | unanchored dsh mirror row | dsh row changed to **non-mirror** with reason + anchors. |
| LOW-6 | LOW | anchor `supervisor.cpp:3417-3419` | **Disagreed:** the spec's anchor is correct — `openSwitcher()` is `:3415` and `catalog_->refreshNow()` is `:3418`; R2-LOW-6's "`:3420-3421`" is wrong. No change. |
| LOW-7 | LOW | `registry.cpp:1049` mislabeled as `touch_workspace` | Separated: `touch_workspace` call sites `:1248/:1279/:1298/:1332`; rename is the direct `UPDATE` in `setDisplayTitle` (`:1045-1053`). |

### R3 — key dispatch (`/tmp/opencode/spec57-gate-R3.md`) — PASS

| # | Sev | Finding | Resolution |
|---|---|---|---|
| LOW-1 | LOW | 57-I10 not the literal predicate | 57-I10 now names all six exact guards (`supervisor.cpp:3395-3411`) incl. the mode-only Switcher guard. |
| LOW-2 | LOW | 57-H1 lacks the seed/preconditions | 57-H1 seeds `D4Fixture` (`errata51_ui_test.cpp:149`), asserts `dispatch_key` returns true, `exitConfirm.open == false`, pre/post mode and non-empty nodes. |
| LOW-3 | LOW | no invariant for the other swallowed globals | Added 57-I15 + 57-F12. |
| LOW-4 | LOW | 57-I9 reads globally | Rephrased "**In the switcher overlay**, …". |
| LOW-5 | LOW | empty-orphaning-set explanation missing | Added to §2.5 as a by-design candidate; 57-OQ1 later closed by user decision (2026-09-25). |
| LOW-6 | LOW | 57-P1 gated on `YMH_LIVE_LLM` | Gate dropped (no LLM; the PTY suite has no such gate). |

### R4 — removal (`/tmp/opencode/spec57-gate-R4.md`)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | HIGH | SW19 un-superseded | Added to the Supersedes header + §8, narrowed to the `partial` marker. |
| F2 | MED | `capturedAtMs` write-only dead state | 57-D1 now **deletes** the field from both structs, its two writes and all fixtures/assertions; §9.5 lists them. |
| F3 | LOW | false `nowMs` mechanism | §2.2 corrected: `nowMs` is reassigned every delivery; the delta is queue latency because both timestamps come from one snapshot. |
| F4 | LOW | fixture cleanup unspecified | §9.5 names `ui_render_golden_test.cpp:1377`. |

### R5 — conformance (`/tmp/opencode/spec57-gate-R5.md`)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| F1 | MED | 5/9 §54 tags invalid | 57-F2/F3/F7/F8/F9 retagged (`—`/F6); F10–F14 added; naming note updated. |
| F2 | MED | 57-U6 cites SW-U19 | Corrected to SW-U4 (`:309`)/SW-U5 (`:623`)/SW-U20 (`:549`). |
| F3 | LOW | dsh :464 anchor points at superseded 22-D1 | Fixed to `22-…:896` (22-D3). |
| F4 | LOW | elided `docs/design/51-…:131` | Expanded to the full path. |
| F5 | LOW | `D4Fixture` cited at `:189-207` | Fixed to `:149` (both in §9.3). |
| F6 | LOW | status-header style; `DESIGN_STATUS.md` lacks a 57 row | **Not changed in this pass**: the tracker row is outside the permitted edit scope (spec file only) and is a doc-sync item; the fenced status block is retained. |
| F7 | LOW | 57-F1's F12 tag a stretch | Kept with rationale: F12 is "flash clock in model"; the failure is a clock-derived label without a ticking model clock. |

### Oracle (`/tmp/opencode/spec57-oracle.md`)

| # | Sev | Finding | Resolution |
|---|---|---|---|
| H1 | HIGH | SW19 silently broken | Superseded/narrowed (header + §8). |
| H2 | HIGH | width pinned by height only | 57-I16 + width golden (57-G4). |
| H3 | HIGH | SW17 tests not named | §9.5 names SW-U15/SW-U6/SW-G2. **Nuance recorded:** with `cwdWorkspacePath` empty and `lastUsedAt == 0` (their defaults), SW-U15/SW-U6 do **not** mechanically fail — they pass because the new comparator degenerates to title/path order. They are named as mandatory **extensions** (they encode the superseded rule), not as failures. Oracle's "will fail" is therefore partially inaccurate; the resolution is stronger than a rename. |
| M1 | MED | proxy over-ranks background activity | Added 57-F10 + acceptance anchor (effective-root-first) + seam; §2.6/§10. |
| M2 | MED | Live async reorder flash | 57-D6 documents the settle; 57-I19 + 57-F11 + 57-U8; explained why it is not blocked (async catalog). |
| M3 | MED | 57-I2/nowMs rationale false | §2.2/57-I2 corrected; conclusion retained with the same-snapshot reason. |
| M4 | MED | 57-I10 untested for 4/5 popups | 57-H4 added covering the other five guards; exact guards pinned. |
| §3 | audit | 22 §4.3 :929-935, :933-935; 51-D4.5; 51-I23; 51-I21 over-broad | `22` §4.3 formats declared in the header + §8; the 51 hint text amended in §8 (clause `51` §6.3 point 5); `51-I21` not amended; Live `51-I23` scope note lives in **57-D2**, not the header/§8 (R3-LOW-2). |
| §4 | — | do not close 57-OQ1 | **Closed by user decision (2026-09-25):** code correct, 57-I10 + 57-H1/57-H4 lock it in; empty-orphaning-set path retained as the by-design explanation. |
| §6 | LOW | casing; 51-I21; effective-root wording; missing tests | Badge verbatim casing reconciled (57-D2); 51-I21 no longer amended; effective-root wording; 57-U7/57-U8 added. |

### Intent changes

- **Requirement 5 under `--resume` — APPROVED (57-D7, 2026-09-25).** This is the
  one intent change. The rule was previously described as "the user-visible rule
  ('the directory I'm in comes first') is preserved". That was **false** for
  `--resume`: `cwdWorkspacePath` is `options_.initial_workspace`, which under
  `ymh --resume S` is S's own workspace (`cli.cpp:485-493`, `:554`), not the
  literal shell cwd. The user has now explicitly approved **effective-root first**:
  requirement 5 holds verbatim for bare and `--workspace` launches; under
  `--resume` the effective root deliberately takes precedence over the shell cwd.
  This **is** a behaviour change from the literal reading, and it is approved —
  not a preservation. See §2.6, 57-D5, 57-I11, 57-D7.
- **No other intent change.** The other four requirements are implemented **with
  the documented deviations** — req-2's badge casing is pinned verbatim in 57-D2,
  and req-3's no-colour fallback is `inverted | bold` (57-D3/57-I18); the only
  remaining naming shift is "current directory" → **effective root**
  (57-D5/57-I11), a clarification of the same field
  (`options_.initial_workspace`).

### Disagreements with reviewers

1. **R2-LOW-6** (anchor `supervisor.cpp:3417-3419`): the reviewer's claimed
   replacement lines are wrong; the spec's anchor is correct (`openSwitcher()`
   `:3415`, `refreshNow()` `:3418`).
2. **Oracle-H3 / R4-H3** (SW-U15/SW-U6 "will fail"): they do not mechanically
   fail with the fields' default values; they are superseded-rule tests that must
   be **extended** (see the table above).
3. **R5-LOW-F6** (`DESIGN_STATUS.md` row): outside this pass's permitted edit
   scope; left to the doc-sync pass.
4. **R1-NEW-5** (`supervisor.cpp:3376`): the reviewer says `:3376` is an inner
   branch return and the function's unconditional return is `:3378`. At HEAD,
   `handle_context` ends at `:3377` and its unconditional `return true;` is
   **`:3376`** (`nl -ba src/ui/supervisor.cpp | sed -n '3374,3378p'`); the spec's
   anchor is correct. No change.

---

## 12. Round-2 re-checks (appended)

Round 2 re-read the revised spec (944 lines) against HEAD `b26a9f344`. The six
reports are `/tmp/opencode/spec57-recheck-R1.md`, `spec57-recheck-R2.md`,
`spec57-recheck-R3.md`, `spec57-gate-R4-recheck.md`, `spec57-recheck-R5.md` and
`spec57-oracle-recheck.md`. Every finding is listed with its resolution;
disagreements are recorded. The spec stayed `draft` at this round (see §15).

### R1 (visual/UX) re-check

| # | Sev | Finding | Resolution |
|---|---|---|---|
| NEW-1 | MED | clamp omits the 2 border columns; badge cannot fit below ~35 cols | 57-D3 pins `w = max(0, min(computed, available_width-2))` and the fixed reserve. (The `= 30` bound this row first recorded was the regression; round 4 corrected 57-I20/57-F13 to `string_width(fixed_prefix) + 1 + kDeleteBadgeWidth` — 35 session / 32 workspace — see §14.) |
| NEW-2 | MED | wrong-file anchor for the workspace hint | Repointed to `src/ui/ui_render.cpp:997-998` in the header, 57-D3 and §8. |
| NEW-3 | LOW | 57-G4's first disjunct is vacuous | Dropped; only the border-column / total-width assertion remains. |
| NEW-4 | LOW | §9.5 heading contradicts its last bullet | Heading reworded to "Mandatory updates to existing tests". |
| NEW-5 | LOW | `supervisor.cpp:3376` anchor | **Disagreed** — `:3376` IS `handle_context`'s unconditional return (function ends `:3377`); no change. |
| H1/H2/M1/M2/L1/L2 | — | prior findings | Confirmed fixed (round 1). |

### R2 (ordering/data) re-check

| # | Sev | Finding | Resolution |
|---|---|---|---|
| MEDIUM-1 | MED | width invariance not computable; two formulas | One formula pinned in 57-D3/§4/57-I16: `w = max(footer, max_disarmed_row, kDeleteBadgeReserve)` with a fixed, state-independent reserve; no junction count feeds `w`. |
| LOW-1 | LOW | wrong anchor for the `51-D4.5` hint text | Repointed to `ui_render.cpp:997-998`; the clause is `51` §6.3 point 5. |
| LOW-2 | LOW | "Intent changes: None" contradicts `--resume` | Rewritten: the `--resume` precedence is an approved intent change (57-D7). |
| LOW-3 | LOW | naming drift ("cwd" after the rename) | 57-U1/57-U4 renamed; prose now says "effective root" (field `cwdWorkspacePath` retained). |
| LOW-4 | LOW | spurious "or canonicalisation fails" | Removed from 57-D6. |
| LOW-5 | LOW | "re-sorts once" imprecise | 57-D6/57-I19/57-F11 now say "re-sorts on each catalog snapshot". |
| LOW-6 | LOW | "badge always fits" vs clipping hedge | 57-I20/57-F13 qualified to the reserve bound. |

### R3 (key dispatch) re-check

| # | Sev | Finding | Resolution |
|---|---|---|---|
| MEDIUM-1 | MED | wrong file anchor (3 places) | Fixed (see R1-NEW-2). |
| MEDIUM-2 | MED | 57-H4 self-contradictory | Rewritten: `exitConfirm.open == false` only for Notice/Context/ModelPicker; the dialog/exitConfirm cases assert the flag is unchanged. |
| MEDIUM-3 | MED | clamp vs "terminal narrower than w" | Clamp pinned to `available_width-2`; the impossible sentence deleted; 57-I20/57-F13 qualified. |
| LOW-1 | LOW | 57-F7 tag `F6` loose | Retagged `—`. |
| LOW-2 | LOW | R5 record says `51-I23` is in the header/§8 | Corrected in the Oracle-§3 audit row: it lives in 57-D2. |
| LOW-3 | LOW | off-by-one exit-confirm anchor | Fixed to `:773-801` (return `:800`). |
| LOW-4 | LOW | 57-I15 "every" overstates | Reworded; Ctrl+C now named. |
| LOW-5 | LOW | undeclared Live-leaf deviation | Header + §8 now name the Live session leaf (`22` :934-935). |
| LOW-6 | LOW | golden range 2379 vs 2377 | Standardised on `:2379-2414` (the TEST line). |

### R4 (removal) re-check

| # | Sev | Finding | Resolution |
|---|---|---|---|
| N1 | MED | wrong file for the 51-D4.5 hint anchor | Fixed to `ui_render.cpp:997-998`; `51-D4.5` code comment vs `51` §6.3 point 5 disambiguated. |
| N2 | LOW | 57-I20 "badge never clipped" contradicts the clamp | 57-I20/57-F13 qualified to `available_width-2 >= kDeleteBadgeReserve`. |
| N3 | LOW | 57-D3 pins literal `- <title>` | 57-D3/§4 now pin `collapsed ? "+ " : "- "` (`ui_render.cpp:922`/`:924`). |
| N4 | LOW | 57-H4 guard count | "other five guards"; contradiction fixed. |

### R5 (conformance) re-check

| # | Sev | Finding | Resolution |
|---|---|---|---|
| N1 | MED | wrong file for the shipped hint | Fixed (see R4-N1). |
| N2 | MED | dangling `nowMs` comment un-amended | `include/ymh/ui/ui_model.hpp:551-554` added to Amends (header + §8); §1's "quotes every clause" claim corrected. |
| N3 | LOW | dispatch row 10 mislabels its handler | Table split into accurate rows (CtrlC `cancelActive :3426`, CtrlN `:3430`, CtrlO `:3434`, tail `handle_input :3462`). |
| N4 | LOW | 57-I15 omits Ctrl+C | Ctrl+C added; "every" reworded. |
| N5 | LOW | SW19 "supersedes" vs "narrows" ambiguity | Clarifying clause scoping "supersede" to the captured segment. |

### Oracle re-check

| # | Sev | Finding | Resolution |
|---|---|---|---|
| N1 | HIGH | `--resume` redefines "current directory" unapproved | **Approved by the user (57-D7):** effective-root first under `--resume`; §2.6/57-D5/57-I11/§11 made mutually consistent; the false "preserved" claim deleted. |
| N2 | MED | border undercount → 2-col clip | Clamp fixed to `available_width-2`; border accounting stated. |
| N3 | MED | `w` includes a hypothetical armed row / circular | Single fixed-reserve formula; `max_armed_row_width` removed; circularity resolved (disarmed content + constant). |
| N4 | MED | 57-I20 overclaims | Qualified; 57-F13 disposition corrected. |
| N5 | MED | dangling `nowMs` comment | Amended (see R5-N2). |
| N6 | LOW | 57-H4 guard count | Fixed (see R4-N4). |
| N7 | LOW | badge/name both `inverted`; only bold distinguishes | 57-I18 reworded; 57-G6 strengthened to assert the bold distinction. |
| N8 | LOW | local F10/F11 collide with §54 F10/F11 | Noted in the Naming paragraph (readability only). |
| N9 | LOW | `:3474` → `:3475` | Fixed in §4. |
| N10 | LOW | footer `Ctrl+D` vs user's `ctrl+d` | Kept and explicitly marked a deliberate literal deviation in 57-D2. |

### Round-2 disagreements

1. **R1-NEW-5** (`supervisor.cpp:3376`): the anchor is correct — see the
   Disagreements item above.

---

## 13. Round-3 re-checks (appended)

Round 3 re-read the revised spec (1165 lines) against HEAD `b26a9f344`. The six
reports are `/tmp/opencode/spec57-oracle-r3check.md` and `spec57-r3check-R1.md` …
`spec57-r3check-R5.md`. One HIGH (Oracle), two MEDIUM (Oracle) and the LOWs were
resolved. **HIGH-1 was settled empirically**, because the FTXUI source reads
oppositely depending on the enclosing layout.

**Measurement (FTXUI `build/_deps/ftxui-src`; `size.cpp:58`, `border.cpp:74-78`,
`composite_decorator.cpp:15-32`).** A throwaway probe rendered `window(title,
content)` with `size(WIDTH, EQUAL, w)` into a `Screen` and read the border-glyph
cell columns and the content-row width (multibyte border glyphs counted as cells
via `ftxui::string_width`, not bytes):

| construction (screen) | total | content |
|---|---|---|
| bare `window` then `size(W,EQUAL,20)` (60 cols) | 22 = w+2 | 20 = w |
| production `dbox{…, window then size(W,EQUAL,20) then clear_under then center}` (60) | 20 = w | 18 = w-2 |
| production `window(content then size(W,EQUAL,20)) then clear_under then center` (60) | 22 = w+2 | 20 = w |
| production `dbox{…, window then size(W,EQUAL,70) then clear_under then center}`, 70-col footer (200) | 70 = w | 68 = w-2 (clips the footer's last 2 cells) |
| production `window(content then size(W,EQUAL,70))`, 70-col footer (200) | 72 | 70 |

So **Reviewer B's reading is correct for the bare expression** (content `w`, total
`w+2`) and **Reviewer A / Oracle's is correct for the shipped production pipeline**
(total `w`, content `w-2`): `center` (`hbox(filler(), vbox(filler(), child,
filler()), filler())`) hands the `Size` node a box only `w` wide. The spec's
construction was the latter, so 57-I16/57-G4's "total `w+2`" was false and the
70-col footer clipped its last 2 chars. **Fix (57-D3, §4, 57-I16, 57-G4): apply the
`size` decorator to the window content**, `window(title, vbox(rows) | size(WIDTH,
EQUAL, w)) | clear_under | center`, which makes the content `w` and the total
`w+2` and keeps the clamp `available_width - 2`.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| Oracle-HIGH-1 | HIGH | border accounting inverted; 57-G4 false; 70-col footer clips | 57-D3/§4/57-I16/57-G4 now apply `size` to the **content** (measured: content `w`, total `w+2`); 57-G4 asserts both by rendering; the 70-col footer no longer clips. |
| Oracle-MED-1 | MED | `switcher_content_width` duplicates row construction | §4 pins `switcher_disarmed_rows(model)` as the single row builder consumed by `render_switcher` and `switcher_content_width`. |
| Oracle-MED-2 / R3-MEDIUM-1 / R4-N1 / R1-NEW-3 | MED/LOW | 57-I16 states the unclamped `w` | 57-I16 now states `w = max(0, min(computed, available_width - 2))` and the content/total semantics. |
| R2-MEDIUM-1 / R4-N2 / R1-NEW-4 | MED/LOW | badge-clip bound overclaims | 57-I20/57-F13 say "not guaranteed fully visible". (This row's first `= 30` bound ignored the fixed prefix; round 4 corrected it to `string_width(fixed_prefix) + 1 + kDeleteBadgeWidth` — 35 session / 32 workspace — see §14.) |
| R1-NEW-1 / R4-N3 / R5-N2 | LOW | "compile-time constant" cannot compile | 57-D3/§4 call `kDeleteBadgeReserve` a fixed runtime `const int` (not `constexpr`; `string.hpp:19`); integer literals pinned. |
| R1-NEW-2 | LOW | ellipsize budget omits the suffix reserve | 57-D3 states the workspace budget `w - (fixed_prefix + 1 + kDeleteBadgeWidth + 1 + suffix_width)`, where the suffix term is `kDeleteSuffixReserve` only while the suffix is rendered (§15). |
| R5-N1 / R2-LOW-1 | LOW | §2.5 table / 57-I15 omit Shift+Up/Down and `handle_dialog` | Table gains row 15 (`:3454`/`:3458`); `handle_dialog` (`:2674`, return `:2719`) added; 57-I15 extended. |
| R5-N4 | LOW | 57-G3/G4 assert via anonymous-namespace helpers | 57-G3/57-G4 now say "render and measure" (a cross-TU test cannot link them). |
| Oracle-LOW-1 | LOW | §11 "implemented as stated" overclaims | Reworded to "implemented with the documented deviations". |
| R2-LOW-2 | LOW | `ui_render.cpp:520-523` off-by-one | Corrected to `:521-523` (57-D3, 57-I20, 57-F13). |
| R2-LOW-3 | LOW | `arm_workspace_delete` range | Corrected to `:2855-2866`. |
| R2-LOW-4 | LOW | 57-D7 wording | Reworded ("holds verbatim for bare launches and for `--workspace` launches"). |

All round-1/2/3 HIGH/MEDIUM findings are resolved. The spec stayed `draft` (not
`verified`) pending the next independent pass (see §15).

---

## 14. Round-4 re-checks (appended)

Round 4 re-read the revised spec against HEAD `b26a9f344` with an independent
FTXUI probe (production `dbox` pipeline). The six reports are
`/tmp/opencode/spec57-r4check-R1.md` … `spec57-r4check-R5.md` and
`spec57-oracle-r4check.md`. The round-3 construction change was confirmed
correct (content `w` / total `w+2` in the shipped pipeline); three MEDIUMs
surfaced, all resolved below.

**M1 — the badge bound ignored the fixed prefix (R4-M1 / R5-W1 / Oracle, 3
reviewers).** The claim that the badge is fully visible for
`w >= 1 + kDeleteBadgeWidth` (= 30) with the row becoming `" " + badge` was
false: the budget at 57-D3 subtracts `string_width(fixed_prefix)`, so the prefix
survives ellipsization (R4 probe B measured: session `"    ["` full only at
`w >= 35`; workspace `"- "` full at `w >= 32`). **Chosen:** keep the prefix fixed
(it is a row-identity marker the budget already excludes) and correct the bound.
57-D3, 57-I20 and 57-F13 now all state
`w >= string_width(fixed_prefix) + 1 + kDeleteBadgeWidth` — 35 session / 32
workspace — with the row `fixed_prefix + " " + badge` (session) /
`fixed_prefix + " " + badge + " " + <suffix>` (workspace). **Disagreement with
Oracle's `33`:** Oracle counted the workspace suffix's leading `" "` as competing
with the badge, but at `w = 32` the suffix has already ellipsized to `{}` (budget
`w - 33 = -1`), leaving `2 + 1 + 29 + 1 + 0 = 33`; the single overflow cell is
absorbed by the trailing separator that follows the badge — FTXUI's
`ComputeShrinkHard` shrink (`box_helper.cpp:46-61`), since there is no right-edge
clip — so the badge is already full at `w = 32` (R4's measurement); the floor is
`2 + 1 + 29`, not `33`. The `= 30` figure is retired (the `:1218`/`:1329`
historical rows — R1-NEW-1 and R2-MEDIUM-1 in §12 — now point here).

**M2 — `SwitcherRow` carried no styling/identity state (R2-MED-1, Oracle-LOW-A).**
`{text, kind}` could not select the per-node styles (cursor workspace cyan/bold
`ui_render.cpp:928-929`, cursor session `inverted` `:967-969`, attention red
`:971`, `fromDisk` vs live leaf `:956-965`), so `render_switcher` had to re-walk
the model by index and row order/styling could drift. **Chosen:** extend
`SwitcherRow` with `workspace`/`session`/`cursor`/`attention`/`from_disk` (§4) and
require `render_switcher` to style from those fields; 57-G7 asserts the alignment.

**M3 — suffix shrink contradiction (Oracle-LOW-C).** 57-D3 said the suffix may
shrink; 57-I6 said it is ellipsized only for counts exceeding the reserve.
**Chosen:** the suffix **may shrink** — on a terminal narrower than
`kDeleteBadgeReserve` it ellipsizes to `{}` so the never-ellipsized badge stays
visible (this is what makes the workspace floor 32, not 50). 57-I6 now says so;
57-D3's "the name ellipsizes to its remainder and then the suffix ellipsizes"
is retained.

| # | Sev | Finding | Resolution |
|---|---|---|---|
| R4-M1 / R5-W1 / Oracle-MED | MED | badge bound ignores the fixed prefix; `= 30` false | 57-D3/57-I20/57-F13 now bound at `string_width(fixed_prefix) + 1 + kDeleteBadgeWidth` (35/32), row `fixed_prefix + " " + badge`; §4 names `fixed_prefix` vs `name_element`. Oracle's `33` disputed (see M1). |
| R2-MED-1 / Oracle-LOW-A | MED | `SwitcherRow{text,kind}` cannot drive styling; index re-walk drifts | §4 adds `workspace`/`session`/`cursor`/`attention`/`from_disk`; `render_switcher` styles from them; 57-G7 tests alignment. |
| Oracle-LOW-C | MED | 57-I6 (suffix fixed) vs 57-D3 (suffix shrinks) | Suffix **may shrink** to `{}` below `kDeleteBadgeReserve`; 57-I6 reworded; workspace floor 32. |
| R1-LOW-1 / R4 | LOW | 57-I16 "total `w+2`" unqualified for an over-long title | 57-I16 now carries the 57-D3 title exception. |
| R2-LOW-1 / R3-LOW-2 / Oracle-LOW-B | LOW | 57-G4 does not name the cell measure | 57-G4 names `ftxui::string_width`/`Screen::dimx()`/`PixelAt` and forbids `ToString().size()`. |
| R2-LOW-2 | LOW | 57-G4 must not re-derive `w` | 57-G4 takes `w` from the disarmed render's measured content width. |
| R3-LOW-1 | LOW | "full armed shape fits with no ellipsizing" false for a long name | 57-D3/57-I20 now say only the fixed parts fit; the name is ellipsized to its remainder. |
| R3-LOW-3 | LOW | "prefix" ambiguous (fixed marker vs whole `name_element`) | §4/57-D3 name the fixed marker `fixed_prefix` and the whole string `name_element`. |
| R3-LOW-4 | LOW | 57-D3 "total never exceeds `available_width`" has a title exception | Qualified inline with the ≤ 10-col exception. |
| R1-LOW-3 | LOW | §13 row does not demonstrate the clip | Row replaced by the old-shape `w=70` case (`70/68`, clips), which does. |
| R2-LOW-2 / R2-LOW-3 / Oracle-LOW-1 | LOW | `:521-523`, `arm_workspace_delete :2855-2866`, §11 wording | Already corrected in round 3; re-verified present (no change). |

All round-1…4 HIGH/MEDIUM findings are resolved; the round-5 final re-check and
the verification follow.

---

## 15. Round-5 final re-check and verification (appended)

Round 5 re-read the revised spec against HEAD `b26a9f344` with independent FTXUI
probes. The six reports are `/tmp/opencode/spec57-oracle-r5check.md` and
`spec57-r5check-R1.md` … `spec57-r5check-R5.md`. The three round-5 MEDIUMs are
resolved; **Oracle's fifth and final pass returned PASS with zero open
HIGH/MEDIUM and explicitly states the spec may be marked `verified`** (R1, R2 and
R5 also PASS; R3 and R4 raised only the LOWs swept below).

**M1 — badge bound (round-4 carry-over).** The fix agent's dispute of Oracle's
`33` is upheld: the workspace floor is **32**, not `33`/`50` (measured; §14). The
residual false sub-figure — "fully clipped at `w <= 6`/`w <= 3`" — is corrected at
57-D3, 57-I20 and 57-F13 to the measured behaviour (partial `3..34`/`3..31`,
fully clipped `w <= 2`), and the mechanism wording now says FTXUI hard-shrinks
every child (no right-edge clip) instead of "the `fixed_prefix` survives"
(Oracle-LOW-1/LOW-2, R4-MEDIUM). The `w = 32` rationale cites `ComputeShrinkHard`
(`box_helper.cpp:46-61`): the suffix is already `{}` and the lone overflow cell
lands on the trailing separator.

**M2 — `SwitcherRow` fields (round-4 carry-over).** Sufficient and correct (§4);
the only residual is that 57-G7 cannot mechanically prove the *absence* of a
correctly-aligned index re-walk. 57-G7 now says so plainly (a directive, not a
guarantee), and its fixture is strengthened with a collapsed workspace, a Note
leaf and a second workspace (Oracle-LOW-3, R1-LOW-3, R2-LOW-1, R3-LOW-1).

**M3 — suffix shrink and its owner (round-4 carry-over).** The suffix may shrink
to `{}`; 57-D3/57-I6 agree and the workspace floor is 32. R3's ownership question
is closed by naming the ellipsizer: the suffix budget is computed by the **caller
that knows `w`** — `render_switcher` (which receives `available_width`)
pre-ellipsizes the `(N sessions)` string and passes it to `with_delete_confirm`,
whose signature now takes that pre-ellipsized `suffix` (empty for a session) and
has no width (§4, 57-D3). The name-budget prose now uses the subtracted form and
charges `kDeleteSuffixReserve` only while the suffix is rendered, so the name is
not over-truncated for `35 <= w < 53` (R4-LOW-2).

| # | Sev | Finding | Resolution |
|---|---|---|---|
| Oracle-LOW-1 / R4-MEDIUM | LOW/MED | "fully clipped at `w <= 6`/`w <= 3`" false; FTXUI hard-shrinks | 57-D3/57-I20/57-F13 corrected to partial `3..34`/`3..31`, fully clipped `w <= 2`, with measured figures (`w=6 → 4/29`, `w=3 → 1/29`, `w=2 → 0/29`). |
| Oracle-LOW-2 | LOW | floor-32 rationale says "right-edge clipping" | Reworded: no right-clip; the suffix is `{}` and the overflow cell lands on the trailing separator (`ComputeShrinkHard`, `box_helper.cpp:46-61`) — 57-D3 and §14. |
| R4-LOW-2 | LOW | name budget always reserves `kDeleteSuffixReserve` | 57-D3 charges the reserve only while the suffix is rendered (`w >= kDeleteBadgeReserve`); below that it subtracts the suffix's actual ellipsized width. |
| R1-LOW-1 | LOW | 57-D3 budget prose read as *adding* the suffix terms | Reworded to the subtracted form `w - (… + 1 + suffix_width)`, matching §4/§11. |
| R1-LOW-2 | LOW | stale self-referential line numbers | Repointed to the current §12 rows (`:1218`/`:1329`, R1-NEW-1 and R2-MEDIUM-1). |
| R2-LOW-2 | LOW | 57-G4 asserts `N sessions` unconditionally | 57-G4 now pins the test width to `w >= kDeleteBadgeReserve` so the suffix is rendered. |
| R3-LOW-2 | LOW | "name ellipsizes to `{}`" claimed for all `[35,53)` | 57-F13/57-I20 reworded: the name is ellipsized to its remainder (at the bound, to `{}`). |
| R1/R2/R3/R4-LOW | LOW | 57-G7 overclaims a mechanical guarantee | 57-G7 states it is a directive (catches a misaligning re-walk only) and strengthens the fixture; the "never re-walks" requirement is enforced by review. |
| R3-MEDIUM (ownership) | MED | `with_delete_confirm` has no width to ellipsize the suffix | Closed: `render_switcher` (knows `w`) pre-ellipsizes the suffix; the signature takes the pre-ellipsized `suffix` (§4). |

**Verification.** Oracle's fifth pass: **PASS — zero open HIGH/MEDIUM**; it
"explicitly states the spec may be marked `verified`". R1, R2 and R5 also PASS.
R3 and R4 issued `DO NOT APPROVE` on the round-5 MEDIUMs (the false clip figure
and the suffix-ownership gap), both resolved above; their remaining items were
LOWs, all swept. **Status: `verified` (Rev 1).** The residual LOWs are
non-blocking and recorded in the table above. The "stayed `draft`" lines in
§11–§14 describe their own rounds; this section supersedes them.
