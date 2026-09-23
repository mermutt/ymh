#pragma once

// WorkspaceRuntime: the single in-process owner of one workspace's agent
// runtime (00 §9.6, 04 §3.7). It is the seam Milestone 2 grows a
// `WorkspaceHost` daemon around: the daemon hosts one `WorkspaceRuntime` per
// workspace, and the supervisor/CLI reach it only over the socket.
//
// For one workspace root the runtime owns and wires:
//   * the workspace-rooted `ExecutionEnvironment` and one `ResourceGovernor`
//   * the writer-open `SessionPersistence` store and the write lease
//   * the shared `EventBus`
//   * the `SessionManager` and the `AgentRegistry`
//   * the frozen `ToolRegistry` (built-ins incl. git) and the permission
//     policy + gate
//   * the resolved `LLMProvider`, the `LLMPool`, and the `ContextAssembler`
//
// This is a pure extraction of the Milestone-1 wiring that previously lived
// inline in `src/cli/headless.cpp`; observable behavior is unchanged. Callers
// create/resume sessions through `agents()` and take/release the write lease
// through `acquireLease()` / `releaseLease()`.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "ymh/config/config.hpp"
#include "ymh/mcp/mcp_client.hpp"
#include "ymh/session/session_persistence.hpp"

namespace ymh {

class ExecutionEnvironment;
class ResourceGovernor;
class EventBus;
class SessionManager;
class AgentRegistry;
class ToolRegistry;
class PermissionPolicy;
class PermissionGate;
class GrantStore;
class ContextAssembler;
class LLMProvider;
class LLMPool;
class SessionPersistence;
class Executor;
class SkillCatalog;
class TokenEstimator;
class McpManager;
class AgentPresetRoster;
class PlanModeController;
class ModelSelectionController;
class ModelCatalog;
struct AgentConfig;
struct CompactionPolicy;
struct LLMProviderConfig;
struct SessionId;

// Failure surface of `make_workspace_runtime` (04 §3.3 step 6). Codes mirror
// the distinct startup failures the inline wiring reported: a missing
// workspace root, an unopenable session store, and a provider-factory throw.
enum class WorkspaceRuntimeErrorCode : std::uint8_t {
    WorkspaceMissing,
    StoreUnavailable,
    WorkspaceBusy,
    ProviderSetupFailed,
    Internal,
};

struct WorkspaceRuntimeError {
    WorkspaceRuntimeErrorCode code = WorkspaceRuntimeErrorCode::Internal;
    std::string               detail;
};

struct WorkspaceRuntimeOptions {
    Config                config;
    std::filesystem::path root;     // canonical workspace root (04 H2)
    BootId                boot_id;  // minted once by the owner (02 §5.2)

    // When true, the owned `PermissionGate` is attached to `AgentServices`
    // (the interactive TUI path). When false, the loop consults the policy
    // verdict directly and an `Ask` fails closed (the headless path).
    bool attach_permission_gate = false;

    // 20 §5.5 (round-2 H1): the daemon installs a `PermissionBroker`-backed
    // resolver immediately after building the runtime, so the skill tool's
    // usability predicate must know a prompt path will exist even though
    // `attach_permission_gate` is false. Set true only where a resolver is
    // installed (`workspace_host.cpp`); headless leaves it false.
    bool attach_permission_resolver = false;

    // Test hook (mirrors `HeadlessOptions::provider_factory`): when set and it
    // returns a provider, that provider is used instead of resolving one from
    // the `ProviderRegistry`. A throw is reported as `ProviderSetupFailed`.
    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> provider_factory;

    // 11-m2-errata §3.5 (D9): when set, the runtime uses this store instead of
    // opening the real SQLite file, so daemon integration tests can run without
    // a DB. The caller owns the store and must keep it alive for the runtime's
    // lifetime; never set in production (H6: one store / one flock).
    std::function<std::unique_ptr<SessionStore>()> store_factory;

    // 14 §4.5 (E-P7): the daemon's loop adapter. When set, the runtime owns a
    // `LocalPtyService` over it and the `terminal` tool is registered; when
    // null, `pty()` is the internal `UnavailablePtyService`. The caller owns
    // the executor and must outlive the runtime.
    Executor* executor = nullptr;

    // 15 §11.2 test seam: when set, the owned `McpManager` uses this factory
    // instead of the production stdio factory, so tests can inject a scripted
    // client. Never set in production.
    McpClientFactory mcp_client_factory;

    // 46-D2.3: the daemon-owned durable grants store, injected as a non-owning
    // pointer (exactly like `executor`). The caller owns it and must keep it
    // alive for the runtime's lifetime; null disables persistence.
    GrantStore* grant_store = nullptr;
};

class WorkspaceRuntime {
public:
    // Opens the store, resolves the provider, and builds the full runtime.
    // Returns `WorkspaceMissing` when `root` is not an existing directory,
    // `StoreUnavailable` when the store cannot be opened, and
    // `ProviderSetupFailed` when `provider_factory` throws.
    [[nodiscard]] static std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError>
    create(WorkspaceRuntimeOptions options);

