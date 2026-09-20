#include "ymh/agent/agent.hpp"

namespace ymh {

std::string_view agent_error_code_name(AgentErrorCode code) noexcept {
    switch (code) {
        case AgentErrorCode::None:                  return "None";
        case AgentErrorCode::UnknownSession:        return "UnknownSession";
        case AgentErrorCode::LeaseHeldByOther:      return "LeaseHeldByOther";
        case AgentErrorCode::LeaseLost:             return "LeaseLost";
        case AgentErrorCode::StoreUnavailable:      return "StoreUnavailable";
        case AgentErrorCode::InboxFull:             return "InboxFull";
        case AgentErrorCode::AgentDisposed:         return "AgentDisposed";
        case AgentErrorCode::StepLimitExceeded:     return "StepLimitExceeded";
        case AgentErrorCode::ContextAssemblyFailed: return "ContextAssemblyFailed";
        case AgentErrorCode::CompactionFailed:      return "CompactionFailed";
        case AgentErrorCode::ProviderFailed:        return "ProviderFailed";
        case AgentErrorCode::Cancelled:             return "Cancelled";
        case AgentErrorCode::Internal:              return "Internal";
        case AgentErrorCode::DelegationDepthExceeded:
            return "DelegationDepthExceeded";
    }
    return "Internal";
}

AgentErrorCode mapAgentError(LLMErrorCode code) noexcept {
    switch (code) {
        case LLMErrorCode::None:
            return AgentErrorCode::None;
        case LLMErrorCode::Cancelled:
            return AgentErrorCode::Cancelled;
        case LLMErrorCode::ContextLengthExceeded:
            return AgentErrorCode::CompactionFailed;
        default:
            return AgentErrorCode::ProviderFailed;
    }
}

} // namespace ymh
