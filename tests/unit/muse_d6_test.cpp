#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/openai_adapter.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/llm/tool_call_assembler.hpp"

namespace {

using namespace ymh;

std::string frame(const nlohmann::json& json) {
    return "data: " + json.dump() + "\n\n";
}

std::string done_frame() {
    return "data: [DONE]\n\n";
}

nlohmann::json finish_frame(const std::string& reason) {
    nlohmann::json choice;
    choice["index"]         = 0;
    choice["delta"]         = nlohmann::json::object();
    choice["finish_reason"] = reason;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
}

nlohmann::json tool_start_frame(std::uint32_t index, const std::string& id,
                                const std::string& name) {
    nlohmann::json function;
    function["name"]      = name;
    function["arguments"] = "";
    nlohmann::json tool_call;
    tool_call["index"]    = index;
    tool_call["id"]       = id;
    tool_call["function"] = function;
    nlohmann::json choice;
    choice["index"]         = 0;
    choice["delta"]         = nlohmann::json{{"tool_calls", nlohmann::json::array({tool_call})}};
    choice["finish_reason"] = nullptr;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
}

nlohmann::json tool_args_frame(std::uint32_t index, const nlohmann::json& arguments) {
    nlohmann::json function;
    function["arguments"] = arguments;
    nlohmann::json tool_call;
    tool_call["index"]    = index;
    tool_call["function"] = function;
    nlohmann::json choice;
    choice["index"]         = 0;
    choice["delta"]         = nlohmann::json{{"tool_calls", nlohmann::json::array({tool_call})}};
    choice["finish_reason"] = nullptr;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
}

std::string sse_body(std::initializer_list<nlohmann::json> frames) {
    std::string body;
    for (const auto& item : frames) {
        body += frame(item);
    }
    body += done_frame();
    return body;
}

class FakeTransport final : public HttpTransport {
public:
    explicit FakeTransport(std::string body) : body_(std::move(body)) {}

    Task<HttpResponse> postStream(const HttpRequest& request, HttpBodySink on_body,
                                  CancellationToken) override {
        requests.push_back(request);
        for (std::size_t offset = 0; offset < body_.size(); offset += 4096) {
            const std::size_t length = std::min<std::size_t>(4096, body_.size() - offset);
            if (!on_body(body_.data() + offset, length)) {
                break;
            }
        }
        HttpResponse response;
        response.status = 200;
        return Task<HttpResponse>{std::move(response)};
    }

    std::vector<HttpRequest> requests;

private:
    std::string body_;
};

ToolSchema tool_schema(std::string name) {
    ToolSchema schema;
    schema.name.value   = std::move(name);
    schema.description  = "test tool";
    schema.input_schema = nlohmann::json{{"type", "object"}};
    return schema;
}

LLMRequest tool_request(std::vector<ToolSchema> tools) {
    LLMRequest request;
    request.model = "test-model";
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = "go";
    message.content.push_back(std::move(block));
    request.messages.push_back(std::move(message));
    request.tools = std::move(tools);
    return request;
}

LLMProviderConfig muse_provider_config() {
    LLMProviderConfig config;
    config.provider    = "openai-compatible";
    config.base_url    = "http://127.0.0.1:9/v1";
    config.model       = "test-model";
    config.api_key     = std::string{"test-key"};
    config.api_key_env = "YMH_TEST_API_KEY";
    const ModelProfile* profile = find_model_profile("muse-glimmer");
    if (profile != nullptr) {
        config.profile = *profile;
    }
    return config;
}

LLMResponse run(LLMProvider& provider, const LLMRequest& request) {
    return provider.stream(request, [](const StreamEvent&) { return SinkFlow::Continue; }, {}).get();
}

} // namespace

TEST(MuseD6, FinalizeNonObjectStringBecomesEmpty) {
    for (const char* raw : {"[]", "null", "42", "\"x\""}) {
        ToolCallAssembler assembler(1024, ToolArgumentPolicy::NonObjectToEmpty);
        assembler.onStarted(0, "call_1", "read_file");
        assembler.onDelta(0, raw);
        const std::optional<ToolCallAssembled> finished = assembler.onFinished(0);
        ASSERT_TRUE(finished.has_value()) << raw;
        EXPECT_TRUE(finished->arguments.is_object()) << raw;
        EXPECT_TRUE(finished->arguments.empty()) << raw;
        EXPECT_FALSE(assembler.has_error()) << raw;
    }
}

