# ymh — UI Surface Inventory: spec 16 vs RB-10 / RB-11 seam

Purpose: prevent `docs/design/16-daemon-ownership.md` (cross-supervisor visibility,
switching, last-exit prompt) and backlog items **RB-10** (session name top-right)
and **RB-11** (bottom line = counts only) from designing the same pixels twice,
inconsistently. This is an inventory + seam definition, **not** a design.

Method: every current behavior below is verified against `src/` / `include/` with
`file:line`; docs are not cited for current behavior (spec-22 rows cite the
pinned decision in `22-switcher-sessions-errata.md` §… alongside the code
anchor). Read-only analysis.

Ownership legend: **SPEC-16** = spec 16 owns the change; **SPEC-22** =
`22-switcher-sessions-errata.md` owns the change; **BACKLOG** = RB-xx owns it;
**SHARED** = both touch the same widget, seam stated explicitly.

---

## Surface 1 — Header / top line

- **Current:** `render_header()` (`src/ui/ui_render.cpp:364-372`) renders
  `"ymh · <cwd>"` on the left, then `ftxui::filler()` — **the right side is
  empty**. Placed by `build_ui()` at `src/ui/ui_render.cpp:397`. The active
  session is located with the same lookup used elsewhere
  (`src/ui/ui_render.cpp:387-394`, mirrored at `:239-246`).
- **RB-10** adds the active session's name after the filler (right-aligned),
  title from `SessionCell.title` (`include/ymh/ui/ui_model.hpp:203`), falling back
  to a short id (`src/ui/ui_render.cpp:219`). **BACKLOG owns the right slot.**
- **Spec 16** may want a peer-supervisor / ownership indicator. If so it must
  **not** take the right slot.
- **Seam:** the right-side slot is `filler()`-separated content in
  `render_header` (`:364-372`). RB-10 renders the session name there. A spec-16
  indicator (if any) composes in the **left** segment (after cwd) or in the
  switcher, never after RB-10's name. **Verdict: SHARED (right slot → BACKLOG).**

## Surface 2 — Bottom line / session bar

- **Current — per-session list:** `render_session_bar()`
  (`src/ui/ui_render.cpp:212-236`) renders one `[title glyph]` cell **per session
  of the active workspace only** (`:213` looks up `activeWorkspaceId`), placed at
  `src/ui/ui_render.cpp:405`.
- **Current — aggregate counts:** `AggregateStatus{activeCount, waitingCount}`
  (`include/ymh/ui/ui_model.hpp:257-262`); `recompute()` iterates **all sessions
  across all attached workspaces** (`src/ui/ui_model.cpp:225-241`, skipping
  non-`Attached` workspaces), rendered as `"N active · M waiting"` in
  `render_status()` (`src/ui/ui_render.cpp:273-274`), placed at `:413`.
- **RB-11** removes/shrinks the per-session list (`render_session_bar`) so the
  bottom shows counts only. **BACKLOG owns the collapse.**
