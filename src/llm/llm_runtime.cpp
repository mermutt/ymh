#include "ymh/llm/llm_runtime.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace ymh {
namespace {

std::string system_prompt_text(const LLMRequest& request) {
    if (request.messages.empty() || request.messages.front().role != Role::System) {
        return {};
    }
    std::string text;
    for (const ContentBlock& block : request.messages.front().content) {
        if (block.kind == ContentBlockKind::Text) {
            text += block.text;
        }
    }
    return text;
}

nlohmann::json template_json(const LLMRequest& request, const LlmCallConfig& config) {
    nlohmann::json json;
    json["schema_version"] = kTemplateSchemaVersion;

    nlohmann::json envelope;
    envelope["session_id"] = request.session_id.value;
    if (request.purpose.has_value()) {
        envelope["purpose"] = std::string{call_purpose_name(*request.purpose)};
    }
    json["envelope"] = std::move(envelope);

    json["config"]        = config;
    json["system_prompt"] = system_prompt_text(request);

    nlohmann::json tools = nlohmann::json::array();
    for (const ToolSchema& schema : request.tools) {
        tools.push_back(canonical_tool_schema(schema));
    }
    json["tools"] = std::move(tools);
    return json;
}

nlohmann::json messages_json(const LLMRequest& request) {
    nlohmann::json messages = nlohmann::json::array();
    for (const Message& message : request.messages) {
        nlohmann::json encoded;
        encoded["role"] = std::string{role_name(message.role)};

        nlohmann::json blocks = nlohmann::json::array();
        for (const ContentBlock& block : message.content) {
            nlohmann::json encoded_block;
            encoded_block["kind"] = std::string{content_block_kind_name(block.kind)};
            switch (block.kind) {
                case ContentBlockKind::Text:
                case ContentBlockKind::Reasoning:
                    encoded_block["text"] = block.text;
                    break;
                case ContentBlockKind::ToolUse:
                    encoded_block["tool_call_id"] = block.tool_call_id;
                    encoded_block["tool_name"]    = block.tool_name;
                    encoded_block["arguments"]    = block.arguments.dump();
                    break;
                case ContentBlockKind::Image:
                    encoded_block["media_type"] = block.media_type;
                    encoded_block["data"]       = block.data;
                    break;
            }
            blocks.push_back(std::move(encoded_block));
        }
        encoded["content"] = std::move(blocks);
        if (message.role == Role::Tool) {
            encoded["tool_call_id"] = message.tool_call_id;
        }
        messages.push_back(std::move(encoded));
    }
    return messages;
}

LLMResponse run_adapter(const std::shared_ptr<LLMProvider>& adapter,
                        const FrozenRequest&                request,
                        StreamSink                          sink,
                        CancellationToken                   cancel) {
    bool terminal_seen = false;
    StreamSink wrapped = [&sink, &terminal_seen](const StreamEvent& event) -> SinkFlow {
        if (std::holds_alternative<Finished>(event) ||
            std::holds_alternative<StreamError>(event)) {
            terminal_seen = true;
        }
        return sink(event);
    };

    LLMResponse response;
    try {
        response = adapter->stream(request.get(), std::move(wrapped), cancel).get();
    } catch (const std::exception& error) {
        response                = LLMResponse{};
        response.outcome        = StreamOutcome::Failed;
        response.finish         = FinishReason::Error;
        response.error.code     = LLMErrorCode::ProviderInternal;
        response.error.detail   = error.what();
        return response;
    } catch (...) {
        response              = LLMResponse{};
        response.outcome      = StreamOutcome::Failed;
        response.finish       = FinishReason::Error;
        response.error.code   = LLMErrorCode::ProviderInternal;
        response.error.detail = "adapter threw";
        return response;
    }

    if (response.outcome == StreamOutcome::Completed && !terminal_seen) {
        response              = LLMResponse{};
        response.outcome      = StreamOutcome::Failed;
        response.finish       = FinishReason::Error;
        response.error.code   = LLMErrorCode::MalformedResponse;
        response.error.detail = "adapter returned without a terminal event";
    }
    return response;
}

} // namespace

bool call_config_equals(const LlmCallConfig& left, const LlmCallConfig& right) noexcept {
    return left.provider == right.provider && left.endpoint == right.endpoint &&
           left.profile_id == right.profile_id && left.model == right.model &&
           left.reasoning_effort == right.reasoning_effort &&
           left.temperature == right.temperature && left.max_tokens == right.max_tokens &&
           left.stop == right.stop && left.top_p == right.top_p && left.top_k == right.top_k &&
           left.seed == right.seed && left.tool_choice == right.tool_choice;
}

nlohmann::json canonical_tool_schema(const ToolSchema& schema) {
    return nlohmann::json{
        {"name", schema.name.value},
        {"description", schema.description},
        {"parameters", schema.input_schema},
    };
}

std::string tool_schema_digest(const ToolSchema& schema) {
    return sha256_hex(canonical_tool_schema(schema).dump());
}

FrozenRequest::FrozenRequest(LLMRequest request, LlmCallConfig config)
    : request_(std::move(request)), config_(std::move(config)) {}