TEST(MuseD6, FinalizeUnparseableBecomesEmpty) {
    ToolCallAssembler assembler(1024, ToolArgumentPolicy::NonObjectToEmpty);
    assembler.onStarted(0, "call_1", "read_file");
    assembler.onDelta(0, "{not json");
    const std::optional<ToolCallAssembled> finished = assembler.onFinished(0);
    ASSERT_TRUE(finished.has_value());
    EXPECT_TRUE(finished->arguments.is_object());
    EXPECT_TRUE(finished->arguments.empty());
    EXPECT_FALSE(assembler.has_error());
}

TEST(MuseD6, StrictPolicyUnchanged) {
    ToolCallAssembler assembler(1024, ToolArgumentPolicy::Strict);
    assembler.onStarted(0, "call_1", "read_file");
    assembler.onDelta(0, "[]");
    EXPECT_FALSE(assembler.onFinished(0).has_value());
    EXPECT_EQ(assembler.error().code, LLMErrorCode::MalformedToolCall);
}

TEST(MuseD6, ObjectArgumentsArePreserved) {
    ToolCallAssembler assembler(1024, ToolArgumentPolicy::NonObjectToEmpty);
    assembler.onStarted(0, "call_1", "read_file");
    assembler.onDelta(0, "{\"path\":\"a\"}");
    const std::optional<ToolCallAssembled> finished = assembler.onFinished(0);
    ASSERT_TRUE(finished.has_value());
    EXPECT_EQ(finished->arguments, nlohmann::json({{"path", "a"}}));
}

TEST(MuseD6, NoProfileNonObjectStillErrors) {
    ToolCallAssembler assembler(1024);
    assembler.onStarted(0, "call_1", "read_file");
    assembler.onDelta(0, "null");
    EXPECT_FALSE(assembler.onFinished(0).has_value());
    EXPECT_EQ(assembler.error().code, LLMErrorCode::MalformedToolCall);
}

TEST(MuseD6, EchoBackNullArgumentsIsEmptyObject) {
    LLMRequest request;
    request.model = "m";
    Message message;
    message.role = Role::Assistant;
    ContentBlock block;
    block.kind         = ContentBlockKind::ToolUse;
    block.tool_call_id = "call_1";
    block.tool_name    = "read_file";
    block.arguments    = nlohmann::json();
    message.content.push_back(std::move(block));
    request.messages.push_back(std::move(message));

    const nlohmann::json body =
        build_chat_completions_body(request, openai_compatible_capabilities());
    ASSERT_EQ(body["messages"].size(), 1u);
    ASSERT_EQ(body["messages"][0]["tool_calls"].size(), 1u);
    EXPECT_EQ(body["messages"][0]["tool_calls"][0]["function"]["arguments"], "{}");
}

TEST(MuseAdapter, ObjectArgumentsWholeDeltaIsNormalized) {
    const std::string body = sse_body(
        {tool_start_frame(0, "call_1", "read_file"),
         tool_args_frame(0, nlohmann::json{{"path", "a"}}), finish_frame("tool_calls")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const LLMResponse response = run(provider, tool_request({tool_schema("read_file")}));
    EXPECT_EQ(response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(response.tool_calls.size(), 1u);
    EXPECT_EQ(response.tool_calls[0].arguments, nlohmann::json({{"path", "a"}}));
}

TEST(MuseAdapter, StreamedFragmentsAreNotCorrupted) {
    const std::string body = sse_body(
        {tool_start_frame(0, "call_1", "read_file"), tool_args_frame(0, "{\"pa"),
         tool_args_frame(0, "th\":\"a"), tool_args_frame(0, ".txt\"}"),
         finish_frame("tool_calls")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const LLMResponse response = run(provider, tool_request({tool_schema("read_file")}));
    EXPECT_EQ(response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(response.tool_calls.size(), 1u);
    EXPECT_EQ(response.tool_calls[0].arguments, nlohmann::json({{"path", "a.txt"}}));
}

TEST(MuseAdapter, NonObjectStringNormalizedAtFinalize) {
    const std::string body = sse_body(
        {tool_start_frame(0, "call_1", "read_file"), tool_args_frame(0, "["),
         tool_args_frame(0, "]"), finish_frame("tool_calls")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const LLMResponse response = run(provider, tool_request({tool_schema("read_file")}));
    EXPECT_EQ(response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(response.tool_calls.size(), 1u);
    EXPECT_TRUE(response.tool_calls[0].arguments.is_object());
    EXPECT_TRUE(response.tool_calls[0].arguments.empty());
}
