#include "ymh/session/permissions_state.hpp"

namespace ymh {

PermissionsState permissions_state(const EventRange& events) noexcept {
    PermissionsState state;
    for (const EventRecord& record : events) {
        switch (record.event.type) {
            case EventType::SandboxModeChanged:
                try {
                    state.mode = record.event.payload.get<payload::SandboxModeChanged>().mode;
                } catch (...) {
                }
                break;
            case EventType::ApprovalPolicyChanged:
                try {
                    state.policy = record.event.payload.get<payload::ApprovalPolicyChanged>().policy;
                } catch (...) {
                }
                break;
            case EventType::PermissionPresetSelected:
                try {
                    state.preset =
                        record.event.payload.get<payload::PermissionPresetSelected>().preset;
                } catch (...) {
                }
                break;
            default:
                break;
        }
    }
    return state;
}

} // namespace ymh
