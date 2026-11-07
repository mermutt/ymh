// 119 Wave 1: the sandbox seam implementation. The Landlock backend wraps a
// spawn into a confined plan (the in-child `ChildConfinement` applies between
// fork and exec), reports `full`/`partial`, and refuses a confining mode when
// no backend can enforce it. A deployment-only policy service resolves the
// `[sandbox]` baseline; Wave 2 replaces it with the per-session projection.

#include "ymh/execution/sandbox.hpp"

#include <utility>

#include "ymh/execution/confinement.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/process.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace ymh {
namespace {

constexpr std::string_view kLandlockDenials[] = {"Permission denied",
                                                 "Read-only file system",
                                                 "Operation not permitted"};

class LandlockSandboxProvider final : public SandboxProvider {
public:
    [[nodiscard]] std::string_view backend() const noexcept override { return "landlock"; }

    [[nodiscard]] std::optional<SandboxEnforcement> enforcement(
        SandboxMode mode) const noexcept override {
        if (mode == SandboxMode::DangerFullAccess) {
            return std::nullopt;
        }
        if (kernel_confinement().probe() != ConfinementStatus::Enforced) {
            return std::nullopt;
        }
        return kernel_confinement().abi() >= 5 ? SandboxEnforcement::Full
                                               : SandboxEnforcement::Partial;
    }

    [[nodiscard]] ConfinedSpawn confine(const ProcessRequest& requested,
                                        const SandboxPolicy& policy) const override {
        return build(requested.executable, requested.argv, policy);
    }

    [[nodiscard]] ConfinedSpawn confine(const PtyRequest& requested,
                                        const SandboxPolicy& policy) const override {
        return build(requested.executable, requested.argv, policy);
    }

private:
    static ConfinedSpawn build(const std::string& executable,
                               const std::vector<std::string>& argv,
                               const SandboxPolicy& policy) {
        ConfinedSpawn plan;
        plan.executable = executable;
        plan.argv       = argv;
        if (policy.mode == SandboxMode::DangerFullAccess) {
            return plan;
        }
        if (kernel_confinement().probe() != ConfinementStatus::Enforced) {
            throw ToolError{ToolErrorCode::SandboxUnavailable,
                            "no sandbox backend can enforce '" +
                                std::string{mode_name(policy.mode)} + "'"};
        }
        ChildConfinement child;
        child.root               = policy.workspace_root;
        child.workspace_writable = policy.mode == SandboxMode::WorkspaceWrite;
        if (child.workspace_writable) {
            // 119-D7: the workspace-write temp area is the host /tmp. The
            // per-spawn throwaway TMPDIR is gone (119-D5).
            child.temp                      = std::filesystem::path{"/tmp"};
            child.additional_writable_roots = policy.additional_writable_roots;
        }
        child.ro_paths = {std::filesystem::path{"/"}};
        plan.child     = std::move(child);
        plan.enforcement =
            kernel_confinement().abi() >= 5 ? SandboxEnforcement::Full
                                            : SandboxEnforcement::Partial;
        plan.denial_signatures.assign(std::begin(kLandlockDenials), std::end(kLandlockDenials));
        plan.runner_failure_rules.push_back(RunnerFailureRule{{}, {}, {"landlock: "}});
        return plan;
    }

    static std::string_view mode_name(SandboxMode mode) noexcept {
        switch (mode) {
            case SandboxMode::ReadOnly:          return "read-only";
            case SandboxMode::WorkspaceWrite:    return "workspace-write";
            case SandboxMode::DangerFullAccess:  return "danger-full-access";
        }
        return "unknown";
    }
};

} // namespace

SandboxProvider& default_sandbox_provider() {
    static LandlockSandboxProvider provider;
    return provider;
}

std::unique_ptr<SandboxPolicyService> make_deployment_policy_service(
    SandboxMode mode, std::vector<std::filesystem::path> roots) {
    return std::make_unique<DeploymentSandboxPolicyService>(mode, std::move(roots));
}

void ConfinedSpawn::apply(ProcessRequest& request) const {
    request.executable = executable;
    request.argv       = argv;
    for (const auto& [name, value] : env) {
        if (value.has_value()) {
            request.environment.emplace_back(name, *value);
        } else {
            request.env_unset.push_back(name);
        }
    }
    if (child.has_value()) {
        request.child = child;
    }
}

void ConfinedSpawn::apply(PtyRequest& request) const {
    request.executable = executable;
    request.argv       = argv;
    for (const auto& [name, value] : env) {
        if (value.has_value()) {
            request.environment.emplace_back(name, *value);
        } else {
            request.env_unset.push_back(name);
        }
    }
    if (child.has_value()) {
        request.child = child;
    }
}

DeploymentSandboxPolicyService::DeploymentSandboxPolicyService(
    SandboxMode mode, std::vector<std::filesystem::path> roots)
    : mode_(mode), roots_(std::move(roots)) {}

SandboxPolicy DeploymentSandboxPolicyService::resolveDeployment() const {
    SandboxPolicy policy;
    policy.mode                     = mode_;
    policy.additional_writable_roots = roots_;
    return policy;
}

SandboxPolicy DeploymentSandboxPolicyService::resolve(
    const PermissionRequest& request) const {
    SandboxPolicy policy = resolveDeployment();
    policy.workspace_root = request.root;
    return policy;
}

std::optional<SandboxMode> DeploymentSandboxPolicyService::overrideOf(
    const SessionId&) const {
    return std::nullopt;
}

ApprovalPolicy DeploymentSandboxPolicyService::approvalPolicy(const SessionId&) const {
    // Wave 2 reads the session projection.
    return ApprovalPolicy::Ask;
}

std::optional<EscalationRequest> DeploymentSandboxPolicyService::validateEscalation(
    const PermissionRequest&, const EscalationState&) const {
    // Wave 3 enables escalation; Wave 1 has no per-turn state to validate.
    return std::nullopt;
}

void DeploymentSandboxPolicyService::grantEscalation(const EscalationRequest&,
                                                     ToolCallId,
                                                     std::uint64_t) {
    // Wave 3.
}

} // namespace ymh
