#include "ymh/llm/openai_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>

#include "ymh/llm/redaction.hpp"
#include "ymh/llm/sse_parser.hpp"
#include "ymh/llm/tool_call_assembler.hpp"

namespace ymh {
namespace {

constexpr std::size_t kErrorBodyCaptureBytes = 64u << 10;

std::string to_lower_copy(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string trim_copy(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(first, last - first + 1)};
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    return to_lower_copy(haystack).find(to_lower_copy(needle)) != std::string::npos;
}

std::optional<std::string> get_env(const std::string& name) {
    if (name.empty()) {
        return std::nullopt;
    }
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
}

std::string join_url(std::string_view base, std::string_view path) {
    std::string url(base);
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    if (path.empty() || path.front() != '/') {
        url.push_back('/');
    }
    url.append(path);
    return url;
}

LLMError make_error(LLMErrorCode code, std::string detail) {
    LLMError error;
    error.code = code;
    error.detail = std::move(detail);
    error.retryable = is_retryable_code(code);
    return error;
}

FinishReason map_finish_reason(std::string_view value) {
    if (value == "stop") {
        return FinishReason::Stop;
    }
    if (value == "length") {
        return FinishReason::Length;
    }
    if (value == "tool_calls" || value == "function_call") {
        return FinishReason::ToolCalls;
    }
    if (value == "content_filter") {
        return FinishReason::ContentFilter;
    }
    return FinishReason::Other;
}

Usage parse_usage(const nlohmann::json& json) {
    Usage usage;
    usage.input_tokens = json.value("prompt_tokens", std::int64_t{0});
    usage.output_tokens = json.value("completion_tokens", std::int64_t{0});
    if (json.contains("prompt_tokens_details") && json["prompt_tokens_details"].is_object()) {
        usage.cached_tokens =
            json["prompt_tokens_details"].value("cached_tokens", std::int64_t{0});
    }
    if (json.contains("completion_tokens_details") &&
        json["completion_tokens_details"].is_object()) {
        usage.reasoning_tokens =
            json["completion_tokens_details"].value("reasoning_tokens", std::int64_t{0});
    }
    return usage;
}

LLMError classify_error(int status, std::string message, std::string code) {
    LLMError error;
    error.http_status = status;
    error.provider_message = redact_secrets(message);
    error.detail = redact_secrets(code);

    if (status == 401 || status == 403) {
        error.code = LLMErrorCode::Auth;
    } else if (status == 429) {
        error.code = LLMErrorCode::RateLimited;
    } else if (status == 400 || status == 422) {
        if (contains_ci(code, "context_length") || contains_ci(message, "context length") ||
            contains_ci(message, "maximum context") || contains_ci(message, "too many tokens")) {
            error.code = LLMErrorCode::ContextLengthExceeded;
        } else if (contains_ci(code, "content_filter") ||
                   contains_ci(message, "content filter") ||
                   contains_ci(message, "content_policy")) {
            error.code = LLMErrorCode::ContentFiltered;
        } else {
            error.code = LLMErrorCode::BadRequest;
        }
    } else if (status >= 500 && status <= 599) {
        error.code = LLMErrorCode::ServerError;
    } else if (status == 0) {
        error.code = LLMErrorCode::NetworkError;
    } else {
        error.code = LLMErrorCode::ProviderInternal;
    }

    error.retryable = is_retryable_code(error.code);
    return error;
}

LLMError classify_embedded_error(std::string message, std::string code) {
    LLMError error;
    error.provider_message = redact_secrets(message);
    error.detail = redact_secrets(code);

    if (contains_ci(code, "context_length") || contains_ci(message, "context length") ||
        contains_ci(message, "maximum context") || contains_ci(message, "too many tokens")) {
        error.code = LLMErrorCode::ContextLengthExceeded;
    } else if (contains_ci(code, "content_filter") || contains_ci(message, "content filter") ||
               contains_ci(message, "content_policy")) {
        error.code = LLMErrorCode::ContentFiltered;
    } else if (contains_ci(code, "rate") || contains_ci(message, "rate limit")) {
        error.code = LLMErrorCode::RateLimited;
    } else {
        error.code = LLMErrorCode::ProviderInternal;
    }

    error.retryable = is_retryable_code(error.code);
    return error;
}

std::optional<std::pair<std::string, std::string>> parse_error_object(
    const nlohmann::json& json) {
    if (!json.contains("error") || json["error"].is_null()) {
        return std::nullopt;
    }
    const auto& error = json["error"];
    if (error.is_object()) {
        return std::make_pair(error.value("message", std::string{}),
                              error.value("code", std::string{}));
    }
    if (error.is_string()) {
        return std::make_pair(error.get<std::string>(), std::string{});
    }
    return std::nullopt;
}

std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned day_of_year =
        (153u * (month + (month > 2 ? -3u : 9u)) + 2u) / 5u + day - 1u;
    const unsigned day_of_era =
        year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

std::chrono::milliseconds effective_timeout(std::chrono::milliseconds deadline,
                                            std::chrono::milliseconds configured) {
    if (deadline.count() <= 0) {
        return configured;
    }
    return std::min(deadline, configured);
}

bool sleep_with_cancel(std::chrono::milliseconds delay, const CancellationToken& cancel) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + delay;
    const auto slice = std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds{20});
    while (Clock::now() < deadline) {
        if (cancel.cancelled()) {
            return false;
        }
        const auto remaining = deadline - Clock::now();
        std::this_thread::sleep_for(std::min(remaining, slice));
    }
    return !cancel.cancelled();
}

