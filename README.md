# ymh

`ymh` is a terminal-first coding-agent harness written in C++23, in the same
family as Claude Code / OpenCode. It runs an LLM agent loop over a workspace,
with real tools (read/write/edit files, grep, glob, shell, git), an explicit
permission policy, and an event-sourced session log. It is SSH-friendly,
low-overhead, and provider-agnostic (any OpenAI-compatible endpoint; DeepSeek by
default).

Both milestones are implemented and green. **Milestone 1** is the single-process
MVP: core event bus, event-sourced session log with SQLite persistence, LLM
providers (OpenAI-compatible, DeepSeek default) plus a deterministic `FakeLLM`,
the execution environment with tools, permissions, the agent loop, CLI +
headless (`ymh run`), an FTXUI TUI, and markdown/syntax/diff rendering.
**Milestone 2** splits the supervisor TUI from one `WorkspaceHost` daemon per
workspace: JSON-RPC 2.0 over a length-prefixed Unix domain socket, with a shared
`registry.db`. **Phase 2** adds context compaction, PTY support behind the
execution-environment seam, and an MCP adapter into the shared tool registry.
Architecture and component specs live in `docs/design/`.

## Build prerequisites

- CMake ≥ 3.25, Ninja, and a C++23 compiler (GCC 13+ / Clang 16+; built and
  tested here with GCC 16).
- System libraries: **SQLite3, nlohmann_json, spdlog, fmt, libcurl, libgit2,
  cmark-gfm**.
- First configure downloads these via CMake `FetchContent` (network required):
  FTXUI v6.1.9, Asio 1.38.2, CLI11 v2.5.0, GoogleTest v1.17.0
  (GoogleTest only when tests are enabled, which is the default).

Arch Linux:

```sh
sudo pacman -S --needed cmake ninja base-devel sqlite nlohmann-json spdlog fmt \
    curl libgit2 cmark-gfm
```

Debian/Ubuntu equivalents: `cmake ninja-build g++ libsqlite3-dev
nlohmann-json3-dev libspdlog-dev libfmt-dev libcurl4-openssl-dev libgit2-dev
libcmark-gfm-dev`.

## Build and test

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

First-party targets compile with `-Wall -Wextra -Wpedantic -Werror`; the build is
warning-clean by policy. The default test suite is hermetic (no network) and
covers the event system, session store, LLM adapters, tools, permissions, agent
loop, CLI, and TUI render/PTY paths.

## Running

The binary is `build/ymh`.

```sh
ymh                                   # attach to / spawn the cwd workspace daemon, then TUI
ymh run "Find the bug in src/foo.cpp" # headless, prints assistant text + tool activity
ymh --resume <session-id>             # TUI, resumed
ymh run --resume <session-id> "task"  # headless, resumed
ymh list                              # sessions in the workspace
ymh show <session-id>                 # projected messages
ymh replay <session-id>               # raw event log
ymh fork <session-id>                 # fork a session
ymh workspace add <path>              # register a workspace in the shared registry
ymh workspace list                    # list registered workspaces
ymh workspace stop <id|path>          # stop a workspace daemon
ymh config path                       # print the global config file path
ymh --host --workspace <uuid> --root <canonical-root> --socket <path>  # daemon entry (spawned by the supervisor)
ymh --version
ymh --help
```

The workspace defaults to the current directory; override with `--workspace
PATH`. A workspace is identified by its `.ymh/` directory, which holds the
session store (`sessions.db`) and optional config. With no arguments, `ymh`
attaches to (or spawns) that workspace's `WorkspaceHost` daemon, which owns the
cwd and survives the TUI; running workspaces are tracked in a shared
`${XDG_STATE_HOME}/ymh/registry.db`.

## Configuration

Config is **JSONC** (JSON with `//` and `/* */` comments; `#` is not a comment,
and trailing commas are not allowed). TOML is retired: ymh never reads, probes,
or warns about `config.toml`, so a leftover `config.toml` is invisible
(`docs/design/21-config-jsonc-errata.md` §6).

Layered, last writer wins:

```
built-in defaults
  -> global    ($XDG_CONFIG_HOME/ymh/config.jsonc, else ~/.config/ymh/config.jsonc)
  -> workspace (<workspace>/.ymh/config.jsonc)
  -> environment (YMH_* variables)
  -> command line
```

The global file is **required**: if it is empty or absent, ymh exits with a
clear error (exit 2). First run scaffolds the conventional global `config.jsonc`
(and `<workspace>/.ymh/`). An explicit `--config <path>` is **never**
auto-created: a missing explicit path is that same hard error. A path that
exists but is not a regular file is also rejected. The workspace layer is
optional: when `<workspace>/.ymh/config.jsonc` is absent it contributes nothing.

The loader is strict: an unknown key or a bad value is an error, not a silent
default. Example:

```jsonc
{
  "llm": {
    "provider": "openai-compatible",
    "base_url": "https://api.deepseek.com/v1",
    "model": "deepseek-flash",
    "api_key_env": "DEEPSEEK_API_KEY",   // names the env var; the secret is never stored
    "reasoning_effort": "low"
  },
  "agent": {
    "max_steps": 100
  },
  "permissions": {
    "read": "allow",   // read_file, grep, glob
    "write": "ask",    // write_file, edit_file
    "shell": "ask"
  },
  "logging": {
    "level": "info"
  }
}
```

The `llm` section may also be written nested as `llm.default`. Environment
overrides include
`YMH_LLM_PROVIDER`, `YMH_LLM_MODEL`, `YMH_LLM_BASE_URL`, `YMH_API_KEY_ENV`,
`YMH_REASONING_EFFORT`, `YMH_AGENT_MAX_STEPS`, and `YMH_LOG_LEVEL`.

### API key

`DEEPSEEK_API_KEY` is read from the environment (the variable name is
configurable via `llm.api_key_env`). If you keep the key in
`~/.apikey.deepseek` as `export DEEPSEEK_API_KEY=...`, source it first:

```sh
set -a; . "$HOME/.apikey.deepseek"; set +a
```

The key is never written to config or to logs.

### Permissions

`read` tools default to `allow`; `write` and `shell` default to `ask`. The
interactive TUI can answer `ask`; **headless `ymh run` has no approval channel,
so an `ask` verdict resolves fail-closed to `deny`.** To let a headless run
write files or run commands, opt in explicitly in the workspace config:

```jsonc
// <workspace>/.ymh/config.jsonc
{
  "permissions": {
    "write": "allow",
    "shell": "allow"
  }
}
```

## Live tests (real DeepSeek API)

The default suite is offline: 707 hermetic tests plus 16 opt-in live tests (real
`LLMProvider`, real PTY). The live layer is gated behind an explicit opt-in and
is skipped, not failed, when the key or flag is absent:

```sh
set -a; . "$HOME/.apikey.deepseek"; set +a
YMH_LIVE_LLM=1 ctest --test-dir build -R 'Live' --output-on-failure
```

Optional: `YMH_LIVE_LLM_MODEL` overrides the model used by the live tests
(default `deepseek-flash`).

## License

Not yet specified.
