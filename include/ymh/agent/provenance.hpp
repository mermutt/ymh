#pragma once

// Message provenance vocabulary (36 §3.1, 36-D1) and the projection-level
// `SystemMessage` / `ToolResultMessage` (36 §3.3). Pinned by 37 §3-§5.
//
// This header is included by `message.hpp`, which is included by
// `events.hpp`; it must not include `events.hpp`. `payload::ToolOutcome` is
// therefore declared here and consumed by `events.hpp`.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/goal/goal.hpp"
#include "ymh/session/ids.hpp"

namespace ymh {

namespace payload {

// Tool pipeline outcome (01 §4.5). Declared here so `ToolResultMessage` can
// name it without a cycle; `events.hpp` uses it for `payload::ToolResult`.
enum class ToolOutcome : std::uint8_t {
    Ok,
    Error,
    Denied,
    Cancelled,
};

} // namespace payload

// dsh ContextForm (message.d.ts:42-54): a SEMANTIC vocabulary, never visual.
// `None` is the documented default (an absent/unknown value is opaque content).
enum class ContextForm : std::uint8_t {
    None,
    Instructions,
    Catalog,
    Snapshot,
    Notice,
    Relay,
    Recall,
};

[[nodiscard]] inline std::string_view context_form_name(ContextForm form) noexcept {
    switch (form) {
        case ContextForm::None:
            return "none";
        case ContextForm::Instructions:
            return "instructions";
        case ContextForm::Catalog:
            return "catalog";
        case ContextForm::Snapshot:
            return "snapshot";
        case ContextForm::Notice:
            return "notice";
        case ContextForm::Relay:
            return "relay";
        case ContextForm::Recall:
            return "recall";
    }
    return {};
}

[[nodiscard]] inline std::optional<ContextForm> parse_context_form(
    std::string_view name) noexcept {
    if (name == "none") {
        return ContextForm::None;
    }
    if (name == "instructions") {
        return ContextForm::Instructions;
    }
    if (name == "catalog") {
        return ContextForm::Catalog;
    }
    if (name == "snapshot") {
        return ContextForm::Snapshot;
    }
    if (name == "notice") {
        return ContextForm::Notice;
    }
    if (name == "relay") {
        return ContextForm::Relay;
    }
    if (name == "recall") {
        return ContextForm::Recall;
    }
    return std::nullopt;
}

// One named contribution to a Snapshot-form context (dsh ContextSnapshotSection).
struct ContextSnapshotSection {
    std::string name;
    std::string text;

    bool operator==(const ContextSnapshotSection&) const = default;
};

// Producer-declared form plus the fields that form requires. 36-I8 coupling:
// `Snapshot` requires non-empty `sections`; `Notice` requires non-empty
// `summary`; a violation fails at construction.
struct ContextFormed {
    ContextForm                         form = ContextForm::None;
    std::vector<ContextSnapshotSection> sections;   // required iff form == Snapshot
    std::string                         summary;    // required iff form == Notice

    ContextFormed() = default;

    explicit ContextFormed(ContextForm form_value) : form(form_value) { validate(); }

    ContextFormed(ContextForm form_value,
                  std::vector<ContextSnapshotSection> sections_value,
                  std::string                         summary_value = {})
        : form(form_value),
          sections(std::move(sections_value)),
          summary(std::move(summary_value)) {
        validate();
    }

    void validate() const {
        if (form == ContextForm::Snapshot && sections.empty()) {
            throw std::invalid_argument("context form snapshot requires sections");
        }
        if (form == ContextForm::Notice && summary.empty()) {
            throw std::invalid_argument("context form notice requires a summary");
        }
    }

    bool operator==(const ContextFormed&) const = default;
};

// dsh GoalMessageSource (dsh-goal/lib/types/domain.d.ts:33-40). A goal round is
// an ordinary `user/message` whose source carries this ref (44 §5.4).
struct GoalMessageRef {
    GoalId        goal_id  = 0;
    std::uint64_t revision = 0;
    RoundNumber   round    = 0;   // positive admitted round

    bool operator==(const GoalMessageRef&) const = default;
};

// dsh MessageSourceMap (message.d.ts:94-104), flattened. `kind` gates its
// fields (36-I8): `plugin`/`context` only for `Plugin`, `call` only for `Tool`,
// `provider`/`model` only for `Model`, `goal` only for `Goal`.
struct MessageSource {
    enum class Kind : std::uint8_t {
        User,
        Plugin,
        Model,
        Tool,
        Goal,
    };