double next_jitter_sample() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static thread_local std::uniform_real_distribution<double> distribution{0.0, 1.0};
    return distribution(rng);
}

std::string join_text(const Message& message) {
    std::string text;
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Text) {
            text.append(block.text);
        }
    }
    return text;
}

nlohmann::json map_message(const Message& message) {
    switch (message.role) {
        case Role::System:
            return nlohmann::json{{"role", "system"}, {"content", join_text(message)}};

        case Role::Tool:
            return nlohmann::json{{"role", "tool"},
                                  {"tool_call_id", message.tool_call_id},
                                  {"content", join_text(message)}};

        case Role::User: {
            bool has_image = false;
            for (const ContentBlock& block : message.content) {
                if (block.kind == ContentBlockKind::Image) {
                    has_image = true;
                    break;
                }
            }
            if (!has_image) {
                return nlohmann::json{{"role", "user"}, {"content", join_text(message)}};
            }
            nlohmann::json parts = nlohmann::json::array();
            for (const ContentBlock& block : message.content) {
                if (block.kind == ContentBlockKind::Text && !block.text.empty()) {
                    parts.push_back(nlohmann::json{{"type", "text"}, {"text", block.text}});
                } else if (block.kind == ContentBlockKind::Image) {
                    parts.push_back(nlohmann::json{
                        {"type", "image_url"},
                        {"image_url",
                         {{"url", "data:" + block.media_type + ";base64," + block.data}}}});
                }
            }
            return nlohmann::json{{"role", "user"}, {"content", std::move(parts)}};
        }

        case Role::Assistant: {
            const std::string text = join_text(message);
            nlohmann::json tool_calls = nlohmann::json::array();
            for (const ContentBlock& block : message.content) {
                if (block.kind == ContentBlockKind::ToolUse) {
                    tool_calls.push_back(
                        nlohmann::json{{"id", block.tool_call_id},
                                       {"type", "function"},
                                       {"function",
                                        {{"name", block.tool_name},
                                         {"arguments", block.arguments.dump()}}}});
                }
            }
            nlohmann::json result{{"role", "assistant"}};
            if (!text.empty() || tool_calls.empty()) {
                result["content"] = text;
            }
            if (!tool_calls.empty()) {
                result["tool_calls"] = std::move(tool_calls);
            }
            return result;
        }
    }
    return nlohmann::json{{"role", "user"}, {"content", join_text(message)}};
}

// Decodes an OpenAI-compatible SSE stream into the provider-agnostic event
// algebra. One instance per attempt; it tracks the retry barrier (L7).
class OpenAiStreamDecoder {
public:
    OpenAiStreamDecoder(StreamSink& sink,
                        ProviderCapabilities capabilities,
                        std::size_t max_arguments_bytes,
                        std::size_t sse_line_bytes)
        : sink_(sink),
          capabilities_(capabilities),
          assembler_(max_arguments_bytes),
          parser_(sse_line_bytes) {}

