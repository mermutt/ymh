# 108 - Sanitize tool output to valid UTF-8

Status: verified (Rev 2)
Revision: 2
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 2)

## 1. Why

A shell/grep result can contain bytes that are not valid UTF-8 (a binary or
Latin-1 source file, or a lost byte at an external truncation boundary). The
result is embedded in `payload::ToolResult` and serialized with
`nlohmann::json::dump()` for the session DB and the LLM request. nlohmann
throws `type_error.316` (`invalid UTF-8 byte at index ...`) and the whole turn
fails (`turn 3 failed: ... invalid UTF-8 byte at index 2814: 0x0A`).

Tool output is untrusted bytes; the harness must store a valid UTF-8 string.

## 2. Decisions

### 108-D1 - `sanitize_utf8`

Add `std::string sanitize_utf8(std::string_view)` to
`include/ymh/core/text.hpp`:

* valid UTF-8 sequences are copied unchanged;
* each invalid byte/sequence is replaced with U+FFFD (`EF BF BD`);
* overlong forms, surrogates (`D800–DFFF`), and values above `10FFFF` are
  invalid;
* a truncated lead/continuation sequence is replaced, never left partial.

It is a deterministic, I/O-free, allocation-preserving pass (no change when the
input is valid).

### 108-D2 - Apply before the result becomes a JSON string

* `retain_tool_result` (`src/tools/tool.cpp`) sanitizes `result.output` and
  `result.error` **before** retention, so the byte budget and the retention
  notice are computed on the valid string. This covers shell, grep, read,
  MCP, terminal, and every registry tool.
* `to_json(payload::ToolResult)` (`src/session/events.cpp`) sanitizes
  `output`/`error` as a backstop for replayed/legacy events and any producer
  that bypasses the registry.
* `tool_message_content` (`src/session/tool_result_text.cpp`) sanitizes a
  non-denied output before it reaches the LLM message; denial text is already
  ASCII.
* `Session::appendEventLocked` sanitizes the `output`/`error` strings in a
  `ToolResult` event payload before persistence, covering synthetic/exception
  results that bypass `retain_tool_result`.
* `handoff` sanitizes the `result.error` detail it copies into its LLM request
  as a final backstop.

No `dump(..., error_handler_t::replace)` is relied on as the primary fix; the
stored value itself is valid.

### 108-D3 - No semantic change for valid output

Valid UTF-8 (including multibyte and emoji) is byte-identical. Invalid bytes
become U+FFFD, which is what the user/model should see for undecodable data.
The retention size limits, pruning code-point thresholds, and UTF-8 boundary
trims are unchanged; they now operate on valid input.

## 3. Interfaces

```cpp
[[nodiscard]] std::string sanitize_utf8(std::string_view text);
```

No other public API change.

## 4. Implementation

* `include/ymh/core/text.hpp`: inline sanitizer.
* `src/tools/tool.cpp`: sanitize at the start of `retain_tool_result`.
* `src/session/events.cpp`: sanitize in the `ToolResult` `to_json`.
* `src/session/tool_result_text.cpp`: sanitize in `tool_message_content`.
* `src/session/session.cpp`: sanitize the event payload's `output`/`error`
  before append.
* `src/agent/handoff.cpp`: sanitize the error detail used in the handoff
  request.
* `tests/unit/utf8_sanitize_test.cpp`: sanitizer matrices, retention,
  `nlohmann` dump, and denied/valid regressions.

## 5. Invariants

* **108-I1.** A result with `\xC3\n` is stored as U+FFFD + `\n`;
  `nlohmann::json(result).dump()` does not throw.
* **108-I2.** Valid ASCII/multibyte/emoji output is byte-identical.
* **108-I3.** Overlong, surrogate, out-of-range, and truncated sequences are
  replaced; no partial sequence remains.
* **108-I4.** Retention and pruning still cap/handle the sanitized output;
  the turn no longer fails with `type_error.316`.
* **108-I5.** Denial text and valid tool output are unchanged.
* **108-I6.** A synthetic/exception `ToolResult` that never passes retention
  is sanitized in the appended event and in the handoff error text.

## 6. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 108-F1 | invalid byte in shell/grep output | sanitized; JSON dump succeeds |
| 108-F2 | valid emoji/CJK | unchanged |
| 108-F3 | retention cut at a multibyte boundary | existing trims + sanitizer; valid |
| 108-F4 | legacy/synthetic event with invalid output | `to_json` + `appendEventLocked` sanitize before persist |
| 108-F5 | denial text | unchanged ASCII |

## 7. State lifetime

The sanitized string is the new `ToolResult.output`/`error`; nothing extra
persists. The helper is stateless.

## 8. Tests

`tests/unit/utf8_sanitize_test.cpp`: valid/invalid matrix, `\xC3\n` probe,
retention + `dump`, `to_json`, and `tool_message_content`.

## 9. dsh mapping

Not a dsh-mirrored capability; output encoding hygiene.

## 10. Amendments

None; this is a correctness backstop at the tool-result boundary.
