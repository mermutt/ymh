# 107 - Normalize literal read-only shell loops for analysis

Status: verified (Rev 8)
Revision: 8
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 8)

## 1. Why

A read-only in-root loop such as

```sh
for f in hil/cct/a.h hil/cct/b.cpp; do
  echo "====$f===="
  git diff --diff-filter=U "$f"
done
```

is classified `Unknown` because `"$f"` is unexpanded; the `path:"**"` rule
only matches `InRoot`, so it falls back to `default:"ask"`. The user wants to
keep shell loops at their disposal, not be pushed into one-command-per-file.
A blanket shell/default allow would auto-approve outside/unknown commands, so
the loosening is done in the classifier instead.

## 2. Decisions

### 107-D1 - Normalize a closed read-only loop shape

Before risk/scope/scan/temp classification, replace a simple

```text
for VAR in <values>; do <body>; done
```

with the original prefix preserved, the body expanded **once per value** (all
`$VAR`/`${VAR}` occurrences) and joined with `;`, followed by any suffix after
`done`.
The suffix's `$VAR` is replaced with the **last** value (the shell's post-loop
value). Only when all of these hold:

* the loop variable is not a reserved environment name: not `PATH`, `HOME`,
  `IFS`, `CDPATH`, `BASH_ENV`, `ENV`, `SHELL`, `PWD`, `USER`, `LOGNAME`,
  `TERM`, `TMP`/`TEMP`/`TMPDIR`, and not prefixed `LD_`, `LC_`, `GIT_`,
  `XDG_`, `PYTHON`, `BASH`, `ZSH`, or `SSH_`;
* values are literal tokens (`[A-Za-z0-9_./+:-]+`), no leading `-`, no
  quotes, `$`, backtick, glob, braces, `~`, `..`, `;`, `|`, `&`, `<`, `>`,
  `=`, or `#`;
* 1–32 values, all resolve lexically **in-root**;
* every body/suffix segment starts with exactly `echo` or `git diff`; the
  only git option accepted is zero or one `--diff-filter=<letters>` and an
  optional `--`; every other token starting `-` (including all short options,
  `-O...`, `-o...`, `-pO...`, `--no-index`, `--output`, `--ext-diff`,
  `--textconv`, `--open-files-in-pager`, `-c`, `--config-env`) is rejected;
* no segment contains a redirect (`<`, `>`), assignment (`NAME=`), wrapper
  (`sudo`, `env`, `command`, `exec`, `xargs`, `sh`, `bash`, `zsh`, `python`,
  `perl`, `ruby`, `node`, `find`, `sed`, `awk`), brace/glob/`~`/backslash,
  `..`, `#`, backtick, `$(`, or any `$` other than `$VAR`/`${VAR}`;
* `$VAR` is never the first word of a segment (command position), and only
  exact `$VAR`/`${VAR}` identifiers are substituted (`$VARx` is not);
* every absolute path token in the body/suffix is lexically in-root;
* the body/suffix has no other `for`, no nested `do`/`done`, no single
  quotes, no assignment to `VAR`, and no unresolved variable;
* the **expanded** body and suffix are re-validated per value; a value-borne
  option (`-o`, `--output`, ...) or any post-expansion violation disables
  normalization; a value-borne
  option (`-o`, `--output`, ...) or any post-expansion violation disables
  normalization;
* `$VAR` is never the first word of a segment (command position), and only
  exact `$VAR`/`${VAR}` identifiers are substituted (`$VARx` is not);
* the body/suffix has no other `for` (only the one being normalized), no
  nested `do`/`done`, no single quotes, no assignment to `VAR`, and no
  unresolved variable;
* the normalized text is at most 8 KiB.

`for f in a b; do git diff "$f"; done` and the example above are normalized;
`for f in /etc/...` (outside), `for f in *.h`/`$(...)` (unprovable),
`for f in a; do rm "$f"; done`/`"$f"` (write or command position), and loops
with redirects/wrappers/assignments are not.

### 107-D2 - All values stay visible

Per-value expansion is mandatory so `scan_token_protected` and `temp_pass`
see every value. `for f in notes.txt .git/config; do git diff -- "$f"; done` exposes
the `.git` read and still denies. No representative-value shortcut.

### 107-D3 - No policy relaxation

The normalizer changes only what the existing classifier can prove: a
read-only loop over literal in-root values becomes `InRoot`. It does not
change risk, `Forbid`/`Confirm`, `ReadOnly`, `force_ask`, protected paths,
temp exemption, or the rule fold. Execution always uses the original command;
the normalized text is analysis-only. No config key is added.

