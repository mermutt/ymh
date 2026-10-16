# 86 - Paste Placeholder (Collapsed Long Paste) Errata

```
Status: verified (Rev 2)
Revision: 2
Verification status: adversarial reviewer (independent, Rev 2): GATE PASS (0 HIGH / 0 MEDIUM, 1 LOW); Oracle confirm PASS
Component: 86 (errata) - supervisor TUI composer. Collapses a long bracketed
           paste into one compact placeholder chip `[Pasted text #N +M lines]`
           instead of inlining the raw text, keeps the full text out of band in
           a per-draft registry, and expands the chip back to the full text on
           submit so the model receives the complete paste. Backspace/Delete
           adjacent to a chip remove the whole chip atomically; left/right and
           up/down motion treat a chip as one unit. Design only; no
           implementation code.
Revision note: Rev 2 fixes the gate-86 FAIL (0 HIGH / 2 MEDIUM / 5 LOW).
           MEDIUM 1: vertical motion (`composer_move_cursor_vertical`,
           59-D7) is now chip-aware (86-A9/86-D5/86-U14) and the false
           "caret never sits inside a chip" claim in 86-A7/Sec. 4.8 is corrected.
           MEDIUM 2: `expand_paste_chips` collects every live span from the
           ORIGINAL draft and replaces right-to-left (86-D6/PP2/86-U13), so the
           placeholder cannot survive into the request. LOW 1-5: the
           `std::move` contradiction, the 86-A5 overstatement, the PP1
           per-flush-segment scope, the editor-call-site conflation, and the
           86-U8 seam wording are corrected.
Depends on: 66-bracketed-paste-errata.md (verified Rev 5; the DECSET 2004
            lifecycle and `handle_paste_event`/`flush_paste` reassembly this
            errata extends);
            45-ui-interaction-errata.md (verified; Sec. 6 input dispatch);
            48-ui-and-config-errata.md (verified; 48-D4 word delete/motion,
            48-D5.1 glyph motion);
            59-composer-wrapping-errata.md (verified; multi-row draft wrap);
            62-composer-caret-and-step-limit-errata.md (verified; caret shape);
            58-subagent-navigation-errata.md (verified; 58-E8 read-only child
            composer);
            10-supervisor-tui.md (verified; Sec. 8.4 input dispatch);
            include/ymh/ui/ui_model.hpp:216-242 (`InputModel`);
            include/ymh/ui/ui_render.hpp:69 and src/ui/ui_render.cpp:2222-2288
            (`composer_move_cursor_vertical`, 59-D7 vertical composer motion);
            00-architecture.md Sec. 54 (failure-mode tags), Sec. 55;
            56-dsh-fidelity-gap-closure-errata.md (56-D6 non-mirror convention).
Scope: one additive behavior on top of 66's reassembled paste -- the draft may
       hold compact chips instead of raw long text, with an out-of-band store,
       atomic edit/motion, and submit-time expansion. No wire, event-log, model,
       or persistence change; the out-of-band text is supervisor-local and never
       persisted.
Supersedes: 66-D4's unconditional "one bulk verbatim `draft.insert`" for the
            long-paste case (the short-paste case is unchanged);
            the 66-I3 wording "every byte ... reaches the draft" is narrowed to
            "reaches the draft, or reaches the chip registry and the model at
            submit" (86-A1).
Amends: 45 Sec. 6 input dispatch (Backspace/Delete/ArrowLeft/ArrowRight are
        chip-aware); 48-D4/48-D5.1 (`delete_word`, `word_*_boundary`,
        `cursor_left`/`cursor_right`, `delete_forward`, `clear_line` gain chip
        semantics); 59 (wrapping unchanged -- a chip wraps like any text, but
        vertical motion is amended, 86-A9); 62 (caret rendering unchanged;
        horizontal AND vertical [59-D7] motion are amended so the caret edge-snaps
        out of a chip); 66-D8 (the modal composer snapshot mirrors the chip
        registry too).
Tracker: DESIGN_STATUS.md row 86 is orchestrator-owned and added only at
         promotion (AGENTS.md rule); this spec does not create or edit it.
```

## 1. Purpose, scope, and precedence

### 1.1 The feature

Today (66-D4) a bracketed paste is one bulk verbatim insert into
`InputModel::draft`. A large paste therefore fills the composer with many wrapped
rows and pushes everything else off-screen. This errata makes the composer
collapse a *long* paste into a single compact placeholder chip:

* **Short paste** -> inserted verbatim (current behavior, 66-D4 preserved).
* **Long paste** -> one chip `[Pasted text #N +M lines]` is inserted at the
  caret. `N` is a per-draft incrementing index (1, 2, 3, ...); `M` is the pasted
  line count. The full text is retained out of band and **expanded back on
  submit**, so the model receives the complete text and never sees the chip.
* Backspace/Delete adjacent to (or inside) a chip removes the **whole** chip
  atomically; left/right motion jumps over a chip as one unit, and up/down motion
  edge-snaps out of a chip.

The user-facing reference is Claude Code's `[Pasted text #N +M lines]` chip;
the mechanism (placeholder in the input buffer plus an out-of-band store that is
expanded on submit) matches `langchain-ai/deepagents` `paste_collapse.py` and the
atomic-token cursor/delete logic in the openclaude `Cursor.ts` reference. This
spec cites the **pattern**, not a dependency; ymh carries neither project.

### 1.2 Precedence and what this builds on (66)

This errata is **additive** over 66. It does not touch the DECSET/DECRST 2004
lifecycle, the reassembly state machine (`handle_paste_event`,
`src/ui/supervisor.cpp:4341`), the byte cap (`kMaxPasteBytes`,
`src/ui/supervisor.cpp:83`), or the idle flush (`kPasteIdleFlush`,
`src/ui/supervisor.cpp:84`). It changes two things: (1) what a completed flush
does with the reassembled text -- `flush_paste` (`src/ui/supervisor.cpp:4401`)
delegates to a new `InputModel::insert_paste` that chooses verbatim vs chip; and
(2) the composer's caret motion over a chip -- horizontal (86-A4) and vertical
(86-A9) -- plus submit-time expansion. The reassembly lifecycle itself is
untouched.

66's invariants that this errata **must not regress**: a newline inside a paste is
a literal draft character and never Return/submit (66-I2/66-F2); a paste never
submits mid-way (66-I2); a paste at the caret shifts the bytes after it (66-I4);
the one deliberate non-insert is the read-only child view 58-E8 (66-I3/66-I7).

Where 66 said "every byte reaches the draft" (66-I3), this errata narrows the
sentence for the long case: the bytes reach the **chip registry** (out of band)
and the **model at submit**; the draft holds the chip token instead. This is the
one semantic change and it is recorded in the amendment register (86-A1).

### 1.3 Non-goals

* No chip for a **non-bracketed** raw burst (a fast sequence of `Character`
  events without `ESC[200~`/`ESC[201~`). It is indistinguishable from typing; see
  86-D8. Only a 66 flush can produce a chip.
* No chip in the read-only child composer (58-E8): a paste there is still
  discarded (66-D4/66-I7 unchanged).
* No chip in any modal's own text field: a paste is captured into the composer
  (and its snapshot) exactly as 66-D8 does today; only the composer draft chips.
* No mouse/OSC 52/clipboard-manager integration (unchanged from 66 Sec. 10).
* No styling of the chip (no reverse-video/background) in Rev 1; the token's
  bytes render through the existing glyph pipeline (86-D9).

## 2. Amendment register

| ID | Amends | Anchor | Change |
|---|---|---|---|
| 86-A1 | 66-D4 / 66-I3 | `src/ui/supervisor.cpp:4401-4417` | `flush_paste` no longer implements the insert itself; it calls `InputModel::insert_paste(text)`, which inserts verbatim for a short paste and a chip token for a long paste. 66-I3 is narrowed for the long case: the bytes reach the registry (out of band) and the model at submit, not the draft. Short paste and the read-only-child discard are unchanged. |
| 86-A2 | 66-D8 modal snapshot | `src/ui/supervisor.cpp:4421-4426` | The chip registry is mirrored into `modal_composer_snapshot_->input` alongside the draft insert, so a chip created while a modal owns the keyboard is not orphaned when the snapshot is restored. |
| 86-A3 | 45 Sec. 6 input dispatch | `src/ui/supervisor.cpp:4094-4131` | Backspace and Delete are chip-aware (whole-chip erase); ArrowLeft/ArrowRight call the chip-aware `cursor_left`/`cursor_right`. |
| 86-A4 | 48-D4/48-D5.1 | `src/ui/ui_model.cpp:376-473` | `delete_forward`, `delete_word`, `word_left_boundary`, `word_right_boundary`, `cursor_left`, `cursor_right` never split a chip: a span that intersects a chip is widened to the whole chip, and a boundary inside a chip snaps to the chip edge. |
| 86-A5 | 48-D4.5 / 45 Sec. 6 clear path | `src/ui/ui_model.cpp:384-387`; `src/ui/supervisor.cpp:577-579`, `:595-599`, `:4053-4055`, `:1605`, `:2125`, `:3435`, `:3442` | `clear_line` (Ctrl-U) also clears the chip registry, and the listed clear sites call `clear_paste_chips`. The set of draft-replacing sites is **not** fully covered by `clear_line`: the rewind prompt restore (`:2125`), the command-completion writers (`:3435`, `:3442`), and the first-prompt carry-over (`:1605`) assign `draft` directly, so this amendment **requires** each to call `clear_paste_chips` (dropping chips whose bytes they overwrite). Consequence if missed is bounded (stale entries, `next_chip_index` not reset, a chip silently dropped by completion); see 86-F11. |
| 86-A6 | 59 (wrapping / vertical motion) | `src/ui/ui_render.cpp:689-709`, `:2222-2288`; `include/ymh/ui/ui_render.hpp:69` | Wrapping (`wrap_glyphs`) is unchanged: a chip wraps like any text and may span a row boundary. Vertical motion (`composer_move_cursor_vertical`, 59-D7) IS amended (86-A9) so a target boundary strictly inside a chip edge-snaps out of it. |
| 86-A7 | 62 (caret rendering) | `src/ui/ui_render.cpp:711-805` | `render_input` is unchanged; it reads `draft`/`cursor` as strings. The caret can rest on a chip boundary because BOTH the horizontal helpers (86-A4) and vertical motion (86-A9) snap out of a chip interior; `render_input` itself makes no chip-aware claim. |
| 86-A8 | 66-D9 editor hand-off | `src/ui/supervisor.cpp:2868-2892` | `edit_prompt` (Ctrl-E) expands the chips before handing the draft to `$EDITOR`, and clears the registry after copying the edited text back, so the editor shows the full text and the edited result is plain (86-D7). |
| 86-A9 | 59-D7 vertical composer motion | `include/ymh/ui/ui_render.hpp:69`; `src/ui/ui_render.cpp:2222-2288`; `src/ui/supervisor.cpp:4144`, `:4162` | `composer_move_cursor_vertical` gains a chip-span parameter and edge-snaps a target that lands strictly inside a chip (nearest edge by byte distance; tie -> start). Its two callers pass `InputModel::chip_spans()`. This closes gate-86 MEDIUM 1: without it, ArrowUp/ArrowDown can land the caret inside a live token (`draft` is wrapped, so a token can span rows). |

## 3. Decisions

* **86-D1 (threshold; per flush segment).** Each call to
  `InputModel::insert_paste` (i.e. each 66 flush segment) collapses its `text` to
  a chip iff `paste_line_count(text) > kPasteChipLineThreshold (3)` **or**
  `text.size() > kPasteChipByteThreshold (512)`; otherwise that segment is
  inserted verbatim (66-D4). `paste_line_count(t) = 1 + count(t, '\n')`. **No
  trimming**: leading/trailing whitespace and newlines are preserved in the
  out-of-band text and counted toward `M`. One physical paste larger than 66's
  `kMaxPasteBytes` (8 MiB) is flushed as several segments, so it may produce
  several chips (and any segment that crosses `kPasteChipTotalBytes` is inserted
  verbatim). See 4.4 and PP1/PP6.
* **86-D2 (representation).** The chip lives in `draft` as a **literal token**
  whose bytes are exactly its rendered text, and the full text lives in a
  per-draft **registry** (`InputModel::paste_chips`), keyed by `N`. A chip span is
  a byte-exact occurrence of a live registry entry's `token` in the draft. Chosen
  over a structured draft (chips + text runs); see 4.2 for the hazard analysis and
  the rejected alternative.
* **86-D3 (chip text, numbering, pluralisation).** Token =
  `[Pasted text #<N> +<M> line]` for `M == 1`, else
  `[Pasted text #<N> +<M> lines]`. ASCII only. `N` is a per-draft sticky counter
  (`next_chip_index`, starts at 1, never reused within a draft, reset on clear).
  See 4.4.
* **86-D4 (registry ownership + lifetime).** The registry is a member of
  `InputModel`; it is owned by the same object that owns `draft`. It is created
  with the `InputModel`, destroyed with it, never persisted, never on the wire,
  and never in the event log. It is cleared at submit, on `clear_line`, at the
  draft-replacing sites in 86-A5, after `edit_prompt` copy-back, and on
  session-state destruction. Bounded by `kMaxPasteChips` and
  `kPasteChipTotalBytes`; a segment that would exceed either is
  inserted verbatim (no chip), so every chip token in the draft always has a live
  entry. See 4.3 and Sec. 6.
* **86-D5 (atomic edit + motion).** No operation may produce a partial chip, and
  no motion may rest inside a chip. Backspace/Delete on a span that contains or
  abuts a chip erase the whole chip; ArrowLeft/ArrowRight jump over a chip;
  ArrowUp/ArrowDown edge-snap a target that lands inside a chip (86-A9);
  word motion and Ctrl-W widen any span that would split a chip. Backspace with
  the caret strictly inside a chip erases the whole chip and moves the caret to
  its start. See 4.5.
* **86-D6 (submit expansion; fixed point).** `InputModel::expand_paste_chips()`
  is the **sole** producer of the model-bound string: it is called exactly once
  in the `Event::Return` branch of `handle_input` (`src/ui/supervisor.cpp:4040`)
  and its result flows to `classify_enter`/`submit`/`dispatch_command`, then
  `submit` -> `prompt` -> `agent.prompt`. It collects **every** live chip span
  from the ORIGINAL draft (a single left-to-right pass, longest live token first)
  and replaces them right-to-left, so no chip's span can survive whether or not an
  earlier substitution inserted text that looks like another token (gate-86
  MEDIUM 2). `N` resets to 1 on submit. See 4.6 and PP2.
* **86-D7 (interactions).** Read-only child (58-E8): paste discarded, no chip.
  History: `push_history` stores the **expanded** text, so recall restores the
  full text verbatim (never re-chipped). `saved_draft` keeps raw bytes (may hold
  chips); the registry lives with the draft, so an ArrowUp/ArrowDown round-trip
  still expands. `edit_prompt`: expand before the editor, clear after. `/handoff`,
  `/export`: they consume the durable session, not the draft; no change (the
  composer's command dispatch receives the expanded draft, which for a typed
  command is identical). See 4.7.
* **86-D8 (non-bracketed burst).** Out of scope: a raw `Character` burst is never
  collapsed (86-F6). Only the 66 reassembly path can produce a chip.
* **86-D9 (rendering).** Unchanged. The token's bytes render through the existing
  glyph pipeline (`render_input`, `src/ui/ui_render.cpp:711`); the chip may wrap
  across rows. No style is applied in Rev 1.
* **86-D10 (rejected).** A **structured draft** (a variant of text runs and chip
  nodes). Rejected for Rev 1: it forces a rewrite of `InputModel::draft` (today a
  `std::string`), `wrap_glyphs`/`glyph_floor`/`glyph_at`/`glyph_len`
  (`src/ui/ui_render.cpp:689-757`), `composer_move_cursor_vertical`, history,
  `saved_draft`, the modal snapshot, and the `event.character()` insert -- a broad
  regression surface across 45/59/62 for no user-visible gain over a literal
  token. Deferred; the literal-token hazards are recorded in 86-F1/86-F2.

## 4. Design

### 4.1 Seam: where a chip is produced

`flush_paste` (`src/ui/supervisor.cpp:4401`) is the only producer. Today it does
`input.draft.insert(input.cursor, text); input.cursor += text.size();`
(`:4416-4417`). This errata replaces those two statements with
`input.insert_paste(text)` (by value, called with an lvalue so `text` is copied,
**not** moved), which:

1. Validates the paste target exactly as today: empty text or a viewed child
   (`!model_.subagent_path.empty()`) -> no-op; resolve the `SessionUiState` (or
   `pendingComposer`) exactly as `:4407-4414` does.
2. If the text is short (86-D1) -> verbatim insert at the caret, advance the
   caret (identical to 66-D4).
3. If the text is long and under the caps (86-D4) -> allocate `N` from
   `next_chip_index` (bumped past any chip-shaped `#k` token already in the
   draft; see 4.4), build the token, insert the token bytes at the caret, record
   `PasteChip{N, token, text, M}`, advance the caret by the token length.
4. If the text is long but would exceed a cap -> verbatim insert (86-D4).

The modal mirror (`:4421-4426`) calls `insert_paste` on the snapshot's
`InputModel` **with the same `text`**, so the snapshot registry receives its own
entry (86-A2). `text` is not moved (it must be available for both calls); each
call takes it by value and copies what it stores. Because both composers start
from the same counter and draft, the two calls allocate the same `N`. The paste
still never submits (66-I2); the chip is one atomic draft mutation.

### 4.2 Representation: literal token + registry (86-D2)

`draft` stays a `std::string`. A chip occupies a contiguous byte span whose bytes
are exactly the chip text, e.g. `[Pasted text #1 +12 lines]`. The out-of-band
store is `InputModel::paste_chips`, a `std::map<std::size_t, PasteChip>` keyed by
`N`; each `PasteChip` records `{index, token, text, line_count}`.

Recognition rule. A chip span is a byte-exact occurrence in the draft of some
live registry entry's `token`. `InputModel::chip_spans()` computes the live spans
in ascending start order with a single left-to-right pass over the draft, matching
the longest live token at each position; every occurrence is a span (a duplicate
token produces two spans). `chip_ending_at`/`chip_starting_at` consult that list.
A chip-shaped string with no live registry entry (a user typed it, or its entry
was cleared) is **inert literal text**.

Expansion rule (gate-86 MEDIUM 2). `expand_paste_chips` must not search a string
it is mutating. It (1) snapshots `draft` -- which is `const`, so it cannot change
-- (2) collects **all** live spans from that snapshot via `chip_spans()`, then
(3) replaces them right-to-left with the entries' `text`. Because every span is
identified before any replacement, and replacements proceed from the last span to
the first, no substitution can shadow a later one, and no chip span survives.
This is what makes PP2 hold; the previous "first occurrence, one span per entry"
rule did not (a duplicate or an inserted earlier chip's text could leave the real
span in place). Content delivered from a chip's own `text` is never re-scanned
(86-F10).

Why literal token (hazard analysis).

* **Blast radius.** The entire existing composer pipeline (59 wrap, 62 caret,
  48 motion, 45 dispatch, render) already operates on `draft` bytes. A chip whose
  bytes *are* its rendered text needs zero changes there. This is the smallest
  change that satisfies the feature.
* **Hazard H1 -- manual mimicry.** A user who hand-types a byte string identical
  to an existing live chip's token is indistinguishable from that chip. Mitigated
  by 86-D3's allocation rule: `insert_paste` allocates
  `N = max(next_chip_index, 1 + max_chip_shaped_index_in_draft)` and increments,
  so a hand-typed `[Pasted text #3 +2 lines]` forces the next real chip to `#4`;
  the typed token has no registry entry and is inert. The residual -- typing the
  exact token of a chip that is *already* live -- is recorded as 86-F1 (LOW):
  because `expand_paste_chips` replaces **every** occurrence of a live token, the
  typed duplicate is also expanded (so no token survives to the model, PP2), at
  the cost of substituting the same pasted text wherever the user typed it. This
  keeps PP2 unconditional; it is content-correctness, not a leak.
* **Hazard H2 -- cleared entry with a live token.** If a registry entry is
  cleared while its token remains in the draft (the user edited elsewhere and
  typed the token back), the token becomes inert literal text and reaches the
  model as written. Prevented where it matters (86-D4): the registry is only
  cleared together with the draft, and the cap fallback inserts verbatim rather
  than chip, so a chip token is never created without a live entry.

Rejected alternative (86-D10): a structured draft. It would make recognition
exact and collision-free, but at the cost of rewriting the draft type and every
glyph/caret/wrap/history consumer in 45/59/62. The literal-token hazards above
are bounded and recorded; the structured draft is deferred.

### 4.3 Chip registry: owner, eviction, bound (86-D4)

Owner. `InputModel::paste_chips`, a member of the same `InputModel` that owns
`draft`. The per-session composer is `SessionUiState::input`
(`include/ymh/ui/ui_model.hpp:338`); the zero-workspace composer is
`UiModel::pendingComposer` (`:840`); each owns its registry. Session switch is
therefore inherently safe: the registry switches with the draft, so a chip in
session A's draft is never expanded against session B's registry, and a
`forget_session_state` drops draft and registry together.

Clear points (86-A5): submit (`src/ui/supervisor.cpp:577` for `pendingComposer`,
`:595` for a session), `clear_line` (Ctrl-U), the `dispatch_command` branch
(`:4053`), the other draft-replacing sites `:1605`, `:2125`, `:3435`, `:3442`,
`edit_prompt` copy-back (`:2887`), and `InputModel`/`SessionUiState` destruction.
The draft-replacing sites are listed explicitly because they assign `draft`
directly rather than routing through `clear_line`; each must call
`clear_paste_chips` (86-F11 records the consequence if a future site forgets).
Deleting a single chip (86-D5) erases that entry immediately (its out-of-band text
is freed); the remaining chips and `N` are untouched (86-D3 numbering).

Bound (86-D4). `kMaxPasteChips = 64` and `kPasteChipTotalBytes = 16 MiB` per
draft. `insert_paste` checks: if `paste_chips.size() >= kMaxPasteChips` or
`retained_bytes + text.size() > kPasteChipTotalBytes`, the paste is inserted
**verbatim** (no chip). The registry therefore never exceeds its bound, and no
chip token ever exists without a live entry.

### 4.4 Threshold, chip text, numbering (86-D1/86-D3)

Constants (named, tunable):

```cpp
constexpr std::size_t kPasteChipLineThreshold = 3;
constexpr std::size_t kPasteChipByteThreshold = 512;
```

`paste_line_count(t) = 1 + count(t, '\n')` -- so `"a\nb"` is 2 lines and
`"a\nb\n"` is 3. No trimming: the out-of-band text is the exact reassembled
paste (as 66 produced it), and the count reflects it.

Chip text: `[Pasted text #` + decimal(`N`) + ` +` + decimal(`M`) + ` ` +
(`line` if `M == 1` else `lines`) + `]`. Examples:
`[Pasted text #1 +4 lines]`, `[Pasted text #2 +1 line]`. ASCII only.

Numbering: `N` comes from the per-draft sticky counter `next_chip_index`
(initialized 1). Before allocating, `insert_paste` scans `draft` for
chip-shaped tokens `[Pasted text #<digits> ...]` and sets
`N = max(next_chip_index, 1 + max_scanned_index)`, then
`next_chip_index = N + 1`. Consequences (86-D3): `N` is strictly increasing
within a draft and never reused; deleting chip `#2` does **not** renumber `#1` or
`#3`; submit and `clear_line` reset the counter to 1.

### 4.5 Atomic delete, caret skip, word motion (86-D5)

All operations in `InputModel` (`src/ui/ui_model.cpp`), driven by `handle_input`
(`src/ui/supervisor.cpp:3960`):

* `chip_ending_at(pos)` -> the span `[start, end)` of a live chip with
  `start <= pos < end` **or** `end == pos`; else `nullopt`.
* `chip_starting_at(pos)` -> the span with `start <= pos < end` **or**
  `start == pos`; else `nullopt`.
* **Backspace** (`handle_input:4094`): if `chip_ending_at(cursor)` is a span,
  erase `[start, end)`, set `cursor = start`; else erase one glyph as today.
  Backspace with the caret strictly inside a chip erases the whole chip.
* **Delete** (`:4104` -> `delete_forward`): if `chip_starting_at(cursor)` is a
  span, erase `[start, end)`, leave `cursor` at `start`; else one glyph.
* **ArrowLeft** (`:4124` -> `cursor_left`): if `pos` is inside a chip or
  `pos == chip.end`, return `chip.start`; else the glyph step. **ArrowRight**
  (`:4129` -> `cursor_right`): if `pos` is inside a chip or
  `pos == chip.start`, return `chip.end`; else the glyph step. Thus motion from
  just before a chip jumps to its end, and from just after it jumps to its start.
* **Word motion** (`Ctrl+Arrow`, `:4114`/`:4119`): unchanged formula, then snap
  the result to a chip edge if it falls inside a chip (never split).
* **Ctrl-W** (`delete_word`, `:4179`): unchanged formula, then widen the erased
  span to whole chips if it would intersect one.
* **ArrowUp/ArrowDown** (`:4144`/`:4162` -> `composer_move_cursor_vertical`,
  59-D7): the helper gains a `std::span<const ChipSpan>` argument
  (`include/ymh/ui/ui_render.hpp:69`, `src/ui/ui_render.cpp:2222-2288`). After it
  computes its target boundary it **edge-snaps** a target that lies strictly
  inside a chip to the nearest chip edge by byte distance (tie -> the chip's
  start), so a wrapped/narrow composer cannot rest the caret inside a token. The
  two callers pass `input.chip_spans()`. This is gate-86 MEDIUM 1: before it,
  vertical motion could land inside a chip because `draft` wraps a token across
  rows.
* **Select.** The composer has no selection feature (no Shift+motion handler
  exists in `handle_input`); this is stated, not implemented. If a selection is
  added later, it must obey the same no-partial-chip rule.

Adjacent chips: when chip `#k` ends exactly where chip `#k+1` begins, Backspace at
the boundary deletes `#k` (via `chip_ending_at`) and Delete deletes `#k+1` (via
`chip_starting_at`); the tie rule is deliberate and pinned by 86-U4.

### 4.6 Submit expansion (86-D6)

`InputModel::expand_paste_chips() const` is pure and total: it computes
`spans = chip_spans()` (all live spans, ascending start, from the immutable
`draft`), then builds the result by replacing those spans **right-to-left** with
their entries' `text`. Pre-collecting from the original string and replacing
right-to-left is the fix for gate-86 MEDIUM 2: a substitution cannot shadow or
precede a later real span, so **every** chip span is replaced and none survives
into the result. Because a chip's `text` is inserted after the scan, token-shaped
bytes inside that `text` are content and are not re-expanded (86-F10).

Postcondition (PP2). After `expand_paste_chips` returns, the result contains no
byte span equal to any live entry's `token` that was present in `draft` -- proven
by construction: every such span was in `spans` and was replaced.

Expansion point (the only producer). In the `Event::Return` branch of
`handle_input` (`src/ui/supervisor.cpp:4040`) the current line
`const std::string text = input.draft;` (`:4041`) becomes
`const std::string text = input.expand_paste_chips();`. That **one** read is the
sole source of the model-bound string: it flows to `classify_enter(*state, text)`
(`:4042`), `submit(text)` (`:4062`, `:4065`),
`dispatch_command(text)` (`:4051`), and `input.push_history(text)` (`:4052`).
`submit` -> `prompt` -> `agent.prompt` (`:2681-2683`) therefore carries the full
text, and no other caller passes `input.draft` to a model-bound path (the
`edit_prompt` path expands separately, 86-A8). Multiple chips expand together; an
inert token (no live entry) is left as written.

After a successful submit or dispatch, `input.draft` and the registry are cleared
together (86-A5), so `N` resets to 1 for the next draft.

### 4.7 Interactions (86-D7)

* **Read-only child composer (58-E8).** `flush_paste` returns early when
  `model_.subagent_path` is non-empty (`:4404`); no chip is created. Unchanged.
* **History recall.** `push_history` receives the expanded text, so `history_up`
  restores the full text (never a chip). `history_up` saves `saved_draft = draft`
  (raw bytes, may contain a chip) and `history_down` restores it; the registry is
  an `InputModel` member and is not touched by recall, so the restored chip still
  expands.
* **Subprocess editor.** Only `edit_prompt` (Ctrl-E, `:2868-2892`) is
  chip-relevant: it edits the composer draft, so it expands the draft before
  `edit_text_in_editor` (`:2875`) -- the editor shows the full text -- and clears
  the registry after copying the edited text back (`:2887-2888`), leaving plain
  text (86-A8). The other editor call site, `edit_export_file` (`:2852`), edits an
  already-exported markdown FILE, not the composer draft, so it has no chips and is
  unaffected.
* **`/handoff`, `/export`, `/context`.** These act on the durable session or
  workspace, not the live draft. A chip can only appear in a prompt draft; a typed
  command never contains one, so the command surface is unchanged. A chip inside a
  command draft would be expanded by 86-D6 before dispatch; this is harmless and
  recorded.
* **Modal composer (66-D8).** A chip created while a modal owns the keyboard is
  inserted into both the live input and the modal snapshot input (their own
  registries), so `release_modal_composer` does not orphan it (86-A2).

### 4.8 Rendering (86-D9)

`render_input` is unchanged (`src/ui/ui_render.cpp:711-805`): it reads
`active->input.draft` (`:732`) and the caret (`:733`) and renders the contiguous
token bytes through the existing glyph pipeline. The token may wrap across a row
boundary (`wrap_glyphs`, `:689-709`). The caret is guaranteed to rest on a chip
edge -- not inside one -- only because **both** horizontal motion (86-A4) **and**
vertical motion (86-A9, `composer_move_cursor_vertical`) edge-snap; without 86-A9
a wrapped token lets ArrowUp/ArrowDown land the caret inside it (gate-86
MEDIUM 1). With 86-A9 the caret row computation is unaffected. Rev 1 applies no
SGR to the chip.

## 5. Interface sketch (C++)

Signatures only. Every new symbol below has a concrete caller (noted inline).

```cpp
// include/ymh/ui/ui_model.hpp

// 86-D2/D4: the out-of-band store for one chip of one draft.
struct PasteChip {
    std::size_t index = 0;        // N
    std::string token;            // exact in-draft bytes, e.g. "[Pasted text #1 +4 lines]"
    std::string text;             // the full reassembled paste (out of band)
    std::size_t line_count = 0;   // M = 1 + count('\n')
};

// 86-D2: a byte span [first, second) of a chip in `draft`. Also the element type
// of `chip_spans()` and the parameter type of `composer_move_cursor_vertical`.
using ChipSpan = std::pair<std::size_t, std::size_t>;

// 86-D1/D3: thresholds and caps (definitions in the .cpp).
inline constexpr std::size_t kPasteChipLineThreshold = 3;
inline constexpr std::size_t kPasteChipByteThreshold = 512;
inline constexpr std::size_t kMaxPasteChips          = 64;
inline constexpr std::size_t kPasteChipTotalBytes    = 16u * 1024u * 1024u;

struct InputModel {
    // ... existing fields (draft, cursor, history, history_pos, saved_draft) ...

    // 86-D4: chips of THIS draft, keyed by N. Owner: the InputModel.
    std::map<std::size_t, PasteChip> paste_chips;
    // 86-D3: sticky per-draft counter; 1 on construction/clear; never reused.
    std::size_t                      next_chip_index = 1;

    // 86-D3: exact token bytes for (index, line_count). Caller: insert_paste.
    [[nodiscard]] static std::string paste_chip_token(std::size_t index,
                                                      std::size_t line_count);
    // 86-D1: 1 + count('\n'). Caller: insert_paste.
    [[nodiscard]] static std::size_t paste_line_count(std::string_view text);

    // 86-D1/D4: insert ONE reassembled paste segment at the caret. Collapses
    // THIS segment to one chip when long and under the caps, else verbatim.
    // Never submits. Returns the inserted byte length. Caller:
    // SupervisorApp::flush_paste (src/ui/supervisor.cpp:4416, and the modal
    // mirror at :4424). Takes `text` by value (a copy; the caller keeps its own).
    [[nodiscard]] std::size_t insert_paste(std::string text);

    // 86-D2/D6: all live chip spans of `draft`, ascending start; every occurrence
    // of a live token is a span (duplicates included). Pure.
    // Callers: expand_paste_chips; chip_ending_at/chip_starting_at;
    // handle_input ArrowUp/ArrowDown (src/ui/supervisor.cpp:4144, :4162).
    [[nodiscard]] std::vector<ChipSpan> chip_spans() const;

    // 86-D6: replace every live chip span from the ORIGINAL draft with its text,
    // right-to-left; pure, no mutation, postcondition PP2. Callers: handle_input
    // Return branch (src/ui/supervisor.cpp:4041), edit_prompt
    // (src/ui/supervisor.cpp:2875).
    [[nodiscard]] std::string expand_paste_chips() const;

    // 86-D4: drop all chips. Callers: clear_line (ui_model.cpp:384),
    // submit (src/ui/supervisor.cpp:577, :595), dispatch branch (:4053),
    // and the draft-replacing sites :1605, :2125, :3435, :3442 (86-A5).
    void clear_paste_chips();

    // 86-D5: span of a live chip that ends at `pos` or contains it; else nullopt.
    // Caller: backspace_atom. (Adjacent-chip tie rule, Sec. 4.5.)
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
        chip_ending_at(std::size_t pos) const;
    // 86-D5: span of a live chip that starts at `pos` or contains it; else nullopt.
    // Caller: delete_forward.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
        chip_starting_at(std::size_t pos) const;

    // 86-D5: Backspace that erases a whole chip when adjacent/inside. Caller:
    // handle_input Backspace branch (src/ui/supervisor.cpp:4094).
    bool backspace_atom();

    // Amended (86-A4): Delete and motion never split a chip.
    bool                      delete_forward();                 // caller: handle_input:4105
    bool                      delete_word();                    // caller: handle_input:4180
    [[nodiscard]] std::size_t cursor_left(std::size_t pos) const;   // caller: handle_input:4125
    [[nodiscard]] std::size_t cursor_right(std::size_t pos) const;  // caller: handle_input:4130
    [[nodiscard]] std::size_t word_left_boundary(std::size_t pos) const;   // caller: handle_input:4115
    [[nodiscard]] std::size_t word_right_boundary(std::size_t pos) const;  // caller: handle_input:4120
    void                      clear_line();                     // amended (86-A5); caller: handle_input:4173
};
```

Private helper (ui_model.cpp), with callers:

```cpp
// 86-D2: EVERY occurrence of `token` in `draft`, ascending. Callers:
// InputModel::chip_spans (public), InputModel::expand_paste_chips.
[[nodiscard]] std::vector<ChipSpan>
    InputModel::token_occurrences(std::string_view token) const;
```

`chip_spans` merges the per-entry `token_occurrences` lists into one ascending
`std::vector<ChipSpan>`; `expand_paste_chips` uses the same lists to pair each
span with its entry's `text` (the token bytes identify the entry uniquely, because
`N` differs per entry). `chip_ending_at`/`chip_starting_at` iterate `chip_spans()`
(ascending start) and return the first span satisfying the adjacency rule.

Changed public free function (`include/ymh/ui/ui_render.hpp:69`):

```cpp
// 86-A9: chip spans default empty, so the existing two call sites compile
// unchanged until they opt in. A target strictly inside a span edge-snaps.
[[nodiscard]] bool composer_move_cursor_vertical(
    std::string_view draft, std::size_t& cursor, int direction, int text_width,
    std::span<const ChipSpan> chip_spans = {});
```

## 6. State lifetime table

Every row is normative. "Restart" = supervisor process restart; "switch" =
session switch within a live supervisor. New/changed state only.

| State | Type | Created | Owned by | Destroyed / evicted | Survives restart? | Survives switch? | Crash path |
|---|---|---|---|---|---|---|---|
| Chip registry | `std::map<std::size_t, PasteChip>` (`InputModel::paste_chips`) | `InputModel` construction (per `SessionUiState` / `pendingComposer`) | the owning `InputModel` | submit, `clear_line`, the draft-replacing sites in 86-A5, `edit_prompt` copy-back, `forget_session_state`, `SessionUiState`/`UiModel` destruction (Sec. 4.3) | no (supervisor-local; never persisted, never on the wire) | yes (per-session `InputModel`; switches with the draft) | discarded with the process; no draft bytes are lost because the registry is only cleared where the draft is cleared |
| One chip entry | `PasteChip` (in the map) | `InputModel::insert_paste` (86-D1 long+under-cap branch) | the owning `InputModel` | its atomic delete (86-D5), or any of the registry clear points above | no | yes | discarded with the process |
| `next_chip_index` | `std::size_t` | `InputModel` construction (`= 1`) | the owning `InputModel` | reset to 1 on clear/submit; never decremented on delete (86-D3) | no | yes (per-session) | discarded with the process |
| Chip token span | bytes inside `draft` (`std::string`) | `insert_paste` inserts the token at the caret | the owning `InputModel` | atomic delete, `clear_line`, submit, or any draft edit that breaks the token (then inert) | no | yes (the draft is per-session) | discarded with the process |
| Mirror entry in the modal snapshot | `PasteChip` in `modal_composer_snapshot_->input.paste_chips` | `flush_paste` while a modal owns the keyboard (86-A2) | the snapshot `InputModel` | `release_modal_composer` / snapshot reset | no | n/a (modal-local) | discarded with the process |

No durable state, no event-log field, no wire field, and no `registry.db` row is
introduced. The out-of-band text is supervisor-local and dies with the process.

## 7. Invariants

| ID | Invariant | Where |
|---|---|---|
| **PP1** | **Threshold (per flush segment).** Each `insert_paste` call (one 66 flush segment) is collapsed to exactly one chip iff `paste_line_count(text) > 3 || text.size() > 512` and under the caps (PP6); otherwise that segment is inserted verbatim. One physical paste above 66's `kMaxPasteBytes` is several segments and may yield several chips (or a verbatim tail across `kPasteChipTotalBytes`); a single segment is never both chipped and inlined. | 86-D1, Sec. 4.4 |
| **PP2** | **Placeholder never reaches the model.** The model-bound string is produced only by `expand_paste_chips()`, which collects **every** live span from the immutable `draft` via `chip_spans()` and replaces them right-to-left. Postcondition: the result contains no span equal to any live entry's `token` that was present in `draft`; no chip placeholder survives. (Token-shaped bytes inside a chip's own out-of-band `text` are content, not a chip; 86-F10.) | 86-D6, Sec. 4.2, 4.6 |
| **PP3** | **Registry integrity.** Every live chip token present in `draft` corresponds to exactly one live `PasteChip` with matching `token`; a chip-shaped string with no live entry is inert literal text and is not manipulated. | 86-D2/D4, Sec. 4.2-4.3 |
| **PP4** | **Atomicity.** No operation produces a partial chip: a delete whose span contains/abuts a chip erases the whole chip; left/right, up/down (86-A9), and word motion never land inside a chip; Ctrl-W widens to whole chips. Backspace with the caret strictly inside a chip erases the whole chip. | 86-D5, Sec. 4.5 |
| **PP5** | **Numbering.** `N` is per-draft, starts at 1, is strictly increasing, and is never reused within a draft; deleting a chip does not renumber other chips; `N` resets to 1 on submit and on `clear_line`. | 86-D3, Sec. 4.4 |
| **PP6** | **Bound.** `paste_chips.size() <= kMaxPasteChips` and retained text `<= kPasteChipTotalBytes` per draft. A segment that would exceed either is inserted verbatim instead of chipped. | 86-D4, Sec. 4.3 |
| **PP7** | **Never submits.** Producing a chip is one atomic draft mutation and zero `session.submit`/`agent.prompt` calls; embedded newlines are literal (66-I2/66-I4 preserved). | 66-D4, Sec. 4.1 |
| **PP8** | **Read-only child.** While a child is viewed (58-E8) a paste produces no chip and mutates no draft (66-I7 preserved). | 86-D7, Sec. 4.7 |
| **PP9** | **Lifetime.** The registry is owned by the `InputModel`; it is never persisted, never on the wire, and never in the event log. Session switch switches the whole registry with the draft; `forget_session_state` drops both. | 86-D4, Sec. 4.3, Sec. 6 |
| **PP10** | **History.** `push_history` stores the expanded text; recall restores the full text and never re-chips. A `saved_draft` holding a chip still expands after an up/down round-trip (the registry is untouched by recall). | 86-D7, Sec. 4.7 |
| **PP11** | **Purity.** `expand_paste_chips`, `chip_ending_at`, `chip_starting_at`, and the amended motion helpers are pure functions of `(draft, paste_chips)`; they never mutate the registry or the draft. | 86-D2/D5, Sec. 4.2, 4.5 |
| **PP12** | **Same bytes, both paths.** The out-of-band `text` is the exact reassembled paste segment (66's output), untrimmed; the model receives these exact bytes at submit. For a short segment the draft receives these exact bytes directly. | 86-D1/D6, Sec. 4.4, 4.6 |
| **PP13** | **Vertical edge-snap.** After ArrowUp/ArrowDown, the caret does not rest strictly inside any chip: `composer_move_cursor_vertical` snaps a target inside a span to the nearest edge (byte distance; tie -> start). With PP4 this means no motion path can leave the caret inside a chip. | 86-A9/86-D5, Sec. 4.5 |

## 8. Failure modes

F# tags follow 66's convention (the closest 00 Sec. 54 finding plus a label).

* **86-F1 (F6 input-focus, LOW) -- manual mimicry.** A user hand-types a byte
  string identical to a live chip's token. Mitigated by 86-D3 (strictly
  increasing `N` makes a hand-typed token inert unless it matches an *already
  live* chip). In the residual case `expand_paste_chips` replaces **every**
  occurrence (PP2), so the typed duplicate is also expanded to the pasted text --
  the model never receives a placeholder, but the user's literal bytes are
  substituted. Content-correctness, not a leak. Accepted, recorded.
