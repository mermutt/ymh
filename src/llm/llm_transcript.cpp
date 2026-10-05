#include "ymh/llm/llm_transcript.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>
#include <utility>

#include <unistd.h>

#include "ymh/core/logging.hpp"

namespace ymh {
namespace {

std::string now_rfc3339_ms() {
    using namespace std::chrono;
    const auto now  = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - secs).count();
    const std::time_t tt = system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    char base[32];
    std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &tm);
    char out[48];
    std::snprintf(out, sizeof(out), "%s.%03lldZ", base, static_cast<long long>(millis));
    return std::string{out};
}

std::filesystem::path numbered_path(const std::filesystem::path& directory,
                                    const std::string& stem,
                                    std::size_t index) {
    return directory / (stem + "." + std::to_string(index) + ".jsonl");
}

} // namespace

LlmTranscript::LlmTranscript(LlmTranscriptOptions options) : options_(std::move(options)) {
    if (options_.directory.empty()) {
        return;
    }
    if (options_.max_bytes_per_file == 0) {
        options_.max_bytes_per_file = 1;
    }
    if (options_.max_files == 0) {
        options_.max_files = 1;
    }
    if (options_.file_stem.empty()) {
        options_.file_stem = "llm-transcript";
    }

    std::error_code error;
    std::filesystem::create_directories(options_.directory, error);
    if (error) {
        disable("create directory failed");
        return;
    }
    path_ = options_.directory / (options_.file_stem + ".jsonl");

    const auto size = std::filesystem::file_size(path_, error);
    bytes_written_  = error ? 0 : static_cast<std::size_t>(size);

    std::ofstream probe(path_, std::ios::app | std::ios::binary);
    if (!probe) {
        disable("open failed");
        return;
    }
    healthy_.store(true);
}

LlmTranscript::~LlmTranscript() = default;

bool LlmTranscript::healthy() const noexcept {
    return healthy_.load();
}

std::filesystem::path LlmTranscript::path() const {
    return healthy_.load() ? path_ : std::filesystem::path{};
}

void LlmTranscript::record_request(std::uint64_t request_id,
                                   std::uint32_t attempt,
                                   std::string_view model,
                                   std::string_view endpoint_host,
                                   std::string_view body) {
    if (!healthy_.load()) {
        return;
    }
    try {
        nlohmann::json record{
            {"ts", now_rfc3339_ms()},
            {"direction", "request"},
            {"pid", static_cast<std::int64_t>(::getpid())},
            {"request_id", request_id},
            {"attempt", attempt},
            {"model", std::string{model}},
            {"endpoint_host", std::string{endpoint_host}},
            {"body_bytes", body.size()},
            {"body", std::string{body}},
            {"truncated", false},
        };
        append(std::move(record));
    } catch (...) {
        disable("serialize or write failed");
    }
}

void LlmTranscript::record_response(std::uint64_t request_id,
                                    std::uint32_t attempt,
                                    std::string_view model,
                                    std::string_view endpoint_host,
                                    int status,
                                    std::string_view raw,
                                    const nlohmann::json& assembled) {
    if (!healthy_.load()) {
        return;
    }
    try {
        // 74-D12: absent usage is not the same as zero usage.
        const bool usage_missing =
            !(assembled.contains("usage") && assembled["usage"].is_object());
        nlohmann::json record{
            {"ts", now_rfc3339_ms()},
            {"direction", "response"},
            {"pid", static_cast<std::int64_t>(::getpid())},
            {"request_id", request_id},
            {"attempt", attempt},
            {"model", std::string{model}},
            {"endpoint_host", std::string{endpoint_host}},
            {"status", status},
            {"raw_bytes", raw.size()},
            {"raw", std::string{raw}},
            {"assembled", assembled},
            {"usage_missing", usage_missing},
            {"truncated", false},
        };
        append(std::move(record));
    } catch (...) {
        disable("serialize or write failed");
    }
}

void LlmTranscript::append(nlohmann::json record) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!healthy_.load()) {
        return;
    }

    std::string line = record.dump();
    line.push_back('\n');

    // 74-D10/74-F5: a single oversized record is replaced by a small flagged
    // marker so the per-file ceiling is absolute.
    if (line.size() > options_.max_bytes_per_file) {
        nlohmann::json marker{
            {"ts", record.value("ts", std::string{})},
            {"direction", record.value("direction", std::string{})},
            {"pid", static_cast<std::int64_t>(::getpid())},
            {"truncated", true},
            {"note", "record exceeded per-file cap"},
            {"original_bytes", line.size()},
        };
        line = marker.dump();
        line.push_back('\n');
    }

    if (bytes_written_ > 0 && bytes_written_ + line.size() > options_.max_bytes_per_file) {
        rotate_locked();
        if (!healthy_.load()) {
            return;
        }
    }

    std::ofstream out(path_, std::ios::app | std::ios::binary);
    if (!out) {
        disable("open failed");
        return;
    }
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.flush();
    if (!out) {
        disable("write failed");
        return;
    }
    bytes_written_ += line.size();
}

void LlmTranscript::rotate_locked() {
    std::error_code error;
    const std::size_t keep = options_.max_files;
    if (keep <= 1) {
        std::filesystem::resize_file(path_, 0, error);
        if (error) {
            disable("truncate failed");
            return;
        }
        bytes_written_ = 0;
        return;
    }

    std::filesystem::remove(numbered_path(options_.directory, options_.file_stem, keep - 1),
                            error);
    error.clear();
    for (std::size_t target = keep - 1; target >= 2; --target) {
        const auto from = numbered_path(options_.directory, options_.file_stem, target - 1);
        const auto to   = numbered_path(options_.directory, options_.file_stem, target);
        if (std::filesystem::exists(from, error)) {
            std::filesystem::rename(from, to, error);
            error.clear();
        }
    }
    std::filesystem::remove(numbered_path(options_.directory, options_.file_stem, 1), error);
    error.clear();
    std::filesystem::rename(path_, numbered_path(options_.directory, options_.file_stem, 1),
                            error);
    if (error) {
        std::filesystem::resize_file(path_, 0, error);
        if (error) {
            disable("rotate failed");
            return;
        }
    }
    bytes_written_ = 0;
}

void LlmTranscript::disable(std::string_view reason) noexcept {
    healthy_.store(false);
    if (warning_logged_.exchange(true)) {
        return;
    }
    // 74-I10: content-free diagnostic only; transcript bytes never reach spdlog.
    try {
        category_logger(LogCategory::Llm)
            .debug(std::string{"llm transcript disabled: "} + std::string{reason});
    } catch (...) {
    }
}

} // namespace ymh
