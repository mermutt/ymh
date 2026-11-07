#pragma once

// 119 Wave 2: the session-aware `SandboxPolicyService`. It folds the durable
// `sandbox/mode` / `approval/policy` / `permission/preset` events (seeded by
// the header preset from D36), clamps the mode to `[sandbox].max_mode`, and
// answers the agent loop + broker. The deployment-only fallback lives in
// `execution/sandbox.hpp` and is used when a session is not resident.

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ymh/config/config.hpp"
#include "ymh/execution/sandbox.hpp"
#include "ymh/session/permissions_state.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {

class SessionSandboxPolicyService final : public SandboxPolicyService {
public:
    SessionSandboxPolicyService(
        SessionManager& sessions,
        PermissionPresetSettings deployment,
        std::string max_mode,
        std::vector<std::filesystem::path> roots,
        std::map<std::string, PermissionPresetSettings, std::less<>> presets);

    [[nodiscard]] SandboxPolicy resolveDeployment() const override;
    [[nodiscard]] SandboxPolicy resolve(const PermissionRequest& request) const override;
    [[nodiscard]] std::optional<SandboxMode> overrideOf(const SessionId& id) const override;
    [[nodiscard]] ApprovalPolicy approvalPolicy(const SessionId& id) const override;
    [[nodiscard]] std::optional<EscalationRequest> validateEscalation(
        const PermissionRequest& request, const EscalationState& state) const override;

private:
    friend class AgentLoop;
    void grantEscalation(const EscalationRequest& request, ToolCallId retry_call,
                         std::uint64_t approved_generation) override;

    [[nodiscard]] PermissionsState stateFor(const SessionId& id) const;
    [[nodiscard]] PermissionPresetSettings bundleFor(const PermissionsState& state) const;
    [[nodiscard]] SandboxMode effectiveMode(const PermissionsState& state) const;
    [[nodiscard]] ApprovalPolicy effectivePolicy(const PermissionsState& state) const;

    SessionManager&                                              sessions_;
    PermissionPresetSettings                                     deployment_;
    std::string                                                  max_mode_;
    std::vector<std::filesystem::path>                           roots_;
    std::map<std::string, PermissionPresetSettings, std::less<>> presets_;
};

} // namespace ymh
