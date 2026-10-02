#pragma once

// Opt-in full LLM wire transcript (74-llm-transcript-errata.md). Records the
// exact request body handed to `HttpTransport` and the exact response bytes the
// body sink received, as bounded JSONL. Headers are never an input, so the
// `Authorization` header and the API key cannot appear (74-D9).
//
// This is a diagnostic sink: best-effort, never throws, never changes a turn's
// outcome (74-I7). It is not spdlog and is not redacted (74-D1); the transcript
// content must never reach spdlog.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ymh {

// 74-D10: fixed ceilings. The retained byte total per directory is bounded by
// `max_bytes_per_file * max_files`.
struct LlmTranscriptOptions {
    std::filesystem::path directory;                       // required, non-empty
    std::size_t           max_bytes_per_file = 16u << 20;  // 16 MiB
    std::size_t           max_files = 4;                   // 4 files => 64 MiB
    std::string           file_stem = "llm-transcript";    // test seam
};

// One writer per workspace runtime, shared by every per-endpoint provider
// (74-D7); the mutex serializes appends.
class LlmTranscript {
public:
    explicit LlmTranscript(LlmTranscriptOptions options);
    ~LlmTranscript();

    LlmTranscript(const LlmTranscript&) = delete;
    LlmTranscript& operator=(const LlmTranscript&) = delete;

    // True only while the directory/file are writable. Flips to false on the
    // first I/O failure; never flips back (74-I7).
    [[nodiscard]] bool healthy() const noexcept;

    // Current (un-rotated) file. Empty when unhealthy.
    [[nodiscard]] std::filesystem::path path() const;

    // 74-D7: exact request body; `endpoint_host` is URL-derived, never a header.
    void record_request(std::uint64_t request_id,
                        std::uint32_t attempt,
                        std::string_view model,
                        std::string_view endpoint_host,
                        std::string_view body);

    // 74-D7/74-D8: exact raw response bytes plus the best-effort assembled view
    // (`content`, `reasoning`, `tool_calls`, `finish_reason`, `usage`).
    void record_response(std::uint64_t request_id,
                         std::uint32_t attempt,
                         std::string_view model,
                         std::string_view endpoint_host,
                         int status,
                         std::string_view raw,
                         const nlohmann::json& assembled);

private:
    void append(nlohmann::json record);
    void rotate_locked();
    void disable(std::string_view reason) noexcept;

    LlmTranscriptOptions  options_;
    std::filesystem::path path_;
    std::size_t           bytes_written_ = 0;
    std::atomic<bool>     healthy_{false};
    std::atomic<bool>     warning_logged_{false};
    mutable std::mutex    mutex_;
};

} // namespace ymh