* **86-F2 (F1 data-loss) -- cleared entry with a live token.** A registry entry
  cleared while its token remains would turn the token into inert literal text on
  submit. Prevented: the registry is cleared only with the draft (86-A5), and the
  cap fallback (PP6) never creates a token without an entry.
* **86-F3 (F8 resource-cap) -- unbounded registry.** A draft with many large
  pastes could retain unbounded text. Bounded by `kMaxPasteChips` and
  `kPasteChipTotalBytes`; an over-cap segment is inserted verbatim, never chipped.
* **86-F4 (F6 input-focus) -- inert token from a wholesale draft replace.** No
  normal edit can split a token (PP4/PP13: motion never rests inside a chip), so a
  token becomes inert only if the whole draft is replaced by a non-chip path
  (86-A5 sites). The inert token is literal text and is deleted a glyph at a time
  (no crash, no partial semantics); its entry is dropped at the next clear/submit
  and memory stays bounded (PP6).
* **86-F5 (F6 input-focus) -- atomic delete near adjacent chips.** With chip `#k`
  ending where `#k+1` begins, Backspace deletes `#k` and Delete deletes `#k+1`
  (deterministic tie rule, Sec. 4.5; 86-U4 pins it). No partial token is ever
  produced.
* **86-F6 (F1 drop) -- non-bracketed burst.** A fast raw `Character` burst is
  never chipped (86-D8); it inserts verbatim as today. No misclassification
  because there is no burst detector.