FrozenRequest FrozenRequest::freeze(LLMRequest request, LlmCallConfig config) {
    return FrozenRequest{std::move(request), std::move(config)};
}

std::string FrozenRequest::canonical_template() const {
    return template_json(request_, config_).dump();
}

std::string FrozenRequest::template_digest() const {
    return sha256_hex(canonical_template());
}

std::string FrozenRequest::canonical_json() const {
    nlohmann::json json = template_json(request_, config_);
    json["messages"]    = messages_json(request_);
    return json.dump();
}

std::string FrozenRequest::digest() const {
    return sha256_hex(canonical_json());
}

Task<LLMResponse> PreparedCall::stream(FrozenRequest   request,
                                       StreamSink      sink,
                                       CancellationToken cancel) {
    if (consumed_) {
        throw PreparedCallError{LLMErrorCode::InvalidPreparedCall,
                                PreparedCallError::Reason::Consumed,
                                "PreparedCall already consumed"};
    }
    if (!call_config_equals(request.config(), config_)) {
        throw PreparedCallError{LLMErrorCode::InvalidPreparedCall,
                                PreparedCallError::Reason::ConfigMismatch,
                                "FrozenRequest config does not match PreparedCall config"};
    }
    consumed_ = true;
    return Task<LLMResponse>{run_adapter(adapter_, request, std::move(sink), cancel)};
}

AdapterHandle::AdapterHandle(AdapterHandle&& other) noexcept
    : runtime_(other.runtime_), generation_(other.generation_) {
    other.runtime_    = nullptr;
    other.generation_ = 0;
}

AdapterHandle& AdapterHandle::operator=(AdapterHandle&& other) noexcept {
    if (this != &other) {
        if (runtime_ != nullptr) {
            runtime_->remove_generation(generation_);
        }
        runtime_    = other.runtime_;
        generation_ = other.generation_;
        other.runtime_    = nullptr;
        other.generation_ = 0;
    }
    return *this;
}

AdapterHandle::~AdapterHandle() {
    if (runtime_ != nullptr) {
        runtime_->remove_generation(generation_);
    }
}

InterceptorHandle::InterceptorHandle(InterceptorHandle&& other) noexcept
    : runtime_(other.runtime_), id_(other.id_) {
    other.runtime_ = nullptr;
    other.id_      = 0;
}

InterceptorHandle& InterceptorHandle::operator=(InterceptorHandle&& other) noexcept {
    if (this != &other) {
        if (runtime_ != nullptr) {
            runtime_->remove_interceptor(id_);
        }
        runtime_ = other.runtime_;
        id_      = other.id_;
        other.runtime_ = nullptr;
        other.id_      = 0;
    }
    return *this;
}

InterceptorHandle::~InterceptorHandle() {
    if (runtime_ != nullptr) {
        runtime_->remove_interceptor(id_);
    }
}

LlmRuntime::~LlmRuntime() = default;

AdapterHandle LlmRuntime::register_adapter(std::vector<ProviderId>      routes,
                                           std::shared_ptr<LLMProvider> adapter) {
    if (adapter == nullptr) {
        throw std::invalid_argument("register_adapter requires a non-null adapter");
    }
    // Call the virtual before taking mutex_: a provider whose retry_policy()
    // re-enters the runtime must not self-deadlock on the non-recursive lock.
    const RetryPolicy retry = adapter->retry_policy();

    std::vector<RouteKey> keys;
    keys.reserve(routes.size());
    for (ProviderId& id : routes) {
        keys.push_back(RouteKey{RouteKind::Provider, std::move(id), std::string{}});
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t         generation = next_generation_++;
    Registration                registration;
    registration.routes     = std::move(keys);
    registration.adapter    = std::move(adapter);
    registration.retry      = retry;
    registration.generation = generation;
    registrations_.push_back(std::move(registration));
    return AdapterHandle{this, generation};
}

AdapterHandle LlmRuntime::register_endpoint_route(std::string_view             endpoint_name,
                                                  std::string_view             profile_id,
                                                  std::shared_ptr<LLMProvider> adapter) {
    if (adapter == nullptr) {
        throw std::invalid_argument("register_endpoint_route requires a non-null adapter");
    }
    const RetryPolicy retry = adapter->retry_policy();

    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t         generation = next_generation_++;
    Registration                registration;
    registration.routes.push_back(
        RouteKey{RouteKind::Endpoint, std::string{endpoint_name}, std::string{profile_id}});
    registration.adapter    = std::move(adapter);
    registration.retry      = retry;
    registration.generation = generation;
    registrations_.push_back(std::move(registration));
    return AdapterHandle{this, generation};
}

void LlmRuntime::set_route_resolver(RouteResolver resolver) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (resolver_.has_value()) {
        throw std::logic_error("set_route_resolver may be called at most once");
    }
    resolver_ = std::move(resolver);
}