    bool on_bytes(const char* data, std::size_t len) {
        if (stop_ || terminal_ || terminal_error_.code != LLMErrorCode::None) {
            return false;
        }
        const SseFeedStatus status =
            parser_.feed(std::string_view(data, len), [this](const SseEvent& event) {
                handle_sse(event);
            });
        if (status == SseFeedStatus::Overflow) {
            terminal_error_ = make_error(LLMErrorCode::MalformedResponse,
                                         "SSE line exceeds sse_line_bytes");
            return false;
        }
        return !stop_ && terminal_error_.code == LLMErrorCode::None;
    }

    void finish_stream() {
        if (stop_ || terminal_ || terminal_error_.code != LLMErrorCode::None) {
            return;
        }
        parser_.finish([this](const SseEvent& event) { handle_sse(event); });
    }

    [[nodiscard]] bool any_event() const noexcept { return any_event_; }
    [[nodiscard]] bool stop_requested() const noexcept { return stop_; }
    [[nodiscard]] const LLMError& terminal_error() const noexcept { return terminal_error_; }

    LLMResponse finalize() {
        finalize_tool_calls();

        LLMResponse response;
        if (terminal_error_.code != LLMErrorCode::None) {
            emit(StreamEvent{StreamError{terminal_error_}});
            terminal_ = true;
            response.outcome = StreamOutcome::Failed;
            response.finish = FinishReason::Error;
            response.error = terminal_error_;
            return response;
        }

        const FinishReason reason =
            finish_seen_ ? finish_
                         : (tool_calls_.empty() ? FinishReason::Stop : FinishReason::ToolCalls);
        emit(StreamEvent{Finished{reason, usage_, std::nullopt}});
        terminal_ = true;

        response.outcome = StreamOutcome::Completed;
        response.finish = reason;
        response.usage = usage_;
        response.tool_calls = tool_calls_;
        return response;
    }

private:
    void emit(const StreamEvent& event) {
        if (terminal_) {
            return;
        }
        const SinkFlow flow = sink_(event);
        any_event_ = true;
        if (flow == SinkFlow::Stop) {
            stop_ = true;
        }
    }

    bool is_started(std::uint32_t index) const {
        return std::find(started_.begin(), started_.end(), index) != started_.end();
    }

    void handle_sse(const SseEvent& event) {
        if (stop_ || terminal_ || terminal_error_.code != LLMErrorCode::None) {
            return;
        }
        if (event.done) {
            return;
        }

        const nlohmann::json chunk = nlohmann::json::parse(event.data, nullptr, false);
        if (chunk.is_discarded() || !chunk.is_object()) {
            terminal_error_ =
                make_error(LLMErrorCode::MalformedResponse, "undecodable SSE JSON");
            return;
        }

        if (const auto error = parse_error_object(chunk)) {
            terminal_error_ = classify_embedded_error(error->first, error->second);
            return;
        }

        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            handle_usage(chunk["usage"]);
        }