## 3. Interfaces

No public API change. Internal
`std::optional<std::string> normalize_literal_loops(std::string_view text,
const std::filesystem::path& root, const std::filesystem::path& cwd)`
applied in `classify_shell_command_with_scope` and the terminal-open branch.
Only **one** loop is normalized. If the body or suffix contains another
`for` (including a later loop reusing `VAR`), normalization is skipped; the
original text remains Unknown/Ask. This keeps every value visible for the
single loop and avoids cross-loop variable/suffix ambiguity.

## 4. Implementation

* `src/policy/shell_guard.cpp`: literal-loop parser (regex + segment
  validation), per-value variable substitution, suffix last-value
  substitution, 8 KiB cap; feed the normalized text to `classify_impl`,
  `scope_impl`, `scan`, and `temp_pass`.
* `tests/unit/errata107_literal_loops_test.cpp`: loop allow, outside ask,
  glob/substitution/nested/redirect/wrapper/command-position ask, protected
  second value deny, suffix last-value, and the exact user loop.
* `tests/CMakeLists.txt`: add the test.

## 5. Invariants

* **107-I1.** `for f in a.h b.cpp; do git diff -- "$f"; done` (all in-root):
  `scope=InRoot`, `evaluate=Allow` under the `**` rule.
* **107-I2.** `for f in /etc/hostname; do cat "$f"; done`: not normalized,
  `Ask`.
* **107-I3.** `for f in notes.txt .git/config; do git diff -- "$f"; done`: protected
  second value; deny.
* **107-I4.** `for f in a b; do git diff "$f"; done; git diff "$f"`:
  suffix uses last value; `InRoot`, `Allow`.
* **107-I5.** `for f in *.h`, `for f in $(ls)`, `for f in a; do rm "$f"; done`,
  `for f in a; do "$f"; done`, `for f in a; do cat "$f" > x; done`,
  `for f in a; do git diff {b,c}; done`, `for f in a; do echo */../../x; done`,
  `for f in a; do echo $((1)); done`, `for f in a; do git diff --ext-diff
  "$f"; done`, `for f in a; do git diff --output=x "$f"; done`,
  `for f in a; do git diff --no-index /etc/passwd "$f"; done`,
  `for f in a; do git diff -O/etc/passwd "$f"; done`,
  `for f in a; do git diff -pO/etc/passwd "$f"; done`,
  `for f in a; do git diff --config-env=diff.external=EVIL "$f"; done`,
  `for f in a; do git diff --open-files-in-pager=x "$f"; done`,
  `for f in --output=x; do git diff "$f"; done`,
  `for f in a; do ./git diff "$f"; done`, and
  `for PATH in .; do git diff -- "$PATH"; done`: not normalized, `Ask` (or
  protected deny).
* **107-I6.** Risk/Forbid/protected/temp and outside behavior are otherwise
  unchanged.

## 6. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 107-F1 | literal in-root read-only loop | normalized; InRoot/Allow |
| 107-F2 | outside/glob/substitution/nested loop | unchanged; Ask |
| 107-F3 | write/redirect/wrapper/assignment/command-position | unchanged; Ask/Deny |
| 107-F4 | protected second value | per-value body; deny |
| 107-F5 | suffix after `done` | last value; classified |
| 107-F6 | >8 KiB expansion | no normalization; Ask |

## 7. State lifetime

Function-local text and value lists; no persistent state.

## 8. Tests

`tests/unit/errata107_literal_loops_test.cpp` covers I1-I6/F1-F6, including
the exact user loop, `a b` values, `./rm` command-position, protected `.git`
via `git diff -- .git/config`, reserved loop names (`PATH`/`HOME`/`GIT_*`),
brace/glob/`~`/`..`/`#` body escapes, `$VARx` boundary, `git --no-index`/
`--ext-diff`/`--textconv`/`--output`/`-o`/`-oVALUE`/`-O`/`-OVALUE`/
`-pO`/`--config-env=NAME=VAL`/`--open-files-in-pager`, absolute outside body
paths,
value-borne options, `./git`, two loops, quoted `do`/`done`/semicolon, prefix
order, and the 8 KiB cap.

## 9. dsh mapping

Not a dsh-mirrored capability; dsh's classifier has no literal-loop
normalization stage. Deliberate divergence: this only supplies in-root literal
evidence to the existing ymh classifier; no dsh surface is mirrored. Anchor:
56-D6; `src/policy/shell_guard.cpp` (scope classifier).

## 10. Amendments

None to policy semantics; the normalizer only supplies literal in-root
evidence the classifier previously lacked.
