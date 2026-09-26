# 60 — Transcript Scroll Keybindings Errata (Rev 5): Row-Exact Shift+Arrow Step, Shift+Page Top/Bottom

Status: **verified (Rev 5)** (design-first gate closed; see `DESIGN_STATUS.md`). The gate closed with Oracle **PASS** after five revisions (`/tmp/opencode/spec60-oracle-rev5.md`), which confirmed the design is sound, row-exact, and delivers all three user requests; only LOW documentation residuals remained and are swept below. No source or test file is modified by this spec until implementation.

Rev 5 answers the independent re-check of Rev 4 (Oracle,
`/tmp/opencode/spec60-oracle-rev4.md`), which **confirmed the design is sound and
all three user requests are delivered** — the row model, the row-exact step, the
`viewport_rows = y_max - y_min + 1` convention, the same-frame re-clamp, and the
`48-I9`/`51-I8` re-scope all hold — but left documentation fixes. **M-1:** the
view-switch/composer offset is **not** a one-frame self-correction. The stored
`viewport_rows` lags the box by **two** draws (`observeGeometry` runs *after*
`build_ui` (60-D13) while `focus_row` reads the store *during* `build_ui`), so a
box-height change at frame N renders at **N and N+1** and clears at **N+2** (~100
ms at `kFrameInterval = 50ms`, `supervisor.cpp:51`); a parent↔child view
switch lands the offset at N+1, not at the switch frame. Terminal
**resize** is unbounded in ΔV and is now quantified in `60-I12`; `60-F7` names
all three cases (composer growth, view switch, resize). **L-6:** Rev 4's
"re-entering an already-materialized child preserves its prior scroll" is
**false** — `return_subagent` (`supervisor.cpp:1042-1049`) pops and then
`sync_subagent_subscriptions` (`:1013`) erases every child that left the path
(`UiModel::eraseSession` `ui_model.cpp:906` `sessions.erase(id)`), pinned by
`58-I19` (`58:1780`; `58:519`, `58:1749`), so no scrolled state survives to be
re-entered. `58-I15` is therefore restored to **unaffected** (its original
tail-on-entry claim is true) and the false amendment is removed. **60-F8** is
retagged `—` (it is not a §54-`F8` resource-cap concern). The `position()`
removal is swept explicitly (§6 `60-U1`: `tests/unit/ui_model_test.cpp:615,619,626,630,632`;
`ui_render.cpp:428`). It supersedes Rev 4 where they differ. No source or test
file is modified by this spec.

Rev 4 answers the independent re-check of Rev 3 (Oracle,
`/tmp/opencode/spec60-oracle-rev3.md`), which **confirmed the row model is
correct and that no design change is needed** — the row-exact step, the
`viewport_rows = y_max - y_min + 1` convention, the same-frame re-clamp, and the
`60-I12` composer caveat all hold — but raised two MEDIUMs and six LOWs, all
documentation/consistency fixes. **M-1:** `60-D9`'s "`viewport_rows` is
view-invariant" is false across a view switch (the parent composer is ≤8 rows vs
the child hint's 1, `ui_render.cpp:526-528` vs `:530-536`). **M-2:** `48-I9`/`51-I8`
were marked "unaffected" but are literally violated by `60-D13`'s post-render
mutation. Rev 4 extends `60-D9`/`60-I12` to the view-switch case (a bounded,
self-correcting offset whose exact lag Rev 5 corrects to two draws), changes the
`48-I9`/`51-I8` dispositions to **amended (scope clarified)** with precise new
scope (it also amended `58-I15`, which Rev 5 reverts to **unaffected**), adds the
missing §2 signatures, sweeps `ui_render.cpp:89-95`, corrects the `60-I12` anchor
to `frame.cpp:46`, records the `60-D7` page-landing shift, seeds `60-U3`'s
viewport to the golden's real transcript box height, and pins the child-entry
scroll behaviour. It supersedes Rev 3 where they differ. No source or test file is
modified by this spec.

Rev 3 answers the independent re-check of Rev 2 (Oracle,
`/tmp/opencode/spec60-oracle-rev2.md`), which **confirmed HIGH-1 fixed** — the
row model is exact against FTXUI `frame.cpp:60-69` (`dy = f - external_dimy/2`,
`external_dimy = y_max - y_min`) — but raised three MEDIUMs and six LOWs:
**MEDIUM-A** the `viewport_rows` convention was unpinned (a load-bearing
off-by-one), **MEDIUM-B** `60-I8` was false across a fold/unfold because the
re-clamp lagged the content change, **MEDIUM-C** `60-I12` overclaimed composer
isolation for one frame; plus LOWs D–I (metrics staleness across a view switch,
two unswept `focusPositionRelative` refs, the metrics test-helper gap, the
null-active early return, the double-layout cost, and one wording). Rev 3 pins
`viewport_rows = y_max - y_min + 1` (60-D10), moves the re-clamp to the frame
that measures the content (60-D13), scopes `60-I12` honestly, and sweeps the
LOWs. It supersedes Rev 2 where they differ. No source or test file is modified
by this spec.

Rev 2 answers the independent gate (Oracle, `/tmp/opencode/spec60-oracle.md`):
**HIGH-1** (the primary "by single line" requirement was not delivered — the
step was a fraction of *content* height), **MEDIUM-1** (invalid `F7` tag on
`60-F5`), **MEDIUM-2** (stale load-bearing anchors), **LOW-1** (the `57-F12`
amendment was incomplete), and the Oracle's remaining LOWs (57 §2.5 row
numbering, the streaming test, the 59 interaction, loose citations, the U5
pairing). It supersedes Rev 1's `60-D1`/`60-D2` and removes Rev 1's known
limitation `60-L1`. No source or test file is modified by this spec.

This errata answers the user request, verbatim:

> *"scrolling of log is only by Page Up/Down. Add scrolling by shift + arrows
> up/down by single line. And shift page up/down to beginning/end."*

**Finding (verified on HEAD).** The two bindings exist, but the first half of
the request is **not** delivered. The current global scroll bindings are:

| Key | Sequence | Handler | Line |
|---|---|---|---|
| `PageUp` / `PageDown` | `\x1b[5~` / `\x1b[6~` | `scroll_by(true,true)` / `scroll_by(false,true)` | `src/ui/supervisor.cpp:3644-3651` |
| `Ctrl+Home` / `Ctrl+End` | `\x1b[1;5H`,`\x1b[1;5~` / `\x1b[1;5F`,`\x1b[4;5~` | `scroll_to_top()` / `scroll_to_bottom()` | `src/ui/supervisor.cpp:3652-3659`, predicates `:161-167` |
| `Shift+Up` / `Shift+Down` | `\x1b[1;2A` / `\x1b[1;2B` | `scroll_by(true,false)` / `scroll_by(false,false)` | `src/ui/supervisor.cpp:3660-3667`, predicates `:169-175` |

