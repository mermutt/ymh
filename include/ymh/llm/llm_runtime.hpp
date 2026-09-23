#pragma once

// The provider-neutral call service (`LlmRuntime`) and the deep-frozen request
// envelope, pinned by 28-llm-service-boundary-errata.md §3, §4, §7, §8
// (26-dsh-alignment-part2.md §4.3.1). The agent loop holds an `LlmRuntime&`,
// never a raw `LLMProvider*` (L18). `LlmCallConfig`/`CallPurpose`/
// `call_config_equals` are owned by `llm_call_config.hpp`.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/core/cancellation.hpp"
#include "ymh/core/task.hpp"
#include "ymh/llm/llm_call_config.hpp"
#include "ymh/llm/llm_provider.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/stream.hpp"

namespace ymh {

class LlmRuntime;

// The canonical-serialization schema version (28 §4.2, gate28 LOW-4 pin).
// Independent of the session-DB `kSchemaVersion` and the wire
// `kProtocolVersion`; changes only when the template field set or the
// canonicalization rules re-base every `template_digest`.
inline constexpr int kTemplateSchemaVersion = 1;

// SHA-256 hex digest (lowercase). Shared by the template/tool/prompt digests
// (28 §4.2). Implemented in `src/llm/sha256.cpp`.
[[nodiscard]] std::string sha256_hex(std::string_view data);

// The canonical JSON object for one tool schema (`name`, `description`,
// `parameters` with recursively sorted object keys; 28 §4.2). `parameters` is
// sourced from `ToolSchema::input_schema`. Shared by `canonical_template()`
// and the per-tool header digests.
[[nodiscard]] nlohmann::json canonical_tool_schema(const ToolSchema& schema);
[[nodiscard]] std::string     tool_schema_digest(const ToolSchema& schema);

// Immutable, detached request envelope. `freeze` deep-copies and pins the
// canonical serialization; nothing may mutate after freeze (26-D3, 26-F2).
// The members are `const`, so immutability is enforced by type.
class FrozenRequest {
public:
    [[nodiscard]] static FrozenRequest freeze(LLMRequest request, LlmCallConfig config);

    [[nodiscard]] const LLMRequest&    get() const noexcept { return request_; }
    [[nodiscard]] const LlmCallConfig& config() const noexcept { return config_; }

    // Canonical serialization (28 §4.2): `schema_version`, `envelope`, `config`,
    // `system_prompt`, `tools`.
    [[nodiscard]] std::string canonical_template() const;
    [[nodiscard]] std::string template_digest() const;  // SHA-256 hex of canonical_template()
    // The same object plus the ordered `messages` (28 §4.3).
    [[nodiscard]] std::string canonical_json() const;
    [[nodiscard]] std::string digest() const;  // SHA-256 hex of canonical_json()

private:
    FrozenRequest(LLMRequest request, LlmCallConfig config);

    const LLMRequest    request_;
    const LlmCallConfig config_;
};

// A `PreparedCall` misuse — a second dispatch or a config mismatch between the
// `FrozenRequest` and the `PreparedCall` — throws this (28 §7). It is raised
// above the provider seam and is normalized at the loop boundary.
struct PreparedCallError {
    enum class Reason : std::uint8_t {
        Consumed,
        ConfigMismatch,
    };

    LLMErrorCode code = LLMErrorCode::InvalidPreparedCall;
    Reason       reason = Reason::Consumed;
    std::string  detail;
};

// The carrier for the §3.5 route-lookup failure. It is not a `PreparedCall`
// misuse: no `PreparedCall` is ever produced. Thrown by `prepare_call` /
// `LlmRuntime::stream`.
struct NoProviderRouteError {
    LLMErrorCode code = LLMErrorCode::NoProviderRoute;
    ProviderId   provider;  // the requested id; empty => no default route exists
    std::string  detail = "no route for config.provider";
};

// One model call whose config and adapter registration were resolved together.
// One-shot: `stream` consumes the object; a second call or a config mismatch
// throws `PreparedCallError` (28 §3.1, L24).
class PreparedCall {
public:
    PreparedCall(PreparedCall&&) noexcept = default;
    PreparedCall& operator=(PreparedCall&&) noexcept = default;
    PreparedCall(const PreparedCall&) = delete;
    PreparedCall& operator=(const PreparedCall&) = delete;

