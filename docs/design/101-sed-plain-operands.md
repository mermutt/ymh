# 101 - Shell scope: read-only `sed` with plain in-root operands

Status: verified (Rev 2)
Revision: 2
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 2)

Supersedes **100-D3's** "any file operand is Unknown" and **100-I3** for plain
operands. 100-D1/D2/D4/D5, the script grammar, and every other invariant stay.

## 1. Why

Spec 100 relaxed only stdin-only `sed`. An ordinary inspection command still
asks:

```
cd /w && echo "=== active_wl ===" && sed -n '820,826p;918,924p' tg/traffic_gen.cpp \
  && echo "=== trace ===" && grep -n "CMD_COUNT" tg/traffic_gen.cpp
```

Verified with `RulePermissionPolicy` (`{tool:"*", path:"**", allow}` +
`default: ask`): scope `Unknown`, verdict **Ask**. The `sed` file operand
`tg/traffic_gen.cpp` hits 100-D3's blanket rejection even though it is a plain
in-root path.

## 2. Decision

### 101-D1 - Classify plain operands

In `classify_sed_segment`, when a word is an operand (a non-option after the
script, or any non-option when `-e` was given):

* `-` means stdin: skip it (no path);
* if the token is **unsafe** - `had_unexpanded`, or contains `$`, backtick,
  `*`, `?`, `[`, `{`, `}`, `~`, backslash, `:`, or a newline - return
  `Unknown`. `:` is rejected because `classify_scope_path` has URI/scp
  heuristics (`shell_guard.cpp:1359-1385`) that would otherwise treat
  `https://.../x` or `a:b` as neutral;
* otherwise classify it with `classify_scope_path(token.text, token, root, cur,
  false, false)` and merge the result (`note_scope`).

`-i` and `-f` remain `Unknown` (a write / an unreadable script source); only
read-only scripts with plain operands are relaxed.

### 101-D2 - Result

`InRoot` when the script is read-only, no `-i`/`-f` is present, and every plain
operand is in-root. `Outside` when a plain operand is outside the root.
`Unknown` for any unsafe operand, `-i`, `-f`, or an unreadable script.

## 3. Interfaces

No public signature change. `classify_sed_segment` regains its `root` and `cur`
parameters (100's stdin-only version dropped them) and uses
`classify_scope_path` (`shell_guard.cpp`).

## 4. Implementation

* `src/policy/shell_guard.cpp` - the operand branch in
  `classify_sed_segment`; no other change.
* `tests/unit/errata100_sed_scope_test.cpp` - T7 (`sed -n p x.cpp`) becomes
  `InRoot`; delete its blanket-operand expectation; T13-T15 below.

## 5. Invariants

* **101-I1.** `sed -n '820,826p;918,924p' tg/traffic_gen.cpp` with cwd and root
  `/w` is `InRoot`.
* **101-I2.** A plain operand outside the root is `Outside`.
* **101-I3.** An unsafe operand (`~root/x`, `{a,b}/x`, `$(...)`, `x*`,
  `https://../../etc/passwd`, `a:b`, backslash, newline) is `Unknown`.
* **101-I4.** `-i` and `-f` stay `Unknown`.

## 6. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 101-F1 | operand with a shell metacharacter | `Unknown` |
| 101-F2 | outside plain operand | `Outside` |
| 101-F3 | `-i` / `-f` | `Unknown` |

## 7. State lifetime

None: pure classifier.

## 8. Tests

1. **T13** the motivating command -> `InRoot`; under the policy rule ->
   `Allow`.
2. **T14** `sed -n p x.cpp` (cwd `/w`) -> `InRoot`;
   `sed -n p /etc/passwd` -> `Outside`.
3. **T15** `sed -n p ~root/x`, `sed -n p {a,b}/x`, `sed -n p $(x)`,
   `sed -n p 'x*'`, `sed -n p https://../../etc/passwd`, `sed -n p a:b`,
   `sed -n p 'a\\\nb'` -> `Unknown`; `sed -i p x.cpp` -> `Unknown`;
   `sed -f s.sed x.cpp` -> `Unknown`.

## 9. dsh mapping

Not a dsh-mirrored capability; L1 advisory classifier only.

## 10. Amendments

* **100-D3** - "any file operand makes the result Unknown" is superseded by
  101-D1 for plain operands.
* **100-I3** - superseded by 101-I1..I4.
* 100's "stdin-only" prose (D1, D4, §1) is extended to "stdin or plain in-root
  operands"; the script restrictions are unchanged.
