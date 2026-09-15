#pragma once

// Session & event log (component 01). Pinned by docs/design/01-session.md §3,
// §6, §7, §8: `SessionHeader`, `SessionKind`, `SessionStore`, `Session`,
// `deriveMessages`, and `SessionSnapshot`. No UI/agent-loop type appears here.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/message.hpp"
#include "ymh/core/event.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/session/errors.hpp"
#include "ymh/session/event_store.hpp"
#include "ymh/session/events.hpp"

namespace ymh {

// 01 §2.2; mirrors the SQL CHECK (kind IN ('root','fork','subagent')).
enum class SessionKind : std::uint8_t {
    Root,
    Fork,
    Subagent,
};

[[nodiscard]] std::string_view session_kind_name(SessionKind kind) noexcept;
[[nodiscard]] std::optional<SessionKind> parse_session_kind(std::string_view name) noexcept;

// 01 §3 (final shape fixed by §9.2/§9.10). Deliberately carries no boot nonce,
// no ordinal, no archived.
struct SessionHeader {
    SessionId                  id;
    std::filesystem::path      cwd;            // canonical workspace root; immutable
    std::int64_t               createdAt = 0;  // epoch ms
    std::int64_t               updatedAt = 0;  // epoch ms of last appended event
    std::string                title;
    std::string                model;
    std::string                serverProfile;
    SessionKind                kind = SessionKind::Root;
    std::optional<SessionId>   parentSession;
    std::optional<std::size_t> seedLength;
    std::optional<std::string> metadata;

    bool operator==(const SessionHeader&) const = default;
};

void to_json(nlohmann::json& json, const SessionHeader& header);
void from_json(const nlohmann::json& json, SessionHeader& header);

// 01 I9: the kind/parent/seed matrix plus a UUIDv4 id and an absolute cwd.
// Throws std::invalid_argument (S9) on any violation.
void validateHeader(const SessionHeader& header);

// 01 S14: the workspace root must exist and be a directory. Throws
// std::invalid_argument. Never chdir()s and never creates the directory.
void validateWorkspaceRoot(const std::filesystem::path& cwd);

// 01 I7/S2: records must be ascending by Sequence. Throws CorruptionError.
void validateLog(const EventRange& events);

// 01 S5: fork boundary must be within the parent's resolved view.
class InvalidForkBoundary : public StoreError {
public:
    using StoreError::StoreError;
};

// 01 S6: load/resume of an id the store does not know.
class UnknownSession : public StoreError {
public:
    using StoreError::StoreError;
};

// 01 §6.4 / I21.
struct SessionSnapshot {
    SessionId            session;
    Sequence             at = 0;
    SessionHeader        header;
    std::vector<Message> messages;
    std::size_t          eventCount = 0;
};

// 11-m2-errata §5.2 (D15): sentinel for an unbounded `readAfter`.
inline constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();

// 01 §7: the persistence seam. Spec 02 owns the SQLite implementation.
class SessionStore {
public:
    virtual ~SessionStore() = default;

    virtual SessionHeader                create(SessionHeader header) = 0;
    virtual std::optional<SessionHeader> load(SessionId id) const = 0;
    virtual std::vector<SessionHeader>   list() const = 0;
    virtual void                         erase(SessionId id) = 0;

    virtual EventRange read(SessionId id, Sequence after = 0) const = 0;
    virtual EventRange readRange(SessionId id, Sequence from, Sequence to) const = 0;

    // Persists and returns the store-assigned Sequence. Throws LeaseLost when
    // this process does not hold the session's lease (01 I5, §9.7).
    virtual Sequence append(SessionId id, Event event) = 0;

    // Persists N events as one transaction and returns their sequences in
    // input order (02 §4.4). The default materialisation loops `append`; a
    // durable store overrides it with a real single-transaction batch.
    virtual std::vector<Sequence> appendBatch(SessionId id, std::span<const Event> events) {
        std::vector<Sequence> sequences;
        sequences.reserve(events.size());
        for (const Event& event : events) {
            sequences.push_back(append(id, event));
        }
        return sequences;
    }

