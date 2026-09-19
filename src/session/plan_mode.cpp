#include "ymh/session/plan_mode.hpp"

#include "ymh/session/events.hpp"

namespace ymh {

bool plan_mode_active(const EventRange& events) noexcept {
    bool active = false;
    for (const EventRecord& record : events) {
        if (record.event.type != EventType::PlanMode) {
            continue;
        }
        try {
            active = record.event.payload.get<payload::PlanMode>().active;
        } catch (...) {
            // A malformed payload folds to inactive; the fold never throws.
        }
    }
    return active;
}

} // namespace ymh
