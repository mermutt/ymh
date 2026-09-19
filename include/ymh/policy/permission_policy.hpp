#pragma once

// Permissions and policy (09 §2-§5). The policy is pure: it classifies a request
// and reads its own rule/grant state, performs no I/O, never blocks, and never
// touches the UI (Q1/Q15). The waiting, timeout, and durable-decision machinery
// lives in `PermissionGate` (the v1, transport-free core of the broker, §4.2),
// which fails closed on timeout/cancel (Q3/Q8).

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/task.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {

// Correlation handle only; minted by the gate, never by a client, never durable.
struct PermissionRequestId {
    std::string value;
    auto operator<=>(const PermissionRequestId&) const = default;
};

struct PolicyGeneration {
    std::uint64_t value = 0;
    auto operator<=>(const PolicyGeneration&) const = default;
};

// §19's `PermissionDecision` sketch reconciled as the transient verdict (09
// decision (a)); the durable kind stays payload::PermissionDecisionKind.
enum class PolicyVerdict : std::uint8_t {
    Allow,
    Ask,
    Deny,
};

// Mirrors protocol::PermissionScope (05 §7.6) 1:1.
enum class GrantScope : std::uint8_t { Once, Session, Always };

struct PermissionRequest {
    ToolCallId             call;
    SessionId              session;
    TurnId                 turn = 0;
    StepId                 step = 0;

    std::string            tool;          // == payload::ToolCall.name
    nlohmann::json         arguments;     // == payload::ToolCall.arguments

    // Supplied by 07, never recomputed here: the canonical workspace root and,
    // when the tool targets a path, the resolved root-relative path.
    std::filesystem::path  root;          // canonical (07 X3)
    std::optional<std::filesystem::path> path;   // resolved, root-relative

    SandboxMode            sandbox = SandboxMode::Workspace;
    bool                   destructive = false;
    // 25-D4: non-mutating forced review. `evaluate` returns Ask before rules,
    // tool_defaults, default_verdict, and grants; `tool_is_mutating` never
    // reads it, so `SandboxMode::ReadOnly` does not pre-rule deny the review.
    bool                   force_ask = false;
};

struct PolicyRule {
    std::string   tool;      // exact name, or a glob ("git_*", "*")
    std::string   path;      // root-relative glob ("src/**", "*.lock")
    std::string   command;   // shell command glob ("git push*")

    PolicyVerdict effect = PolicyVerdict::Ask;

    enum class Layer : std::uint8_t {
        Builtin,
        Global,
        Project,
        Profile,
        CommandLine,
        LocalGrant,
        SessionGrant,
    } layer = Layer::Global;

    std::string   id;        // stable, human-readable rule id for `reason`
};

// Additive (15 §6.1, AM-3). A NO-MATCH fallback for a tool-name prefix: it is
// consulted only when no PolicyRule matches, so an operator rule always
// outranks it. `prefix` is a literal prefix (e.g. "mcp.github.").
struct ToolDefault {
    std::string   prefix;
    PolicyVerdict verdict = PolicyVerdict::Ask;
    std::string   id;        // provenance, e.g. "mcp.github.default"
};

struct PermissionConfig {
    std::chrono::milliseconds permission_timeout{5 * 60 * 1000};
    std::size_t               arguments_max_bytes{64u * 1024u};
    std::vector<PolicyRule>   rules;
    std::vector<ToolDefault>  tool_defaults;
    PolicyVerdict             default_verdict{PolicyVerdict::Ask};
};

class PolicyConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Pure classification (09 §3.1). `evaluate` is total and deterministic;
// `remember` is the only mutation.
class PermissionPolicy {
public:
    virtual ~PermissionPolicy() = default;

    virtual PolicyVerdict evaluate(const PermissionRequest&) const = 0;

    virtual void remember(const PermissionRequest&,
                          payload::PermissionDecisionKind,
                          GrantScope) = 0;

    virtual PolicyGeneration generation() const noexcept = 0;

    virtual const std::vector<PolicyRule>& rules() const noexcept = 0;
};

// The v1 rule-set implementation. Validates its config at construction
// (PolicyConfigError at load, never at execution — Q-F4/Q-F12).
class RulePermissionPolicy final : public PermissionPolicy {
public:
    explicit RulePermissionPolicy(PermissionConfig config);

    PolicyVerdict evaluate(const PermissionRequest& request) const override;
    void          remember(const PermissionRequest& request,
                           payload::PermissionDecisionKind kind,
                           GrantScope scope) override;
    PolicyGeneration generation() const noexcept override { return generation_; }
    const std::vector<PolicyRule>& rules() const noexcept override { return ordered_rules_; }

private:
    void reorder();

    PermissionConfig   config_;
    PolicyGeneration   generation_;
    std::vector<PolicyRule> ordered_rules_;
    std::vector<PolicyRule> base_rules_;

    mutable std::mutex mutex_;
    std::map<std::string, std::vector<PolicyRule>> session_grants_;
    std::vector<PolicyRule>                        local_grants_;
};

// 07 §5.4 / 09 §3.7. By the time a tool runs the decision is durable and
// non-Ask; `evaluate` is the optional sub-operation seam (v1 tools do not use
// it, so the fixed handle returns the recorded kind).
class PermissionHandle {
public:
    virtual ~PermissionHandle() = default;

    virtual payload::PermissionDecisionKind decision() const noexcept = 0;
    virtual Task<payload::PermissionDecisionKind> evaluate(const PermissionRequest&) = 0;
};

class StaticPermissionHandle final : public PermissionHandle {
public:
    explicit StaticPermissionHandle(payload::PermissionDecisionKind decision)
        : decision_(decision) {}

    payload::PermissionDecisionKind decision() const noexcept override { return decision_; }
    Task<payload::PermissionDecisionKind> evaluate(const PermissionRequest&) override {
        return Task<payload::PermissionDecisionKind>(decision_);
    }

private:
    payload::PermissionDecisionKind decision_;
};

struct PermissionOutcome {
    payload::PermissionDecisionKind decision = payload::PermissionDecisionKind::Deny;
    std::string                     reason;
};

// v1, transport-free core of the broker (09 §4.2, §5). It resolves a verdict
// plus (for Ask) a human decision into a terminal outcome, surfacing Ask via an
// attention hook and failing closed on timeout/cancel. The durable
// PermissionDecision is emitted through the decision hook; the loop appends it.
class PermissionGate {
public:
    using AttentionHook = std::function<void(const PermissionRequestId&,
                                             const PermissionRequest&)>;
    using DecisionHook  = std::function<void(const payload::PermissionDecision&)>;

    PermissionGate(PermissionPolicy& policy, PermissionConfig config);

    void set_attention_hook(AttentionHook hook);
    void set_decision_hook(DecisionHook hook);

    // AL-U20 test seam: snapshot the installed decision hook under `hook_mutex_`
    // so a test can invoke the real production capture after its owner is torn
    // down. Not used on any production path.
    [[nodiscard]] DecisionHook decision_hook_for_test() const;

    // Allow/Deny short-circuit; Ask waits up to permission_timeout (0 => wait
    // only when `subscribers_attached`). Never throws; failure is fail-closed.
    PermissionOutcome resolve(const PermissionRequest& request,
                              CancellationToken cancel,
                              bool subscribers_attached = true);

    // Human decision. First wins; returns false for an unknown/expired id.
    bool decide(const PermissionRequestId& id,
                payload::PermissionDecisionKind decision,
                GrantScope scope,
                std::string reason);

    [[nodiscard]] std::size_t pendingCount() const;

private:
    void emit_decision(const PermissionRequest& request,
                       const PermissionOutcome& outcome);

    struct Pending {
        bool                            resolved = false;
        payload::PermissionDecisionKind decision = payload::PermissionDecisionKind::Deny;
        std::string                     reason;
        PermissionRequest               request;
    };

    PermissionPolicy& policy_;
    PermissionConfig  config_;

    // 24-D16/AL34: the hooks are guarded by this leaf mutex (lock order §7.4
    // #8). `set_*_hook` writes under it; `emit_decision` and `resolve` copy the
    // hook under it and invoke the copy outside, so a concurrent clear can
    // never race an in-flight invocation (AL-F24).
    mutable std::mutex hook_mutex_;
    AttentionHook      attention_hook_;
    DecisionHook       decision_hook_;

    mutable std::mutex              mutex_;
    std::condition_variable         cv_;
    std::map<std::string, Pending>  pending_;
    std::uint64_t                   next_id_ = 1;
};

// Hard deny helper (Q7): ReadOnly + a mutating tool is denied before any rule.
[[nodiscard]] bool tool_is_mutating(const PermissionRequest& request) noexcept;

} // namespace ymh
