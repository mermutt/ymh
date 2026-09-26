#pragma once

// In-memory ordered event log helpers for the session layer (01 §4.6, §6.2).
//
// The durable `Sequence` is owned by `SessionStore`; this header only provides
// the value types that carry it (`EventRecord` / `EventRange`, pinned by
// `ymh/core/event.hpp`) and a small ordered `EventLog` used by in-process
// consumers. It deliberately performs no persistence.

#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ymh/core/event.hpp"

namespace ymh {

// Append-only in-memory log. Ordering is by insertion; `head()` is the highest
// assigned `Sequence` (0 when empty). It never assigns sequences itself.
class EventLog {
public:
    EventLog() = default;

    void push(EventRecord record) { records_.push_back(std::move(record)); }

    void assign(EventRange records) { records_ = std::move(records); }

    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

    [[nodiscard]] Sequence head() const noexcept {
        return records_.empty() ? 0 : records_.back().seq;
    }

    [[nodiscard]] const EventRange& records() const noexcept { return records_; }

    [[nodiscard]] EventRange recordsAfter(Sequence after) const {
        EventRange result;
        for (const EventRecord& record : records_) {
            if (record.seq > after) {
                result.push_back(record);
            }
        }
        return result;
    }

    [[nodiscard]] EventRange recordsBetween(Sequence from, Sequence to) const {
        EventRange result;
        for (const EventRecord& record : records_) {
            if (record.seq >= from && record.seq <= to) {
                result.push_back(record);
            }
        }
        return result;
    }

private:
    EventRange records_;
};

} // namespace ymh