- **Spec 16** owns the *scope/definition* of the surviving counts (does "active"
  span other supervisors' daemons?) and any ownership badge. It must **not**
  reintroduce a per-session list in the bottom row.
- **Seam:** `render_session_bar` (`:212-236,405`) is deleted/compacted by RB-11;
  the surviving widget is the aggregate in `render_status` (`:273-274`), already
  computed cross-workspace at `src/ui/ui_model.cpp:225-241`. Spec 16 changes
  *what the counts mean*, RB-11 changes *whether the list is shown*.
  **Verdict: SHARED.**

## Surface 3 — Switcher (Live) and `/sessions` (History)

- **Current (Live):** `SwitcherOverlayModel`
  (`include/ymh/ui/ui_model.hpp:308`); `open()` (`src/ui/ui_model.cpp:910`) builds
  a **workspaces → sessions** tree. Spec 22 (`22-switcher-sessions-errata.md` §3)
  makes it **live-only**: a workspace node renders iff `WorkspaceModel::live ==
  true` AND `daemonStatus ∈ {Attached, Stopping}`, and entries are **evicted**
  when their daemon dies (`evict_dead_workspaces`,
  `src/ui/supervisor.cpp:995`). `NotRunning` and `Unreachable` no longer render;
  only `Owned`/`Stopping` marks remain reachable (§3.4). Ordering is
  **effective-root first, then last usage descending, tie-broken by title
  ascending (case-insensitive) then `canonical_path`** (57-D5, superseding
  22-D1's workspace-order half).
  The old "across **all** `model.workspaces`, sorted by title" claim and the
  "title order matches the registry canonical-path order" claim are both false
  and superseded (§1.3.1 22-S1/22-S3, 22-A5). Rendered by `render_switcher()`
  (`src/ui/ui_render.cpp:461`). Tab toggles expand
  (`src/ui/supervisor.cpp:1711`); Enter focuses a session/workspace
  (`src/ui/supervisor.cpp:1725-1726` → `UiModel::focusSession`/`focusWorkspace`,
  `src/ui/ui_model.cpp:885,893`).
- **Current (History, `/sessions`):** the same `SwitcherOverlayModel` and
  `render_switcher` are reused with `source = SwitcherSource::History`
  (`SupervisorApp::open_sessions`, `src/ui/supervisor.cpp:775-777`); **no second
  overlay** (22-D3, §4.3, O14/C4). `/sessions`
  (`src/ui/command_registry.cpp:217`) lists stored sessions for **every
  registered workspace** (live or not), read **directly from disk**
  (`<workspace>/.ymh/sessions.db`), no daemon required, degrading per workspace
  with a note when a DB is missing / not a file / unreadable / corrupt / schema
  mismatch / read-only / unavailable (§4.1, §4.4, §4.5). Sessions are
  newest-first (`updated_at` desc, `id` asc); workspace groups use the same
  effective-root/last-usage rule (57-D5). Selecting a stored session in a
  non-running workspace spawns/attaches that daemon then resumes it
  (`resume_from_history`, `src/ui/supervisor.cpp:888`; §5); a spawn/resume
  failure surfaces a workspace-independent status-bar notice and never injects a
  phantom workspace (§5.3).
- **Cross-workspace data already exists:** the CLI attaches **every registered
  workspace with a live daemon** (read-only discovery, no spawn) at
  `src/cli/cli.cpp:365-393`, and each workspace connection populates its session
  cells via `session.list` (`refresh_sessions`, `src/ui/supervisor.cpp:1093`). So
  one supervisor already *lists and can focus* another supervisor's live
  workspace sessions.
- **Spec 16** owns the ownership/attach/detach semantics; **spec 22** owns the
  live-only narrowing and the History source. Both **reuse** this widget.
- **Seam:** spec 16 extends `WorkspaceNode`/`SessionNode` (e.g. an ownership or
  attached-elsewhere flag) and the `render_switcher` row; spec 22 adds the
  History source to the same overlay; no new overlay. RB-11 does **not** touch
  the switcher. **Verdict: SPEC-16 (semantics) + SPEC-22 (live-only/History);
  widget reused; SHARED boundary.**

## Surface 4 — Exit path

- **Current:** `SupervisorApp::requestExit()` (`src/ui/supervisor.cpp:237-242`)
  sets `quit_` and calls `screen_->Exit()` — **no confirmation**. Reached from:
  Ctrl+D (`src/ui/supervisor.cpp:713-715`), the `/exit` command via
  `context.request_exit` (`src/ui/supervisor.cpp:452`; command at
  `src/ui/command_registry.cpp:143-149`), and the single-process TUI's own
  `requestExit()` (`src/ui/ui_application.cpp:149-154`).
- **No confirmation UI exists anywhere** in the TUI (grep for `confirm` finds only
  an unrelated comment at `src/ui/supervisor.cpp:333`).
- **Reusable modal pattern:** the permission dialog `render_dialog()`
  (`src/ui/ui_render.cpp:286-312`, an `ftxui::window(...) | center`) and the
  switcher overlay are composed as `dbox` overlays in `build_ui`
  (`src/ui/ui_render.cpp:416-421`). Spec 16's last-supervisor prompt should reuse
  this overlay/`window` pattern.
- **Seam:** spec 16 hooks the confirmation **inside/around `requestExit`**
  (`src/ui/supervisor.cpp:237`); Ctrl+D and `/exit` remain thin callers.
  **Verdict: SPEC-16.**

## Surface 5 — Command registry

- **Current:** `CommandRegistry::builtin()` (`src/ui/command_registry.cpp:93-158`)
  registers `/new /clear /model /compact /exit /help`. Dispatch
  (`src/ui/command_registry.cpp:71-91`) is UI-local. `complete(prefix)` exists at
  `include/ymh/ui/command_registry.hpp:37` / `src/ui/command_registry.cpp:60-69`,
  and live hints are rendered from it (`src/ui/supervisor.cpp:467-480`,
  `src/ui/ui_render.cpp:198-210`).
- **Tab is unbound in the input handler** (`handle_input`,
  `src/ui/supervisor.cpp:609-696`); it is bound **only in the switcher**
  (`src/ui/supervisor.cpp:591`). That gap is **RB-08**.
- **Spec 16** owns any new command it needs (e.g. `/attach`, `/switch`) by
  registering it in `builtin()`; it should **not** add keybindings inside command
  handlers.
- **Seam:** registry is data-driven. Spec 16 adds `Command` entries; **RB-08**
  wires `Event::Tab` in `handle_input` to `complete()` and must stay generic (no
  hardcoded command names) so spec-16 commands appear automatically.
  **Verdict: SHARED (spec 16 = command entries; RB-08 = Tab wiring).**

---

## Conflict register (where the two would otherwise collide)

| # | Conflict | Resolution |
|---|----------|------------|
| C1 | Header **right slot**: RB-10 wants the session name; spec 16 may want a peer/ownership indicator there. | Right slot → RB-10 (session name). Spec-16 indicators go left of the filler or in the switcher. |
| C2 | Bottom line: raw note 4 says "both supervisors should display that besides the current session there is one more" — read as a *list*, it contradicts RB-11's counts-only. | Must be a **count/badge**, not a per-session list. Counts widget is `render_status` (`ui_render.cpp:273-274`). |
| C3 | Exit semantics: `/exit` + Ctrl+D currently quit immediately; spec 16 makes exit conditional (last-supervisor prompt). | Spec 16 owns the semantics at `requestExit` (`supervisor.cpp:237`); registry/keybindings stay thin callers. |
| C4 | Cross-supervisor visibility: the switcher already lists/focuses other live workspaces' sessions (`cli.cpp:365-393`, `ui_model.cpp:910`); spec 16 could duplicate this in the header. | Spec 16 **extends** the switcher; do not build a second visibility widget. |
| C5 | Count scope: `AggregateStatus` already spans **all attached workspaces** (`ui_model.cpp:225-241`), not the active one. | RB-11 documents this scope; spec 16 must not redefine it — only extend the notion of "attached" to its ownership model. |

## Summary

| Surface | Owner | Seam |
|---|---|---|
| Header / top line | SHARED — right slot → RB-10 | RB-10 name after `filler()`; spec 16 left-side/switcher |
| Bottom line / session bar | SHARED — RB-11 collapse, spec 16 count scope | delete `render_session_bar`; keep aggregate `render_status` |
| Switcher (Live) + `/sessions` (History) | SPEC-16 semantics + SPEC-22 live-only/History (reuse widget) | extend `WorkspaceNode`/`render_switcher`; RB-11 untouched |
| Exit path | SPEC-16 | hook `requestExit` (`supervisor.cpp:237`); reuse modal overlay |
| Command registry | SHARED — spec 16 commands, RB-08 Tab | register in `builtin()`; generic Tab → `complete()` |
