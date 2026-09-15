#pragma once

// Tool-layer error taxonomy (07 §2.1-§2.2). Lives in the execution layer so
// `ExecutionEnvironment::resolve()` can throw `ToolError{PathEscape}` without
// the execution library depending on the tool library (the dependency direction
// is tools → execution). The names and namespace are unchanged.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ymh {

enum class ToolErrorCode : std::uint8_t {
    UnknownTool,        // no registration for the name (08 §4.2, L-F8)
    InvalidArguments,   // arguments violate input_schema
    PathEscape,         // resolve() rejected an out-of-root path (F1, §18)
    NotFound,           // fs/git target does not exist
    ResourceExhausted,  // subprocess/PTY cap exhausted (F8, §9.11)
    Timeout,            // process/output deadline exceeded
    Io,                 // filesystem/process I/O failure
    Internal,           // tool invariant violated / unexpected exception
};

[[nodiscard]] std::string_view to_string(ToolErrorCode code) noexcept;

class ToolError final : public std::runtime_error {
public:
    ToolError(ToolErrorCode code, std::string message);
    [[nodiscard]] ToolErrorCode code() const noexcept { return code_; }

private:
    ToolErrorCode code_;
};

enum class ToolRegistryErrorCode : std::uint8_t {
    DuplicateName,      // same name already registered at another version
    DuplicateVersion,   // identical (name, version) already registered
    InvalidName,        // ToolName grammar violation
    InvalidSchema,      // input_schema malformed or outside the pinned subset
    RegistryFrozen,     // add/remove after freeze()
};

class ToolRegistryError final : public std::runtime_error {
public:
    ToolRegistryError(ToolRegistryErrorCode code, std::string message);
    [[nodiscard]] ToolRegistryErrorCode code() const noexcept { return code_; }

private:
    ToolRegistryErrorCode code_;
};

} // namespace ymh