        if (chunk.contains("choices") && chunk["choices"].is_array()) {
            for (const auto& choice : chunk["choices"]) {
                if (!choice.is_object()) {
                    continue;
                }
                if (choice.contains("delta") && choice["delta"].is_object()) {
                    handle_delta(choice["delta"]);
                }
                if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
                    finish_ = map_finish_reason(choice["finish_reason"].get<std::string>());
                    finish_seen_ = true;
                    finalize_tool_calls();
                }
                if (terminal_error_.code != LLMErrorCode::None) {
                    return;
                }
            }
        }
    }

    void handle_usage(const nlohmann::json& json) {
        if (usage_.has_value()) {
            terminal_error_ =
                make_error(LLMErrorCode::ProviderInternal, "duplicate usage event");
            return;
        }
        usage_ = parse_usage(json);
        emit(StreamEvent{UsageEvent{*usage_}});
    }

    void handle_delta(const nlohmann::json& delta) {
        if (delta.contains("content") && delta["content"].is_string()) {
            std::string text = delta["content"].get<std::string>();
            if (!text.empty()) {
                emit(StreamEvent{TextDelta{std::move(text)}});
            }
        }

        if (capabilities_.reasoning) {
            for (const char* field : {"reasoning_content", "reasoning"}) {
                if (delta.contains(field) && delta[field].is_string()) {
                    std::string text = delta[field].get<std::string>();
                    if (!text.empty()) {
                        emit(StreamEvent{ReasoningDelta{std::move(text)}});
                    }
                    break;
                }
            }
        }

        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto& tool_call : delta["tool_calls"]) {
                handle_tool_call(tool_call);
                if (terminal_error_.code != LLMErrorCode::None) {
                    return;
                }
            }
        }
    }

    void handle_tool_call(const nlohmann::json& tool_call) {
        if (!tool_call.is_object()) {
            terminal_error_ = make_error(LLMErrorCode::ProviderInternal,
                                         "tool-call element is not an object");
            return;
        }

        const auto index = tool_call.value("index", std::uint32_t{0});
        std::string id = tool_call.value("id", std::string{});
        std::string name;
        std::string arguments;
        if (tool_call.contains("function") && tool_call["function"].is_object()) {
            name = tool_call["function"].value("name", std::string{});
            if (tool_call["function"].contains("arguments") &&
                tool_call["function"]["arguments"].is_string()) {
                arguments = tool_call["function"]["arguments"].get<std::string>();
            }
        }

        if (!is_started(index)) {
            if (id.empty() && name.empty()) {
                terminal_error_ = make_error(LLMErrorCode::ProviderInternal,
                                             "tool-call delta before start");
                return;
            }
            assembler_.onStarted(index, id, name);
            if (assembler_.has_error()) {
                terminal_error_ = assembler_.error();
                return;
            }
            started_.push_back(index);
            unfinished_.push_back(index);
            emit(StreamEvent{ToolCallStarted{index, id, name}});
        }

        if (!arguments.empty()) {
            assembler_.onDelta(index, arguments);
            if (assembler_.has_error()) {
                terminal_error_ = assembler_.error();
                return;
            }
            emit(StreamEvent{ToolCallDelta{index, arguments}});
        }
    }

    void finalize_tool_calls() {
        for (const std::uint32_t index : unfinished_) {
            std::optional<ToolCallAssembled> assembled = assembler_.onFinished(index);
            if (assembler_.has_error()) {
                terminal_error_ = assembler_.error();
                return;
            }
            if (assembled.has_value()) {
                tool_calls_.push_back(*assembled);
                emit(StreamEvent{ToolCallFinished{index, *assembled}});
            }
        }
        unfinished_.clear();
    }

    StreamSink&         sink_;
    ProviderCapabilities capabilities_;
    ToolCallAssembler   assembler_;
    SseParser           parser_;
    std::optional<Usage> usage_;
    FinishReason        finish_ = FinishReason::Other;
    bool                finish_seen_ = false;
    bool                any_event_ = false;
    bool                stop_ = false;
    bool                terminal_ = false;
    LLMError            terminal_error_;
    std::vector<ToolCallAssembled> tool_calls_;
    std::vector<std::uint32_t>     started_;
    std::vector<std::uint32_t>     unfinished_;
};

struct CurlWriteContext {
    HttpBodySink        on_body;
    CancellationToken   cancel;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string         body;
    int                 status = 0;
    bool                capture_body = false;
    bool                aborted = false;
};

std::once_flag g_curl_init;

void ensure_curl_initialized() {
    std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t curl_write_callback(char* data, std::size_t size, std::size_t count, void* user) {
    auto* context = static_cast<CurlWriteContext*>(user);
    const std::size_t total = size * count;
    if (context->cancel.cancelled()) {
        context->aborted = true;
        return 0;
    }
    if (context->on_body && !context->on_body(data, total)) {
        context->aborted = true;
        return 0;
    }
    if (context->capture_body && context->body.size() < kErrorBodyCaptureBytes) {
        const std::size_t room = kErrorBodyCaptureBytes - context->body.size();
        context->body.append(data, std::min(room, total));
    }
    return total;
}

std::size_t curl_header_callback(char* data, std::size_t size, std::size_t count, void* user) {
    auto* context = static_cast<CurlWriteContext*>(user);
    const std::size_t total = size * count;
    const std::string_view line(data, total);

    if (line.rfind("HTTP/", 0) == 0) {
        const auto space = line.find(' ');
        if (space != std::string_view::npos) {
            context->status = std::atoi(std::string(line.substr(space + 1)).c_str());
            context->capture_body = context->status >= 400;
        }
    } else if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        context->headers.emplace_back(to_lower_copy(trim_copy(line.substr(0, colon))),
                                      trim_copy(line.substr(colon + 1)));
    }
    return total;
}