    [[nodiscard]] const LlmCallConfig& config() const noexcept { return config_; }
    [[nodiscard]] const RetryPolicy&   retry_policy() const noexcept { return retry_; }

    [[nodiscard]] Task<LLMResponse> stream(FrozenRequest request,
                                           StreamSink sink,
                                           CancellationToken cancel);

private:
    friend class LlmRuntime;

    PreparedCall(std::shared_ptr<LLMProvider> adapter, RetryPolicy retry, LlmCallConfig config)
        : adapter_(std::move(adapter)), retry_(std::move(retry)), config_(std::move(config)) {}

    std::shared_ptr<LLMProvider> adapter_;
    RetryPolicy                  retry_;
    LlmCallConfig                config_;
    bool                         consumed_ = false;
};

struct ProviderInfo {
    ProviderId           id;
    std::vector<ModelId> models;
    ProviderCapabilities capabilities;
};

// RAII; move-only; destructor unregisters the routes (28 §3.1).
class AdapterHandle {
public:
    AdapterHandle(AdapterHandle&& other) noexcept;
    AdapterHandle& operator=(AdapterHandle&& other) noexcept;
    ~AdapterHandle();
    AdapterHandle(const AdapterHandle&) = delete;
    AdapterHandle& operator=(const AdapterHandle&) = delete;

private:
    friend class LlmRuntime;

    AdapterHandle(LlmRuntime* runtime, std::uint64_t generation) noexcept
        : runtime_(runtime), generation_(generation) {}

    LlmRuntime*   runtime_ = nullptr;
    std::uint64_t generation_ = 0;
};

// The `llm/stream` waterfall analogue: a registered interceptor wraps the next
// call. Registration returns an RAII handle; ordering is registration order.
using StreamNext = std::function<Task<LLMResponse>(const FrozenRequest&,
                                                   StreamSink,
                                                   CancellationToken)>;
using StreamInterceptor = std::function<Task<LLMResponse>(const FrozenRequest&,
                                                          StreamSink,
                                                          CancellationToken,
                                                          StreamNext)>;

class InterceptorHandle {
public:
    InterceptorHandle(InterceptorHandle&& other) noexcept;
    InterceptorHandle& operator=(InterceptorHandle&& other) noexcept;
    ~InterceptorHandle();
    InterceptorHandle(const InterceptorHandle&) = delete;
    InterceptorHandle& operator=(const InterceptorHandle&) = delete;

private:
    friend class LlmRuntime;

    InterceptorHandle(LlmRuntime* runtime, std::uint64_t id) noexcept
        : runtime_(runtime), id_(id) {}

    LlmRuntime*   runtime_ = nullptr;
    std::uint64_t id_ = 0;
};

// The provider-neutral service. The loop holds an `LlmRuntime&`, not an
// `LLMProvider*`. Adapters are owned by `shared_ptr`; the runtime keeps a
// registration alive until every in-flight dispatch that captured it has
// finished (28 §3.3). Registry mutation is mutex-guarded; `stream`/
// `prepare_call` copy the selected adapter's `shared_ptr` under the lock and
// release it before dispatching.
// 54-D1: typed, profile-aware route identity. Provider-kind keys use
// `profile_id == ""`; Endpoint-kind keys are `{Endpoint, endpoint_name,
// profile_id}`. Typed (not `"@" + name`) so a provider id literally named `@foo`
// cannot collide with endpoint `foo`.
enum class RouteKind : std::uint8_t { Provider, Endpoint };
struct RouteKey {
    RouteKind   kind = RouteKind::Provider;
    std::string name;
    std::string profile_id;