    virtual bool isLeaseHolder(SessionId id) const = 0;

    // 11-m2-errata §5.2 (D15): highest committed Sequence in the session's
    // resolved view; 0 when it has no events. Throws UnknownSession when the id
    // is not in the store. Non-pure so the M1 in-memory fakes keep compiling;
    // the durable store overrides it with an indexed MAX(sequence).
    [[nodiscard]] virtual Sequence headSequence(SessionId id) const {
        if (!load(id).has_value()) {
            throw UnknownSession("unknown session");
        }
        Sequence head = 0;
        for (const EventRecord& record : read(id, 0)) {
            head = std::max(head, record.seq);
        }
        return head;
    }

    // 11-m2-errata §5.2 (D15): at most `limit` committed events with seq >
    // `after`, ascending. limit == 0 returns empty. Non-pure for the same
    // reason as `headSequence`; the durable store overrides it with `LIMIT ?`
    // so it stops reading at the bound instead of scanning and truncating.
    [[nodiscard]] virtual EventRange readAfter(SessionId id, Sequence after,
                                               std::size_t limit) const {
        EventRange all = read(id, after);
        if (limit < all.size()) {
            all.resize(limit);
        }
        return all;
    }
};

// 01 §6.3: the pure projection. Reads only the header and the resolved event
// view; no I/O, no clock, no randomness (I7).
[[nodiscard]] std::vector<Message> deriveMessages(const SessionHeader& header,
                                                  const EventRange& events);

[[nodiscard]] SessionId make_session_id();
[[nodiscard]] EventId   make_event_id();

class Session {
public:
    Session(SessionHeader header, SessionStore& store, EventBus& bus);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) = delete;

    [[nodiscard]] const SessionHeader& header() const noexcept { return header_; }
    [[nodiscard]] SessionId            id() const noexcept { return header_.id; }
    [[nodiscard]] SessionKind          kind() const noexcept { return header_.kind; }

    // Resolved logical view (fork prefix ++ own), ascending by Sequence.
    [[nodiscard]] EventRange events() const;
    // This session's physical events only (excludes any inherited prefix).
    [[nodiscard]] EventRange ownEvents() const;

    [[nodiscard]] std::vector<Message> deriveMessages() const;

    template <class P>
    Sequence append(const P& payload) {
        TypedEvent<P> typed;
        typed.id         = make_event_id();
        typed.session_id = id();
        typed.timestamp  = std::chrono::system_clock::now();
        typed.payload    = payload;
        return appendEvent(encode(typed));
    }

    Sequence appendEvent(Event event);

    // One transaction for the whole span; publishes after commit in input
    // order and returns the store-assigned sequences (02 §4.4, A4/A5).
    std::vector<Sequence> appendBatch(std::span<const Event> events);

    // Live-only publish (never appended, never durable): the destination of
    // ToolContext::emit(Event) (07 §5.1, 01 §6). Stamps the session id and
    // timestamp when unset.
    void emit(Event event);

    [[nodiscard]] SessionSnapshot snapshot() const;

    [[nodiscard]] TurnId nextTurnId() const noexcept { return nextTurn_; }
    [[nodiscard]] StepId nextStepId() const noexcept { return nextStep_; }

    static Session resume(const SessionHeader& header, SessionStore& store, EventBus& bus);
    static Session fork(const Session& parent, std::size_t seedLength, SessionStore& store,
                        EventBus& bus);
    static Session replay(const SessionHeader& header, SessionStore& store, EventBus& bus);

private:
    void reload();

    SessionHeader header_;
    SessionStore* store_ = nullptr;
    EventBus*     bus_   = nullptr;
    EventRange    log_;
    TurnId        nextTurn_ = 1;
    StepId        nextStep_ = 1;
    std::mutex    appendMutex_;
};

} // namespace ymh
