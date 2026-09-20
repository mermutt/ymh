#pragma once

// Job identity/status value vocabulary, pinned by 44-goals-jobs-commands.md
// §5.5 (26 §4.3.8, 26-D19). Kept in a dependency-free header so the durable
// `payload::JobChanged` codec can name a `JobStatus` without pulling in the
// registry's `Agent`/`Task` dependencies.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ymh {

using JobOrdinal = std::uint64_t;

// dsh types.d.ts:54. `<kind>-<ordinal>`; authorization is ownership, not
// secrecy (26-I8).
struct JobId {
    std::string kind;
    JobOrdinal  ordinal = 0;

    bool operator==(const JobId&) const = default;
};

// 26 §4.3.8; dsh types.d.ts:14.
enum class JobStatus : std::uint8_t { Running, Stopping, Completed, Killed, Failed };

// jobs.completion_delivery (26 §4.9:1328).
enum class CompletionDelivery : std::uint8_t { Wakeup, Quiet };

[[nodiscard]] inline std::string_view job_status_name(JobStatus status) noexcept {
    switch (status) {
        case JobStatus::Running:
            return "running";
        case JobStatus::Stopping:
            return "stopping";
        case JobStatus::Completed:
            return "completed";
        case JobStatus::Killed:
            return "killed";
        case JobStatus::Failed:
            return "failed";
    }
    return {};
}

[[nodiscard]] inline std::optional<JobStatus> parse_job_status(std::string_view name) noexcept {
    if (name == "running") {
        return JobStatus::Running;
    }
    if (name == "stopping") {
        return JobStatus::Stopping;
    }
    if (name == "completed") {
        return JobStatus::Completed;
    }
    if (name == "killed") {
        return JobStatus::Killed;
    }
    if (name == "failed") {
        return JobStatus::Failed;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string_view completion_delivery_name(
    CompletionDelivery delivery) noexcept {
    return delivery == CompletionDelivery::Wakeup ? "wakeup" : "quiet";
}

[[nodiscard]] inline std::optional<CompletionDelivery> parse_completion_delivery(
    std::string_view name) noexcept {
    if (name == "wakeup") {
        return CompletionDelivery::Wakeup;
    }
    if (name == "quiet") {
        return CompletionDelivery::Quiet;
    }
    return std::nullopt;
}

// The predictable textual form `<kind>-<ordinal>`.
[[nodiscard]] inline std::string to_string(const JobId& id) {
    return id.kind + "-" + std::to_string(id.ordinal);
}

// Parses `<kind>-<ordinal>`; a malformed id yields nullopt (never throws).
[[nodiscard]] inline std::optional<JobId> parse_job_id(std::string_view text) {
    const std::size_t dash = text.rfind('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= text.size()) {
        return std::nullopt;
    }
    JobOrdinal ordinal = 0;
    for (std::size_t index = dash + 1; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        ordinal = ordinal * 10 + static_cast<JobOrdinal>(c - '0');
    }
    return JobId{std::string{text.substr(0, dash)}, ordinal};
}

} // namespace ymh
