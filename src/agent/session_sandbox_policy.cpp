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

std::string mode_name(SandboxMode mode) noexcept {
    switch (mode) {
        case SandboxMode::ReadOnly:         return "read-only";
        case SandboxMode::WorkspaceWrite:   return "workspace-write";
        case SandboxMode::DangerFullAccess: return "danger-full-access";
    }
    return "workspace-write";
}

ApprovalPolicy parse_policy(std::string_view policy) noexcept {
    return policy == "never" ? ApprovalPolicy::Never : ApprovalPolicy::Ask;
}

} // namespace

SessionSandboxPolicyService::SessionSandboxPolicyService(
    SessionManager& sessions, PermissionPresetSettings deployment, std::string max_mode,
    bool allow_escalation_to_danger_full_access, std::vector<std::filesystem::path> roots,
    std::map<std::string, PermissionPresetSettings, std::less<>> presets)
    : sessions_(sessions),
      deployment_(std::move(deployment)),
      max_mode_(std::move(max_mode)),
      allow_danger_(allow_escalation_to_danger_full_access),
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
    // 119-D22: consume the single-use grant bound to this retry call.
    {
        std::lock_guard<std::mutex> lock(grant_mutex_);
        if (grant_mode_.has_value() && grant_call_ == request.call) {
            policy.mode = *grant_mode_;
            grant_mode_.reset();
        }
    }
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
    const PermissionRequest& request, const EscalationState& state) const {
    if (request.tool != "shell" && request.tool != "terminal") {
        return std::nullopt;
    }
    const auto mode_it = request.arguments.find("sandbox_permissions");
    const auto why_it  = request.arguments.find("justification");
    if (mode_it == request.arguments.end() || !mode_it->is_string() ||
        why_it == request.arguments.end() || !why_it->is_string() ||
        why_it->get<std::string>().empty()) {
        return std::nullopt;
    }
    if (!state.last_denied_call.has_value() || state.used ||
        request.call == *state.last_denied_call ||
        request.turn != state.turn) {
        return std::nullopt;
    }
    const auto command_it = request.arguments.find("command");
    const std::string command =
        command_it != request.arguments.end() && command_it->is_string()
            ? command_it->get<std::string>()
            : std::string{};
    if (command.empty() || command != state.last_denied_command) {
        return std::nullopt;
    }
    const SandboxMode requested = parse_mode(mode_it->get<std::string>());
    if (mode_it->get<std::string>() != "workspace-write" &&
        mode_it->get<std::string>() != "danger-full-access") {
        return std::nullopt;
    }
    const SandboxMode base = effectiveMode(stateFor(request.session));
    if (sandbox_rank(mode_name(requested)) <= sandbox_rank(mode_name(base))) {
        return std::nullopt;
    }
    if (!max_mode_.empty() &&
        sandbox_rank(mode_name(requested)) > sandbox_rank(max_mode_)) {
        return std::nullopt;
    }
    if (requested == SandboxMode::DangerFullAccess && !allow_danger_) {
        return std::nullopt;
    }
    return EscalationRequest{*state.last_denied_call, command, requested};
}

void SessionSandboxPolicyService::grantEscalation(const EscalationRequest& request,
                                                  ToolCallId retry_call,
                                                  std::uint64_t approved_generation) {
    std::lock_guard<std::mutex> lock(grant_mutex_);
    grant_mode_       = request.mode;
    grant_call_       = std::move(retry_call);
    grant_generation_ = approved_generation;
}

} // namespace ymh