`Shift+Up`/`Shift+Down` already route to `ConversationScroll::lineUp/lineDown`
(`src/ui/supervisor.cpp:2774-2789`), but that step is `kLineStep = 0.04f`
(`include/ymh/ui/ui_model.hpp:131`), and the transcript is positioned with
`focusPositionRelative(0, position())` (`src/ui/ui_render.cpp:428`). FTXUI maps
that `y` to `focused.box.y_min = int(min_y * y_)` over the **content** height
(`build/_deps/ftxui-src/src/ftxui/dom/focus.cpp:41-44`), so one step moves
`≈ 0.04 × H` rows: one row only at `H ≈ 25`, ~4 rows at `H = 100`, ~8 rows at
`H = 200`. On a long log `Shift+Up` jumps several lines — exactly the user's
complaint. Rev 2 replaces the fraction step with a **row-exact** one (§1 60-D2,
§2); the genuinely new binding is still `Shift+PageUp`/`Shift+PageDown` →
top/bottom (`60-D3`), which is terminal-dependent (`60-D4`).

## 1. Decisions

| ID | Decision | Rationale / cites |
|---|---|---|
| 60-D1 | **`Shift+Up`/`Shift+Down` keep their binding (`supervisor.cpp:3660-3667`) and now move the view by exactly one rendered row.** The routing is unchanged; the step semantics move into a row-exact `ConversationScroll` (`60-D2`). The user's first half is delivered. | Supersedes Rev 1's "record, do not rebind — already satisfied" (`60-D1`) and Rev 1's `60-L1`; the fraction step is the defect (`ui_render.cpp:428`, FTXUI `focus.cpp:41-44`). |
| 60-D2 | **The scroll position is a row-exact integer offset.** `ConversationScroll` holds `int top` — the index of the first visible content row — plus the last measured `content_rows` and `viewport_rows`; `lineUp`/`lineDown` change `top` by exactly 1. The renderer emits a **row-indexed focus** decorator (`row_focus`) fed the *return* of `focus_row(content)` — i.e. `clamp(top + (viewport_rows-1)/2, 0, content-1)` (§2). **`viewport_rows` is the rendered row count `y_max - y_min + 1` (60-D10), never FTXUI's internal `external_dimy = y_max - y_min`; `(viewport_rows-1)/2` is exactly FTXUI's `external_dimy/2` (`frame.cpp:60-63`), so `dy = top` row-exactly.** Alternatives are rejected on **measured effort**, not impossibility: (i) a model-side pre-wrapped row model duplicates FTXUI/markdown wrapping and the `expand_all_folds`/reasoning-separator layout (`ui_render.cpp:343-430`) — rejected as high-cost duplication; (ii) keeping the content fraction and only shrinking `kLineStep` cannot be row-exact at every `H` — rejected as it fails the requirement. | Rev 2 replaces Rev 1's "render→model coupling is impossible" (overstated) and Rev 1's "keep the fraction + known limitation" (unacceptable). FTXUI `focusPositionRelative` is a fraction of `min_y` (`focus.cpp:41-44`); only an integer row is exact. |
| 60-D3 | **Add `Shift+PageUp` / `Shift+PageDown` → `scroll_to_top()` / `scroll_to_bottom()`.** Match `event.input() == "\x1b[5;2~"` / `"\x1b[6;2~"` (the xterm modifier encoding for Shift+PageUp/PageDown), exactly like the shipped `is_shift_up`/`is_shift_down` (`supervisor.cpp:169-175`). | New capability, additive; mirrors the existing predicate style; no FTXUI change needed (§2). |
| 60-D4 | **The new binding is opportunistic, not guaranteed.** FTXUI *does* pass the sequence through (see §2 evidence), but many terminals bind Shift+PageUp/PageDown to their **own** scrollback and never forward it (xterm's default `Shift <Key>Prior: scroll-back(...)` translations; hterm binds `Shift-PGUP` by default). The **reliable** top/bottom bindings remain `Ctrl+Home`/`Ctrl+End` (already shipped, `supervisor.cpp:3652-3659`); 60 invents no new fallback key. | "Do not silently invent a key": the reliable alternative already exists; the requested alias is added where the terminal delivers it. |
| 60-D5 | **Dispatch placement.** Insert the two branches **after** the `Shift+Up`/`Shift+Down` branches (`:3660-3667`) and **before** `return handle_input(event)` (`:3668`). Popups still precede them (57-D4, 57-I15); the helpers resolve the target through `viewed()` (`:995`, `ui_model.cpp:818-832`) so a viewed child scrolls, not the parent (58-I12). | Preserves 57-D4/57-I15 and 58-I12 exactly. |
| 60-D6 | **Harness key seam.** Add `"shift-page-up"` / `"shift-page-down"` to `dispatch_key` (`supervisor.cpp:4023`), mapping to `Event::Special("\x1b[5;2~")` / `Event::Special("\x1b[6;2~")`, beside the existing `"shift-up"`/`"shift-down"` (`:4093-4098`). | Test-only; mirrors the shipped seam. |
| 60-D7 | **`PageUp` / `PageDown` stay content-fraction-based and unchanged.** One page is `max(1, int(content_rows × kPageStep))` rows with `kPageStep = 0.20f` (`ui_model.hpp:130`) — the same 20%-of-content semantics as today, now expressed in rows so it composes with the row model. A viewport-relative page is **rejected**: the user asked to keep page-wise scrolling, the shipped contract is content-relative, and spec 59's variable viewport does not change the content size. **L-3 (landing shift):** "unchanged" means the page *step* (`0.2·C` rows) and the end behaviour are preserved, **not** an identical intermediate row. Today's centred fraction lands at `dy = int(C·f) - (V-1)/2` (`ui_render.cpp:428`, FTXUI `frame.cpp:60-64`); Rev 3 lands at `dy = top` with `top` stepped by `page_rows()`. The absolute landing therefore shifts by ~`V/2`, but `PageDown` reaching `max_top()` still re-follows and `PageUp` at `0` still clamps (60-D8). | Preserves 60-D7 Rev 1; records the 59 interaction the Oracle's LOW-4 asked for. |
| 60-D8 | **Follow/tail is a row state, not a sentinel.** `following == true` means "pinned to the last viewport of content". It is cleared by any upward scroll (`lineUp`, `pageUp`, `toTop`) and re-enabled by `toBottom` (Ctrl+End, Shift+PageDown, PageDown reaching `max_top()`), or by `lineDown`/`pageDown` reaching `max_top()`. While `following`, the renderer pins the last row (`focus_row == content_rows`); while `!following`, `top` is absolute, so **streamed content appended below does not move the viewport** and `onNewContent()` raises `unseen` (`ui_model.cpp:552-556`). | Pins the streaming case the Oracle's LOW-3 flagged; preserves 48-F14 (`onNewContent` never clears `following`). |
| 60-D9 | **The scroll state is per-session (hence per-view).** `ConversationScroll` already lives in each `SessionUiState` (`ui_model.hpp:275-285`), and the transcript pane renders `viewedSession()` (`ui_render.cpp:1787`). A `subagent_path` push/pop therefore keeps each live view's own `top`/`following` (an ancestor's survives a nested pop; a popped child is erased, 58-I19); the measurement is applied to `viewed()->scroll` (60-D13), never the parent's. **Staleness (LOW-D):** a session not rendered while another is viewed keeps only its own last-known `content_rows`/`viewport_rows` (in its `ConversationScroll`), but a view switch cannot apply a stale *content* height — `render_conversation` re-measures the newly-viewed session in the frame that first renders it and `focus_row` consumes that fresh `content`, while `viewport_rows` is terminal-derived but **not view-invariant** (M-1): the transcript box differs per view — the parent composer is up to 8 rows (`ui_render.cpp:526-528`) vs the child's 1-row read-only hint (`:530-536`), so ΔV can be up to 7. The single `scroll_metrics_` is written by the *previous* view's `SetBox` and read by the frame callback before the new view's `SetBox` (60-D10), so the frame after the switch observes the old view's `viewport_rows` against the new box and renders `dy = top + (V_old-1)/2 - (V_new-1)/2` (≤3 rows for ΔV≤7); the store lags the box by **two** draws (60-I12), so on a parent↔child switch the offset lands at N+1 and clears at N+2. `top` is per-session and preserved, and `content_rows` is fresh. | Answers "does the subagent view share this state?"; preserves 58-I12/58-D3 and 58-I15 (a child's state is materialized at `following == true`, its scroll at the tail; §7.1). |
| 60-D10 | **Measurement seam (render measures, app applies in the same frame).** A pure `TranscriptMetrics { int content_rows; int viewport_rows; }` is filled by `render_conversation` (content height from `ComputeRequirement`) and by a file-local `TranscriptViewportNode` (viewport height from `SetBox`; built by `wrap_viewport_metrics`). **Convention (MEDIUM-A): `viewport_rows = box.y_max - box.y_min + 1`, the rendered row count — not FTXUI's internal `external_dimy = y_max - y_min` (`frame.cpp:60`), which is one less.** `SupervisorApp` holds a single `TranscriptMetrics scroll_metrics_` (only the viewed session is rendered), passed by pointer to `build_ui` each frame; the frame callback (`supervisor.cpp:3679-3682`) applies `viewed()->scroll.observeGeometry(scroll_metrics_.content_rows, scroll_metrics_.viewport_rows)` **in the same frame** (60-D13, MEDIUM-B). At that instant `content_rows` is fresh (written by `render_conversation` during build) and `viewport_rows` is the previous frame's (written by `SetBox` during `ftxui::Render`, which runs *after* the callback returns) — hence the **two-draw** viewport lag of `60-I12` (the stored value lags the box by two draws, not one). The per-session `ConversationScroll` is the geometry store, so no per-session cache is needed (LOW-D). The pure renderers (`render_conversation`, `build_ui`) **never mutate the model** (48-I9, 51-I8). The frame callback *does* apply `observeGeometry()` after `build_ui` returns; that is a bounded, idempotent write to the view-scope `ConversationScroll` only (not to the renderer proper), which is why 48-I9/51-I8 are **amended (scope clarified)** in §7.2 rather than left "unaffected" (M-2). **Cost (LOW-H):** `render_conversation` calls `content->ComputeRequirement()` once to measure and the pipeline then lays the conversation out again, so markdown/wrap runs ~2× per frame — accepted, bounded, and confined to the transcript pane. | Keeps the pure renderers pure; makes the offset self-clamping in the frame that changes the content, not one event late (MEDIUM-B). The measurement node mirrors the existing custom-node pattern (`LeftBar` `ui_render.cpp:50-83`, `CaretAnchor` `:96-119`). |
| 60-D11 | **Composer height (spec 59) changes only the viewport, not the step.** `kComposerMaxRows = 8` (`59-D1`, `ui_render.cpp:528`) makes `viewport_rows` variable; `top` is viewport-independent, so a composer growth keeps the same anchor row and the same one-row step, and `observeGeometry()` re-clamps `top` to the new `max_top()`. The frame that grows the composer renders with a stale `viewport_rows`; the store lags the box by **two** draws, so the offset renders at N and N+1 and clears at N+2 (60-I12). | Answers the "variable viewport height" question; 59 owns the composer, 60 owns the transcript. |
| 60-D12 | **`Ctrl+Home`/`Ctrl+End` are unchanged** and remain the reliable top/bottom bindings (`supervisor.cpp:3652-3659`). | The user asked for top/bottom; the pair already exists. |
| 60-D13 | **The re-clamp runs in the frame that measures the content (MEDIUM-B).** `SupervisorApp`'s frame callback (`supervisor.cpp:3679-3682`) calls `build_ui(model_, size, theme, &scroll_metrics_)` and then, if `viewed() != nullptr`, applies the measurement via `viewed()->scroll.observeGeometry(scroll_metrics_.content_rows, scroll_metrics_.viewport_rows)`. `build_ui`/`render_conversation` stay pure; the post-build application of geometry is a bounded, idempotent view-scope mutation (48-I9/51-I8 amended, §7.2). The *same* frame's rendered offset is computed from the pre-frame `top`, but `focus_row(measured)` clamps its return to `[0, content-1]` and FTXUI's `Frame::SetBox` clamps `dy` to `[0, C-V]` (`frame.cpp:63-64`), so the rendered top equals the re-anchored `top`; at frame end the stored and rendered tops agree. | `observeGeometry()` was previously applied only at the next `handle_event`; a fold that shrank the content left `top` above the new `max_top()` for the whole inter-frame window (MEDIUM-B). Applying at the measurement site makes `60-I8` hold at every frame and dispatch boundary. Resize/viewport changes remain behind by **two** draws: `viewport_rows` is written by `SetBox` during `ftxui::Render` (after the callback returns) and is read by `focus_row` during the *next* `build_ui`, so a box change at N renders at N and N+1 and clears at N+2 — see `60-I12`. |

