// 119 Wave 1: the sandbox seam implementation. The Landlock backend wraps a
// spawn into a confined plan (the in-child `ChildConfinement` applies between
// fork and exec), reports `full`/`partial`, and refuses a confining mode when
// no backend can enforce it. A deployment-only policy service resolves the
// `[sandbox]` baseline; Wave 2 replaces it with the per-session projection.

#include "ymh/execution/sandbox.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <utility>

#include "ymh/execution/confinement.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/process.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace ymh {
namespace {

constexpr std::string_view kBwrapDenials[] = {"Read-only file system", "EROFS",
                                              "Permission denied", "Operation not permitted"};

int run_quiet(const std::vector<std::string>& argv) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
        }
        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const std::string& arg : argv) {
            raw.push_back(const_cast<char*>(arg.c_str()));
        }
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// 119-D42: a positive and a negative probe. A wrapper that accepts argv but
// does not enforce the binds passes (a) and fails (c), so it is never `full`.
std::mutex                 g_bwrap_mutex;
std::optional<bool>        g_bwrap_cache;

bool bwrap_probe() {
    std::lock_guard<std::mutex> lock(g_bwrap_mutex);
    std::optional<bool>& cached = g_bwrap_cache;
    if (cached.has_value()) {
        return *cached;
    }
    const auto fail = [&]() {
        cached = false;
        return false;
    };
    const int version_rc = run_quiet({"bwrap", "--version"});
    if (version_rc != 0) {
        return fail();
    }
    char grant_template[] = "/tmp/ymh-bwrap-grant-XXXXXX";
    const char* grant_dir = ::mkdtemp(grant_template);
    if (grant_dir == nullptr) {
        return fail();
    }
    const std::string grant{grant_dir};
    std::vector<std::string> outside_candidates;
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        outside_candidates.emplace_back(home);
    }
    outside_candidates.emplace_back("/var/tmp");
    outside_candidates.emplace_back("/run/user/" + std::to_string(::getuid()));
    std::string outside;
    for (const std::string& candidate : outside_candidates) {
        if (::access(candidate.c_str(), W_OK) == 0 &&
            candidate.rfind("/tmp", 0) != 0) {
            outside = candidate + "/ymh-bwrap-probe-" + std::to_string(::getpid());
            break;
        }
    }
    if (outside.empty()) {
        ::rmdir(grant_dir);
        return fail();
    }
    const auto base = [&]() {
        return std::vector<std::string>{"bwrap", "--die-with-parent", "--ro-bind", "/",
                                        "/",     "--dev",           "/dev"};
    };
    std::vector<std::string> positive = base();
    positive.insert(positive.end(), {"--tmpfs", "/tmp", "--bind", grant, grant, "--",
                                     "/bin/sh", "-c", "echo ok > '" + grant + "/x'"});
    const bool positive_ok = run_quiet(positive) == 0;
    std::vector<std::string> negative = base();
    negative.insert(negative.end(), {"--", "/bin/sh", "-c",
                                     "echo x > '" + outside + "/y'"});
    const bool negative_ok = run_quiet(negative) != 0;
    ::unlink((outside + "/y").c_str());
    ::unlink((grant + "/x").c_str());
    ::rmdir(grant_dir);
    cached = positive_ok && negative_ok;
    return *cached;
}

class BwrapSandboxProvider final : public SandboxProvider {
public:
    [[nodiscard]] std::string_view backend() const noexcept override { return "bwrap"; }

    [[nodiscard]] std::optional<SandboxEnforcement> enforcement(
        SandboxMode mode) const noexcept override {
        if (mode == SandboxMode::DangerFullAccess) {
            return std::nullopt;
        }
        return bwrap_probe() ? std::optional<SandboxEnforcement>{SandboxEnforcement::Full}
                             : std::nullopt;
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
        if (policy.mode == SandboxMode::DangerFullAccess) {
            plan.executable = executable;
            plan.argv       = argv;
            return plan;
        }
        if (!bwrap_probe()) {
            throw ToolError{ToolErrorCode::SandboxUnavailable,
                            "bwrap is not functional on this host"};
        }
        if (policy.workspace_root.empty()) {
            throw ToolError{ToolErrorCode::SandboxUnavailable,
                            "workspace-write requires a workspace root"};
        }
        std::vector<std::string> wrapper{"bwrap",
                                         "--die-with-parent",
                                         "--ro-bind",
                                         "/",
                                         "/",
                                         "--dev",
                                         "/dev",
                                         "--proc",
                                         "/proc",
                                         "--unshare-pid"};
        if (policy.mode == SandboxMode::WorkspaceWrite) {
            wrapper.insert(wrapper.end(), {"--tmpfs", "/tmp"});
            wrapper.insert(wrapper.end(), {"--bind", policy.workspace_root.string(),
                                           policy.workspace_root.string()});
            for (const auto& extra : policy.additional_writable_roots) {
                wrapper.insert(wrapper.end(), {"--bind", extra.string(), extra.string()});
            }
        }
        wrapper.insert(wrapper.end(), {"--chdir", policy.workspace_root.string(), "--"});
        plan.executable = "bwrap";
        plan.argv       = std::move(wrapper);
        plan.argv.insert(plan.argv.end(), argv.begin(), argv.end());
        plan.enforcement = SandboxEnforcement::Full;
        plan.denial_signatures.assign(std::begin(kBwrapDenials), std::end(kBwrapDenials));
        plan.runner_failure_rules.push_back(RunnerFailureRule{{}, {}, {"bwrap: "}});
        return plan;
    }
};


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

class ChainedSandboxProvider final : public SandboxProvider {
public:
    [[nodiscard]] std::string_view backend() const noexcept override {
        return bwrap_probe() ? bwrap_.backend() : landlock_.backend();
    }

    [[nodiscard]] std::optional<SandboxEnforcement> enforcement(
        SandboxMode mode) const noexcept override {
        return bwrap_probe() ? bwrap_.enforcement(mode) : landlock_.enforcement(mode);
    }

    [[nodiscard]] ConfinedSpawn confine(const ProcessRequest& requested,
                                        const SandboxPolicy& policy) const override {
        return bwrap_probe() ? bwrap_.confine(requested, policy)
                             : landlock_.confine(requested, policy);
    }

    [[nodiscard]] ConfinedSpawn confine(const PtyRequest& requested,
                                        const SandboxPolicy& policy) const override {
        return bwrap_probe() ? bwrap_.confine(requested, policy)
                             : landlock_.confine(requested, policy);
    }

private:
    BwrapSandboxProvider    bwrap_;
    LandlockSandboxProvider landlock_;
};

void reset_sandbox_probe_cache_for_test() {
    std::lock_guard<std::mutex> lock(g_bwrap_mutex);
    g_bwrap_cache.reset();
}

SandboxProvider& default_sandbox_provider() {
    static ChainedSandboxProvider provider;
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
