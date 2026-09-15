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
// inline in `src/cli/headless.cpp` and `src/ui/ui_application.cpp`; observable
// behavior is unchanged. Callers create/resume sessions through `agents()` and
// take/release the write lease through `acquireLease()` / `releaseLease()`.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "ymh/config/config.hpp"
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
class ContextAssembler;
class LLMProvider;
class LLMPool;
class SessionPersistence;
struct AgentConfig;
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

    // Test hook (mirrors `HeadlessOptions::provider_factory`): when set and it
    // returns a provider, that provider is used instead of resolving one from
    // the `ProviderRegistry`. A throw is reported as `ProviderSetupFailed`.
    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> provider_factory;

    // 11-m2-errata §3.5 (D9): when set, the runtime uses this store instead of
    // opening the real SQLite file, so daemon integration tests can run without
    // a DB. The caller owns the store and must keep it alive for the runtime's
    // lifetime; never set in production (H6: one store / one flock).
    std::function<std::unique_ptr<SessionStore>()> store_factory;
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
    [[nodiscard]] LLMProvider*          provider() noexcept;
    [[nodiscard]] LLMPool&              pool() noexcept;
    [[nodiscard]] ContextAssembler&     context() noexcept;

    [[nodiscard]] const AgentConfig&       agent_config() const noexcept;
    [[nodiscard]] const LLMProviderConfig& provider_config() const noexcept;

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

} // namespace ymh
