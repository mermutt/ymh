# 103 - Temp-root exemption: neutral read-only operands

Status: verified (Rev 10)
Revision: 10
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 10)

Amends 90 sec. 4.8.2 / 90-D23 / 90-A25.

## 1. Why

`md5sum a > /tmp/x` is auto-denied (`shell-guard:outside-root`) while
`echo hi > /tmp/x` is allowed. The temp pass accepts only a **harmless
producer** (`echo`/`printf`/`:`/`true`) writing `/tmp`; any read verb sets
`has_read`/`has_unknown` (`src/policy/shell_guard.cpp:2377-2385`,
`:2594-2620`), and 90 Rev 13 says "reads are not exempt". The result is a
false denial for a benign local command, with no prompt (the Forbid is a hard
deny). Observed live with `md5sum` + `/tmp` redirection.

## 2. Decisions

### 103-D1 - Neutral read-only verbs, bare command word only

Add a closed whitelist of read-only executables whose **operands** do not
disqualify the temp exemption:

* checksums: `md5sum`, `sha1sum`, `sha224sum`, `sha256sum`, `sha384sum`,
  `sha512sum`, `b2sum`, `cksum`;
* `stat`, `cmp`.

The command word must be the **bare** name: `toks[cw].text == executable` with
no `/` (so `./md5sum`, `/tmp/md5sum`, and any explicitly named workspace script
are not neutral). The command word must also be the segment's **first** token
(`cw == b`): any prefix (`env`, `sudo`, `chroot`, `command`, or a `NAME=value`
assignment including `PATH=`, `CDPATH=`, `BASH_ENV=`) makes the request
non-exempt. This matters because `env --chdir=/etc` changes the child cwd and a
`PATH=`/`CDPATH=`/`BASH_ENV=` assignment changes resolution/startup; the
I/O-free temp pass cannot model those. The rule applies to **every** segment:
a prefix on any segment (`PATH=... md5sum`, `env --chdir=...`) sets
`has_unknown`, and an **assignment-only segment** (`PATH=...`, `CDPATH=...`,
`FOO=...` with no command) also sets `has_unknown` because a standalone
assignment persists in the shell and can change a later segment's resolution
or cwd. For the un-prefixed case the shell's
effective `PATH` is fixed to `/usr/local/bin:/usr/bin:/bin` by
`ProcessEnvMode::Minimal` (`src/execution/process.cpp:403`), so a bare name
cannot resolve to a workspace script.

Neutral reads are **`shell` only**. Neither `terminal open` nor `terminal
write` gains them: the PTY cwd is not modeled (and `terminal open` also
inherits the daemon environment/PATH, `src/execution/pty.cpp:446-455`), so a
read-bearing terminal command stays non-exempt (103-I5). No other verb is
neutral: `cat`, `grep`, `rg`, `sed`, `git`,
`docker`, `sh`, `bash`, and unknown executables keep today's behavior. In
particular the screenshot's `docker exec ...` command stays non-exempt
(container reads are not in-root; also see 103-D5).

### 103-D2 - Reads must be canonical in-root

A neutral-read operand is accepted only if it is:

* absolute (relative operands resolve against the tracked `cwd`), concrete
  (no `$`, backtick, `*?[`, braces, `~`, `..`), not `-` (stdin);
* canonically under the workspace root, and not protected.

A non-qualifying operand is **not dropped**: it sets `has_unknown` (and, if
protected, `protected_target`) so `temp_pass_eligible` refuses the request.
Outside-root, symlink-escaping (a root symlink resolving outside), protected,
and non-concrete operands are therefore non-exempt. The canonical check runs
in `request_is_tmp_exempt` (I/O allowed there), not in the I/O-free
`temp_pass`; the lexical in-root check in `TempRole::NeutralRead` is a
pre-filter.

### 103-D3 - Closed option allowlists

Only these exact tokens are accepted; every other option token, unknown
option, or cluster makes the request non-exempt:

* checksums: `-b`, `-t`, `--binary`, `--text`, `--tag`, `--quiet`, `--status`,
  `--warn`, `--strict`, `-z`, `--zero`, `--help`, `--version`. `--` ends
  options. No value-taking options. `-c`/`--check` are absent (so `-wc`,
  `-c`, `--chec`, and `-cc` are all rejected by the allowlist). At least one
  operand; `-` is rejected.
* `stat`: `-f`, `--file-system`, `-t`, `--terse`, `--help`, `--version`, plus
  `-c`/`--format`/`--printf` with their value skipped (`-c VALUE`,
  `--format=VALUE`, `--printf=VALUE`; a missing value is unknown). `-L`/
  `--dereference` and clusters are rejected. `--` ends options. At least one
  operand; `-` is rejected.
* `cmp`: `-b`, `-l`, `-s`, `-v`, `--print-bytes`, `--verbose`, `--quiet`,
  `--silent`, `--help`, `--version`; valued `-i SKIP`/`--ignore-initial=SKIP`
  and `-n LIMIT`/`--bytes=LIMIT` (the value must be digits/`:`; long forms
  require `=`). `--` ends options. Exactly two operands; neither `-`.

Every skipped option value is itself checked: for `stat`, the `-c`/`--format`/
`--printf` value must be `temp_token_concrete` (no `$`, backtick, braces,
glob, `~`, or `..`); for `cmp`, the `-i`/`-n` value must be digits/`:`. A
non-conforming value sets `has_unknown`. This closes command substitution in a
value (`stat -c "$(cat /etc/passwd)" a > /tmp/x`).

### 103-D4 - The write boundary is unchanged

The exemption still requires every **write** target to be a concrete strict
`/tmp` descendant, a write-producing command, and no other writes. Neutral
reads are an additional eligibility input only; they never bypass 90's write
rules. A compound command remains exempt only while every segment stays within
the write-only `/tmp` bound; any non-temp write, destructive, or unknown
segment sets `has_unknown`.

### 103-D5 - Unknown wrappers stay non-exempt

`docker exec`, `sh -c` with an unparsed script, and other unmodeled commands
keep `has_unknown`, so a `/tmp` redirect combined with them remains
`shell-guard:outside-root`. This spec deliberately does not try to prove a
container command read-only.

### 103-D6 - Read-policy boundary (explicit)

The temp exemption returns `Allow` before the rule fold (90-D23). A neutral
read is therefore not separately re-priced against `permissions.read`; the
trust boundary is the **workspace itself**: the read must canonicalize
in-root and be non-protected, and the only write is `/tmp`. A command that
reads outside the workspace is not exempt.

## 3. Interfaces

`ShellClassification` (`include/ymh/policy/shell_guard.hpp:36-43`) gains:

```cpp
std::vector<std::filesystem::path> neutral_reads;  // canonical-checked by
                                                   // request_is_tmp_exempt
```

No public function signature changes.

## 4. Implementation