int curl_progress_callback(void* user,
                           curl_off_t,
                           curl_off_t,
                           curl_off_t,
                           curl_off_t) {
    auto* context = static_cast<CurlWriteContext*>(user);
    if (context->cancel.cancelled()) {
        context->aborted = true;
        return 1;
    }
    return 0;
}

LLMError map_curl_error(CURLcode code) {
    switch (code) {
        case CURLE_OPERATION_TIMEDOUT:
            return make_error(LLMErrorCode::Timeout, "transport timeout");
        case CURLE_COULDNT_CONNECT:
            return make_error(LLMErrorCode::NetworkError, "connection failed");
        case CURLE_COULDNT_RESOLVE_HOST:
            return make_error(LLMErrorCode::NetworkError, "host resolution failed");
        case CURLE_COULDNT_RESOLVE_PROXY:
            return make_error(LLMErrorCode::NetworkError, "proxy resolution failed");
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
            return make_error(LLMErrorCode::NetworkError, "TLS failure");
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
        case CURLE_WRITE_ERROR:
            return make_error(LLMErrorCode::NetworkError, "transport read/write failure");
        default:
            return make_error(LLMErrorCode::NetworkError, "transport failure");
    }
}

std::optional<std::chrono::milliseconds> retry_after_from(
    const std::vector<std::pair<std::string, std::string>>& headers) {
    for (const auto& [name, value] : headers) {
        if (name == "retry-after") {
            return parse_retry_after(value);
        }
    }
    return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------

LLMError map_http_error(int status, std::string_view body) {
    std::string message;
    std::string code;
    if (!body.empty()) {
        const nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
        if (!json.is_discarded() && json.is_object()) {
            if (const auto error = parse_error_object(json)) {
                message = error->first;
                code = error->second;
            }
        }
    }
    return classify_error(status, std::move(message), std::move(code));
}

std::chrono::milliseconds backoff_delay(const RetryPolicy& policy,
                                        std::uint32_t attempt,
                                        double jitter_sample) {
    const double exponent = attempt > 0 ? static_cast<double>(attempt - 1) : 0.0;
    const double base = static_cast<double>(policy.base_delay.count()) * std::pow(2.0, exponent);
    const double capped = std::min(base, static_cast<double>(policy.max_delay.count()));
    const double clamped = std::clamp(jitter_sample, 0.0, 1.0);
    const double factor = 1.0 + policy.jitter * (2.0 * clamped - 1.0);
    const double delay = std::max(0.0, capped * factor);
    return std::chrono::milliseconds{static_cast<std::int64_t>(delay)};
}

std::optional<std::chrono::milliseconds> parse_retry_after(std::string_view value) {
    const std::string text = trim_copy(value);
    if (text.empty()) {
        return std::nullopt;
    }

    const bool all_digits =
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (all_digits) {
        const auto seconds = std::stoll(text);
        return std::chrono::milliseconds{seconds * 1000};
    }

    std::tm tm{};
    std::istringstream stream(text);
    stream >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    if (stream.fail()) {
        return std::nullopt;
    }

    const std::int64_t days =
        days_from_civil(tm.tm_year + 1900, static_cast<unsigned>(tm.tm_mon + 1),
                        static_cast<unsigned>(tm.tm_mday));
    const std::int64_t target =
        days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t delta = std::max<std::int64_t>(0, target - now);
    return std::chrono::milliseconds{delta * 1000};
}

nlohmann::json build_chat_completions_body(const LLMRequest& request,
                                           const ProviderCapabilities& capabilities) {
    nlohmann::json body;
    body["model"] = request.model;

    nlohmann::json messages = nlohmann::json::array();
    for (const Message& message : request.messages) {
        messages.push_back(map_message(message));
    }
    body["messages"] = std::move(messages);

    if (!request.tools.empty()) {
        nlohmann::json tools = nlohmann::json::array();
        for (const ToolSchema& tool : request.tools) {
            tools.push_back(nlohmann::json{
                {"type", "function"},
                {"function",
                 {{"name", tool.name.value},
                  {"description", tool.description},
                  {"parameters", tool.input_schema}}}});
        }
        body["tools"] = std::move(tools);
    }

    body["stream"] = true;
    if (capabilities.usage_streaming) {
        body["stream_options"] = nlohmann::json{{"include_usage", true}};
    }

    const GenerationParameters& parameters = request.parameters;
    if (parameters.temperature.has_value()) {
        body["temperature"] = *parameters.temperature;
    }
    if (parameters.top_p.has_value()) {
        body["top_p"] = *parameters.top_p;
    }
    if (parameters.max_output_tokens.has_value()) {
        body["max_tokens"] = *parameters.max_output_tokens;
    }
    if (!parameters.stop.empty()) {
        body["stop"] = parameters.stop;
    }
    if (parameters.tool_choice.has_value()) {
        body["tool_choice"] = *parameters.tool_choice;
    }
    if (parameters.reasoning_effort.has_value()) {
        body["reasoning_effort"] = *parameters.reasoning_effort;
    }
    if (parameters.seed.has_value()) {
        body["seed"] = *parameters.seed;
    }

    return body;
}

// ---------------------------------------------------------------------------
// CurlHttpTransport
// ---------------------------------------------------------------------------

Task<HttpResponse> CurlHttpTransport::postStream(const HttpRequest& request,
                                                 HttpBodySink on_body,
                                                 CancellationToken cancel) {
    ensure_curl_initialized();

    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        response.transport_error =
            make_error(LLMErrorCode::ProviderInternal, "curl initialization failed");
        return Task<HttpResponse>{std::move(response)};
    }

    CurlWriteContext context;
    context.on_body = std::move(on_body);
    context.cancel = cancel;

    curl_slist* headers = nullptr;
    for (const auto& [name, value] : request.headers) {
        headers = curl_slist_append(headers, (name + ": " + value).c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &context);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS,
                     static_cast<long>(request.connect_timeout.count()));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.total_timeout.count()));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                     static_cast<long>(std::max<std::int64_t>(
                         1, (request.idle_timeout.count() + 999) / 1000)));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode result = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
    response.headers = context.headers;
    response.body = context.body;

    if (result != CURLE_OK) {
        if (context.aborted && cancel.cancelled()) {
            response.transport_error = make_error(LLMErrorCode::Cancelled, "cancelled");
        } else if (context.aborted) {
            response.transport_error =
                make_error(LLMErrorCode::Cancelled, "consumer aborted stream");
        } else {
            response.transport_error = map_curl_error(result);
        }
    } else if (context.aborted) {
        response.transport_error =
            make_error(LLMErrorCode::Cancelled, "consumer aborted stream");
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return Task<HttpResponse>{std::move(response)};
}

