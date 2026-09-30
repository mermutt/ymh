# 71 — Localcode Skills Import & Imported-`permissions` Verdict Errata

```
Status: verified (Rev 1) · reviewer: Oracle gate (round 2) · gate: 0 HIGH / 0 MEDIUM
Verification status: Oracle gate round 2 PASS — 0 HIGH / 0 MEDIUM; round 1 (2 MEDIUM, 7 LOW) fixed in Rev 1
Revision: 1
Component: 71 (errata) — amends 25-ui-ux-errata.md (25-D14/25-D15, the first-run
           localcode import), 50-skills-mcp-and-session-lifecycle-errata.md
           (50-D1, the skill/command discovery roots) and 20-skills.md
           (§2.1 on-disk layout, §1.2 user tier), and 46-permissions-ui-errata.md
           (46-D12, the `permission`→`permissions` mapping). Adds no new RPC, no
           event type, no config key, no persistence schema change; adds one CLI
           free function (`import_localcode_skills`) and one prompt sentence.
Depends on: 25-ui-ux-errata.md (import trigger), 20-skills.md (verified; skill
            on-disk layout and tiers), 50-skills-mcp-and-session-lifecycle-errata.md
            (verified; `~/.ymh`, `~/.claude` roots), 46-permissions-ui-errata.md
            (verified; `permissions` config section), 52-endpoints-models-and-dsh-agent-presets.md
            (verified; the non-clobber `link()` install, 52-F21)
Scope: (1) the first-run localcode import must also bring the user's
       `~/.localcode/skills` tree across into the ymh **user skills tier**; (2) a
       verdict, proven from code, on whether ymh consumes the imported
       `permissions` section.
Amends: 25-D14 (adds the skills copy to the same trigger), 50-D1 (adds a third
        *import source* for the user tier; the discovery roots are unchanged),
        46-D12 (confirms the mapping is consumed, so it stays)
Retained: 25-D14/25-D15 (config import), 52-F21 (non-clobber install),
          20-skills.md §2.1/§1.2 (layout, user tier)
```

## 1. Purpose, scope, and precedence

The localcode import (`25-D14`/`25-D15`, `src/cli/cli.cpp:1054`) already turns
`~/.localcode/config.json` into a ymh config on first run. A user who migrates
from localcode keeps their config but loses their **skills**, which live beside
the config as a directory tree. This errata extends the *same* import to copy that
tree into the ymh user skills tier, with the same first-run, never-overwrite
semantics the config copy uses.

It also closes a question raised about the imported `permissions` section:
whether ymh actually consumes it. Verdict in §3 (`71-D5`): **yes — the section is
consumed; keep copying it.** The proof is `file:line`, not inference.

Precedence: where this errata conflicts with `25-D14` on the *content* of the
import, this document wins; the trigger, prompt-consent, and non-clobber model of
`25-D14`/`52-F21` are retained verbatim. Skill discovery roots (`50-D1`) are
unchanged — the copy targets the already-trusted user tier, it does not add a new
root.

## 2. Amendment register

| ID | Affects | Change |
|---|---|---|
| **71-A1** | 25-D14 / 25-D15 | The import now also copies `~/.localcode/skills` into `<config-root>/skills`. The prompt names skills. |
| **71-A2** | 50-D1 | Adds an *import source* into the user tier; adds no discovery root and no trust change. |
| **71-A3** | 46-D12 / config schema | The imported `permissions` section is retained (consumed; `71-D5`). |
| **71-A4** | `include/ymh/cli/cli.hpp` | New symbol `import_localcode_skills` (71-D6). |
| **71-A5** | 52-F21 | Reuses the stage-temp + `link()` no-clobber install for every copied file. |

## 3. Decision register

