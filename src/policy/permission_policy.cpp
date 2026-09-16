#include "ymh/policy/permission_policy.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "ymh/execution/glob.hpp"

namespace ymh {
namespace {

using Clock = std::chrono::steady_clock;

int effect_rank(PolicyVerdict verdict) {
    switch (verdict) {
        case PolicyVerdict::Allow: return 0;
        case PolicyVerdict::Ask:   return 1;
        case PolicyVerdict::Deny:  return 2;
    }
    return 2;
}

long long literal_characters(const std::string& pattern) {
    long long count = 0;
    for (char c : pattern) {
        if (c != '*' && c != '?') {
            ++count;
        }
    }
    return count;
}

long long byte_order(const std::string& pattern) {
    long long sum = 0;
    for (unsigned char c : pattern) {
        sum += c;
    }
    return sum;
}

void append_dimension(std::vector<long long>& key, const std::string& pattern) {
    if (pattern.empty()) {
        key.push_back(0);
        key.push_back(0);
        key.push_back(0);
        key.push_back(0);
        return;
    }
    const bool literal = pattern.find_first_of("*?") == std::string::npos;
    key.push_back(literal ? 1 : 0);
    key.push_back(literal_characters(pattern));
    key.push_back(static_cast<long long>(pattern.size()));
    key.push_back(byte_order(pattern));
}

std::vector<long long> specificity_key(const PolicyRule& rule) {
    std::vector<long long> key;
    key.push_back((rule.tool.empty() ? 0 : 1) + (rule.path.empty() ? 0 : 1) +
                  (rule.command.empty() ? 0 : 1));
    append_dimension(key, rule.tool);
    append_dimension(key, rule.path);
    append_dimension(key, rule.command);
    return key;
}

bool rule_greater(const PolicyRule& left, const PolicyRule& right) {
    const std::vector<long long> left_key = specificity_key(left);
    const std::vector<long long> right_key = specificity_key(right);
    if (left_key != right_key) {
        return std::lexicographical_compare(right_key.begin(), right_key.end(),
                                            left_key.begin(), left_key.end());
    }
    if (left.layer != right.layer) {
        return static_cast<int>(left.layer) > static_cast<int>(right.layer);
    }
    return effect_rank(left.effect) > effect_rank(right.effect);
}

void validate_glob(const std::string& pattern) {
    std::size_t open = 0;
    for (char c : pattern) {
        if (c == '[') {
            ++open;
        } else if (c == ']') {
            if (open == 0) {
                throw PolicyConfigError{"malformed glob: " + pattern};
            }
            --open;
        }
    }
    if (open != 0) {
        throw PolicyConfigError{"malformed glob: " + pattern};
    }
}

void validate_rule(const PolicyRule& rule) {
    if (rule.tool.empty() && rule.path.empty() && rule.command.empty()) {
        throw PolicyConfigError{"rule has no match dimensions: " + rule.id};
    }
    validate_glob(rule.tool);
    validate_glob(rule.path);
    validate_glob(rule.command);
}

bool rule_sort_less(const PolicyRule& left, const PolicyRule& right) {
    if (left.layer != right.layer) {
        return static_cast<int>(left.layer) < static_cast<int>(right.layer);
    }
    if (left.tool != right.tool) return left.tool < right.tool;
    if (left.path != right.path) return left.path < right.path;
    if (left.command != right.command) return left.command < right.command;
    return effect_rank(left.effect) < effect_rank(right.effect);
}

std::string path_pattern(const PermissionRequest& request) {
    if (!request.path.has_value()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path relative =
        std::filesystem::relative(*request.path, request.root, ec);
    return ec ? request.path->generic_string() : relative.generic_string();
}

std::string command_pattern(const PermissionRequest& request) {
    if (!request.arguments.is_object()) {
        return {};
    }
    if (request.tool == "shell" && request.arguments.contains("command") &&
        request.arguments["command"].is_string()) {
        return request.arguments["command"].get<std::string>();
    }
    if (request.tool == "terminal") {
        if (request.arguments.contains("command") &&
            request.arguments["command"].is_string()) {
            return request.arguments["command"].get<std::string>();
        }
        if (request.arguments.contains("argv") &&
            request.arguments["argv"].is_array()) {
            std::string joined;
            for (const auto& element : request.arguments["argv"]) {
                if (!element.is_string()) {
                    continue;
                }
                if (!joined.empty()) {
                    joined.push_back(' ');
                }
                joined += element.get<std::string>();
            }
            return joined;
        }
    }
    return {};
}

} // namespace

bool tool_is_mutating(const PermissionRequest& request) noexcept {
    if (request.destructive) {
        return true;
    }
    static constexpr std::string_view kMutating[] = {
        "write_file", "edit_file", "shell", "pty", "terminal", "checkout", "git_checkout",
    };
    for (std::string_view name : kMutating) {
        if (request.tool == name) {
            return true;
        }
    }
    return false;
}

RulePermissionPolicy::RulePermissionPolicy(PermissionConfig config)
    : config_(std::move(config)), generation_{1} {
    for (const PolicyRule& rule : config_.rules) {
        validate_rule(rule);
    }
    reorder();
}

void RulePermissionPolicy::reorder() {
    std::vector<PolicyRule> base = config_.rules;
    base.insert(base.end(), local_grants_.begin(), local_grants_.end());
    std::sort(base.begin(), base.end(), rule_sort_less);

    std::vector<PolicyRule> all = base;
    for (const auto& [session, grants] : session_grants_) {
        (void)session;
        all.insert(all.end(), grants.begin(), grants.end());
    }
    std::sort(all.begin(), all.end(), rule_sort_less);

    base_rules_ = std::move(base);
    ordered_rules_ = std::move(all);
}

PolicyVerdict RulePermissionPolicy::evaluate(const PermissionRequest& request) const {
    if (request.sandbox == SandboxMode::ReadOnly && tool_is_mutating(request)) {
        return PolicyVerdict::Deny;
    }

    std::vector<PolicyRule> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        candidates = base_rules_;
        const auto it = session_grants_.find(request.session.value);
        if (it != session_grants_.end()) {
            candidates.insert(candidates.end(), it->second.begin(), it->second.end());
        }
    }

