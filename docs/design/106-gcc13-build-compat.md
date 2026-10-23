# 106 - GCC 13 build compatibility

Status: verified (Rev 2)
Revision: 2
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 2)

## 1. Why

GCC 13.4 builds `src/execution/process.cpp` with
`-Werror=unused-result` for the four `(void)::write(errsig.write_fd, ...)`
calls in the child-after-fork error path. GCC 16 accepts the `(void)` cast;
GCC 13 rejects the ignored `warn_unused_result` attribute. The writes are
deliberately best-effort (the child is about to `_exit(127)`), so the fix is to
consume the result without changing behavior.

## 2. Decision

Replace each `(void)::write(...)` with:

```cpp
[[maybe_unused]] const auto write_result = ::write(...);
```

The assignment counts as a use for `warn_unused_result`; `[[maybe_unused]]`
suppresses `-Wunused-variable`. No `(void)` suppression and no global
`-Wno-unused-result`.

### 106-D2 - Disambiguate the optional<json> return

GCC 13 with jammy's nlohmann_json makes `return it->second;` in
`last_submitted_params` (`src/ui/supervisor.cpp`) ambiguous: `basic_json`'s
`operator ValueType()` and `std::optional`'s converting constructor both
match. Use `std::make_optional(it->second)`, which constructs the optional
explicitly and copies the map value once. No behavior change.

## 3. Invariants

* **106-I1.** No `(void)::write` remains in `src/execution/process.cpp`.
* **106-I2.** The four child error-path writes still happen and still ignore
  failures; behavior is unchanged.
* **106-I3.** GCC 16 and GCC 13 both compile the touched files with the
  project's `-Werror` flags.
* **106-I4.** `last_submitted_params` returns a copy of the stored JSON, not a
  reference to the map entry; `std::make_optional` preserves that.

## 4. Implementation

* `src/execution/process.cpp`: the four best-effort child `write` results.
* `src/ui/supervisor.cpp`: `last_submitted_params` returns
  `std::make_optional(it->second)`.

## 5. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 106-F1 | GCC 13 unused-result | assignment consumes the result; no warning |
| 106-F2 | unused local | `[[maybe_unused]]` suppresses the variable warning |

## 6. State lifetime

Build-time only; the local `write_result` is dead after the adjacent
`_exit(127)`. Nothing persists.

## 7. Tests

Full suite on the development compiler; GCC 13 build check in the jammy
container. `rg '\(void\)::write' src/execution/process.cpp` must be empty.

## 8. dsh mapping

Not a dsh-mirrored capability; build-only.

## 9. Amendments

README: note that GCC 13's `-O3` can emit a false
`-Wmaybe-uninitialized` in the stream variant code; use `-O2`/RelWithDebInfo
for GCC 13.