std::vector<ProviderInfo> LlmRuntime::list_providers() const {
    std::vector<std::shared_ptr<LLMProvider>> adapters;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        adapters.reserve(registrations_.size());
        for (const Registration& registration : registrations_) {
            adapters.push_back(registration.adapter);
        }
    }

    // Virtual calls happen after mutex_ is released (LOW-3): a provider method
    // that re-enters the runtime cannot self-deadlock here.
    std::vector<ProviderInfo> providers;
    providers.reserve(adapters.size());
    for (const std::shared_ptr<LLMProvider>& adapter : adapters) {
        ProviderInfo info;
        info.id           = adapter->id();
        info.capabilities = adapter->capabilities();
        for (const ModelInfo& model : adapter->models()) {
            info.models.push_back(model.id);
        }
        providers.push_back(std::move(info));
    }
    return providers;
}

InterceptorHandle LlmRuntime::add_stream_interceptor(StreamInterceptor interceptor) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t         id = next_interceptor_++;
    interceptors_.emplace_back(id, std::move(interceptor));
    return InterceptorHandle{this, id};
}

Task<PreparedCall> LlmRuntime::prepare_call(LlmCallConfig config, CancellationToken) const {
    RetryPolicy                  retry;
    std::shared_ptr<LLMProvider> adapter = resolve_adapter(config, retry);
    return Task<PreparedCall>{
        PreparedCall{std::move(adapter), std::move(retry), std::move(config)}};
}

Task<LLMResponse> LlmRuntime::stream(const FrozenRequest& request,
                                     StreamSink          sink,
                                     CancellationToken   cancel) {
    RetryPolicy                  retry;
    std::shared_ptr<LLMProvider> adapter = resolve_adapter(request.config(), retry);

    std::vector<StreamInterceptor> chain;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chain.reserve(interceptors_.size());
        for (const auto& entry : interceptors_) {
            chain.push_back(entry.second);
        }
    }

    StreamNext next = [adapter](const FrozenRequest& frozen,
                                StreamSink           next_sink,
                                CancellationToken    next_cancel) {
        return Task<LLMResponse>{run_adapter(adapter, frozen, std::move(next_sink), next_cancel)};
    };
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        StreamInterceptor interceptor = *it;
        StreamNext        inner       = next;
        next = [interceptor, inner](const FrozenRequest& frozen,
                                    StreamSink           next_sink,
                                    CancellationToken    next_cancel) {
            return interceptor(frozen, std::move(next_sink), next_cancel, inner);
        };
    }
    return next(request, std::move(sink), cancel);
}

std::shared_ptr<LLMProvider> LlmRuntime::resolve_adapter(const LlmCallConfig& config,
                                                         RetryPolicy& out_retry) const {
    if (!config.endpoint.empty()) {
        const RouteKey wanted{RouteKind::Endpoint, config.endpoint, config.profile_id};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const Registration& registration : registrations_) {
                if (std::find(registration.routes.begin(), registration.routes.end(), wanted) !=
                    registration.routes.end()) {
                    out_retry = registration.retry;
                    return registration.adapter;
                }
            }
        }

        std::optional<RouteResolver> resolver;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            resolver = resolver_;
        }
        if (resolver.has_value()) {
            const std::shared_ptr<LLMProvider> built =
                (*resolver)(config.endpoint, config.profile_id);
            if (built != nullptr) {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const Registration& registration : registrations_) {
                    if (std::find(registration.routes.begin(), registration.routes.end(), wanted) !=
                        registration.routes.end()) {
                        out_retry = registration.retry;
                        return registration.adapter;
                    }
                }
            }
        }

        throw NoProviderRouteError{LLMErrorCode::NoProviderRoute, config.provider,
                                   "no route for config.endpoint"};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const Registration*         match = nullptr;

    if (!config.provider.empty()) {
        const RouteKey wanted{RouteKind::Provider, config.provider, std::string{}};
        for (const Registration& registration : registrations_) {
            if (std::find(registration.routes.begin(), registration.routes.end(), wanted) !=
                registration.routes.end()) {
                match = &registration;
                break;
            }
        }
    } else {
        for (const Registration& registration : registrations_) {
            const auto route = std::find_if(
                registration.routes.begin(), registration.routes.end(),
                [](const RouteKey& candidate) {
                    return candidate.kind == RouteKind::Provider && !candidate.name.empty();
                });
            if (route != registration.routes.end()) {
                match = &registration;
                break;
            }
        }
    }

    if (match == nullptr) {
        throw NoProviderRouteError{LLMErrorCode::NoProviderRoute, config.provider,
                                   "no route for config.provider"};
    }
    out_retry = match->retry;
    return match->adapter;
}

void LlmRuntime::remove_generation(std::uint64_t generation) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    registrations_.erase(
        std::remove_if(registrations_.begin(), registrations_.end(),
                       [generation](const Registration& registration) {
                           return registration.generation == generation;
                       }),
        registrations_.end());
}

void LlmRuntime::remove_interceptor(std::uint64_t id) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    interceptors_.erase(std::remove_if(interceptors_.begin(), interceptors_.end(),
                                       [id](const auto& entry) { return entry.first == id; }),
                        interceptors_.end());
}

} // namespace ymh