    friend bool operator==(const RouteKey&, const RouteKey&) = default;
};

// 54-D1: injected lazy factory. `endpoint_name` is a named endpoint (never "");
// `profile_id` is the selected model's profile id ("" = inert). Returns nullptr
// on a construction failure; `resolve_adapter` then fails loud. The resolver owns
// the check-build-insert (under its own cache mutex) and retains the
// `AdapterHandle`, so `resolve_adapter` re-reads the registered route afterwards.
using RouteResolver = std::function<std::shared_ptr<LLMProvider>(
    std::string_view endpoint_name, std::string_view profile_id)>;

class LlmRuntime {
public:
    LlmRuntime() = default;
    ~LlmRuntime();

    LlmRuntime(const LlmRuntime&) = delete;
    LlmRuntime& operator=(const LlmRuntime&) = delete;

    // Registers `adapter` under `routes`. Contract: `adapter` must be non-null
    // (throws `std::invalid_argument` otherwise); every registered route
    // therefore resolves to a live adapter (28 §3.3). Each id maps to
    // `RouteKey{Provider, id, ""}` (54-D2).
    AdapterHandle register_adapter(std::vector<ProviderId> routes,
                                   std::shared_ptr<LLMProvider> adapter);

    // 54-D1: registers a named-endpoint route keyed by the `(endpoint, profile)`
    // pair. The caller retains the returned `AdapterHandle` (the route is
    // unregistered when the handle drops).
    AdapterHandle register_endpoint_route(std::string_view             endpoint_name,
                                          std::string_view             profile_id,
                                          std::shared_ptr<LLMProvider> adapter);

    // 54-I13: one-shot. A second call throws and leaves `resolver_` unchanged.
    void set_route_resolver(RouteResolver resolver);
    [[nodiscard]] std::vector<ProviderInfo> list_providers() const;
    InterceptorHandle add_stream_interceptor(StreamInterceptor interceptor);

    // Resolves `config.provider` (empty => the runtime's registered default
    // route) and binds one adapter generation. Throws `NoProviderRouteError`
    // when no route matches (28 §3.5, L-F27). The input config is stored
    // verbatim; `config.provider` is never back-filled.
    [[nodiscard]] Task<PreparedCall> prepare_call(LlmCallConfig config,
                                                  CancellationToken cancel) const;

    // The public interceptor-chain entry for callers that already hold a
    // `FrozenRequest` and need no one-shot generation binding (28 §3.1).
    [[nodiscard]] Task<LLMResponse> stream(const FrozenRequest& request,
                                           StreamSink sink,
                                           CancellationToken cancel);

private:
    friend class AdapterHandle;
    friend class InterceptorHandle;

    struct Registration {
        std::vector<RouteKey>        routes;
        std::shared_ptr<LLMProvider> adapter;
        RetryPolicy                  retry;
        std::uint64_t                generation = 0;
    };

    // 54-D3: endpoint-first per-request selection. A non-empty `config.endpoint`
    // resolves ONLY through the Endpoint-kind route or the injected resolver (no
    // fallback); an empty `config.endpoint` uses the Provider-kind
    // `config.provider` route / default route. Throws `NoProviderRouteError` on
    // a miss.
    [[nodiscard]] std::shared_ptr<LLMProvider> resolve_adapter(const LlmCallConfig& config,
                                                               RetryPolicy& out_retry) const;

    void remove_generation(std::uint64_t generation) noexcept;
    void remove_interceptor(std::uint64_t id) noexcept;

    mutable std::mutex         mutex_;
    std::vector<Registration>  registrations_;
    std::uint64_t              next_generation_ = 1;
    std::vector<std::pair<std::uint64_t, StreamInterceptor>> interceptors_;
    std::optional<RouteResolver> resolver_;
    std::uint64_t             next_interceptor_ = 1;
};

} // namespace ymh
