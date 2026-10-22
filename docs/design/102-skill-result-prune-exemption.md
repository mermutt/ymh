# 102 - Tool-result pruner: never prune `skill` loads

Status: verified (Rev 2)
Revision: 2
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 2)

Amends 32 sec. 5 / 40 sec. 4 (the tool-result pruner).

## 1. Why

`skills/chatcat-release/SKILL.md` is 11 KB. The pruner's threshold is **8192
Unicode code points** (`include/ymh/agent/tool_result_pruner.hpp:20`), so
`ToolResultPruner::prune_session` replaces the middle of the `skill` result with
`[... tool result middle pruned ...]` (head 4096 + tail 1024). The model sees a
truncated skill, re-calls `skill`, and the new result is pruned again: the skill
can never be followed. Observed live on a fresh session.

`skill` is not noise: it is a model-requested instruction document, already
bounded by `skills.max_skill_bytes` (64 KiB default,
`include/ymh/config/config.hpp:393`), and the only way to load a skill under
`~/.config/ymh/skills/` (outside the workspace root, so `read_file` cannot).
Pruning it defeats the tool.

## 2. Decision

### 102-D1 - Exempt `skill` results

In `ToolResultPruner::prune_session`, skip a `payload::ToolResult` only when
`name == "skill"` (the tool name set by `src/skills/skill_tool.cpp:45`) **and**
`outcome == ToolOutcome::Ok`. Only a successful load carries a catalog body; the
unknown-skill error echoes the model-supplied `name` (`:57-59`), which is bounded
only by `max_arguments_bytes` (1 MiB), so an error result keeps the normal
threshold. Every other result (shell, read_file, grep, MCP, skill errors)
keeps the existing threshold/head/tail behavior.

### 102-D2 - The cap stays the skill tool's

The exemption does not raise `skills.max_skill_bytes` or the context budget. A
skill body larger than the cap is **skipped with a warning** by the catalog
(`skill_catalog.cpp:112-115`; 20 sec. 3.4 / SK-F8) and never reaches the tool, so
a successful result is always within the cap.

## 3. Interfaces

No signature change. The skip is one predicate in
`ToolResultPruner::prune_session`.

## 4. Implementation

* `src/agent/tool_result_pruner.cpp` - skip `name == "skill" && outcome ==
  ToolOutcome::Ok` in the surface loop.
* `tests/unit/tool_result_pruner_test.cpp` - add the third `name` parameter to
  the fixture helper and test T1 below.

## 5. Invariants

* **102-I1.** An 11 000-code-point **successful** `skill` result is stored
  verbatim; no `kPruneMarker` and no `context/prune` event.
* **102-I2.** A 9 000-code-point `shell` result is still pruned exactly as
  before; so is a `skill` **error** result above the threshold.
* **102-I3.** The pruner's threshold/head/tail for every other tool are
  unchanged.

## 6. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 102-F1 | skill result above the threshold | preserved |
| 102-F2 | non-skill result above the threshold | pruned (unchanged) |

## 7. State lifetime

None: the pruner is stateless apart from its config.

## 8. Tests

1. **T1** a session with an 11 000-char successful `skill` result, a
   9 000-char `shell` result, and a 9 000-char `skill` **error** result:
   `prune_session` prunes two (shell and skill error, exactly one replacement
   each); the successful skill output is byte-identical, has no `kPruneMarker`,
   and has no `context/prune` event; the other two have the marker.

## 9. dsh mapping

Not a dsh-mirrored capability; output retention only.

## 10. Amendments

* **32 sec. 5** - the pruner's surface now excludes tool name `skill`.
* **40 sec. 4** - same exemption.