| ID | Decision | Anchor |
|---|---|---|
| **71-D1** | **Source and destination.** The source is `<localcode-dir>/skills`, i.e. the sibling of the imported `config.json` (`localcode_config_path().parent_path()`, `src/config/config.cpp:1739`). The destination is the ymh **user tier** `<config-root>/skills`, i.e. `default_global_config_path().parent_path()/skills` (`src/config/config.cpp:1725`, `src/skills/skill_roots.cpp:29`). One directory per skill, `<root>/<name>/SKILL.md` (20-skills.md §2.1). | 20-skills.md §2.1 (`:168-180`), §1.2 (`:90`) |
| **71-D2** | **What counts as a skill.** A source directory is a skill iff it *directly* contains a **regular** `SKILL.md` (after no symlink resolution — see 71-D3). Any other directory is skipped with a note; a loose non-directory entry is skipped silently. The importer does not re-validate frontmatter: the catalog already skips malformed skills at discovery, so a malformed copy is inert, not harmful (20-skills.md §11 SK7). | 20-skills.md §2.1 (`:172`), §11 SK7 (`:1719`) |
| **71-D3** | **Never overwrite; recurse for supporting files.** Every copied file is installed with the stage-temp + `link()` pattern of `write_imported_config` (`src/cli/cli.cpp:870-882`): write a 0600 temp, `link()` it into place, `EEXIST` ⇒ skip. A skill's optional supporting files (20-skills.md §2.1 `:173`, "never auto-loaded") are copied too, so a skill dir is copied whole; only regular files are copied, symlinks and special files are not followed. | 20-skills.md §2.1 (`:173`), 52-F21 (`src/cli/cli.cpp:873`) |
| **71-D4** | **Trigger / first-run.** The skills copy runs iff the config import runs and installs: the `25-D14` trigger (Tui command, no explicit `--config`, the conventional global config **directory absent**, `~/.localcode/config.json` a regular file, interactive, consent). It runs only after `write_imported_config` returns true. Thus it is first-run-only by construction and can never run against an already-scaffolded user. | 25-D14, `src/cli/cli.cpp:1057-1074`, `:1110-1114` |
| **71-D5** | **The imported `permissions` section is KEPT — it is consumed.** `apply_permissions` parses `[permissions]` (`src/config/config.cpp:448`, dispatched at `:1351-1353`): `permissions.default` → `config.permissions.default_verdict` (`:470-471`) and the imported `permissions.rules` → `config.permissions.rules` (`:513-537`). The consumer `to_permission_config` reads `default_verdict` (`src/cli/wiring.cpp:116-117`) and `rules` (`:153`), producing the `PermissionConfig` used to build the policy (`src/host/workspace_host.cpp:812`, `src/agent/workspace_runtime.cpp:267`). The importer's `skip_permissions`→`permissions.default="allow"` (`src/config/config.cpp:2134-2137`) and `permission`→`permissions.rules` (`:2145-2173`) therefore reach the live permission model. **Do not drop it.** | 46-D12, `src/config/config.cpp:1351`, `src/cli/wiring.cpp:117` |
| **71-D6** | **New symbol (normative).** `std::size_t ymh::import_localcode_skills(const std::filesystem::path& localcode_dir, const std::filesystem::path& skills_root, std::ostream& err)` copies `<localcode_dir>/skills/*` into `<skills_root>/` and returns the number of skill directories copied. It is best-effort per file: a failure is a note and the loop continues. Concrete caller: `maybe_import_localcode_config` (`src/cli/cli.cpp:1113`). | `include/ymh/cli/cli.hpp:113` |
| **71-D7** | **Prompt.** The consent line names skills alongside the other imported material: `Import MCP servers, model parameters, API keys, permission rules, and skills?`. The `[Y/n]` contract is unchanged. | 25-D14 |

Interface sketch (`include/ymh/cli/cli.hpp`):

```cpp
// 71-D1/D6: copies a localcode skills tree (<localcode_dir>/skills/<name>/…)
// into the ymh user skills tier (<skills_root>/<name>/…), one directory per
// skill, identified by a regular top-level SKILL.md (20-skills.md §2.1). Copies
// optional supporting files. Never overwrites an existing destination file
// (71-D3). Missing source or a directory without SKILL.md is skipped. Returns
// the number of skill directories copied. `err` receives per-skill notes.
[[nodiscard]] std::size_t import_localcode_skills(
    const std::filesystem::path& localcode_dir,
    const std::filesystem::path& skills_root,
    std::ostream& err);
```

## 4. Invariants

| ID | Invariant |
|---|---|
| **71-I1** | After a successful import, for every source skill directory with a regular top-level `SKILL.md`, `<skills_root>/<name>/SKILL.md` exists with byte-identical content (71-D1/71-D2). |
| **71-I2** | No pre-existing destination file is modified, truncated, or replaced by the import — the `link()` EEXIST skip is the only outcome for an existing path (71-D3). |
| **71-I3** | The copy is idempotent: a second invocation (or one against a partially populated destination) leaves every existing destination byte unchanged and adds only absent files (71-D3). |
| **71-I4** | No file outside `<localcode_dir>/skills` is read and no file outside `<skills_root>` is written; symlinks are not followed (71-D3). |
| **71-I5** | The config import's outcome and the `permissions` section are unchanged by this errata; `permissions.default`/`permissions.rules` remain in the imported document (71-D5). |
| **71-I6** | A per-file copy failure degrades to a note; it never flips the import result to false and never aborts the remaining skills (71-D6). |
| **71-I7** | The skills copy cannot run when the global config directory already exists or the prompt is declined (71-D4). |

## 5. Failure modes (`71-F#`)

| 71-F# | Failure | Detection | Required behavior |
|---|---|---|---|
| **71-F1** | `<localcode_dir>/skills` absent | `is_directory` false | no-op: no bytes written, no note (71-D1) |
| **71-F2** | A source entry is not a directory | `is_directory` false | skip silently |
| **71-F3** | A skill directory has no regular top-level `SKILL.md` | `is_regular_file(<dir>/SKILL.md)` false | skip + one note; other skills still copied (71-D2) |
| **71-F4** | Destination file already exists | `link()` returns `EEXIST` | skip; count it; never overwrite (71-D3/71-I2) |
| **71-F5** | Source unreadable / dest dir not creatable / temp write fails | `ifstream`/`create_directories`/`write` error | note + continue; import still succeeds (71-I6) |
| **71-F6** | Symlink (to a file or directory) inside the source tree | `recursive_directory_iterator` does not follow directory symlinks; `is_regular_file` false for symlinks that resolve outside | not copied (71-D3/71-I4) |
| **71-F7** | Crash mid-copy | process death | only fully `link()`-ed files exist; at most one `<file>.import.tmp` remains and is reclaimed on the next run by the `O_EXCL` reopen path (71-D3, 52-F21) |