Rev 1's `60-L1` (a fraction step is not one row) is deleted: `60-D2` makes the
step exact. Rev 3/Rev 4 record one **self-correcting** discrepancy whose lag Rev 5
corrects to **two draws** — a box-height change in the composer/viewport renders
with the stored `viewport_rows` for frames N and N+1 and clears at N+2 (`60-I12`)
— which is a rendering-frame caveat, not a permanent scroll limitation; the
composer height still does not change `top` or the step.

## 2. Interface sketch (signatures only)

```cpp
// include/ymh/ui/ui_model.hpp — ConversationScroll Rev 3 (replaces :124-146, including the leading comment whose `fraction` description becomes stale).
struct ConversationScroll {
    static constexpr float kPageStep = 0.20f;   // content fraction, unchanged (60-D7)

    // Index of the first visible transcript row (0 = first content row).
    // Meaningful only while `following == false`; observeGeometry() clamps it
    // to [0, max_top()].
    int  top           = 0;
    // Last measured rendered content height, in rows (TranscriptMetrics).
    int  content_rows  = 0;
    // Last measured transcript viewport height, in rows (TranscriptMetrics).
    int  viewport_rows = 0;
    bool following     = true;
    bool unseen        = false;

    [[nodiscard]] int max_top() const {
        return std::max(0, content_rows - std::max(1, viewport_rows));
    }
    [[nodiscard]] int page_rows() const {
        return std::max(1, static_cast<int>(static_cast<float>(content_rows) * kPageStep));
    }
    // Row index handed to the renderer's row-indexed focus (60-D2). `content`
    // is the just-measured height so the upper clamp is never stale; `following`
    // yields `content` (one past the last row) and the frame clamps to bottom.
    // `viewport_rows` is the rendered row count `y_max - y_min + 1` (60-D10), so
    // `(vp-1)/2` equals FTXUI's `external_dimy/2` (`frame.cpp:60-63`).
    [[nodiscard]] int focus_row(int content) const {
        if (following) {
            return content;
        }
        const int vp = std::max(1, viewport_rows);
        return std::clamp(top + (vp - 1) / 2, 0, std::max(0, content - 1));
    }

    void lineUp();      // top -= 1; clears following
    void lineDown();    // top += 1; re-follows at max_top()
    void pageUp();      // top -= max(1, content_rows*kPageStep)
    void pageDown();    // top += the same; re-follows at max_top()
    void toTop();       // following=false, top=0
    void toBottom();    // following=true, top=max_top(), unseen=false
    void onNewContent();// if !following: unseen = true
    void observeGeometry(int content_rows, int viewport_rows);  // clamp top
};

// src/ui/ui_render.cpp — file-local row-indexed focus (60-D2). It is the exact
// integer analogue of FTXUI's `focusPositionRelative` (focus.cpp:31-55): same
// `focused.enabled = true`, same single-cell box, same `component_active`
// default (false), so 48-D5.1's caret tie-break is unchanged; only the box's
// y is an integer row instead of `int(min_y * y_)`.
class RowFocus final : public ftxui::NodeDecorator {
public:
    RowFocus(ftxui::Element child, int row);
    void ComputeRequirement() override;   // focused.box.y = row (x = 0)
};
[[nodiscard]] ftxui::Decorator row_focus(int row);   // row_focus(r)(child)

// include/ymh/ui/ui_render.hpp — pure measurement out-param (60-D10).
struct TranscriptMetrics {
    int content_rows  = 0;   // from ComputeRequirement().min_y
    int viewport_rows = 0;   // from SetBox: y_max - y_min + 1 (MEDIUM-A; NOT external_dimy)
};
[[nodiscard]] ftxui::Element build_ui(const UiModel& model, TerminalSize size,
                                      const Theme& theme,
                                      TranscriptMetrics* out = nullptr);

// src/ui/ui_render.cpp — file-local viewport measurement (60-D10), mirroring the
// existing custom-node pattern (LeftBar :50-83, CaretAnchor :96-119). It reports
// the rendered row count, NOT FTXUI's internal external_dimy (MEDIUM-A).
class TranscriptViewportNode final : public ftxui::Node {
public:
    TranscriptViewportNode(ftxui::Element child, TranscriptMetrics* out);
    void SetBox(ftxui::Box box) override;   // out->viewport_rows = y_max - y_min + 1
};
[[nodiscard]] ftxui::Element wrap_viewport_metrics(ftxui::Element child,
                                                   TranscriptMetrics* out);

// src/ui/supervisor.cpp — inserted after :3667, before :3668 (60-D5).
bool is_shift_page_up(const ftxui::Event& event);    // "\x1b[5;2~"
bool is_shift_page_down(const ftxui::Event& event);  // "\x1b[6;2~"
if (is_shift_page_up(event))   { scroll_to_top();    return true; }
if (is_shift_page_down(event)) { scroll_to_bottom(); return true; }
```