    WorkspaceRuntime(const WorkspaceRuntime&) = delete;
    WorkspaceRuntime& operator=(const WorkspaceRuntime&) = delete;
    WorkspaceRuntime(WorkspaceRuntime&&) = delete;
    WorkspaceRuntime& operator=(WorkspaceRuntime&&) = delete;
    ~WorkspaceRuntime();

    [[nodiscard]] const std::filesystem::path& root() const noexcept;

    [[nodiscard]] ExecutionEnvironment& environment() noexcept;
    [[nodiscard]] ResourceGovernor&     governor() noexcept;
    [[nodiscard]] EventBus&             bus() noexcept;
    [[nodiscard]] SessionStore&         store() noexcept;
    [[nodiscard]] SessionPersistence*   persistence() noexcept;

    // True when the runtime opened the real SQLite `SessionPersistence`; false
    // when a `store_factory` supplied an in-memory fake (D9 store seam). Only a
    // durable store carries a cross-process write lease, so the lease methods
    // below are no-ops when this is false.
    [[nodiscard]] bool hasDurableStore() const noexcept;
    [[nodiscard]] SessionManager&       sessions() noexcept;
    [[nodiscard]] AgentRegistry&        agents() noexcept;
    [[nodiscard]] ToolRegistry&         tools() noexcept;
    [[nodiscard]] PermissionPolicy&     policy() noexcept;
    [[nodiscard]] PermissionGate&       gate() noexcept;
    [[nodiscard]] SkillCatalog&         skills() noexcept;
    [[nodiscard]] const SkillCatalog&   skills() const noexcept;
    [[nodiscard]] bool                  has_provider() const noexcept;
    [[nodiscard]] LLMPool&              pool() noexcept;
    [[nodiscard]] ContextAssembler&     context() noexcept;

    // 25-D2: the owned plan-mode controller, for the `session.set_mode` handler
    // and `deleteSession`'s memo cleanup.
    [[nodiscard]] PlanModeController& plan_mode() noexcept;

    // 53-D7: the owned model catalog and mid-flight selection controller, for
    // the `session.set_model`/`session.create` resolution and `deleteSession`'s
    // memo cleanup.
    [[nodiscard]] ModelCatalog&             model_catalog() noexcept;
    [[nodiscard]] ModelSelectionController& model_selection() noexcept;

    [[nodiscard]] const AgentConfig&       agent_config() const noexcept;
    // 52-D15/52-I12: the deployment default permission preset name pinned into
    // each new session.
    [[nodiscard]] const std::string&       default_permission_preset() const noexcept;
    // 52-D15/52-I11: the permission preset name to pin for a session started
    // with `agent_preset`. The preset's row binding is honored only when it does
    // not widen the deployment baseline; otherwise the deployment default is
    // returned.
    [[nodiscard]] std::string effective_permission_preset(
        const std::optional<std::string>& agent_preset) const;
    [[nodiscard]] const LLMProviderConfig& provider_config() const noexcept;

    // 18 §3.4 (CX-08): read-only accessors for the assembled-context inspector.
    [[nodiscard]] const TokenEstimator&        estimator() const noexcept;
    [[nodiscard]] const CompactionPolicy*      compaction_policy() const noexcept;
    [[nodiscard]] std::vector<McpServerStatus> mcp_statuses() const;

    // 24-D9/AL28: the owned MCP manager, so the coordinator can shut it down
    // explicitly before `TransportServer::stop()` (mirrors `gate()`).
    [[nodiscard]] McpManager& mcp() noexcept;

    // 45-D9.8: the owned agent-preset roster (built from `config.presets`), so
    // `agent.list`/`agent.select` reach the same instance the loops are given
    // via `AgentServices::presets`.
    [[nodiscard]] AgentPresetRoster& presets() noexcept;

    // 24-D9/AL28: ordered child teardown after quiesce and before transport
    // stop — close every PTY session, then shut the MCP manager down within
    // `grace`. Idempotent (`McpManager` guards its `shutdown_` flag).
    void shutdownChildren(std::chrono::milliseconds grace);

    // Write-lease convenience: the durable store is the sole lease authority
    // (02 §5). `acquireLease` returns false when another writer holds the
    // session; `releaseLease` returns false when nothing was released. Both may
    // throw on a store failure, exactly like the underlying store calls. When
    // `hasDurableStore()` is false there is no cross-process lease to take:
    // `acquireLease` reports success, `releaseLease` reports nothing released,
    // and `renewLeases` does nothing.
    bool acquireLease(const SessionId& id);
    bool releaseLease(const SessionId& id);
    void renewLeases();

private:
    class Impl;

    explicit WorkspaceRuntime(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

// Convenience factory over `WorkspaceRuntime::create`.
[[nodiscard]] std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError>
make_workspace_runtime(WorkspaceRuntimeOptions options);

// 20 §5.5 (SK17): whether the model-invoked `skill` tool is usable in a runtime
// with the given policy and permission path. `Deny` is always unusable; `Allow`
// needs no prompt path; `Ask` needs one (the M1 gate or the M2 broker resolver).
[[nodiscard]] bool skill_tool_usable(const PermissionPolicy& policy,
                                     bool                   prompt_path_available);

} // namespace ymh
