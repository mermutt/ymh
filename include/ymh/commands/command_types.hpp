#pragma once

// Command identity/source value vocabulary, pinned by 44-goals-jobs-commands.md
// §5.6 (26 §4.3.8, 26-D20). Kept in a dependency-free header so the durable
// `payload::CommandRun` codec can name a `CommandSource` without pulling in the
// handler/`Agent` dependencies of `commands/command.hpp`.

#include <cstdint>
#include <optional>
#include <string_view>

namespace ymh {

// 26 §4.3.8. dsh CommandId (dsh-commands brand.d.ts).
using CommandId = std::uint64_t;

// 26 §4.3.9. dsh has only User; ymh adds Agent (44-D16).
enum class CommandSource : std::uint8_t { User, Agent };

enum class CommandOutcomeKind : std::uint8_t { Success, Error };

[[nodiscard]] inline std::string_view command_source_name(CommandSource source) noexcept {
    return source == CommandSource::Agent ? "agent" : "user";
}

[[nodiscard]] inline std::optional<CommandSource> parse_command_source(
    std::string_view name) noexcept {
    if (name == "user") {
        return CommandSource::User;
    }
    if (name == "agent") {
        return CommandSource::Agent;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string_view command_outcome_kind_name(
    CommandOutcomeKind kind) noexcept {
    return kind == CommandOutcomeKind::Error ? "error" : "success";
}

} // namespace ymh