    Kind                          kind = Kind::User;
    std::string                   plugin;                 // Kind::Plugin
    ContextFormed                 context;                // Kind::Plugin
    std::optional<ToolCallId>     call;                   // Kind::Tool
    std::string                   provider;               // Kind::Model
    std::string                   model;                  // Kind::Model
    std::optional<GoalMessageRef> goal;                   // Kind::Goal
    // 55-A6/55-D12: a `Kind::Plugin` relay's sender SessionId ("" otherwise).
    std::string                   sender{};

    void validate() const {
        const bool has_plugin   = !plugin.empty();
        const bool has_context  = context != ContextFormed{};
        const bool has_call     = call.has_value();
        const bool has_provider = !provider.empty();
        const bool has_model    = !model.empty();
        const bool has_goal     = goal.has_value();
        const bool has_sender   = !sender.empty();
        switch (kind) {
            case Kind::User:
                if (has_plugin || has_context || has_call || has_provider || has_model || has_goal ||
                    has_sender) {
                    throw std::invalid_argument("message source kind user carries gated fields");
                }
                break;
            case Kind::Plugin:
                if (has_call || has_provider || has_model || has_goal) {
                    throw std::invalid_argument("message source kind plugin carries gated fields");
                }
                break;
            case Kind::Model:
                if (has_plugin || has_context || has_call || has_goal || has_sender) {
                    throw std::invalid_argument("message source kind model carries gated fields");
                }
                break;
            case Kind::Tool:
                if (has_plugin || has_context || has_provider || has_model || has_goal ||
                    has_sender) {
                    throw std::invalid_argument("message source kind tool carries gated fields");
                }
                break;
            case Kind::Goal:
                if (has_plugin || has_context || has_call || has_provider || has_model ||
                    has_sender) {
                    throw std::invalid_argument("message source kind goal carries gated fields");
                }
                break;
        }
    }

