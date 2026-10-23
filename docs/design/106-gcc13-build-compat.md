# 106 - GCC 13 build compatibility

Status: verified (Rev 1)
Revision: 1
Verification status: verified - independent gate 0 HIGH / 0 MEDIUM (Rev 1)

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

## 3. Invariants

* **106-I1.** No `(void)::write` remains in `src/execution/process.cpp`.
* **106-I2.** The four child error-path writes still happen and still ignore
  failures; behavior is unchanged.
* **106-I3.** GCC 16 and GCC 13 both compile the file with the project's
  `-Werror` flags.

## 4. Failure modes

| ID | Scenario | Required behavior |
|---|---|---|
| 106-F1 | GCC 13 unused-result | assignment consumes the result; no warning |
| 106-F2 | unused local | `[[maybe_unused]]` suppresses the variable warning |

## 5. State lifetime

Build-time only; the local `write_result` is dead after the adjacent
`_exit(127)`. Nothing persists.

## 6. Tests

Full suite on the development compiler; GCC 13 build check in the jammy
container. `rg '\(void\)::write' src/execution/process.cpp` must be empty.

## 7. dsh mapping

Not a dsh-mirrored capability; build-only.

## 8. Amendments

README: note that GCC 13's `-O3` can emit a false
`-Wmaybe-uninitialized` in the stream variant code; use `-O2`/RelWithDebInfo
for GCC 13.
