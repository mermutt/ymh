#include "ymh/agent/session_sandbox_policy.hpp"

#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/permissions_state.hpp"

namespace ymh {
namespace {

SandboxMode parse_mode(std::string_view mode) noexcept {
    if (mode == "read-only") {
        return SandboxMode::ReadOnly;
    }
    if (mode == "danger-full-access") {
        return SandboxMode::DangerFullAccess;
    }
    return SandboxMode::WorkspaceWrite;
}

ApprovalPolicy parse_policy(std::string_view policy) noexcept {
    return policy == "never" ? ApprovalPolicy::Never : ApprovalPolicy::Ask;
}

} // namespace

SessionSandboxPolicyService::SessionSandboxPolicyService(
    SessionManager& sessions, PermissionPresetSettings deployment, std::string max_mode,
    std::vector<std::filesystem::path> roots,
    std::map<std::string, PermissionPresetSettings, std::less<>> presets)
    : sessions_(sessions),
      deployment_(std::move(deployment)),
      max_mode_(std::move(max_mode)),
      roots_(std::move(roots)),
      presets_(std::move(presets)) {}

PermissionsState SessionSandboxPolicyService::stateFor(const SessionId& id) const {
    try {
        const std::shared_ptr<Session> session = sessions_.sessionPtr(id);
        PermissionsState               state  = permissions_state(session->events());
        // 119-D36: the header preset seeds the fold when no preset event exists.
        if (!state.preset.has_value() && session->header().permission_preset.has_value()) {
            state.preset = session->header().permission_preset;
        }
        return state;
    } catch (...) {
        return {};
    }
}

PermissionPresetSettings SessionSandboxPolicyService::bundleFor(
    const PermissionsState& state) const {
    if (state.preset.has_value()) {
        if (const auto it = presets_.find(*state.preset); it != presets_.end()) {
            return it->second;
        }
    }
    return deployment_;
}

SandboxMode SessionSandboxPolicyService::effectiveMode(const PermissionsState& state) const {
    const PermissionPresetSettings bundle = bundleFor(state);
    std::string                    mode   = state.mode.value_or(bundle.sandbox);
    if (!max_mode_.empty() && sandbox_rank(mode) > sandbox_rank(max_mode_)) {
        mode = max_mode_;
    }
    return parse_mode(mode);
}

ApprovalPolicy SessionSandboxPolicyService::effectivePolicy(
    const PermissionsState& state) const {
    const PermissionPresetSettings bundle = bundleFor(state);
    return parse_policy(state.policy.value_or(bundle.approval));
}

SandboxPolicy SessionSandboxPolicyService::resolveDeployment() const {
    SandboxPolicy policy;
    policy.mode                      = parse_mode(deployment_.sandbox);
    policy.additional_writable_roots = roots_;
    return policy;
}

SandboxPolicy SessionSandboxPolicyService::resolve(const PermissionRequest& request) const {
    SandboxPolicy policy = resolveDeployment();
    policy.mode          = effectiveMode(stateFor(request.session));
    policy.workspace_root = request.root;
    return policy;
}

std::optional<SandboxMode> SessionSandboxPolicyService::overrideOf(const SessionId& id) const {
    const PermissionsState state = stateFor(id);
    if (!state.mode.has_value()) {
        return std::nullopt;
    }
    return parse_mode(*state.mode);
}

ApprovalPolicy SessionSandboxPolicyService::approvalPolicy(const SessionId& id) const {
    return effectivePolicy(stateFor(id));
}

std::optional<EscalationRequest> SessionSandboxPolicyService::validateEscalation(
    const PermissionRequest&, const EscalationState&) const {
    // Wave 3 enables the one-shot escalation.
    return std::nullopt;
}

void SessionSandboxPolicyService::grantEscalation(const EscalationRequest&, ToolCallId,
                                                  std::uint64_t) {
    // Wave 3.
}

} // namespace ymh