`render_conversation` measures the content, then decorates:

```cpp
Element content = ftxui::vbox(std::move(rows));
content->ComputeRequirement();
const int measured = std::max(1, content->requirement().min_y);
if (out != nullptr) { out->content_rows = measured; }
Element framed = std::move(content) |
                 row_focus(active->scroll.focus_row(measured)) |
                 ftxui::yframe | ftxui::vscroll_indicator;
return out != nullptr ? wrap_viewport_metrics(std::move(framed), out) : std::move(framed);
```

**Null path (LOW-G).** The shipped `render_conversation` returns early when
`active == nullptr` (`ui_render.cpp:408-411`) *before* the measurement block
above, so on that path it must leave `*out` **untouched** rather than write a
stale `content_rows`. The app applies the measurement only when
`viewed() != nullptr` (60-I9), so the persistent `scroll_metrics_` member cannot
leak a stale value into a null view; the early return is pre-existing and
unchanged by 60.

**Cost (LOW-H).** The `content->ComputeRequirement()` above is a full layout of
the wrapped conversation (markdown + wrap), and the pipeline then lays it out
again to paint, so the transcript costs ~2× layout per frame. Accepted: it is
bounded by the rendered pane, confined to the transcript, and buys the row-exact
measurement without a model-side pre-wrap (60-D2).

**Viewport measurement (MEDIUM-A).** `wrap_viewport_metrics` wraps `framed` in
`TranscriptViewportNode`, whose `SetBox` writes
`out->viewport_rows = box.y_max - box.y_min + 1` (the rendered row count). This
runs during `ftxui::Render`, after `build_ui` returns — which is why the
composer-growth frame still uses the previous `viewport_rows` (60-I12).

Method bodies (`src/ui/ui_model.cpp`, replacing `:503-556`):

```cpp
void ConversationScroll::observeGeometry(int content, int viewport) {
    content_rows  = std::max(0, content);
    viewport_rows = std::max(1, viewport);   // viewport = y_max - y_min + 1 (60-D10)
    if (content_rows == 0) { following = true; top = 0; unseen = false; return; }
    if (following)         { top = max_top(); return; }
    top = std::clamp(top, 0, max_top());     // content shrank under the anchor
}
void ConversationScroll::lineUp() {
    if (following) { following = false; top = max_top(); }
    top = std::max(0, top - 1);
}
void ConversationScroll::lineDown() {
    if (following) { return; }
    top = std::min(max_top(), top + 1);
    if (top >= max_top()) { following = true; unseen = false; }
}
void ConversationScroll::pageUp() {
    if (following) { following = false; top = max_top(); }
    top = std::max(0, top - page_rows());
}
void ConversationScroll::pageDown() {
    if (following) { return; }
    top = std::min(max_top(), top + page_rows());
    if (top >= max_top()) { following = true; unseen = false; }
}
void ConversationScroll::toTop()        { following = false; top = 0; }
void ConversationScroll::toBottom()     { following = true; top = max_top(); unseen = false; }
void ConversationScroll::onNewContent() { if (!following) { unseen = true; } }
// page_rows() = max(1, int(float(content_rows) * kPageStep))  (60-D7)
```