* **86-F7 (F8 permission) -- none.** The chip path mutates only the local draft
  and a supervisor-local registry; it cannot submit, execute, or widen
  permissions (66-F6 preserved).
* **86-F8 (F1 drop) -- session switch / state forget.** The registry is a member
  of the same `InputModel` as the draft; a switch switches both, and
  `forget_session_state` drops both, so a surviving chip always has its entry.
* **86-F9 (F3 late-event-after-close) -- modal snapshot orphan.** A chip created
  while a modal owns the keyboard is mirrored into the snapshot registry (86-A2);
  restoring the snapshot does not orphan the chip.
* **86-F10 (F1 drop, LOW) -- content that looks like a token.** A chip's
  out-of-band `text` may itself contain bytes equal to another live chip's token.
  Those bytes are delivered verbatim as content and are not re-expanded (the
  snapshot/right-to-left rule never re-scans inserted text). This is the one case
  where `[Pasted text ...]`-shaped text can appear in the model-bound string; it
  is the user's content, not a placeholder. Recorded, accepted.
* **86-F11 (F6 input-focus, LOW) -- draft replaced without clearing.** A
  draft-replacing site that does not call `clear_paste_chips` (86-A5: `:1605`,
  `:2125`, `:3435`, `:3442`) leaves stale registry entries and does not reset
  `next_chip_index`; a completion write could silently drop a chip's out-of-band
  text. Bounded by PP6. Prevented by 86-A5 (those sites must clear); recorded as
  the residual if a future site is added without clearing.
