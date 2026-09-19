#pragma once

// SQLite-backed persistence and write lease (component 02). Pinned by
// docs/design/02-persistence.md §3-§7: `PersistenceConfig`, `BootId`,
// `HolderIdentity`, `LeaseState`, and `SessionPersistence`. The store is the
// sole assigner of `Sequence` (01 I2 / P6) and the only mutating layer.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ymh/session/errors.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_handle.hpp"

namespace ymh {

// 02 §2.1: minted once by the daemon at startup and supplied via
// PersistenceConfig; the store never mints it and it is not part of
// SessionHeader.
struct BootId {
    std::string value;

    auto operator<=>(const BootId&) const = default;
};

struct HolderIdentity {
    std::int32_t pid = 0;
    BootId       boot_id;

    auto operator<=>(const HolderIdentity&) const = default;
};

// 02 §2.1 / §5.3.
enum class LeaseState : std::uint8_t {
    HeldByMe,
    HeldByOther,
    Expired,
    Absent,
};

// 02 §4.1.
struct PersistenceConfig {
    std::filesystem::path db_path;
    std::filesystem::path lock_path;
    BootId                boot_id;

    std::chrono::milliseconds lease_ttl{15'000};
    std::chrono::milliseconds renew_interval{5'000};

    std::chrono::milliseconds busy_timeout{5'000};
    int                       wal_autocheckpoint_pages{1'000};

    std::size_t snapshot_threshold_events{256};

    std::size_t max_payload_bytes{4u * 1024u * 1024u};

    std::size_t               max_chunk_batch{32};
    std::chrono::milliseconds chunk_flush_interval{100};
};

inline constexpr int         kSchemaVersion   = 1;
inline constexpr std::int32_t kApplicationId  = 0x594D4801;

class SessionPersistence final : public SessionStore {
public:
    static std::unique_ptr<SessionPersistence> open(const PersistenceConfig& config);
    static std::unique_ptr<SessionPersistence> openReadOnly(const PersistenceConfig& config);
    ~SessionPersistence() override;

    SessionPersistence(const SessionPersistence&) = delete;
    SessionPersistence& operator=(const SessionPersistence&) = delete;

    SessionHeader                create(SessionHeader header) override;
    std::optional<SessionHeader> load(SessionId id) const override;
    std::vector<SessionHeader>   list() const override;
    void                         erase(SessionId id) override;
    EventRange                   read(SessionId id, Sequence after = 0) const override;
    EventRange readRange(SessionId id, Sequence from, Sequence to) const override;
    Sequence   append(SessionId id, Event event) override;
    bool       isLeaseHolder(SessionId id) const override;

    [[nodiscard]] bool isUnprompted(SessionId id) const override;
    [[nodiscard]] bool hasDependents(SessionId id) const override;
    Sequence           eraseWithEvent(SessionId id, Event event) override;

    [[nodiscard]] Sequence   headSequence(SessionId id) const override;
    [[nodiscard]] EventRange readAfter(SessionId id, Sequence after,
                                       std::size_t limit) const override;

    std::vector<Sequence> appendBatch(SessionId id, std::span<const Event> events) override;

    [[nodiscard]] LeaseState leaseState(SessionId id) const;
    bool                     acquireLease(SessionId id);
    void                     renewLeases();
    bool                     releaseLease(SessionId id);

    void                           checkpoint(SessionId id);
    std::optional<SessionSnapshot> loadSnapshot(SessionId id) const;
    [[nodiscard]] bool             snapshotIsCurrent(SessionId id) const;
    void                           discardSnapshot(SessionId id);

    [[nodiscard]] int schemaVersion() const;
    void              close();

private:
    class Impl;

    explicit SessionPersistence(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace ymh