**FTXUI evidence.** `Event` exposes no shift-modified page variants
(`build/_deps/ftxui-src/include/ftxui/component/event.hpp:56-61` defines only
`PageUp`/`PageDown`, `event.cpp:325-326` = `\x1b[5~`/`\x1b[6~`). The input parser
terminates a CSI on a final byte in `0x40-0x7E` and returns `SPECIAL` with the
whole `pending_` string (`terminal_input_parser.cpp:360-379`), so
`\x1b[5;2~` / `\x1b[6;2~` reach `handle_event_inner` as `Event::Special`,
distinguishable from `PageUp`/`PageDown`. What the **terminal** sends is the
variable part (60-D4). Row-exactness was verified against the fetched FTXUI:
`focusPosition`/`focusPositionRelative` map a focus row `f` to a top row
`clamp(f - external_dimy/2, 0, internal_dimy - external_dimy - 1)`
(`frame.cpp:60-64`), where `external_dimy = y_max - y_min = viewport_rows - 1`
and `internal_dimy = content_rows`; so with `f = top + (viewport_rows-1)/2`,
`dy = top` exactly, and stepping the integer `f` by 1 steps the viewport by
exactly one row in the scrolling range and clamps at both ends. **The `+1` is
load-bearing (MEDIUM-A): if `viewport_rows` were taken as `y_max - y_min`, `f`
would use `(V-2)/2`, the rendered top would drift one row for even `V`, and
`max_top()` would read `C-V+1` (re-following one row low).**

## 3. Invariants

