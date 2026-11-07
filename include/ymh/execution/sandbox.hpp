#pragma once

// 119 (Rev 5) Wave 1: the sandbox seam. `SandboxMode` is the single mode enum
// (moved and renamed from `confinement.hpp`); the provider wraps a spawn into a
// `ConfinedSpawn` and reports what it actually enforced, or refuses the spawn.
// Network and process visibility are explicitly outside this vocabulary
// (`config path`/`/sandbox` print `network: unconfined`).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ymh/core/event.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {

class ProcessRequest;   // ymh/execution/process.hpp (forward only: no cycle)
class PtyRequest;       // ymh/execution/pty.hpp
class PermissionRequest;

enum class SandboxMode : std::uint8_t {
    ReadOnly,          // no writes anywhere except /dev/null
    WorkspaceWrite,    // workspace root + /tmp + declared roots
    DangerFullAccess,  // no provider call; the raw argv is spawned
};

// `Partial` means the active backend governs only a subset of the promised
// file effects (older Landlock ABI, or an unhandled right class).
enum class SandboxEnforcement : std::uint8_t { Full, Partial };

// 119-D2: `never` rejects an approval request; it never grants one.
enum class ApprovalPolicy : std::uint8_t { Ask, Never };

struct SandboxPolicy {
    SandboxMode                        mode = SandboxMode::WorkspaceWrite;
    std::filesystem::path              workspace_root;              // session cwd, immutable
    std::vector<std::filesystem::path> additional_writable_roots;   // validated, canonical
};

// The in-child half of a plan (replaces 89's `ConfinementPolicy`). `ro_paths`
// is {"/"}; `/dev/null` is granted file-only RW by the backend.
struct ChildConfinement {
    std::filesystem::path                root;
    std::optional<std::filesystem::path> temp;
    std::vector<std::filesystem::path>   additional_writable_roots;
    std::vector<std::filesystem::path>   ro_paths;
    bool                                 workspace_writable = true;
};

struct SandboxFacts {
    SandboxMode                       mode = SandboxMode::WorkspaceWrite;
    std::optional<SandboxEnforcement> enforcement;          // absent under DangerFullAccess
    bool                              denied = false;
    bool                              runner_failure = false;
};

// Per-turn escalation bookkeeping (Wave 2 owns it; the types are pinned now so
// the service signature is stable).
struct EscalationState {
    std::optional<ToolCallId> last_denied_call;
    std::string               last_denied_command;
    TurnId                    turn{};
    bool                      used = false;
};

struct EscalationRequest {
    ToolCallId  denied_call;
    std::string command;
    SandboxMode mode = SandboxMode::WorkspaceWrite;
};

struct RunnerFailureRule {
    std::vector<int>         allowed_exit_codes;      // empty => any nonzero
    std::vector<std::string> informational_lines;     // exact, case-insensitive
    std::vector<std::string> fatal_signatures;        // substring, case-insensitive
};

struct ConfinedSpawn {
    std::string executable;   // the caller's, or the wrapper's
    std::vector<std::string> argv;
    // Overlay; a nullopt value unsets the variable (the env_scrub mechanism).
    std::vector<std::pair<std::string, std::optional<std::string>>> env;
    std::optional<ChildConfinement>   child;        // applied between fork and exec
    std::optional<SandboxEnforcement> enforcement;  // absent => unconfined
    std::vector<std::string>          denial_signatures;
    std::vector<RunnerFailureRule>    runner_failure_rules;

    void apply(ProcessRequest& request) const;
    void apply(PtyRequest& request) const;
};

class SandboxProvider {
public:
    virtual ~SandboxProvider() = default;

    [[nodiscard]] virtual std::string_view backend() const noexcept = 0;

    // The decision input (`prepareToolCall` in W3); the spawn re-checks that
    // the plan's enforcement equals this value.
    [[nodiscard]] virtual std::optional<SandboxEnforcement> enforcement(
        SandboxMode mode) const noexcept = 0;

    // Throws `ToolError{ToolErrorCode::SandboxUnavailable, ...}` when the mode
    // cannot be enforced; never returns an unconfined plan for a confining mode.
    [[nodiscard]] virtual ConfinedSpawn confine(const ProcessRequest& requested,
                                                const SandboxPolicy& policy) const = 0;
    [[nodiscard]] virtual ConfinedSpawn confine(const PtyRequest& requested,
                                                const SandboxPolicy& policy) const = 0;
};

// Wave 1 deployment-only resolver; Wave 2 adds the session projection and the
// escalation grant (the private `grantEscalation` hook is already reserved).
class SandboxPolicyService {
public:
    virtual ~SandboxPolicyService() = default;

    [[nodiscard]] virtual SandboxPolicy resolveDeployment() const = 0;
    [[nodiscard]] virtual SandboxPolicy resolve(const PermissionRequest& request) const = 0;
    [[nodiscard]] virtual std::optional<SandboxMode> overrideOf(const SessionId&) const = 0;
    [[nodiscard]] virtual ApprovalPolicy approvalPolicy(const SessionId&) const = 0;

    // 119-D31: pre-approval validation; the grant itself is service-owned and
    // recorded through the private hook below (Wave 2).
    [[nodiscard]] virtual std::optional<EscalationRequest> validateEscalation(
        const PermissionRequest& request, const EscalationState& state) const = 0;

private:
    friend class AgentLoop;
    virtual void grantEscalation(const EscalationRequest& request, ToolCallId retry_call,
                                 std::uint64_t approved_generation) = 0;
};

// The deployment-only implementation (Wave 1). `mode`/`roots` come from the
// parsed `[sandbox]` baseline; `root` is filled per call from the request.
class DeploymentSandboxPolicyService final : public SandboxPolicyService {
public:
    DeploymentSandboxPolicyService(SandboxMode mode, std::vector<std::filesystem::path> roots);

    [[nodiscard]] SandboxPolicy resolveDeployment() const override;
    [[nodiscard]] SandboxPolicy resolve(const PermissionRequest& request) const override;
    [[nodiscard]] std::optional<SandboxMode> overrideOf(const SessionId&) const override;
    [[nodiscard]] ApprovalPolicy approvalPolicy(const SessionId&) const override;
    [[nodiscard]] std::optional<EscalationRequest> validateEscalation(
        const PermissionRequest& request, const EscalationState& state) const override;

private:
    void grantEscalation(const EscalationRequest& request, ToolCallId retry_call,
                         std::uint64_t approved_generation) override;

    SandboxMode                        mode_;
    std::vector<std::filesystem::path> roots_;
};

// Wave 1 wiring: the process-wide Landlock-backed provider, and a
// deployment-only policy service (mode + roots from `[sandbox]`).
[[nodiscard]] SandboxProvider& default_sandbox_provider();
[[nodiscard]] std::unique_ptr<SandboxPolicyService> make_deployment_policy_service(
    SandboxMode mode, std::vector<std::filesystem::path> roots);

} // namespace ymh