* **86-F12 (F6 input-focus, LOW) -- vertical motion inside a chip (closed).**
  Before 86-A9, ArrowUp/ArrowDown could land the caret inside a wrapped token
  (gate-86 MEDIUM 1). Closed by the edge-snap (PP13); 86-U14 pins it.

## 9. dsh (DeepSeek Harness) mapping

| dsh concept | ymh | Justification |
|---|---|---|
| bracketed-paste reassembly | ymh event-stream reassembly, unchanged (66-D1/D3, `supervisor.cpp:4341`) | **non-mirror**: dsh delegates paste to its host TUI runtime; FTXUI has no bracketed-paste primitive, so the seam stays in the ymh event layer. Anchor: 66-D1 (architectural absence). |
| paste -> editor insert | `InputModel::insert_paste` (verbatim or chip), `supervisor.cpp:4416` | mirrored semantics (atomic insert) realized on ymh's `InputModel` draft; extends 66-D4. Anchor: 45 Sec. 6 (`handle_input`), 66-D4. |
| long-paste collapse + expand on submit | `InputModel::paste_chips` registry + `expand_paste_chips` (86-D2/D4/D6) | **non-mirror**: dsh has no composer-placeholder primitive; this is a ymh host-TUI affordance. The out-of-band-store-and-expand-on-submit pattern follows the cited prior art (`langchain-ai/deepagents` `paste_collapse.py`; Claude Code's `[Pasted text #N +M lines]`). Anchor: 86-D1/D2 (deliberate scope decision) + 66-D1 (the host runtime lacks the primitive). |
| atomic token edit/motion | chip-aware `backspace_atom`/`delete_forward`/`cursor_left`/`cursor_right` (86-D5) | **non-mirror**: dsh's editor owns token atomicity; ymh realizes it on the literal token. Follows the openclaude `Cursor.ts` atomic-token pattern. Anchor: 86-D5 + 48-D4/D5.1 (the motion helpers being amended). |
| paste -> transcript/event log | none (registry is supervisor-local) | **non-mirror**: the out-of-band text is deliberately not durable -- it is transient composer state, not session content; the expanded text reaches the model as the prompt and lands in the durable log only as a normal user message. Anchor: 86-D4, PP9. |

## 10. Test plan

Hermetic; no live LLM. New file
`tests/unit/errata86_paste_placeholder_test.cpp` (added to
`tests/CMakeLists.txt` beside `unit/errata66_ui_test.cpp` and
`unit/errata85_turn_failure_test.cpp`). It reuses 66's harness pattern: build the
app with `make_supervisor_harness`, `seed_active_workspace` +
`activate_session` (`include/ymh/ui/supervisor_harness.hpp:115`, `:53`), then
`dispatch_event(ftxui::Event::Special("\x1b[200~"))`, a content run, and
`dispatch_event(ftxui::Event::Special("\x1b[201~"))` (cf.
`tests/unit/errata66_ui_test.cpp:38-39`, `:66-74`, `:98-116`). The model-bound
text is read from `harness->last_submitted_params("agent.prompt")` (`:78`).

| ID | Test | Asserts |
|---|---|---|
| 86-U1 | `LongPasteBecomesOneChipAndDoesNotSubmit` | Open marker + 6-line body + close -> draft is exactly `[Pasted text #1 +6 lines]`, `paste_chips` has one entry with `index == 1`, `line_count == 6`, `text` == the body; `submitted_count("agent.prompt") == 0`. Fails pre-fix (draft == raw body). |
| 86-U2 | `ShortPasteInsertsVerbatim` | Open + `"line1\nline2"` (2 lines, < 512 bytes) + close -> draft == `"line1\nline2"`, `paste_chips` empty. 66-D4/66-U1 preserved. |
| 86-U3 | `BackspaceRemovesWholeChipAndSkipsCaret` | Insert chip `#1`; ArrowLeft once -> `cursor == chip.start`; ArrowRight once -> `cursor == chip.end`; ArrowLeft, then Backspace -> draft empty and `paste_chips` empty. A single Backspace from `chip.end` must remove the whole token (length > 1). |
| 86-U4 | `DeleteAtChipStartAndAdjacentTie` | Insert chip `#1`, place the caret at its start, then Delete -> `#1` is fully removed and the caret is unchanged (still at the start). For two adjacent chips `#1#2` with the caret at the shared boundary: Backspace removes `#1` (left chip, via `chip_ending_at`, first match by ascending `N`), Delete removes `#2` (right chip, via `chip_starting_at`) -- no partial token either way (Sec. 4.5 tie rule). |
| 86-U5 | `SubmitExpandsChipToFullText` | Insert a long chip; dispatch `Event::Return`; `last_submitted_params("agent.prompt")["message"]` equals the full pasted text (byte-exact), and it contains neither `[Pasted text` nor a live token; `paste_chips` is empty and `next_chip_index == 1` after submit; then `history_up` restores the full text (never a chip) -- PP10. |
| 86-U6 | `NumberingIsStableAndMonotonic` | Chip `#1`, chip `#2`, delete `#2` (Backspace at its end), chip again -> the new chip is `#3` (no renumber of `#1`; `#2` not reused). |
| 86-U7 | `RegistryClearedOnClearLineAndSubjectToCaps` | After inserting a chip: Ctrl-U clears draft and `paste_chips`; a command completion that rewrites `draft` (`:3435`/`:3442`, driven as the 46 tests do) also leaves `paste_chips` empty (86-A5); and a synthetic sequence of `kMaxPasteChips + 1` long pastes leaves `paste_chips.size() == kMaxPasteChips` with the last segment inserted verbatim (PP6/PP9). |
| 86-U8 | `ReadOnlyChildPasteProducesNoChip` | Drive the REAL child-focus flow (there is no `subagent_path` setter; `enter_subagent` is private): seed a child via the harness, then `dispatch_key("ctrl-t")` + `dispatch_key("down")` + `dispatch_key("enter")`, `drain_actions`, and ASSERT `model().subagent_path.size() == 1` -- exactly as `tests/unit/errata58_ui_test.cpp:170-180` does. Then a long paste leaves the frozen composer unchanged and `paste_chips` empty (66-I7/PP8). |
| 86-U9 | `ChipSurvivesModalCloseViaSnapshotRegistry` | While the switcher owns input, insert a long chip, then Escape -> the draft holds the token and it still expands (mirrors 66-U8, covers 86-A2/86-F9). |
| 86-U10 | `NonBracketedBurstIsNotChipped` | Dispatch 600 raw `Event::Character` bytes with embedded `Event::Return` semantics absent -> no chip is created; the draft holds the verbatim bytes (86-D8/86-F6). |
| 86-U11 | `ExpansionAndMotionArePure` | Call `expand_paste_chips`/`chip_ending_at`/`cursor_left` twice; draft and `paste_chips` are byte-identical before and after (PP11). |
| 86-U12 (golden) | `UiRenderGolden.ChipRendersAsLiteralToken` in `tests/unit/ui_render_golden_test.cpp` | Build a model whose active session draft is `[Pasted text #1 +4 lines]`; `normalize(render_to_ansi(model, TerminalSize{72, 20}, Theme{false}))` contains the token text and the composer row prefix `> ` (cf. `ui_render_golden_test.cpp:329`). Pins 86-D9/no render change. |
| 86-U13 | `ExpansionLeavesNoChipSpan` (PP2 proof) | Unit-test `expand_paste_chips` directly with a seed InputModel: (a) two chips with a literal copy of chip `#1`'s token typed BEFORE and AFTER `#1` -- the result contains NONE of the three live-token occurrences (all replaced), and equals the expected concatenation; (b) a chip `#1` whose out-of-band `text` itself contains chip `#2`'s token plus a real `#2` -- the real `#2` span is replaced (right-to-left from the original), so no *chip* span survives; (c) an inert `[Pasted text #9 +2 lines]` with no entry is left verbatim. Asserts the PP2 postcondition "no live-token span present in `draft` survives". |
| 86-U14 | `VerticalMotionSnapsOutOfChip` (MEDIUM 1) | Seed a draft of exactly one chip with `composer_terminal_width_` narrow enough that the token wraps; place the caret at the chip start, `composer_move_cursor_vertical(draft, cursor, +1, width, input.chip_spans())`; the returned cursor is NOT strictly inside `[start, end)` -- it is `start` or `end`. Repeat with ArrowDown/ArrowUp through `dispatch_key`. Fails pre-86-A9 (the target lands inside). Covers PP13/86-F12. |

`tests/CMakeLists.txt` adds `unit/errata86_paste_placeholder_test.cpp` beside the
existing errata test entries (e.g. `:80-82`).

## 11. Non-goals and recorded risks

* 86-F1 LOW (manual mimicry) is accepted and bounded (86-D3 allocation rule);
  PP2 holds unconditionally because all occurrences expand.
* A chip may wrap across a row boundary (86-A6/86-D9); motion still treats it as
  one unit -- horizontal (86-A4) and vertical (86-A9). Accepted.
* No chip styling in Rev 1 (86-D9); a later errata may add reverse-video without
  changing the stored bytes.
* A structured draft (86-D10) remains the collision-free long-term option and is
  deferred, not dropped.
* `saved_draft` may hold raw chip bytes while history holds expanded text; the
  registry lives with the draft, so this asymmetry is inert (86-D7/PP10).
* 86-F10 LOW (a chip's own text containing another token) is content, not a
  placeholder; 86-F11 LOW (a future draft-replacing site that forgets to clear).

## 12. Revision log

| Date | Revision | Note |
|---|---|---|
| 2026-10-16 | Rev 1 (draft) | Initial design. Builds on 66 (verified Rev 5). Pins the threshold (86-D1), the literal-token representation and registry (86-D2/D4), the chip text/numbering (86-D3), atomic edit/motion (86-D5), submit expansion (86-D6), interactions (86-D7), and non-bracketed scope (86-D8). Invariants PP1-PP12; failure modes 86-F1-F9; amendment register 86-A1-A8; state-lifetime table; dsh mapping (56-D6 anchors); hermetic test plan 86-U1-U12. Not yet independently reviewed. |
| 2026-10-16 | Rev 2 (draft) | Gate-86 FAIL (0 HIGH / 2 MEDIUM / 5 LOW) fixed. MEDIUM 1: vertical motion is now chip-aware (86-A9, edge-snap in `composer_move_cursor_vertical`; 86-A7/86-A6 amended; 86-D5, PP4/PP13, 86-F12, Sec. 4.5/4.8, 86-U14). MEDIUM 2: `expand_paste_chips` collects ALL live spans from the immutable draft and replaces right-to-left (86-D2/D6, PP2, 86-F1, 86-U13) so no chip placeholder survives. LOW 1: `std::move` removed (Sec. 4.1); LOW 2: 86-A5 now enumerates the draft-replacing sites (:1605, :2125, :3435, :3442) and no longer claims exhaustiveness (86-F11); LOW 3: PP1/86-D1 scoped per flush segment; LOW 4: Sec. 4.7 distinguishes `edit_prompt` (draft) from `edit_export_file` (exported file); LOW 5: 86-U8 uses the real child-focus flow (`errata58_ui_test.cpp:170-180`). Invariants PP1-PP13; failure modes 86-F1-F12; amendment register 86-A1-A9; tests 86-U1-U14. |