| ID | Invariant |
|---|---|
| 60-I1 | `Shift+Up`/`Shift+Down` move the viewed transcript by **exactly one rendered row**: `top` changes by ±1 and the viewport's first visible row changes by exactly 1 (or is clamped at the top/bottom). `lineUp` clears `following` (seeding `top = max_top()` first); `lineDown` while following is a no-op; neither pages nor jumps to an end. |
| 60-I2 | `PageUp`/`PageDown` keep page-wise scrolling of `max(1, int(content_rows × kPageStep))` rows, unchanged in semantics (60-D7). |
| 60-I3 | When the terminal delivers `\x1b[5;2~`/`\x1b[6;2~`, they call `scroll_to_top()`/`scroll_to_bottom()` on the viewed session: `toTop` yields `following == false, top == 0`; `toBottom` yields `following == true, top == max_top(), unseen == false`. |
| 60-I4 | When the terminal intercepts the key, the app receives no event and nothing changes; `Ctrl+Home`/`Ctrl+End` remain the reliable top/bottom bindings and behave as before (60-D4, `supervisor.cpp:3652-3659`). |
| 60-I5 | Every new/recorded scroll binding acts on `viewedSession()` — the viewed child when `subagent_path` is non-empty — never the active session (58-I12; `ui_model.cpp:818-832`). |
| 60-I6 | Every new/recorded scroll binding is dispatched **after** the popup guards, so an open popup consumes it and no popup state is mutated (57-D4, 57-I15; guards `supervisor.cpp:3591-3607` precede the branches `:3644-3668`). |
| 60-I7 | The new bindings mutate only the viewed `ConversationScroll` and mark `UiDirtyFlag::Conversation` (`supervisor.cpp:2788,2797,2806`); they never touch `InputModel` (draft/cursor/history), the event log, or any `agent.*`/`session.*` RPC. |
| 60-I8 | `top` is in `[0, max_top()]` at every frame and dispatch boundary: `observeGeometry()` re-clamps it in the frame that measures the content/viewport (60-D13, so a fold/unfold cannot leave it out of range for an inter-frame window), and `lineUp`/`lineDown`/`pageUp`/`pageDown` clamp against `max_top()`. Within a single frame the render may compute `focus_row` from the pre-frame `top`, but `focus_row(measured)` clamps its return to `[0, max(0, content-1)]` and FTXUI clamps `dy` to `[0, C-V]`, so the rendered top equals the re-anchored `top`. |
| 60-I9 | `scroll_by`/`scroll_to_top`/`scroll_to_bottom` with no viewed session (`viewed() == nullptr`) are no-ops and never dereference null (`supervisor.cpp:2776-2777,2793-2794,2802-2803`). |
| 60-I10 | The pure renderers `render_conversation`/`build_ui` never mutate the model (48-I9/51-I8 **as amended**, §7.2): they only **report** `content_rows`/`viewport_rows` into `TranscriptMetrics` and never write the model. The application is app wiring: `SupervisorApp`'s frame callback applies the fresh `content_rows` (and the last-known `viewport_rows`) via `observeGeometry()` in the same frame (60-D13); this is an explicit, bounded, idempotent post-layout observation of view-scope scroll state, not a claim of renderer purity. In steady state the application is idempotent (unchanged metrics leave `top` unchanged), so re-rendering the same model is stable, and the golden tests exercise the pure renderers directly. |
| 60-I11 | While `following`, streaming/appended content keeps the last viewport pinned (`focus_row == content_rows`) and leaves `unseen == false`; while `!following`, appended content leaves `top` unchanged and sets `unseen == true` (60-D8). |
| 60-I12 | A composer-height change (spec 59) changes only `viewport_rows`; `top` is unchanged, so the anchor row and the one-row step are unaffected, and `observeGeometry()` re-clamps `top` to the new `max_top()` (60-D11). **Honest two-draw-lag caveat (MEDIUM-C, corrected by M-1):** `focus_row` consumes the *stored* `viewport_rows`, which lags the box by **two** draws. `observeGeometry` runs *after* `build_ui` (60-D13), while `focus_row` reads the session's stored value *during* `build_ui`; so at frame N `focus_row` uses the value `observeGeometry` wrote at N-1, which came from `scroll_metrics_` set by `SetBox` at N-2 (FTXUI's `Frame::SetBox` reads `requirement_.focused.box` at `frame.cpp:46` and computes `dy` at `:60-64`). A box whose height changed at frame N therefore renders with `V_old` against a `V_new` box at frames **N and N+1**: `dy = top + (V_old-1)/2 - (V_new-1)/2`; it clears at **N+2** once `SetBox` has written `V_new` and the store has propagated it. At 20 fps (`kFrameInterval = 50ms`, `supervisor.cpp:51`) that is ~100 ms. This covers three cases: (i) the **composer-growth frame** (`V_old`/`V_new` from `kComposerMaxRows`, `ui_render.cpp:528`; ΔV≤7 ⇒ offset ≤3 rows); (ii) the **view-switch frame** (M-1: a `subagent_path` push/pop changes the box between the parent composer ≤8 rows `:526-528` and the child hint's 1 row `:530-536`, ΔV≤7 ⇒ offset ≤3 rows; on a parent↔child switch the offset lands at **N+1**, not at the switch frame, and clears at N+2); and (iii) terminal **resize** (60-D13), where ΔV is the terminal's geometry delta and is therefore **unbounded** — the offset magnitude scales with ΔV and is *not* capped at 3 rows (only cases (i)/(ii) are bounded), though it is still transient and clears by N+2. The stored `top` is unchanged in all three. Computing the offset at `SetBox` time is infeasible without reimplementing FTXUI's frame (the focus box is fixed during `ComputeRequirement`), so this two-draw discrepancy is documented, not hidden. |

## 4. Failure modes

F# tags per `00-architecture.md` §54 (`:4832-4856`; F6 = input/keybinding focus).

| ID | Shared | Mode | Handling |
|---|---|---|---|
| 60-F1 | F6 | A new global scroll binding reaches its handler while a popup is open. | 60-I6; popup guards precede the branches (`supervisor.cpp:3591-3607` precede `:3644-3668`). |
| 60-F2 | — | The terminal binds Shift+PageUp/Down to its own scrollback and never forwards the sequence. | Documented (60-D4); no crash, binding simply inert; Ctrl+Home/End fallback (60-I4). |
| 60-F3 | — | A terminal emits a non-xterm modified-PageUp sequence (e.g. macOS Terminal). | Inert (no branch matches); Ctrl+Home/End fallback (60-I4); no new key invented. |
| 60-F4 | — | `\x1b[5;2~` arrives split across reads and is mis-parsed. | The parser only emits on the `~` final byte (`terminal_input_parser.cpp:360-379`); the identical pattern already ships for `\x1b[1;2A` (`supervisor.cpp:169-175`). |
| 60-F5 | F6 | A scroll key mutates the parent's scroll while a child is viewed (wrong session). | `viewed()` (`supervisor.cpp:995`; `ui_model.cpp:818-832`), locked by 60-I5/58-I12; the geometry is applied to `viewed()->scroll` (60-D9/60-D13), so the parent's state is never written. |
| 60-F6 | — | No viewed session when a scroll key is pressed. | Early return, no deref (60-I9). |
| 60-F7 | — | A stale `content_rows`/`viewport_rows` (content or composer changed since the last measurement) leaves `top` out of range. | `observeGeometry()` runs in the frame that measures the content (60-D13) and re-clamps against the fresh `content_rows`; `focus_row` takes the freshly measured `content`; FTXUI clamps `dy` as a last resort. The residual cases are **composer growth, the view switch, and terminal resize** — each renders with a stale `viewport_rows` for a transient two-draw-lag offset (60-I12), but `top` itself is never out of range. |
| 60-F8 | — | A very long transcript makes `content_rows` exceed the integer row range or overflows the focus box. | `content_rows` is an `int` bounded by the rendered rows (FTXUI `min_y`, itself an `int`); `focus_row` clamps to `[0, content-1]`; no arithmetic exceeds `int`. |

## 5. dsh mapping

| dsh behaviour | this spec | justification / anchor |
|---|---|---|
| Page / top / bottom transcript scroll | **mirrored** (60-D7, 60-D12) | Shipped `PageUp`/`PageDown` and `Ctrl+Home`/`Ctrl+End` (`supervisor.cpp:3644-3659`); 60 adds no page semantics. |
| Fine line-step transcript scroll (Shift+Up/Down) | **mirrored** (60-D1/60-D2) | ymh's finer-than-page step on `ConversationScroll` (`ui_model.hpp:129-146`); the reference harness's transcript view is specified only with `PageUp`/`PageDown` scroll (`10-supervisor-tui.md:1058-1059`), so the finer step is ymh's own refinement of the same capability, not a divergence. |
| Shift+PageUp/Down as a top/bottom alias | **not mirrored** — deliberate scope decision | A terminal-convenience alias of the existing `Ctrl+Home`/`Ctrl+End` (`supervisor.cpp:3652-3659`); no reference-harness counterpart, and deliverability is terminal-dependent (60-D4). |
| Popup consumes keys before globals | **mirror** of F6 | `00-architecture.md:4850`, 57-D4, 60-I6. |

## 6. Test plan

Measure rendered **cells**, never `std::string::size()`/`ToString().size()`;
use `ftxui::string_width` and `ftxui::Screen::PixelAt` (the cell-measuring
helpers: the `render_screen` helper at `tests/unit/ui_render_golden_test.cpp:2193`
and `string_width` at `:2237,2282-2283`). The shipped `render_screen`
(`:2193-2199`) calls `build_ui(model, size, theme)` with **no** metrics, so the
test plan adds a metrics-aware overload
`render_screen(model, size, theme, TranscriptMetrics* out)` that forwards `out`
to `build_ui` and then runs `ftxui::Render` (LOW-F).

| ID | Test | Asserts |
|---|---|---|
| 60-U1 | `tests/unit/ui_model_test.cpp` — extend `UiModel.ConversationScrollFollowAndUnseen` (`:611`); **rewrite** the removed-API assertions at `:615,619,626,630,632` | 60-I1/I3/I8: with `observeGeometry(40, 20)`, from following `lineUp()` → `following == false, top == 19`; a second `lineUp()` → `top == 18` (exactly one row); `lineDown()` twice returns to `following == true`; `toTop()` → `top == 0`; `toBottom()` → `following` and clears `unseen`. **API-removal sweep (LOW):** §2 replaces `ConversationScroll`, dropping `kLineStep`/`fraction`/`position()`; the existing `position()` assertions at `tests/unit/ui_model_test.cpp:615,619,626,630,632` must be rewritten to the row model (`following`/`top`) or the build breaks. The other call site is `ui_render.cpp:428` (`focusPositionRelative(0, active->scroll.position())`), replaced by the row-indexed `row_focus(focus_row(...))` decorator (§2); both are swept in the same change. |
| 60-U2 | `tests/unit/errata58_ui_test.cpp` — extend `Errata58.UI58_H6_ScrollKeysActOnViewedChild` (`:253`) or add `UI60_ShiftPageKeysActOnViewedChild` | 60-I5/58-I12: with a child viewed, `dispatch_key("shift-page-up")` sets the **child** `scroll.following == false && top == 0` and leaves the parent following; `dispatch_key("shift-page-down")` sets the child following; `dispatch_key("shift-up")` changes the child only. |
| 60-U3 | `tests/unit/ui_render_golden_test.cpp` — new golden beside `ScrolledConversationClipsAndHints` (`:396`) | 60-I1: with `observeGeometry(N, viewport)` seeded to the golden's **actual transcript box height** — `viewport_rows = box.y_max - box.y_min + 1` for the rendered `size` (L-4; not an arbitrary value) — and `N` the measured `content_rows`, one `lineUp()` moves the first visible row by exactly one (compute the top row from `render_screen(...)` cells, not byte offsets), and the last row is absent; `toTop`/`toBottom` show `line-0`/`line-N` respectively. |
| 60-U4 | `tests/unit/ui_render_golden_test.cpp` — new golden using the metrics-aware `render_screen` (`:2193`) | 60-I8/I10: `focus_row` still consumes the model position; the `Ctrl+End` hint row (`ui_render.cpp:432-439`) appears iff `!following`; `content_rows > 0` and `viewport_rows > 0`. **Helper gap (LOW-F):** `build_ui(..., &metrics)` alone can only observe `content_rows`; `viewport_rows` is written by `SetBox` during `ftxui::Render`. The test therefore calls `render_screen(model, size, theme, &metrics)` (which runs `ftxui::Render`) and asserts **after** it. |
| 60-U5 | `tests/unit/errata58_ui_test.cpp` — harness guard test, **paired** | 60-I6/57-I15: (a) with the switcher open, `dispatch_key("shift-page-up")` and `dispatch_key("page-up")` change no session's scroll; (b) **without** a popup, `dispatch_key("shift-page-up")` changes the viewed session — (b) is what isolates the new branch (the Oracle's test-plan note). |
| 60-U6 | `tests/unit/ui_model_test.cpp` — composer isolation | 60-I7: `dispatch_key("shift-page-up")` does not mutate `input.draft`, `input.cursor`, or history. |
| 60-U7 | `tests/unit/ui_model_test.cpp` — streaming while scrolled (Oracle LOW-3) | 60-I11: with `observeGeometry(40, 20)`, `lineUp()` then `onNewContent()` (as a streamed token would) leaves `top` unchanged, sets `unseen == true`, and does not clear `following` semantics; `toBottom()` clears `unseen`. |
| 60-U8 | `tests/unit/ui_model_test.cpp` — resize re-clamp (60-F7/60-I12) | `observeGeometry(100, 20)`, `lineUp()` ×5, then `observeGeometry(100, 8)` (composer grew) leaves `top` unchanged; `observeGeometry(3, 20)` (content shrank) clamps `top` to `max_top() == 0`; `observeGeometry(0, 20)` restores `following`. |
| 60-U9 | `tests/unit/ui_model_test.cpp` — fold re-clamp, both directions (60-D13/60-I8, MEDIUM-B) | With `observeGeometry(300, 20)` and `top == 250` (`!following`), a collapse `observeGeometry(200, 20)` clamps `top` to `max_top() == 180`; a subsequent expand `observeGeometry(300, 20)` leaves `top == 180` (in range). The frame callback applies `observeGeometry()` with the freshly measured `content_rows` in the same frame (60-D13); this asserts the re-clamp semantics it invokes. |

