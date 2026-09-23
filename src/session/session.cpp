#include "ymh/session/session.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ymh {
namespace {

constexpr std::string_view kHexDigits = "0123456789abcdef";

bool is_hex_digit(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_uuid_v4(std::string_view value) noexcept {
    if (value.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
        if (hyphen) {
            if (value[index] != '-') {
                return false;
            }
        } else if (!is_hex_digit(value[index])) {
            return false;
        }
    }
    if (value[14] != '4') {
        return false;
    }
    const char variant = value[19];
    return variant == '8' || variant == '9' || variant == 'a' || variant == 'A' || variant == 'b' ||
           variant == 'B';
}

std::string uuid_v4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint32_t> dist{0, 0xFFu};

    std::array<unsigned char, 16> bytes{};
    for (unsigned char& byte : bytes) {
        byte = static_cast<unsigned char>(dist(rng));
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3Fu) | 0x80u);

    std::string out;
    out.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            out.push_back('-');
        }
        out.push_back(kHexDigits[bytes[index] >> 4]);
        out.push_back(kHexDigits[bytes[index] & 0x0Fu]);
    }
    return out;
}

std::int64_t epoch_ms(std::chrono::system_clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count();
}

bool is_trim_byte(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\f' || value == '\v';
}

// RFC 3629 acceptance: well-formed sequences only, rejecting overlongs,
// surrogates, > U+10FFFF, truncated sequences, and stray continuations (RN8).
bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        if (lead <= 0x7F) {
            ++index;
            continue;
        }
        std::size_t     continuation_count = 0;
        unsigned char   first_lower        = 0x80;
        unsigned char   first_upper        = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            continuation_count = 1;
        } else if (lead == 0xE0) {
            continuation_count = 2;
            first_lower        = 0xA0;
        } else if (lead >= 0xE1 && lead <= 0xEC) {
            continuation_count = 2;
        } else if (lead == 0xED) {
            continuation_count = 2;
            first_upper        = 0x9F;
        } else if (lead >= 0xEE && lead <= 0xEF) {
            continuation_count = 2;
        } else if (lead == 0xF0) {
            continuation_count = 3;
            first_lower        = 0x90;
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            continuation_count = 3;
        } else if (lead == 0xF4) {
            continuation_count = 3;
            first_upper        = 0x8F;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) {
            return false;
        }
        const unsigned char first = static_cast<unsigned char>(text[index + 1]);
        if (first < first_lower || first > first_upper) {
            return false;
        }
        for (std::size_t offset = 2; offset <= continuation_count; ++offset) {
            const unsigned char byte = static_cast<unsigned char>(text[index + offset]);
            if (byte < 0x80 || byte > 0xBF) {
                return false;
            }
        }
        index += continuation_count + 1;
    }
    return true;
}

Message make_text_message(Role role, std::string text) {
    Message message;
    message.role = role;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

Message make_synthetic_tool_message(const ToolCallId& id, payload::ToolOutcome outcome) {
    Message message;
    message.role         = Role::Tool;
    message.tool_call_id = id;
    message.source       = tool_message_source(id);
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = outcome == payload::ToolOutcome::Cancelled ? "tool call cancelled"
                                                            : "tool call failed";
    message.content.push_back(std::move(block));
    return message;
}

void erase_tool_id(std::vector<ToolCallId>& pending, const ToolCallId& id) {
    pending.erase(std::remove(pending.begin(), pending.end(), id), pending.end());
}

bool contains_tool_id(const std::vector<ToolCallId>& pending, const ToolCallId& id) {
    return std::find(pending.begin(), pending.end(), id) != pending.end();
}

} // namespace

std::string_view session_kind_name(SessionKind kind) noexcept {
    switch (kind) {
        case SessionKind::Root:
            return "root";
        case SessionKind::Fork:
            return "fork";
        case SessionKind::Subagent:
            return "subagent";
    }
    return {};
}

std::optional<SessionKind> parse_session_kind(std::string_view name) noexcept {
    if (name == "root") {
        return SessionKind::Root;
    }
    if (name == "fork") {
        return SessionKind::Fork;
    }
    if (name == "subagent") {
        return SessionKind::Subagent;
    }
    return std::nullopt;
}