// ---------------------------------------------------------------------------
// OpenAICompatibleProvider
// ---------------------------------------------------------------------------

OpenAICompatibleProvider::OpenAICompatibleProvider(LLMProviderConfig config,
                                                   ProviderCapabilities capabilities,
                                                   std::shared_ptr<HttpTransport> transport)
    : config_(std::move(config)),
      capabilities_(capabilities),
      transport_(std::move(transport)) {}

ProviderId OpenAICompatibleProvider::id() const {
    return "openai-compatible";
}

ProviderCapabilities OpenAICompatibleProvider::capabilities() const {
    return capabilities_;
}

std::vector<ModelInfo> OpenAICompatibleProvider::models() const {
    if (config_.model.empty()) {
        return {};
    }
    return {ModelInfo{config_.model, config_.model, 0}};
}

Task<LLMResponse> OpenAICompatibleProvider::stream(const LLMRequest& request,
                                                   StreamSink sink,
                                                   CancellationToken cancel) {
    const auto start_time = std::chrono::steady_clock::now();
    const auto elapsed = [&start_time] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
    };

    const auto failed = [&](LLMError error) -> Task<LLMResponse> {
        error.retryable = is_retryable_code(error.code);
        sink(StreamEvent{StreamError{error}});
        LLMResponse response;
        response.outcome = StreamOutcome::Failed;
        response.finish = FinishReason::Error;
        response.error = error;
        response.request_id = request.request_id;
        response.latency = elapsed();
        return Task<LLMResponse>{std::move(response)};
    };

    const auto cancelled = [&]() -> Task<LLMResponse> {
        LLMResponse response;
        response.outcome = StreamOutcome::Cancelled;
        response.finish = FinishReason::Other;
        response.request_id = request.request_id;
        response.latency = elapsed();
        return Task<LLMResponse>{std::move(response)};
    };

    if (request.model.empty()) {
        return failed(make_error(LLMErrorCode::ConfigError, "empty effective model"));
    }
    if (!request.tools.empty() && !capabilities_.tool_calls) {
        return failed(make_error(LLMErrorCode::UnsupportedModel,
                                 "provider does not support tool calls"));
    }
    if (request.parameters.reasoning_effort.has_value() && !capabilities_.reasoning) {
        return failed(make_error(LLMErrorCode::BadRequest,
                                 "provider does not support reasoning"));
    }

    const std::optional<std::string> api_key = get_env(config_.api_key_env);
    if (!api_key.has_value() || api_key->empty()) {
        return failed(make_error(LLMErrorCode::Auth,
                                 "missing API key in " + config_.api_key_env));
    }

    const nlohmann::json body = build_chat_completions_body(request, capabilities_);
    const std::string payload = body.dump();
    const std::string url = join_url(config_.base_url, "/chat/completions");

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "application/json");
    headers.emplace_back("Accept", "text/event-stream");
    headers.emplace_back("Authorization", "Bearer " + *api_key);
    headers.emplace_back("Expect", "");
    for (const auto& header : config_.headers) {
        headers.push_back(header);
    }

    const std::uint32_t max_attempts = std::max<std::uint32_t>(1, config_.retry.max_attempts);

    for (std::uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
        if (cancel.cancelled()) {
            return cancelled();
        }

        OpenAiStreamDecoder decoder(sink, capabilities_, config_.max_arguments_bytes,
                                    config_.sse_line_bytes);

        HttpRequest http;
        http.method = "POST";
        http.url = url;
        http.headers = headers;
        http.body = payload;
        http.connect_timeout = config_.connect_timeout;
        http.idle_timeout = config_.idle_timeout;
        http.total_timeout = effective_timeout(request.deadline, config_.request_timeout);

        HttpResponse response = transport_
                                    ->postStream(
                                        http,
                                        [&decoder](const char* data, std::size_t len) {
                                            return decoder.on_bytes(data, len);
                                        },
                                        cancel)
                                    .get();

        if (cancel.cancelled() || decoder.stop_requested()) {
            return cancelled();
        }

        LLMError error;
        bool have_error = false;
        if (decoder.terminal_error().code != LLMErrorCode::None) {
            error = decoder.terminal_error();
            have_error = true;
        } else if (response.transport_error.code != LLMErrorCode::None) {
            error = response.transport_error;
            have_error = true;
        }

        if (!have_error && response.status >= 200 && response.status < 300) {
            decoder.finish_stream();
            if (decoder.terminal_error().code != LLMErrorCode::None) {
                error = decoder.terminal_error();
                have_error = true;
            } else {
                LLMResponse decoded = decoder.finalize();
                decoded.request_id = request.request_id;
                decoded.latency = elapsed();
                return Task<LLMResponse>{std::move(decoded)};
            }
        }

        if (!have_error) {
            error = map_http_error(response.status, response.body);
            have_error = true;
        }

        if (is_retryable_code(error.code) && !decoder.any_event() && attempt < max_attempts) {
            std::chrono::milliseconds delay =
                backoff_delay(config_.retry, attempt, next_jitter_sample());
            if (error.code == LLMErrorCode::RateLimited && config_.retry.honor_retry_after) {
                if (const auto retry_after = retry_after_from(response.headers)) {
                    delay = *retry_after;
                }
            }
            if (!sleep_with_cancel(delay, cancel)) {
                return cancelled();
            }
            continue;
        }

        return failed(error);
    }

    return failed(make_error(LLMErrorCode::ProviderInternal, "retry budget exhausted"));
}

} // namespace ymh