**Pre-fix evidence.** On HEAD, `Shift+PageUp`/`Shift+PageDown` fall through to
`handle_input` (`supervisor.cpp:3668`, `:3239`) and are unhandled (no branch
matches `\x1b[5;2~`/`\x1b[6;2~`); 60-U2/60-U3 fail before the change.
`Shift+Up`/`Shift+Down` route but are not row-exact: 60-U1/60-U3 fail on the
fraction model (`position()` returns `0.96f`, not a one-row move).

## 7. Supersedes / amendments

### 7.1 Named amendments (precise anchors, against HEAD)

- **Amends `57-switcher-sessions-popup-errata.md` §2.5** (`:197-219`): the
  dispatch table gains row **16** `Shift+PageUp`/`Shift+PageDown` →
  `scroll_to_top/bottom()` immediately after row 15 (`:218`), and the
  `handle_input` tail **renumbers to row 17** (it is row 16 today at `:219` —
  the Oracle's LOW-2). The popup-first order is unchanged (57-D4).
  **Re-anchoring (MEDIUM-2):** 57's printed line numbers are stale on HEAD; the
  authoritative HEAD lines are guards `supervisor.cpp:3591-3607`, globals
  `:3620-3640`, page `:3644-3651`, top/bottom `:3652-3659`, Shift+Up/Down
  `:3660-3667`, tail `:3668`. This amendment uses the HEAD numbers and does not
  propagate 57's stale ones.
- **Amends `57-I15`** (`57:824`): the set of globals consumed by an open popup
  now also includes `Shift+PageUp`/`Shift+PageDown`. Its printed anchors
  (`:3414`…`:3454-3460`) are stale; the HEAD anchors are `:3620`, `:3628`,
  `:3632`, `:3636`, `:3640`, `:3644-3651`, `:3652-3659`, `:3660-3667`.
- **Amends `57-F12`** (`57:848`): the parenthetical named-globals list
  currently reads `(Ctrl+S/P/N/O, Ctrl+C, PageUp/Down, Ctrl+Home/End)` and
  **already omits `Shift+Up`/`Shift+Down`** (the Oracle's LOW-1). The amendment
  changes it to `(Ctrl+S/P/N/O, Ctrl+C, PageUp/Down, Ctrl+Home/End,
  Shift+Up/Down, Shift+PageUp/PageDown)` — adding both the previously omitted
  pair (which 57-I15 already names at `:824`) and the two new bindings — and
  re-anchors its `:3395-3411` to `supervisor.cpp:3591-3607`.
- **Amends `58-I12`** (`58:1773`): scroll/fold keys acting on `viewedSession()`
  now explicitly include `Shift+PageUp`/`Shift+PageDown` (the existing
  `PageUp`/`PageDown`/`Ctrl+Home`/`Ctrl+End`/`Shift+Up`/`Shift+Down` are
  unchanged).
- **Amends `58` §6.B dispatch row** (`58:483`): the
  `PageUp/Down, Ctrl+Home/End, Shift+Up/Down, Ctrl+O` row gains
  `Shift+PageUp`/`Shift+PageDown`.
- **Amends `58-H6`** (`58:2307-2308`): `ScrollKeysActOnViewedChild` is extended
  to assert the two new bindings also act on the child (58-I12).
- **Amends `58-D3`** (`58:350-359`, `:737`): its parenthetical "same tail-follow
  (`position()` `:137`; `focusPositionRelative`, `ui_render.cpp:428`)" is
  updated to the Rev 2 row model — `ConversationScroll::top`/`focus_row()` and
  the row-indexed `row_focus(...)` decorator. The decision itself (the subagent
  view reuses one `ConversationScroll` per session) is unchanged. **LOW-E:**
  `58:737` (§4, "`vscroll_indicator` + `focusPositionRelative`") is swept in the
  same amendment; the Oracle's rev-2 check found it still named the removed
  decorator.
- **Amends `48-D5.1`** (`48:372-388`, `:1113`): the sentence "`focusPositionRelative` is
  retained for conversation scrolling" is updated to "the conversation's scroll
  decorator is the row-indexed `row_focus(...)` (`ui_render.cpp`), which sets the
  same `focused` slot (`enabled = true`, `component_active = false`)". The
  focus-ownership contract is unchanged: the caret remains the single owner via
  `component_active` (`ui_render.cpp:89-119`). **LOW-E:** `48:1113` (the Rev 8
  gate-resolution note, which also names `focusPositionRelative`) is swept in the
  same amendment. **L-1:** the `CaretAnchor` header comment
  (`ui_render.cpp:89-95`; the class declaration is `:96`, per HEAD) also names
  `focusPositionRelative` in that same single-focus-owner rationale; it is swept with `:428` in the same change.
- **Amends `48-I9`** (`48:424-425`) **and `51-I8`** (`51:378`): "rendering never
  mutates the model" is **re-scoped** (M-2). Its subject is the **pure renderers**
  (`render_conversation`, `build_ui`), which remain pure: they only report
  `TranscriptMetrics` and never write the model. The amendment states explicitly
  that `60-D13`'s frame callback additionally applies
  `viewed()->scroll.observeGeometry()` *after* `build_ui` returns — an explicit,
  bounded, idempotent mutation of the **view-scope `ConversationScroll`**
  (`top`/`content_rows`/`viewport_rows`/`following`/`unseen`) only. It writes no
  `InputModel`, event-log, or `agent.*`/`session.*` state (60-I7) and is a no-op
  in steady state (60-I10). The former "the frame callback is not a renderer"
  justification (60-D10/60-I10) is withdrawn as a definitional dodge; the honest
  disposition is **amended (scope clarified)**, see §7.2.
- **`58-I15` stays UNAFFECTED** (`58:1776`; L-6, reverting Rev 4): "entering a
  child sets its scroll to following (tail)" is **TRUE** as written.
  `enter_subagent` (`supervisor.cpp:1032-1039`) materializes the child at the
  default `following == true` (default-constructed `ConversationScroll`,
  `ui_model.hpp:129-146`) and never calls `toBottom`; the child is therefore
  already at the tail on entry. Rev 4's "re-entering an already-materialized child
  preserves its prior scroll" rests on a **false premise**: `return_subagent`
  (`supervisor.cpp:1042-1049`) pops the path and then
  `sync_subagent_subscriptions` (`:1013`) **erases** every child that left the
  path (`UiModel::eraseSession` `ui_model.cpp:906`, `sessions.erase(id)`), pinned
  by `58-I19` (`58:1780`; `58:519`, `58:1749` — "its state erased … re-entry
  replays from `Beginning`"). A scrolled child is destroyed on Esc/workspace-switch/
  path-clear and re-created at the default `following == true`; **no scrolled state
  ever survives to be re-entered**, so the re-scope to "first materialization" is
  vacuous (pop erases, so every entry *is* a first materialization) and the Rev 4
  rationale ("forcing `toBottom` would discard a reading position") is moot. The
  genuinely different case that *does* survive is an **ancestor** keeping its
  scroll across a nested pop — that is `58-I19`'s survivor, not "re-entry". No
  `58-I15` amendment. See §7.2.
- **Amends `10-supervisor-tui.md` §9.2** (`:1058-1059`): the input-editor
  binding list `Up`/`Down` history, `PageUp`/`PageDown` scroll gains the row-exact
  `Shift+Up`/`Shift+Down` step and the `Shift+PageUp`/`Shift+PageDown`
  top/bottom alias; the `Ctrl+Home`/`Ctrl+End` top/bottom binding is documented
  there for the first time (it is already shipped, `supervisor.cpp:3652-3659`).

### 7.2 Invariant sweep — proved unaffected

| Spec / invariant | Disposition | Why |
|---|---|---|
| `45-I2` (ArrowUp/Down: command list vs history, `45:1643`) | unaffected | Shift+Up/Down are matched at `supervisor.cpp:3660-3667` **before** `handle_input`; `45-I2`'s plain arrows are untouched. |
| `45-I3` (Tab completion, `45:1644`) and `45-I20` (agent cycling, `45:1661`) | unaffected | The new sequences are `\x1b[5;2~`/`\x1b[6;2~`, not `Tab`/`TabReverse`; they never reach `handle_input`. |
| `45-D1`/`45-D2`/`45-D9` | unaffected | Global scroll is not a composer key; no draft/history/command-list path is entered (60-I7). |
| `48-D5.1` (single focus owner; `48:372-388`, `:1113`) | **amended** (decorator identity only) | `focus_row` sets the same `focused.enabled = true` / `component_active = false` slot as `focusPositionRelative` (`focus.cpp:31-55`), so the caret's `component_active` tie-break (`requirement.hpp:35-44`) is unchanged. See §7.1. |
| `48-I8` (caret at `input.cursor`, `48:421-423`) | unaffected | No composer/caret change; 60 touches only the transcript decorator. |
| `48-I9` (rendering never mutates the model, `48:424-425`) and `51-I8` (`51:378`) | **amended (scope clarified)** | The pure renderers (`render_conversation`/`build_ui`) only **report** `TranscriptMetrics` and never write the model. But `60-D13`'s frame callback *does* apply `viewed()->scroll.observeGeometry()` inside `component->Render()`→`OnRender()`, so the unqualified "rendering never mutates the model" is literally violated; the honest scope is: **the renderer proper stays pure, and the post-render geometry observation is an explicit, bounded, idempotent mutation of view-scope scroll state only** (no `InputModel`/event-log/agent state; a no-op in steady state, 60-I7/60-I10). See §7.1. |
| `48-F14` (background reasoning while scrolled up does not change `following`, `48:781-782`) | unaffected | Only an explicit key clears `following` (60-D8); `onNewContent` still never touches `following` (`ui_model.cpp:552-556`). |
| `48` §9 key-ownership matrix (`48:457-464`) | unaffected | Every row there is a composer key (`Arrow*`, `Enter`, `Esc`, `Ctrl+O`); 60 adds global scroll bindings dispatched before the composer, so no row changes ownership. |
| `48-D7.3` (conversation `content_width`, `48:906-907`) | unaffected | Width is unchanged; 60 changes no layout. |
| `51-D4` (Ctrl+Q exit, Ctrl+D delete, `51:827-848`) | unaffected | 60 touches neither Ctrl+Q nor Ctrl+D. |
| `51-F3` (composer columns clip, `51:387`) | unaffected | No composer width/height change; 59 owns composer height. |
| `58-I3` (pane/strip/hint render `viewedSession()`, `58:1764`) | unaffected | No render target changes; 60 only routes scroll keys to the already-viewed session (60-I5). |
| `58-I4` (read-only child composer guard, `58:1765`, `supervisor.cpp:3257-3265`) | unaffected | The guard is inside `handle_input`, which the new branches precede; a child's composer stays read-only. |
| `58-I15` (child seeded to following on entry, `58:1776`) | **unaffected** (Rev 5, reverting Rev 4) | `enter_subagent` (`supervisor.cpp:1032-1039`) materializes the child at the default `following == true` and never calls `toBottom`, so "entering a child sets its scroll to following (tail)" is literally true. Rev 4's "re-entry preserves prior scroll" is false: pop erases the child state (`supervisor.cpp:1042-1049` → `:1013` → `ui_model.cpp:906`), pinned by `58-I19` (`58:1780`), so no scrolled child survives to be re-entered (every entry is a first materialization). See §7.1. |
| `58-D3` (subagent view reuses the same `ConversationScroll`, `58:350-359`, `:737`) | **amended** (parenthetical only) | The reuse is unchanged; only the `position()`/`focusPositionRelative` wording updates (both refs, LOW-E). See §7.1. |
| `59-D1` (`kComposerMaxRows = 8`, `59:20`; `ui_render.cpp:528`) | unaffected, interaction recorded | The composer height changes `viewport_rows`, which `observeGeometry()` folds into `max_top()`; the line step is viewport-independent and the page step stays content-fraction (60-D7/60-D11). No 59 invariant pins transcript scroll. The composer-growth frame renders with a stale `viewport_rows` (60-I12) — a self-correcting two-draw-lag caveat, not a 59 invariant change. |
| `10` §9.2 (`10:1058-1059`) | **amended** | See §7.1 — the binding list is extended. |
| `17` / `25` scroll invariants | none exist | Exhaustive search for scroll/`following`/`PageUp`/`Shift+Up` found no invariant in either spec (`17:198` and `25` mention scroll only in prose, not as an invariant). |

No source or test file is modified by this spec; it is design-only until the
independent gate marks it `verified`.