## 6. State lifetime

| State | Created | Destroyed / evicted | Owner | Survives restart? | Crash path |
|---|---|---|---|---|---|
| Copied skill files (`<skills_root>/<name>/…`) | `import_localcode_skills` during a first-run import | never by ymh; only the user removes them | the user (config root) | yes | each file is installed atomically by `link()`; a crash leaves only complete files |
| Stage temp `<file>.import.tmp` | per file install (71-D3) | `unlink()` immediately after `link()` (or after a failure) | the importer | no (transient) | may persist after a crash; reclaimed by the `O_EXCL` reopen path on the next run; contains no secret beyond the file being copied |
| `copied`/`skipped` counters | `import_localcode_skills` | function return | the importer (stack) | no | in-memory only |

## 7. dsh (DeepSeek Harness) mapping

| dsh capability | ymh mirror | Justification (anchor) |
|---|---|---|
| Claude-Code-compatible skill discovery at `~/.claude/skills` | Mirrored (50-D1) | — |
| A localcode `~/.localcode` onboarding import | **Not mirrored** — ymh-specific | dsh has **no localcode concept**; this is an architectural absence, not a missed feature. The whole localcode import is already a ymh-only onboarding decision (25-D14/25-D15), and this errata only extends that decision to the adjacent skills tree. Anchor: 25-D14, 50-D1. |
| A distinct "imported skills" tier or trust marker | **Not mirrored** — absent by design | Imported skills land in the already-trusted user tier (`~/.config/ymh/skills`, 20-skills.md §1.2) and gain no new provenance or trust state; adding a tier would invent state with no consumer. Anchor: 20-skills.md §1.2 (`:90`), 56-D6. |

## 8. Test plan

Hermetic; `tests/unit/spec71_localcode_import_test.cpp` (registered in
`tests/CMakeLists.txt`). All tests use an isolated `XDG_CONFIG_HOME`/`HOME`
(`test::TempWorkspace`); the real `~/.localcode`, `~/.config/ymh`, `~/.ymh`, and
`~/.claude` are never touched.

| ID | Test | Locks |
|---|---|---|
| **71-U1** | A `.localcode/skills` tree (two skills; one with a supporting file) is copied into the user tier with the `<name>/SKILL.md` layout and byte-identical content, **and** the config is written. Fails pre-change. | 71-I1, 71-D1/D2 |
| **71-U2** | With a pre-existing destination `SKILL.md`, `import_localcode_skills` leaves it byte-identical and still copies a second skill; a second call copies nothing. Fails pre-change. | 71-I2, 71-I3, 71-D3 |
| **71-U3** | A directory without `SKILL.md` and a loose file are skipped; an absent `.localcode/skills` is a no-op. Fails pre-change. | 71-F1/F2/F3, 71-D2 |
| **71-U4** | The imported document's `permissions` section (from `skip_permissions` + a `permission` rule) parses into `Config.permissions` and reaches `to_permission_config` with `default_verdict == Allow`. **Passes pre-change** — it is the verdict-lock for the KEEP decision, not a behavior change (71-D5). | 71-I5 |
| **71-U5** | With the global config directory already present, `maybe_import_localcode_config` copies no skills (first-run gate). Passes pre-change. | 71-I7, 71-D4 |
| **71-U6** | Declining the prompt, or a non-interactive call, copies no skills into the user tier. **Passes pre-change** (the gate already declines); a lock on 71-D4's consent half. | 71-I7 |
| **71-U7** | A directory whose `SKILL.md` is a symlink, and a symlinked skill directory, are skipped (not followed): only the real skill is copied and the symlinked `SKILL.md` produces the `no regular SKILL.md` note. Fails pre-change (no note). | 71-I4, 71-D2 |

Pre-change evidence (feature absent — `import_localcode_skills` a no-op and not
wired into `maybe_import_localcode_config`): U1/U2/U3/U7 fail, U4/U5/U6 pass.
After the change all seven pass. The four feature tests are therefore
non-vacuous; U4 is the deliberate verdict lock for 71-D5 and is expected to pass
on both sides.

## 9. Open questions

- **71-OQ1** (track, not blocking): should the copied skills carry provenance so
  `/skills` can show "imported from localcode"? Deferred — no consumer today
  (the catalog has no provenance field for the user tier beyond `SkillSource::
  User`); re-open if the user asks.
- **71-OQ2** (track): the import is first-run-only. A user who installed ymh
  before localcode gained skills will not get them copied. Mirrors the config
  import's existing limitation (25-D14); re-open only with a deliberate
  re-import command.