bool is_placeholder_title(std::string_view title) noexcept {
    return title.empty() || title == "tui" || title == "main";
}

std::string normalize_title(std::string_view title) {
    std::size_t begin = 0;
    std::size_t end   = title.size();
    while (begin < end && is_trim_byte(title[begin])) {
        ++begin;
    }
    while (end > begin && is_trim_byte(title[end - 1])) {
        --end;
    }
    std::string normalized{title.substr(begin, end - begin)};
    if (normalized.empty()) {
        throw std::invalid_argument("session title must not be empty");
    }
    for (const char value : normalized) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (byte < 0x20 || byte == 0x7F) {
            throw std::invalid_argument("session title must not contain control characters");
        }
    }
    if (!is_valid_utf8(normalized)) {
        throw std::invalid_argument("session title must be valid UTF-8");
    }
    if (normalized.size() > kMaxSessionTitleBytes) {
        throw std::invalid_argument("session title exceeds the maximum byte length");
    }
    return normalized;
}

std::optional<std::string> derive_auto_title(std::string_view prompt) {
    const std::size_t newline = prompt.find('\n');
    const std::string_view line =
        newline == std::string_view::npos ? prompt : prompt.substr(0, newline);

    std::size_t begin = 0;
    std::size_t end   = line.size();
    while (begin < end && is_trim_byte(line[begin])) {
        ++begin;
    }
    while (end > begin && is_trim_byte(line[end - 1])) {
        --end;
    }

    std::string collapsed;
    bool        pending_space = false;
    for (std::size_t index = begin; index < end; ++index) {
        if (is_trim_byte(line[index])) {
            pending_space = true;
            continue;
        }
        if (pending_space && !collapsed.empty()) {
            collapsed.push_back(' ');
        }
        pending_space = false;
        collapsed.push_back(line[index]);
    }
    if (collapsed.empty()) {
        return std::nullopt;
    }
    for (const char value : collapsed) {
        const unsigned char byte = static_cast<unsigned char>(value);
        if (byte < 0x20 || byte == 0x7F) {
            return std::nullopt;
        }
    }
    if (!is_valid_utf8(collapsed)) {
        return std::nullopt;
    }
    if (collapsed.size() > kAutoTitleBytes) {
        std::size_t cut = kAutoTitleBytes;
        while (cut > 0 && (static_cast<unsigned char>(collapsed[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        collapsed.resize(cut);
        collapsed += "...";
    }
    return collapsed;
}

void to_json(nlohmann::json& json, const SessionHeader& header) {
    json = nlohmann::json{
        {"id", header.id.value},
        {"cwd", header.cwd.string()},
        {"created_at", header.createdAt},
        {"updated_at", header.updatedAt},
        {"title", header.title},
        {"model", header.model},
        {"server_profile", header.serverProfile},
        {"kind", std::string{session_kind_name(header.kind)}},
        {"parent_session", header.parentSession ? nlohmann::json(header.parentSession->value)
                                                : nlohmann::json(nullptr)},
        {"seed_length", header.seedLength ? nlohmann::json(*header.seedLength)
                                          : nlohmann::json(nullptr)},
        {"metadata", header.metadata ? nlohmann::json(*header.metadata) : nlohmann::json(nullptr)},
        {"agent_preset", header.agent_preset ? nlohmann::json(*header.agent_preset)
                                             : nlohmann::json(nullptr)},
        {"permission_preset", header.permission_preset ? nlohmann::json(*header.permission_preset)
                                                       : nlohmann::json(nullptr)},
        {"depth", header.depth},
    };
}

void from_json(const nlohmann::json& json, SessionHeader& header) {
    header.id.value      = json.at("id").get<std::string>();
    header.cwd           = std::filesystem::path{json.at("cwd").get<std::string>()};
    header.createdAt     = json.at("created_at").get<std::int64_t>();
    header.updatedAt     = json.at("updated_at").get<std::int64_t>();
    header.title         = json.value("title", std::string{});
    header.model         = json.value("model", std::string{});
    header.serverProfile = json.value("server_profile", std::string{});
    header.kind          = parse_session_kind(json.at("kind").get<std::string>())
                               .value_or(SessionKind::Root);
    if (json.contains("parent_session") && !json.at("parent_session").is_null()) {
        header.parentSession = SessionId{json.at("parent_session").get<std::string>()};
    } else {
        header.parentSession = std::nullopt;
    }
    if (json.contains("seed_length") && !json.at("seed_length").is_null()) {
        header.seedLength = json.at("seed_length").get<std::size_t>();
    } else {
        header.seedLength = std::nullopt;
    }
    if (json.contains("metadata") && !json.at("metadata").is_null()) {
        header.metadata = json.at("metadata").get<std::string>();
    } else {
        header.metadata = std::nullopt;
    }
    if (json.contains("agent_preset") && !json.at("agent_preset").is_null()) {
        header.agent_preset = json.at("agent_preset").get<std::string>();
    } else {
        header.agent_preset = std::nullopt;
    }
    if (json.contains("permission_preset") && !json.at("permission_preset").is_null()) {
        header.permission_preset = json.at("permission_preset").get<std::string>();
    } else {
        header.permission_preset = std::nullopt;
    }
    header.depth = json.value("depth", std::uint32_t{0});
}

void validateHeader(const SessionHeader& header) {
    if (!is_uuid_v4(header.id.value)) {
        throw std::invalid_argument("SessionId is not a UUIDv4: " + header.id.value);
    }
    if (header.cwd.empty() || !header.cwd.is_absolute()) {
        throw std::invalid_argument("SessionHeader.cwd must be an absolute path");
    }

    switch (header.kind) {
        case SessionKind::Root:
            if (header.parentSession.has_value() || header.seedLength.has_value()) {
                throw std::invalid_argument("root session must have no parent and no seed");
            }
            break;
        case SessionKind::Fork:
            if (!header.parentSession.has_value() || !header.seedLength.has_value()) {
                throw std::invalid_argument("fork session requires a parent and a seed length");
            }
            break;
        case SessionKind::Subagent:
            if (!header.parentSession.has_value()) {
                throw std::invalid_argument("subagent session requires a parent");
            }
            if (header.seedLength.has_value() && *header.seedLength != 0) {
                throw std::invalid_argument("subagent seed length must be absent or zero");
            }
            break;
    }
}

void validateWorkspaceRoot(const std::filesystem::path& cwd) {
    std::error_code error;
    const bool exists = std::filesystem::exists(cwd, error);
    if (error || !exists || !std::filesystem::is_directory(cwd, error)) {
        throw std::invalid_argument("workspace root does not exist or is not a directory: " +
                                    cwd.string());
    }
}

void validateLog(const EventRange& events) {
    Sequence previous = 0;
    for (const EventRecord& record : events) {
        if (record.seq <= previous) {
            throw CorruptionError("event log is not ascending by Sequence");
        }
        previous = record.seq;
    }
}

std::vector<Message> deriveMessages([[maybe_unused]] const SessionHeader& header,
                                    const EventRange& events) {
    std::vector<Message> messages;
    std::vector<Sequence> origins;
    std::vector<ToolCallId> pending;

    auto push = [&](Message message, Sequence origin) {
        messages.push_back(std::move(message));
        origins.push_back(origin);
    };

    for (const EventRecord& record : events) {
        const Event& event = record.event;
        switch (event.type) {
            case EventType::SessionStarted:
            case EventType::SessionEnded:
            case EventType::SessionRenamed:
            case EventType::PlanMode:
            case EventType::SessionModelChanged:
            case EventType::LlmRequestHeader:
            case EventType::AgentPresetSelected:
            case EventType::GoalChange:
            case EventType::CommandRun:
            case EventType::CommandDone:
            case EventType::JobChanged:
            case EventType::McpServerStatusChanged:
            case EventType::ContextPrune:
                break;
            case EventType::TurnStarted: {
                const auto& value = event.payload.get<payload::TurnStarted>();
                (void)value;
                pending.clear();
                break;
            }
            case EventType::StepStarted:
            case EventType::StepEnded:
                break;
            case EventType::TurnEnded:
                pending.clear();
                break;
            case EventType::TurnCancelled: {
                const auto& value = event.payload.get<payload::TurnCancelled>();
                (void)value;
                for (const ToolCallId& id : pending) {
                    push(make_synthetic_tool_message(id, payload::ToolOutcome::Cancelled),
                         record.seq);
                }
                pending.clear();
                break;
            }
            case EventType::TurnFailed: {
                const auto& value = event.payload.get<payload::TurnFailed>();
                (void)value;
                for (const ToolCallId& id : pending) {
                    push(make_synthetic_tool_message(id, payload::ToolOutcome::Error), record.seq);
                }
                pending.clear();
                break;
            }
            case EventType::UserMessage: {
                const auto& value = event.payload.get<payload::UserMessage>();
                Message message;
                message.role    = Role::User;
                message.content = value.content;
                message.source  = value.source;
                push(std::move(message), record.seq);
                break;
            }
            case EventType::AssistantChunk:
                break;
            case EventType::AssistantAttempt:
                break;
            case EventType::AssistantMessage: {
                const auto& value = event.payload.get<payload::AssistantMessage>();
                Message message;
                message.role    = Role::Assistant;
                message.content = value.content;
                message.source  = value.source;
                push(std::move(message), record.seq);
                for (const ContentBlock& block : value.content) {
                    if (block.kind == ContentBlockKind::ToolUse && !block.tool_call_id.empty() &&
                        !contains_tool_id(pending, block.tool_call_id)) {
                        pending.push_back(block.tool_call_id);
                    }
                }
                break;
            }
            case EventType::ToolCall: {
                const auto& value = event.payload.get<payload::ToolCall>();
                if (!value.id.empty() && !contains_tool_id(pending, value.id)) {
                    pending.push_back(value.id);
                }
                break;
            }
            case EventType::ToolResult: {
                const auto& value = event.payload.get<payload::ToolResult>();
                if (value.source.call.has_value() && *value.source.call != value.id) {
                    throw CorruptionError("tool result provenance call does not match its id");
                }
                erase_tool_id(pending, value.id);
                Message message;
                message.role         = Role::Tool;
                message.tool_call_id = value.id;
                message.source       = value.source;
                message.context      = value.context;
                ContentBlock block;
                block.kind = ContentBlockKind::Text;
                block.text = value.output;
                message.content.push_back(std::move(block));
                // 32 C26: a same-`id` replacement (the pruner's) replaces the
                // projected Tool message in place, so exactly one is counted.
                bool replaced = false;
                for (std::size_t index = 0; index < messages.size(); ++index) {
                    if (messages[index].role == Role::Tool &&
                        messages[index].tool_call_id == value.id) {
                        messages[index] = std::move(message);
                        origins[index]  = record.seq;
                        replaced        = true;
                        break;
                    }
                }
                if (!replaced) {
                    push(std::move(message), record.seq);
                }
                break;
            }
            case EventType::PermissionDecision:
                break;
            case EventType::ContextInjected: {
                const auto& value = event.payload.get<payload::ContextInjected>();
                Message message = make_text_message(value.role, value.text);
                message.source  = value.source;
                message.context = value.context;
                push(std::move(message), record.seq);
                break;
            }
            case EventType::ContextCompaction: {
                const auto& value = event.payload.get<payload::ContextCompaction>();
                std::vector<Message> kept;
                std::vector<Sequence> keptOrigins;
                for (std::size_t index = 0; index < messages.size(); ++index) {
                    if (origins[index] > value.boundary) {
                        kept.push_back(std::move(messages[index]));
                        keptOrigins.push_back(origins[index]);
                    }
                }
                messages = std::move(kept);
                origins  = std::move(keptOrigins);
                messages.insert(messages.begin(), make_text_message(Role::System, value.summary));
                origins.insert(origins.begin(), record.seq);
                break;
            }
            case EventType::TokenUsage:
            case EventType::SubagentSpawned:
            case EventType::SubagentFanIn:
                break;
        }
    }

    return messages;
}

SessionId make_session_id() {
    return SessionId{uuid_v4()};
}

EventId make_event_id() {
    return EventId{uuid_v4()};
}

Session::Session(SessionHeader header, SessionStore& store, EventBus& bus)
    : header_(std::move(header)), store_(&store), bus_(&bus) {
    validateHeader(header_);
    reload();
}

Session::Session(Session&& other) noexcept
    : header_(std::move(other.header_)),
      store_(other.store_),
      bus_(other.bus_),
      log_(std::move(other.log_)),
      nextTurn_(other.nextTurn_),
      nextStep_(other.nextStep_) {}

void Session::reload() {
    log_ = store_->read(header_.id);
    validateLog(log_);

    // 19 §5.2: the own log is authoritative for the title. A fork's resolved
    // view may carry a parent rename in the seed window; ownEvents() excludes it.
    for (const EventRecord& record : ownEvents()) {
        if (record.event.type == EventType::SessionRenamed) {
            header_.title = record.event.payload.get<payload::SessionRenamed>().title;
        } else if (record.event.type == EventType::SessionModelChanged) {
            header_.model = record.event.payload.get<payload::SessionModelChanged>().model;
        }
    }

    TurnId maxTurn = 0;
    StepId maxStep = 0;
    for (const EventRecord& record : log_) {
        if (record.event.type == EventType::TurnStarted) {
            maxTurn = std::max(maxTurn, record.event.payload.get<payload::TurnStarted>().turn);
        } else if (record.event.type == EventType::StepStarted) {
            maxStep = std::max(maxStep, record.event.payload.get<payload::StepStarted>().step);
        }
    }
    nextTurn_ = maxTurn + 1;
    nextStep_ = maxStep + 1;
}

EventRange Session::events() const {
    std::lock_guard<std::mutex> lock(appendMutex_);
    return log_;
}

EventRange Session::ownEvents() const {
    std::lock_guard<std::mutex> lock(appendMutex_);
    if (header_.kind == SessionKind::Fork && header_.seedLength.has_value()) {
        const std::size_t prefix = *header_.seedLength;
        if (prefix >= log_.size()) {
            return {};
        }
        return EventRange{log_.begin() + static_cast<std::ptrdiff_t>(prefix), log_.end()};
    }
    return log_;
}

std::vector<Message> Session::deriveMessages() const {
    std::lock_guard<std::mutex> lock(appendMutex_);
    return ymh::deriveMessages(header_, log_);
}

Sequence Session::appendEvent(Event event) {
    std::lock_guard<std::mutex> lock(appendMutex_);
    return appendEventLocked(std::move(event));
}

Sequence Session::appendEventLocked(Event event) {
    if (event.session_id.value != header_.id.value) {
        throw std::invalid_argument("appendEvent: event does not belong to this session");
    }
    if (!store_->isLeaseHolder(header_.id)) {
        throw LeaseLost("not the lease holder for session " + header_.id.value);
    }

    const Sequence seq = store_->append(header_.id, event);
    header_.updatedAt  = epoch_ms(event.timestamp);
    // 19 §5.2: mirror updatedAt materialization for the title and the model.
    if (event.type == EventType::SessionRenamed) {
        header_.title = event.payload.get<payload::SessionRenamed>().title;
    } else if (event.type == EventType::SessionModelChanged) {
        header_.model = event.payload.get<payload::SessionModelChanged>().model;
    }
    log_.push_back(EventRecord{seq, event});
    if (event.type == EventType::TurnStarted) {
        nextTurn_ = std::max(nextTurn_, event.payload.get<payload::TurnStarted>().turn + 1);
    } else if (event.type == EventType::StepStarted) {
        nextStep_ = std::max(nextStep_, event.payload.get<payload::StepStarted>().step + 1);
    }
    bus_->publishCommitted(EventRecord{seq, event});
    return seq;
}

std::optional<Sequence> Session::appendAutoRename(std::string_view firstUserText) {
    std::lock_guard<std::mutex> lock(appendMutex_);

    const std::size_t prefix =
        (header_.kind == SessionKind::Fork && header_.seedLength.has_value())
            ? *header_.seedLength
            : 0;
    const std::size_t first = std::min(prefix, log_.size());
    for (std::size_t index = first; index < log_.size(); ++index) {
        if (log_[index].event.type == EventType::SessionRenamed) {
            return std::nullopt;
        }
    }
    if (!is_placeholder_title(header_.title)) {
        return std::nullopt;
    }
    const std::optional<std::string> derived = derive_auto_title(firstUserText);
    if (!derived.has_value()) {
        return std::nullopt;
    }

    TypedEvent<payload::SessionRenamed> typed;
    typed.id         = make_event_id();
    typed.session_id = id();
    typed.timestamp  = std::chrono::system_clock::now();
    typed.payload    = payload::SessionRenamed{*derived, payload::RenameOrigin::Auto};
    return appendEventLocked(encode(typed));
}

std::vector<Sequence> Session::appendBatch(std::span<const Event> events) {
    std::lock_guard<std::mutex> lock(appendMutex_);

    for (const Event& event : events) {
        if (event.session_id.value != header_.id.value) {
            throw std::invalid_argument("appendBatch: event does not belong to this session");
        }
    }
    if (!store_->isLeaseHolder(header_.id)) {
        throw LeaseLost("not the lease holder for session " + header_.id.value);
    }

    std::vector<Sequence> sequences = store_->appendBatch(header_.id, events);
    for (std::size_t index = 0; index < events.size(); ++index) {
        const Event& event = events[index];
        header_.updatedAt  = epoch_ms(event.timestamp);
        // 19 §5.2: a batched rename mirrors the last such event's title/model.
        if (event.type == EventType::SessionRenamed) {
            header_.title = event.payload.get<payload::SessionRenamed>().title;
        } else if (event.type == EventType::SessionModelChanged) {
            header_.model = event.payload.get<payload::SessionModelChanged>().model;
        }
        log_.push_back(EventRecord{sequences[index], event});
        if (event.type == EventType::TurnStarted) {
            nextTurn_ = std::max(nextTurn_, event.payload.get<payload::TurnStarted>().turn + 1);
        } else if (event.type == EventType::StepStarted) {
            nextStep_ = std::max(nextStep_, event.payload.get<payload::StepStarted>().step + 1);
        }
    }
    for (std::size_t index = 0; index < events.size(); ++index) {
        bus_->publishCommitted(EventRecord{sequences[index], events[index]});
    }
    return sequences;
}

void Session::emit(Event event) {
    if (event.session_id.value.empty()) {
        event.session_id = header_.id;
    }
    if (event.timestamp == std::chrono::system_clock::time_point{}) {
        event.timestamp = std::chrono::system_clock::now();
    }
    bus_->publish(std::move(event));
}

SessionSnapshot Session::snapshot() const {
    std::lock_guard<std::mutex> lock(appendMutex_);
    SessionSnapshot snapshot;
    snapshot.session    = header_.id;
    snapshot.at         = log_.empty() ? 0 : log_.back().seq;
    snapshot.header     = header_;
    snapshot.messages   = ymh::deriveMessages(header_, log_);
    snapshot.eventCount = log_.size();
    return snapshot;
}

Session Session::resume(const SessionHeader& header, SessionStore& store, EventBus& bus) {
    return Session(header, store, bus);
}

Session Session::replay(const SessionHeader& header, SessionStore& store, EventBus& bus) {
    return Session(header, store, bus);
}

Session Session::fork(const Session& parent, std::size_t seedLength, SessionStore& store,
                      EventBus& bus) {
    const EventRange parentView = parent.events();
    if (seedLength > parentView.size()) {
        throw InvalidForkBoundary("fork seed length exceeds the parent's resolved view");
    }

    const std::int64_t now = epoch_ms(std::chrono::system_clock::now());

    SessionHeader child;
    child.id            = make_session_id();
    child.cwd           = parent.header().cwd;
    child.createdAt     = now;
    child.updatedAt     = now;
    child.title         = "";
    child.model         = parent.header().model;
    child.serverProfile = parent.header().serverProfile;
    child.kind          = SessionKind::Fork;
    child.parentSession = parent.id();
    child.seedLength    = seedLength;
    child.depth         = parent.header().depth;
    child.permission_preset = parent.header().permission_preset;
    validateHeader(child);

    store.create(child);

    Session childSession(child, store, bus);
    childSession.append(payload::SessionStarted{
        .model         = child.model,
        .serverProfile = child.serverProfile,
        .title         = child.title,
    });
    return childSession;
}

SessionHeader Session::header() const {
    std::lock_guard<std::mutex> lock(appendMutex_);
    return header_;
}

} // namespace ymh
