# 61 — `main` / `dev` branch split (process spec)

Status: **implemented** — repository-structure decision, self-verified. This is
not a component spec: it changes no runtime architecture, no interface, and no
product behaviour when an endpoint + model are configured.

## 1. Decision

The office deployment needs a checkout that (a) contains only code, (b) contains
no vendor mention whatsoever, and (c) builds the same `ymh` binary as the
development tree — while home development keeps the design docs and the test
suite. The tree is therefore split into two branches that share one empty root
commit:

| Branch | Contents | Commits |
|---|---|---|
| `main` | `.github/ cmake/ CMakeLists.txt .gitignore include/ presets/ src/` | 2: empty root + code import |
| `dev` | `docs/ tests/ README.md HANDOFF.md AGENTS.md` | empty root + the filtered `master` history + 1 adaptation commit |

Both branches are rooted at the **same empty commit** (`E`, "Initial commit"),
so a merge is ancestry-clean:

```sh
git checkout -b tmp main
git merge dev          # no --allow-unrelated-histories, no conflicts
```

`tmp` reproduces the `master` tip tree except for the sanitization (§3) and the
dev-side adaptations (§5).

## 2. Why an empty shared root

`main` and `dev` own **disjoint path sets**. A common ancestor is all that is
needed for git to merge them additively; `E` provides exactly that without
inventing file content that belongs to neither side. `E` carries the original
root commit's author/committer timestamp so the history reads naturally.

## 3. Sanitization (`main` only)

The compiled-in vendor values were the only real mentions left once `docs/`,
`tests/`, `README.md`, `HANDOFF.md` and `AGENTS.md` moved to `dev`. They were
removed rather than renamed — this build ships **no** endpoint, model or
credential-env defaults.

| File | Change |
|---|---|
| `include/ymh/config/config.hpp` | `EndpointSettings::base_url` and `LlmSettings::{base_url,model,api_key_env}` have no compiled-in value; the surrounding comments no longer name a vendor |
| `src/config/config.cpp` | the first-run scaffold emits `base_url`/`model`/`api_key_env` as commented-out placeholders; `resolve_model`'s final fallback is `fill_default_model(resolved, config, std::string{}, "builtin")` — a total resolution with an **empty** model id |
| `include/ymh/llm/provider_registry.hpp`, `src/llm/provider_registry.cpp` | `deepseek_config()` is deleted (it was test-only) and the comments are neutralized |
| `include/ymh/agent/preset.hpp` | comment neutralized |
| `CMakeLists.txt` | tests guard (below) |

Invariant **B3**: no tracked file in `main` matches `(?i)deepseek`.

### 3.1 Tests guard

`tests/` lives on `dev`, so `main` alone has no suite to add. `YMH_BUILD_TESTS`
defaults to `ON`, which would break the documented configure command, so:

```cmake
if(YMH_BUILD_TESTS AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    set(YMH_BUILD_TESTS OFF)
endif()
```

On the merged tree `tests/` exists again and the guard is inert, so the guard is
the only `main`-side line that differs from `master`'s `CMakeLists.txt`.

## 4. Behaviour when nothing is configured

`resolve_model` stays total (§3) — it never throws for an unset model. The
fail-fast lives where a connection is actually built: `ProviderRegistry::create`
already rejects an empty `base_url` or `api_key_env` with
`LLMErrorCode::ConfigError` **before any network I/O**. So an unconfigured office
build fails loudly at provider construction, while `ymh list`, `ymh show`,
`ymh replay` and `ymh config` keep working (they never resolve a provider).

## 5. `dev`-side adaptations (one commit)

The tests legitimately relied on the built-in vendor endpoint/model. They now
pin their own values:

- `tests/support/dev_llm_config.hpp` — new, dev-only: provides the pinned
  `ymh::deepseek_config()` triple (the values `main` no longer compiles in), so
  the existing call sites keep working. `main` never sees this header.
- `tests/unit/{config_test,provider_registry_test,errata46_localcode_test,llm_live_test}.cpp`
  include it.
- `Config.DefaultsAreVendorFree` + `Config.DevPinIsNotTheBuiltinDefault` replace
  the two tests that asserted the compiled-in vendor defaults.
- `Config52` endpoint-default assertions expect an empty `base_url`.
- Harnesses that silently depended on the built-in default now state an explicit
  endpoint/model/key-env: `errata53_ui_test.cpp` (`config_with_models`),
  `supervisor_harness_test.cpp` (`UI45_D7_StatusUsesEffectiveModel`,
  `UI45_D10_ResumeFailureKeepsUsableFocus`), `workspace_runtime_test.cpp`
  (`options_for`).

## 6. Rebuilding the split (reproducible)

`master`'s history is linear, so no `filter-repo`/`filter-branch` is required.
Both steps are driven by the commit graph.

`E` (identical on both branches):

```sh
E=$(GIT_AUTHOR_DATE="2026-09-14 16:19:36 -0500" \
    GIT_COMMITTER_DATE="2026-09-14 16:19:36 -0500" \
    git commit-tree 4b825dc642cb6eb9a060e54bf8d69288fbee4904 -m "Initial commit")
```

`main` = `E` + one commit holding the sanitized tree with `docs/`, `tests/`,
`README.md`, `HANDOFF.md`, `AGENTS.md` removed (build the tree with a scratch
index: `git read-tree master`, `git rm -r --cached <those>`, `git update-index
--cacheinfo` for the six edited blobs, `git write-tree`).

`dev` = `E` + `master`'s commits replayed in order, each restricted to the kept
paths, pruning commits whose filtered tree is unchanged, with the kept root
re-parented onto `E` (author/committer identities and dates preserved via
`git commit-tree` env). This yields the same 180 commits; `dev`'s docs/tests are
byte-identical to `master`'s.

## 7. Invariants

- **B1** `git merge-base main dev == E`; merging requires no flags.
- **B2** after `git merge dev` into a branch at `main`, the tree equals the
  `master` tip tree modulo §3 and §5.
- **B3** `main` mentions no vendor anywhere; `main` configures and builds
  standalone (no `tests/`).
- **B4** for every kept path, `dev`'s content equals `master`'s plus exactly the
  §5 adaptation.

## 8. Verification performed

- `main`: fresh configure/build with no `tests/` directory → configure and the
  `ymh` target build clean (the guard turns tests off).
- merged (`main` + `dev`): full suite **2158 tests, 2143 pass**. The only
  failures are `GlobalTestEnv.ProcessWideIsolationIsInstalled` and
  `GlobalTestEnv.DefaultPathsResolveInsideIsolation`, which are **pre-existing
  and unrelated**: they pass in isolation (3/3) and fail only when a PTY test ran
  earlier in the same process and left `XDG_STATE_HOME`/`XDG_CONFIG_HOME` pointing
  at its scratch dir (`/tmp/ymh_pty_*`). Under `ctest` (one process per test)
  they pass. The files involved are untouched by this split.

## 9. Consequences / caveats

- `.gitignore` lives on `main` only, so `git status` on `dev` lists `build/`,
  `build-tsan/` and `.sisyphus/` as untracked. The merged tree is clean.
- `.github/workflows/ci.yml` lives on `main` and still invokes
  `tests/ymh_tests`; that step cannot pass on a docs-less checkout. The office
  push is a code push — run CI from the merged tree, or with
  `-DYMH_BUILD_TESTS=OFF`.
- New work should branch from `master`; periodically re-run §6 so `main` and
  `dev` follow. A change that touches both an interface and its test lands as
  two commits (one per branch) or as one merge.