* `src/policy/shell_guard.cpp`
  - `TempInfo` gains `std::vector<std::filesystem::path> neutral_reads`.
  - `TempRole::NeutralRead` (add the enum value after `Neutral`): if the token
    is not concrete, or its absolute path is not lexically in-root, or the path
    is lexically protected, set `has_unknown` (and `protected_target` when
    protected) and return **without pushing**; otherwise push the absolute path
    to `neutral_reads`. Never set `has_read`, never add to `targets`. This
    makes `temp_pass_eligible` refuse the request; the policy's protected
    check is a second gate for writes.
  - `temp_segment`: allow a whitelisted verb with the **bare** command word as
    an output-redirect producer (`:2380-2385`); validate options and operands
    per 103-D1/D3. The new branches must run **before** the generic
    `kReadVerbs` loop (`:2547-2555`), otherwise `stat`/`cmp` are caught as
    ordinary reads. The operand collector must honor `--`: after it, every
    token is an operand (a post-`--` `-b` is a path, not an option).
  - `classify_shell_command_with_scope`: copy `temp.neutral_reads` into the
    classification (`:2799-2801`).
  - Both terminal constructors (`terminal open`, `:2929-2942`, and `terminal
    write`, the `terminal` branch's input text) copy `temp.neutral_reads` for
    diagnostics but set `tmp_exempt` only when `temp.neutral_reads.empty()`;
    a read-bearing terminal command is non-exempt (the PTY cwd/PATH is not
    modeled).
  - `request_is_tmp_exempt`: canonicalize each neutral read with
    `temp_resolve_canonical`, require `path_is_within_root(root, canonical)`
    and `!path_is_protected(root, canonical)`; otherwise return false.
  - `temp_segment`: for **every** segment, `cw` must equal `b` (the command
    word is the segment's first token); any prefix/assignment (`env`, `sudo`,
    `chroot`, `command`, `NAME=value`) sets `has_unknown`. If the segment has
    no command word (`cw >= e`), any `NAME=value` token also sets
    `has_unknown` (a standalone assignment persists into later segments).
    No environment I/O is added.
  - Option values: when skipping a valued option, reject a non-concrete
    `stat` format value and a non-numeric `cmp` value (103-D3); set
    `has_unknown` in that case.
* `tests/unit/errata103_temp_reads_test.cpp` (new; add to
  `tests/CMakeLists.txt`).

## 5. Invariants

* **103-I1.** `md5sum a > /tmp/x`, `sha256sum a b > /tmp/x`, `stat a >
  /tmp/x`, `cmp a b > /tmp/x` with `a`,`b` in-root regular files: temp-exempt
  and `Allow` under `ask_all`.
* **103-I2.** The same with an outside operand (`/etc/hostname`), a
  root-relative `..`, an in-root symlink resolving outside, or a protected
  operand (`.git/config`, including `cmp -l .git/config f | tee /tmp/x`):
  not exempt.
* **103-I3.** `md5sum -c list > /tmp/x`, `md5sum -wc list > /tmp/x`,
  `md5sum --chec list > /tmp/x`, `md5sum - > /tmp/x`, `stat -L a > /tmp/x`,
  `stat -Lc a > /tmp/x`, `stat > /tmp/x`, `cmp a > /tmp/x`, `./md5sum a >
  /tmp/x`, any unknown option, `PATH=<workspace>:$PATH md5sum a > /tmp/x`,
  `PATH=<outside>:$PATH md5sum a > /tmp/x`, `PATH=<workspace>; md5sum a >
  /tmp/x`, `CDPATH=/etc; cd ssl; md5sum x > /tmp/x`, `env --chdir=/etc
  md5sum hostname > /tmp/x`, `sudo md5sum a > /tmp/x`, `stat -c "$(cat /etc/passwd)" a >
  /tmp/x`, and `cmp -i "$(id)" a b > /tmp/x`: not exempt.
* **103-I4.** `echo hi > /tmp/x` stays exempt; `rm -rf a > /tmp/x` stays
  non-exempt; `md5sum a > /tmp/x; rm b` stays non-exempt; `docker exec ... >
  /tmp/x` stays non-exempt.
* **103-I5.** Non-temp requests are unchanged: no neutral reads are collected
  for a command without an output redirect. Terminal open **and terminal
  write** never gain neutral reads: `terminal open cwd=/tmp/sub
  command="echo hi > out"` stays exempt, but `terminal open cwd=/tmp/sub
  command="md5sum a > out"` (read present, even in-root) is non-exempt;
  `terminal write "md5sum a > /tmp/x\n"` is non-exempt while `terminal write
  "echo hi > /tmp/x\n"` stays exempt.

## 6. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 103-F1 | checksum/stat/cmp of in-root files + `/tmp` write | exempt, `Allow` |
| 103-F2 | any neutral-read operand outside root | non-exempt (`shell-guard:outside-root`) |
| 103-F3 | symlink in root pointing outside | non-exempt |
| 103-F4 | `-c`/`--check`/cluster, `-L`, stdin, unknown option, wrong arity | non-exempt |
| 103-F5 | `docker`/`sh -c`/unknown wrapper + `/tmp` write | non-exempt |
| 103-F6 | workspace script named `md5sum`, a wrapper (`env`/`sudo`/`chroot`), an assignment prefix, or an assignment-only segment | non-exempt (bare first token only; no persistent assignment) |

## 7. State lifetime

`neutral_reads` is a per-request vector inside `ShellClassification` /
`TempInfo`; created in `classify_shell_command_with_scope` and the
terminal-open constructor, consumed by `request_is_tmp_exempt`, and destroyed
with the request. Nothing persists.

## 8. Tests

`tests/unit/errata103_temp_reads_test.cpp`: T1 allow matrix (I1, incl.
`--` and `stat -c`; `md5sum -- a > /tmp/x` and a post-`--` `-b` operand); T2 outside/symlink/protected (I2) and assignment-only segments
(`PATH=<ws>; md5sum a > /tmp/x`, `CDPATH=/etc; cd ssl; md5sum x > /tmp/x`);
T3 option/stdin/check/arity/`./md5sum`/wrapper (I3, incl. `-wc`, `--chec`,
`env --chdir`, a `PATH=` assignment, and a `$(...)` option value); T4 regressions and compound
(I4); T5 terminal negative (read-bearing open and write; including an in-root read)
and the read-free `echo > out` / `echo > /tmp/x` positive (I5).

## 9. dsh mapping

No new dsh capability. This spec **amends** the existing non-mirror temp-root
cell in 90 sec. 12 (`docs/design/90-path-scoped-permissions-errata.md:2349`):
the divergence reason is unchanged (dsh's per-call approval has no `/tmp`
dimension), but the bound is now "direct write/delete/create plus neutral
reads of the 103-D1 set whose operands canonicalize in-root and are
non-protected"; outside reads, non-neutral reads, unknown wrappers, and path
tools keep the normal/root-confined verdict. 56-D6 anchor: 90-D23;
`src/policy/shell_guard.cpp:2036-2116`.

## 10. Amendments

"Reads are not exempt" becomes "a closed set of read-only verbs with
canonical in-root, non-protected operands is neutral; all other reads stay
non-exempt" in:

* 90 sec. 1.2 "reads keep the normal read policy" (`:24-25`);
* 90 sec. 1.3 goal 3 (`:76-80`) and sec. 1.4 (`:162-169`);
* 90 sec. 1.7 guarantee matrix, `/tmp` direct-write row (`:287-291`);
* 90 sec. 4.1's "one deliberate exception" summary (`:190-198`);
* 90 sec. 4.4 temp-target pass, step 1, the read-source list, and the
  `tmp_targets`/read/write classification (`:683-756`);
* 90 sec. 4.4 step 2, the read-source list, and the `tmp_targets` paragraph;
* 90 sec. 4.8.2's "A request is **tmp-exempt** iff ..." and "A shell
  **read** ... is **not** exempt" paragraphs, and sec. 4.8.3's "Reads keep
  the normal ..." sentence;
* 90 sec. 6.2 evaluate order, sec. 8 `request_is_tmp_exempt` row, sec. 10
  PS29/PS30, and PS-F64;
* 90-D23, 90-D27, 90-D28, 90-A25, 90-A28.
* 90 sec. 6.2 evaluate order (`:1793-1800`);
* 90 sec. 12 dsh non-mirror row (`:2349-2360`);
* 90-D23 (`:1737-1762`), 90-A25 (`:359-362`), 90-A28 (`:365`).

(Anchors are by section and by the quoted Rev 13 wording; line numbers shift
with this errata and are therefore not normative.)

All other sections of 90 are unchanged.
