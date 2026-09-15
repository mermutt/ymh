#pragma once

// Headless mode (§42): `ymh run "task"` drives the real `AgentLoop` without
// FTXUI, prints assistant text + tool activity + the final result, and returns
// an exit code reflecting the turn's terminal event.
//
// The runner owns the single-process MVP wiring: it opens the workspace's
// SQLite store (`<workspace>/.ymh/sessions.db`), builds the environment, tools,
// permission policy, context assembler, provider, and `AgentRegistry`, then
// creates or resumes a session and sends one user message.
//
// Test hooks (not used in production):
//   * `provider_factory` injects a `FakeLLM` (or any provider) directly,
//     bypassing the `ProviderRegistry`.
//   * `YMH_FAKE_LLM_SCRIPT` names a JSON file that scripts a `FakeLLM`.
//   * `cancel_poll` requests cancellation while the turn runs.

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

#include "ymh/config/config.hpp"
#include "ymh/core/event.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/provider_registry.hpp"

namespace ymh {

struct HeadlessOptions {
    std::filesystem::path workspace;  // canonicalised by the runner
    std::string           task;
    std::optional<SessionId> resume;  // resume this session instead of creating one
    Config                config;
    bool                  verbose = false;  // echo reasoning deltas to stderr

    std::ostream* out = nullptr;  // default: std::cout
    std::ostream* err = nullptr;  // default: std::cerr

    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> provider_factory;
    std::function<bool()> cancel_poll;
};

struct HeadlessResult {
    int         exit_code = 0;
    SessionId   session;
    std::string assistant_text;
    std::string terminal;  // "turn/end" | "turn/cancel" | "turn/fail" | ""
};

[[nodiscard]] HeadlessResult run_headless(const HeadlessOptions& options);

} // namespace ymh
