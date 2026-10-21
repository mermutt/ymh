# 98 - Null-device redirects are not outside paths

```
Status: verified (Rev 3)
Revision: 3
Verification status: adversarial reviewer (independent, Rev 3): GATE PASS (0 HIGH / 0 MEDIUM)
Component: 98 (errata) -- the spec-90 shell target-scope walk classifies every
           redirect target as a path, so `2>/dev/null` (and `>/dev/null`,
           `2>/dev/null | ...`) makes an otherwise fully in-root command
           `Outside`, and an in-root allow rule cannot match it. The user sees
           `Permission required` for a plain
           `cat a.md b.md ... 2>/dev/null` in their own repo.
Depends on: 89 (shell guard), 90 (target scope), 94 (scope walk).
Amends: 90 sec. 4.4 (redirect truth-table row, `90-*.md:820`), 94 sec. 4.2
        (`94-*.md:402`).
Tracker: a `DESIGN_STATUS.md` row is required before commit.
```

## 1. Problem

Command from the user's screenshot:

```text
cat cfg_knobs_expanded.md knobs_expanded.md knobs_summary.md knobs_table.md \
    code_update_for_read_write.md 2>/dev/null
```

Evaluated with the user's config (`default: ask`, `rules: [{tool:"*",
path:"**", effect:"allow"}]`), both of these are `Allow`:

```text
cat cfg_knobs_expanded.md knobs_expanded.md          = Allow
cat a 2>&1                                           = Allow
cat a | head                                         = Allow
```

and both of these are `Ask`:

```text
cat cfg_knobs_expanded.md 2>/dev/null                = ASK
cat a >/dev/null                                     = ASK
```

`classify_segment_scope` (`src/policy/shell_guard.cpp:1772-1779`) treats the
token after **every** redirect as a path target:

```cpp
if (toks[k].kind == TokKind::Redirect && k + 1 < e &&
    toks[k + 1].kind == TokKind::Word) {
    const auto scope = classify_scope_path(toks[k + 1].text, ... true, false);
    if (scope.has_value()) { note_scope(result, *scope); }
    ++k;
}
```

`/dev/null` is absolute and outside the workspace root, so `note_scope` records
`Outside`, the `{tool:"*", path:"**"}` rule requires `InRoot`
(`permission_policy.cpp:178-181`), and the verdict falls to `default: ask`.

The whitelist already exists but is only wired into the older risk classifier:
`is_dev_whitelist` (`shell_guard.cpp:105-109`: `/dev/null`, `/dev/stdin`,
`/dev/stdout`, `/dev/stderr`, `/dev/tty`, `/dev/fd/*`) is consulted at `:662`
(`OpRisk::Skip`), not by `classify_segment_scope`.

## 2. Decisions

### 98-D1 - An exact `/dev/null` **output** redirect target is neutral, in BOTH scans

There are two scans that can classify a redirect target:

1. the redirect loop at `:1772-1781`, and
2. the general operand loop at `:1871-1893`, which currently does
   `if (toks[k].kind == Redirect) { continue; }` and then classifies the
   **target word** on the next iteration as an operand.

Rev 1 patched only (1); (2) re-classified `/dev/null` as `Outside`, so the
command still asked. Rev 2 pins both:

```cpp
// (1) redirect loop
if (toks[k].kind == TokKind::Redirect && k + 1 < e &&
    toks[k + 1].kind == TokKind::Word) {
    const std::string& op = toks[k].text;
    if (!(is_output_only_redirect(op) &&
          toks[k + 1].text == "/dev/null")) {
        const auto scope = classify_scope_path(toks[k + 1].text, toks[k + 1],
                                               root, cur, true, false);
        if (scope.has_value()) { note_scope(result, *scope); }
    }
    ++k;
}

// (2) operand loop: skip a RUN of redirect operators, then the target word
if (toks[k].kind == TokKind::Redirect) {
    while (k < e && toks[k].kind == TokKind::Redirect) { ++k; }
    continue;   // the for-loop's ++k then skips the target
}
```

The exemption is the **exact string** `/dev/null`, not `is_dev_whitelist`
(`:105-108`): that helper also covers `/dev/fd/*`, and `/dev/fd/3` (or
`/dev/fd/../fd/3`) aliases an arbitrary open descriptor, which could be a
workspace file. `is_output_only_redirect` already exists (`:120-122`).

The operand loop must skip a **run** of redirect tokens, because `&>>`
tokenizes as `Redirect("&>") Redirect(">") Word("/dev/null")`
(`:184-206`); skipping one operator would reclassify the target as an operand.

### 98-D2 - Input redirects and device **arguments** stay classified

Only output redirects to the exact `/dev/null` are exempted. `cmd < /dev/null`,
`cmd < /dev/sda`, and `cat /dev/sda` still classify their target (input reads
stay conservative). `>/dev/fd/3`, `>/dev/stdout`, and `>/dev/tty` are **not**
exempted by this spec: `/dev/fd/*` is an arbitrary-descriptor alias.

### 98-D3 - No other behavior changes

The legacy `OpRisk` path (`:662`) is unchanged. The temp-root exemption
(`:2070-2105`) is unchanged. No config, permission, or transport change.

### 98-D4 - The security boundary is unchanged

The exemption applies only to the literal `/dev/null`, which cannot address the
workspace. A redirect to any other path (`> ~/out`, `> /etc/x`,
`> /dev/fd/3`, `> /tmp/x` outside the temp root) still classifies as `Outside`
and still asks/denies.

## 3. Invariants

* **98-I1.** `cat <in-root files> 2>/dev/null` and `... >/dev/null` classify
  `InRoot` (no outside target).
* **98-I2.** `2>&1`, `>&2`, and pipes remain neutral (unchanged).
* **98-I3.** `cat /dev/sda`, `cmd < /dev/sda`, `> ~/out`, `> /etc/x` remain
  `Outside`.
* **98-I4.** The exemption is exactly the literal `/dev/null`; no device
  aliases and no other names.
* **98-I5.** No change to `OpRisk`, temp exemption, or policy precedence.

## 4. Tests

1. **T1** the exact screenshot command -> `InRoot` -> Allow under a
   `{tool:"*", path:"**"}` rule.
2. **T2** `cat a >/dev/null`, `cat a >>/dev/null`, `cat a &>/dev/null`,
   `cat a &>>/dev/null`, `cat a >|/dev/null`, `cat a 2>/dev/null | head`,
   `cat a "/dev/null" > "/dev/null"` -> `InRoot`.
3. **T3** `cat a 2>&1`, `cat a | head` -> unchanged (`InRoot`).
4. **T4** `cat /dev/sda`, `cat a < /dev/sda`, `cat a > ~/out`, `cat a > /etc/x`,
   `cat a > /dev/fd/3` -> `Outside` (or `Unknown`), i.e. not exempted.
4b. **T4b** both scans: `cat a 2>/dev/null` has no `Outside` note from either
   the redirect loop or the operand loop (regression for Rev 1 H1).
5. **T5** the 89/90/94 shell-guard suites still pass (no risk-class or
   scope regression).

## 5. dsh mapping

| dsh behavior | ymh | Anchor |
|---|---|---|
| `/dev/null` redirection is not a workspace escape | Mirrored | 98-D1; `shell_guard.cpp:105-109`, `:1772-1779` |
| Device reads remain sensitive | Mirrored | 98-D2; `shell_guard.cpp:660-665` |

## 6. Checklist

* [ ] Exempt exact `/dev/null` output targets in BOTH scans (98-D1)
* [ ] T1-T5 registered and passing
* [ ] No change to OpRisk / temp exemption (98-D3)
* [ ] ASCII; anchors resolve; tracker row before commit