    const PolicyRule* winner = nullptr;
    for (const PolicyRule& rule : candidates) {
        if (!rule.tool.empty() && !glob_match(rule.tool, request.tool)) {
            continue;
        }
        if (!rule.path.empty()) {
            if (!request.path.has_value() ||
                !path_glob_match(rule.path, path_pattern(request))) {
                continue;
            }
        }
        if (!rule.command.empty() &&
            !glob_match(rule.command, command_pattern(request))) {
            continue;
        }
        if (winner == nullptr || rule_greater(rule, *winner)) {
            winner = &rule;
        }
    }

    if (winner == nullptr) {
        return config_.default_verdict;
    }
    return winner->effect;
}

void RulePermissionPolicy::remember(const PermissionRequest& request,
                                    payload::PermissionDecisionKind kind,
                                    GrantScope scope) {
    if (kind == payload::PermissionDecisionKind::Deny || scope == GrantScope::Once) {
        return;
    }

    PolicyRule grant;
    grant.tool = request.tool;
    grant.path = path_pattern(request);
    grant.command = command_pattern(request);
    grant.effect = PolicyVerdict::Allow;
    grant.id = "grant:" + request.tool;

    std::lock_guard<std::mutex> lock(mutex_);
    if (scope == GrantScope::Always) {
        grant.layer = PolicyRule::Layer::LocalGrant;
        local_grants_.push_back(grant);
    } else {
        grant.layer = PolicyRule::Layer::SessionGrant;
        session_grants_[request.session.value].push_back(grant);
    }
    ++generation_.value;
    reorder();
}

PermissionGate::PermissionGate(PermissionPolicy& policy, PermissionConfig config)
    : policy_(policy), config_(std::move(config)) {}

void PermissionGate::set_attention_hook(AttentionHook hook) {
    attention_hook_ = std::move(hook);
}

void PermissionGate::set_decision_hook(DecisionHook hook) {
    decision_hook_ = std::move(hook);
}

void PermissionGate::emit_decision(const PermissionRequest& request,
                                   const PermissionOutcome& outcome) {
    if (!decision_hook_) {
        return;
    }
    payload::PermissionDecision decision;
    decision.call = request.call;
    decision.decision = outcome.decision;
    decision.reason = outcome.reason;
    decision_hook_(decision);
}

PermissionOutcome PermissionGate::resolve(const PermissionRequest& request,
                                          CancellationToken cancel,
                                          bool subscribers_attached) {
    const PolicyVerdict verdict = policy_.evaluate(request);
    if (verdict == PolicyVerdict::Allow) {
        PermissionOutcome outcome{payload::PermissionDecisionKind::Allow, "rule"};
        emit_decision(request, outcome);
        return outcome;
    }
    if (verdict == PolicyVerdict::Deny) {
        PermissionOutcome outcome{payload::PermissionDecisionKind::Deny, "rule"};
        emit_decision(request, outcome);
        return outcome;
    }

    PermissionRequestId id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id.value = std::to_string(next_id_++);
        Pending pending;
        pending.request = request;
        pending_.emplace(id.value, std::move(pending));
    }

    if (attention_hook_) {
        attention_hook_(id, request);
    }

    const bool bounded =
        config_.permission_timeout.count() > 0 || !subscribers_attached;
    const std::chrono::milliseconds timeout =
        config_.permission_timeout.count() > 0 ? config_.permission_timeout
                                               : std::chrono::minutes{5};
    const Clock::time_point deadline = Clock::now() + timeout;

    PermissionOutcome outcome{payload::PermissionDecisionKind::Deny, "timeout"};
    {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            auto it = pending_.find(id.value);
            if (it == pending_.end()) {
                outcome = {payload::PermissionDecisionKind::Deny, "expired"};
                break;
            }
            if (it->second.resolved) {
                outcome = {it->second.decision, it->second.reason};
                break;
            }
            if (cancel.cancelled()) {
                outcome = {payload::PermissionDecisionKind::Deny, "cancelled"};
                break;
            }
            if (bounded && Clock::now() >= deadline) {
                outcome = {payload::PermissionDecisionKind::Deny, "timeout"};
                break;
            }
            if (bounded) {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - Clock::now());
                const auto slice = std::min(std::chrono::milliseconds{25},
                                            std::max(remaining, std::chrono::milliseconds{0}));
                cv_.wait_for(lock, slice);
            } else {
                cv_.wait_for(lock, std::chrono::milliseconds{25});
            }
        }
        pending_.erase(id.value);
    }

    emit_decision(request, outcome);
    return outcome;
}

bool PermissionGate::decide(const PermissionRequestId& id,
                            payload::PermissionDecisionKind decision,
                            GrantScope scope,
                            std::string reason) {
    PermissionRequest request;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(id.value);
        if (it == pending_.end() || it->second.resolved) {
            return false;
        }
        it->second.resolved = true;
        it->second.decision = decision;
        it->second.reason = std::move(reason);
        request = it->second.request;
    }
    cv_.notify_all();
    policy_.remember(request, decision, scope);
    return true;
}

std::size_t PermissionGate::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

} // namespace ymh
