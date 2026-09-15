#include "ymh/execution/errors.hpp"

#include <utility>

namespace ymh {

std::string_view to_string(ToolErrorCode code) noexcept {
    switch (code) {
        case ToolErrorCode::UnknownTool:       return "UnknownTool";
        case ToolErrorCode::InvalidArguments:  return "InvalidArguments";
        case ToolErrorCode::PathEscape:        return "PathEscape";
        case ToolErrorCode::NotFound:          return "NotFound";
        case ToolErrorCode::ResourceExhausted: return "ResourceExhausted";
        case ToolErrorCode::Timeout:           return "Timeout";
        case ToolErrorCode::Io:                return "Io";
        case ToolErrorCode::Internal:          return "Internal";
    }
    return "Internal";
}

ToolError::ToolError(ToolErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

ToolRegistryError::ToolRegistryError(ToolRegistryErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code_(code) {}

} // namespace ymh