    bool operator==(const MessageSource&) const = default;
};

[[nodiscard]] inline MessageSource message_source(MessageSource::Kind kind) {
    MessageSource source;
    source.kind = kind;
    return source;
}

[[nodiscard]] inline MessageSource tool_message_source(std::optional<ToolCallId> call) {
    MessageSource source;
    source.kind = MessageSource::Kind::Tool;
    source.call = std::move(call);
    return source;
}

[[nodiscard]] inline MessageSource plugin_message_source(std::string   plugin,
                                                         ContextFormed context) {
    MessageSource source;
    source.kind    = MessageSource::Kind::Plugin;
    source.plugin  = std::move(plugin);
    source.context = std::move(context);
    return source;
}

[[nodiscard]] inline MessageSource model_message_source(std::string provider,
                                                        std::string model) {
    MessageSource source;
    source.kind     = MessageSource::Kind::Model;
    source.provider = std::move(provider);
    source.model    = std::move(model);
    return source;
}

[[nodiscard]] inline MessageSource goal_message_source(GoalMessageRef ref) {
    MessageSource source;
    source.kind = MessageSource::Kind::Goal;
    source.goal = std::move(ref);
    return source;
}

[[nodiscard]] inline std::string_view message_source_kind_name(
    MessageSource::Kind kind) noexcept {
    switch (kind) {
        case MessageSource::Kind::User:
            return "user";
        case MessageSource::Kind::Plugin:
            return "plugin";
        case MessageSource::Kind::Model:
            return "model";
        case MessageSource::Kind::Tool:
            return "tool";
        case MessageSource::Kind::Goal:
            return "goal";
    }
    return {};
}

[[nodiscard]] inline std::optional<MessageSource::Kind> parse_message_source_kind(
    std::string_view name) noexcept {
    if (name == "user") {
        return MessageSource::Kind::User;
    }
    if (name == "plugin") {
        return MessageSource::Kind::Plugin;
    }
    if (name == "model") {
        return MessageSource::Kind::Model;
    }
    if (name == "tool") {
        return MessageSource::Kind::Tool;
    }
    if (name == "goal") {
        return MessageSource::Kind::Goal;
    }
    return std::nullopt;
}

// The prepended system message (projection only; never a durable event).
struct SystemMessage {
    MessageId     id;
    std::string   text;
    std::string   prompt_digest;
    MessageSource source;   // Kind::Plugin, ContextForm::None
};

// A projected tool-role message; `context` may carry a Notice (Wave 4).
struct ToolResultMessage {
    ToolCallId                 call;
    std::string                name;
    payload::ToolOutcome       outcome = payload::ToolOutcome::Ok;
    std::string                output;
    bool                       truncated = false;
    std::optional<std::string> error;
    ContextFormed              context;   // form == Notice iff a notice is present
};

// ---------------------------------------------------------------------------
// JSON (persistence / wire). Unknown enum strings fail loudly (01 S3).
// ---------------------------------------------------------------------------

inline void to_json(nlohmann::json& json, const ContextSnapshotSection& section) {
    json = nlohmann::json{{"name", section.name}, {"text", section.text}};
}

inline void from_json(const nlohmann::json& json, ContextSnapshotSection& section) {
    section.name = json.value("name", std::string{});
    section.text = json.value("text", std::string{});
}

inline void to_json(nlohmann::json& json, const ContextFormed& value) {
    value.validate();
    json = nlohmann::json{{"form", std::string{context_form_name(value.form)}}};
    if (value.form == ContextForm::Snapshot) {
        json["sections"] = value.sections;
    } else if (value.form == ContextForm::Notice) {
        json["summary"] = value.summary;
    }
}

inline void from_json(const nlohmann::json& json, ContextFormed& value) {
    const auto form = parse_context_form(json.at("form").get<std::string>());
    if (!form) {
        throw nlohmann::json::other_error::create(
            501, "unknown context form: " + json.at("form").get<std::string>(), &json);
    }
    ContextFormed decoded;
    decoded.form = *form;
    if (json.contains("sections") && !json.at("sections").is_null()) {
        decoded.sections = json.at("sections").get<std::vector<ContextSnapshotSection>>();
    }
    decoded.summary = json.value("summary", std::string{});
    decoded.validate();
    value = std::move(decoded);
}

inline void to_json(nlohmann::json& json, const GoalMessageRef& value) {
    json = nlohmann::json{
        {"goal_id", value.goal_id},
        {"revision", value.revision},
        {"round", value.round},
    };
}

inline void from_json(const nlohmann::json& json, GoalMessageRef& value) {
    value.goal_id  = json.at("goal_id").get<GoalId>();
    value.revision = json.at("revision").get<std::uint64_t>();
    value.round    = json.at("round").get<RoundNumber>();
}

inline void to_json(nlohmann::json& json, const MessageSource& value) {
    value.validate();
    json = nlohmann::json{{"kind", std::string{message_source_kind_name(value.kind)}}};
    switch (value.kind) {
        case MessageSource::Kind::User:
            break;
        case MessageSource::Kind::Plugin:
            if (!value.plugin.empty()) {
                json["plugin"] = value.plugin;
            }
            if (value.context != ContextFormed{}) {
                json["context"] = value.context;
            }
            if (!value.sender.empty()) {
                json["sender"] = value.sender;
            }
            break;
        case MessageSource::Kind::Model:
            if (!value.provider.empty()) {
                json["provider"] = value.provider;
            }
            if (!value.model.empty()) {
                json["model"] = value.model;
            }
            break;
        case MessageSource::Kind::Tool:
            if (value.call.has_value()) {
                json["call"] = *value.call;
            }
            break;
        case MessageSource::Kind::Goal:
            if (value.goal.has_value()) {
                json["goal"] = *value.goal;
            }
            break;
    }
}

inline void from_json(const nlohmann::json& json, MessageSource& value) {
    const auto kind = parse_message_source_kind(json.at("kind").get<std::string>());
    if (!kind) {
        throw nlohmann::json::other_error::create(
            501, "unknown message source kind: " + json.at("kind").get<std::string>(), &json);
    }
    MessageSource decoded;
    decoded.kind = *kind;
    if (json.contains("plugin")) {
        decoded.plugin = json.at("plugin").get<std::string>();
    }
    if (json.contains("context") && !json.at("context").is_null()) {
        decoded.context = json.at("context").get<ContextFormed>();
    }
    if (json.contains("call") && !json.at("call").is_null()) {
        decoded.call = json.at("call").get<ToolCallId>();
    }
    if (json.contains("provider")) {
        decoded.provider = json.at("provider").get<std::string>();
    }
    if (json.contains("model")) {
        decoded.model = json.at("model").get<std::string>();
    }
    if (json.contains("goal") && !json.at("goal").is_null()) {
        decoded.goal = json.at("goal").get<GoalMessageRef>();
    }
    if (json.contains("sender")) {
        decoded.sender = json.at("sender").get<std::string>();
    }
    decoded.validate();
    value = std::move(decoded);
}

} // namespace ymh
