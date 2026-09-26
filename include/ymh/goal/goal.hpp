#pragma once

// The durable goal value types, pinned by 44-goals-jobs-commands.md §5.1
// (26 §4.3.8). Goal state is event-sourced: the durable source of truth is the
// `goal/change` event; these are the value types the pure projection folds into
// `GoalProjection`/`GoalProjectionState`. No UI type appears here (44-I15).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace ymh {

// 42 §3.1 declares `GoalId` as a shared type only; this spec owns the real
// type (44 §5.1). It is the same alias.
using GoalId      = std::uint64_t;
using RoundNumber = std::uint32_t;

// 26 §4.3.8. Compare-and-set identity for one exact revision (44-I2).
struct GoalRef {
    GoalId        id       = 0;
    std::uint64_t revision = 0;

    bool operator==(const GoalRef&) const = default;
};

// dsh GoalBlockReason (dsh-goal/lib/types/types.d.ts:40-45).
struct GoalBlockReason {
    std::string code;     // lower-kebab-case, validated
    std::string message;  // non-empty, trimmed

    bool operator==(const GoalBlockReason&) const = default;
};

// 26 §4.3.8; dsh types.d.ts:38.
enum class GoalPhase : std::uint8_t { Active, Paused, Blocked, Complete };

// dsh types.d.ts:58. Process-local; never persisted (44-I6).
enum class GoalActivation : std::uint8_t { Armed, Disarmed };

// 26 §4.3.8. The full durable state written by a non-clear change.
struct GoalSnapshot : GoalRef {
    std::string                    objective;
    GoalPhase                      phase           = GoalPhase::Active;
    std::optional<GoalBlockReason> blocked;         // present iff phase == Blocked
    std::uint32_t                  max_goal_rounds = 0;

    bool operator==(const GoalSnapshot&) const = default;
};

// The replay fold (dsh types.d.ts:90-99): snapshot + admitted rounds + the two
// envelope-derived timestamps. `created_at`/`updated_at` come from the
// `goal/change` envelope timestamp (the payload carries no timestamp; 44 §6.2).
struct GoalProjection {
    GoalSnapshot goal;
    RoundNumber  rounds_started = 0;
    std::int64_t created_at     = 0;
    std::int64_t updated_at     = 0;

    bool operator==(const GoalProjection&) const = default;
};

// dsh types.d.ts:74-83. The live view adds process-local activation.
struct GoalView : GoalProjection {
    GoalActivation activation = GoalActivation::Disarmed;

    bool operator==(const GoalView&) const = default;
};

// The projection state (dsh types.d.ts:100-108). `failure` is the first strict
// replay error; host access rejects that state (44 §2.4).
struct GoalProjectionState {
    std::optional<GoalProjection> current;
    std::vector<GoalId>           seen_goal_ids;   // rejects id reuse
    std::optional<std::string>    failure;

    bool operator==(const GoalProjectionState&) const = default;
};

struct CreateGoalRequest {
    std::string                    objective;
    std::optional<std::uint32_t>   max_goal_rounds;
};

struct EditGoalRequest {
    std::optional<std::string>   objective;
    std::optional<std::uint32_t> max_goal_rounds;
};

// ---------------------------------------------------------------------------
// JSON (persistence / wire). Unknown enum strings fail loudly (01 S3).
// ---------------------------------------------------------------------------

[[nodiscard]] inline std::string_view goal_phase_name(GoalPhase phase) noexcept {
    switch (phase) {
        case GoalPhase::Active:
            return "active";
        case GoalPhase::Paused:
            return "paused";
        case GoalPhase::Blocked:
            return "blocked";
        case GoalPhase::Complete:
            return "complete";
    }
    return {};
}

[[nodiscard]] inline std::optional<GoalPhase> parse_goal_phase(std::string_view name) noexcept {
    if (name == "active") {
        return GoalPhase::Active;
    }
    if (name == "paused") {
        return GoalPhase::Paused;
    }
    if (name == "blocked") {
        return GoalPhase::Blocked;
    }
    if (name == "complete") {
        return GoalPhase::Complete;
    }
    return std::nullopt;
}

inline void to_json(nlohmann::json& json, const GoalRef& value) {
    json = nlohmann::json{{"id", value.id}, {"revision", value.revision}};
}

inline void from_json(const nlohmann::json& json, GoalRef& value) {
    value.id       = json.at("id").get<GoalId>();
    value.revision = json.at("revision").get<std::uint64_t>();
}

inline void to_json(nlohmann::json& json, const GoalBlockReason& value) {
    json = nlohmann::json{{"code", value.code}, {"message", value.message}};
}

inline void from_json(const nlohmann::json& json, GoalBlockReason& value) {
    value.code    = json.at("code").get<std::string>();
    value.message = json.at("message").get<std::string>();
}

inline void to_json(nlohmann::json& json, const GoalSnapshot& value) {
    json = nlohmann::json{
        {"id", value.id},
        {"revision", value.revision},
        {"objective", value.objective},
        {"phase", std::string{goal_phase_name(value.phase)}},
        {"max_goal_rounds", value.max_goal_rounds},
    };
    if (value.blocked.has_value()) {
        json["blocked"] = *value.blocked;
    }
}

inline void from_json(const nlohmann::json& json, GoalSnapshot& value) {
    value.id              = json.at("id").get<GoalId>();
    value.revision        = json.at("revision").get<std::uint64_t>();
    value.objective       = json.at("objective").get<std::string>();
    const auto phase      = parse_goal_phase(json.at("phase").get<std::string>());
    if (!phase.has_value()) {
        throw nlohmann::json::other_error::create(
            501, "unknown goal phase: " + json.at("phase").get<std::string>(), &json);
    }
    value.phase           = *phase;
    value.max_goal_rounds = json.at("max_goal_rounds").get<std::uint32_t>();
    if (json.contains("blocked") && !json.at("blocked").is_null()) {
        value.blocked = json.at("blocked").get<GoalBlockReason>();
    } else {
        value.blocked = std::nullopt;
    }
}

} // namespace ymh
