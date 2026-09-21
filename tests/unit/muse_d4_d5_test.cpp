#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/llm/leaked_call_detector.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/llm/openai_adapter.hpp"
#include "ymh/llm/provider_registry.hpp"

namespace {

using namespace ymh;

std::string frame(const nlohmann::json& json) {
    return "data: " + json.dump() + "\n\n";
}

std::string done_frame() {
    return "data: [DONE]\n\n";
}

nlohmann::json content_frame(const std::string& text) {
    nlohmann::json choice;
    choice["index"]         = 0;
    choice["delta"]         = nlohmann::json{{"content", text}};
    choice["finish_reason"] = nullptr;
    nlohmann::json chunk;
    chunk["choices"] = nlohmann::json::array({choice});
    return chunk;
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

struct RunResult {
    LLMResponse              response;
    std::vector<StreamEvent> events;
};

RunResult run(LLMProvider& provider, const LLMRequest& request) {
    RunResult result;
    result.response = provider
                          .stream(
                              request,
                              [&result](const StreamEvent& event) {
                                  result.events.push_back(event);
                                  return SinkFlow::Continue;
                              },
                              {})
                          .get();
    return result;
}

std::size_t count_tool_events(const std::vector<StreamEvent>& events) {
    std::size_t count = 0;
    for (const StreamEvent& event : events) {
        if (std::holds_alternative<ToolCallStarted>(event) ||
            std::holds_alternative<ToolCallDelta>(event) ||
            std::holds_alternative<ToolCallFinished>(event)) {
            ++count;
        }
    }
    return count;
}

ToolSchema tool_schema(std::string name) {
    ToolSchema schema;
    schema.name.value   = std::move(name);
    schema.description  = "test tool";
    schema.input_schema = nlohmann::json{{"type", "object"}};
    return schema;
}

LLMRequest content_request(std::vector<ToolSchema> tools) {
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

LLMProviderConfig muse_provider_config(std::size_t max_arguments_bytes = 1u << 20) {
    LLMProviderConfig config;
    config.provider            = "openai-compatible";
    config.base_url            = "http://127.0.0.1:9/v1";
    config.model               = "test-model";
    config.api_key             = std::string{"test-key"};
    config.api_key_env         = "YMH_TEST_API_KEY";
    config.max_arguments_bytes = max_arguments_bytes;
    const ModelProfile* profile = find_model_profile("muse-glimmer");
    if (profile != nullptr) {
        config.profile = *profile;
    }
    return config;
}

const std::string kToolChannelTurn =
    "<|start|>assistant to=self<|message|>reasoning mentions "
    "<atem:invoke name=\"ignored\"></atem:invoke><|eom|>"
    "<|start|>assistant to=read_file<|message|><atem:function_calls>"
    "<atem:invoke name=\"read_file\"><atem:parameter name=\"path\">a.txt</atem:parameter>"
    "</atem:invoke></atem:function_calls><|eom|>"
    "<|start|>assistant to=user<|message|>done<|eot|>";

} // namespace

TEST(MuseD4, WholeContentLeakedJsonIsLifted) {
    const LeakedParse parse = parse_leaked_json_call(
        "<tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"a\"}}</tool_call>");
    EXPECT_TRUE(parse.complete_block_seen);
    EXPECT_FALSE(parse.malformed_block_seen);
    ASSERT_EQ(parse.calls.size(), 1u);
    EXPECT_EQ(parse.calls[0].name, "read_file");
    EXPECT_EQ(parse.calls[0].arguments, nlohmann::json({{"path", "a"}}));
}

TEST(MuseD4, TwoBlocksAreBothConverted) {
    const LeakedParse parse = parse_leaked_json_call(
        "<tool_call>{\"name\":\"one\",\"arguments\":{}}</tool_call>"
        "<tool_call>{\"name\":\"two\",\"arguments\":{}}</tool_call>");
    EXPECT_TRUE(parse.complete_block_seen);
    ASSERT_EQ(parse.calls.size(), 2u);
    EXPECT_EQ(parse.calls[0].name, "one");
    EXPECT_EQ(parse.calls[1].name, "two");
}

TEST(MuseD4, OuterJsonNotObjectIsError) {
    const LeakedParse parse = parse_leaked_json_call("<tool_call>[1,2]</tool_call>");
    EXPECT_TRUE(parse.complete_block_seen);
    EXPECT_TRUE(parse.malformed_block_seen);
    EXPECT_TRUE(parse.calls.empty());
}

TEST(MuseD4, CompleteMalformedBlockIsNotText) {
    const LeakedParse parse = parse_leaked_json_call("<tool_call>not-json</tool_call>");
    EXPECT_TRUE(parse.complete_block_seen);
    EXPECT_TRUE(parse.malformed_block_seen);
}

TEST(MuseD4, EmbeddedBlockInProseIsText) {
    const LeakedParse parse = parse_leaked_json_call(
        "Example: <tool_call>{\"name\":\"read_file\",\"arguments\":{}}</tool_call> end");
    EXPECT_FALSE(parse.complete_block_seen);
    EXPECT_TRUE(parse.calls.empty());
}

TEST(MuseD4, LeakedUnknownToolIsError) {
    const LeakedParse parse = parse_leaked_json_call(
        "<tool_call>{\"name\":\"hallucinated\",\"arguments\":{}}</tool_call>");
    ASSERT_EQ(parse.calls.size(), 1u);
    EXPECT_EQ(parse.calls[0].name, "hallucinated");
}

TEST(MuseD4, PartialFragmentIsText) {
    const LeakedParse parse = parse_leaked_json_call("<tool_call>{\"name\":\"read_file\"");
    EXPECT_FALSE(parse.complete_block_seen);
}

TEST(MuseD4, NoDoubleExecutionWhenStructuredCallsPresent) {
    const std::string body = sse_body(
        {tool_start_frame(0, "call_1", "read_file"),
         content_frame("<tool_call>{\"name\":\"read_file\",\"arguments\":{}}</tool_call>"),
         finish_frame("tool_calls")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const RunResult result = run(provider, content_request({tool_schema("read_file")}));
    EXPECT_EQ(result.response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(result.response.tool_calls.size(), 1u);
    EXPECT_EQ(result.response.tool_calls[0].id, "call_1");
}

TEST(MuseD4, DetectorCapBoundsMemory) {
    const std::string leaked =
        "<tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"aaaaaaaaaaaaaaaaaaaa\"}}"
        "</tool_call>";
    const std::string body = sse_body({content_frame(leaked), finish_frame("stop")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(16),
                                      openai_compatible_capabilities(), transport);

    const RunResult result = run(provider, content_request({tool_schema("read_file")}));
    EXPECT_EQ(result.response.outcome, StreamOutcome::Completed);
    EXPECT_TRUE(result.response.tool_calls.empty());
}

TEST(MuseD5, NativeAtemInToolChannelIsDetected) {
    const std::optional<std::vector<std::string>> names = detect_native_atem_calls(kToolChannelTurn);
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(*names, std::vector<std::string>{"read_file"});
}

TEST(MuseD5, WrapperlessInvokeIsDetected) {
    const std::optional<std::vector<std::string>> names = detect_native_atem_calls(
        "<|start|>assistant to=read_file<|message|>"
        "<atem:invoke name=\"read_file\"></atem:invoke><|eom|>");
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(*names, std::vector<std::string>{"read_file"});
}

TEST(MuseD5, BareInvokeIsDetected) {
    const std::optional<std::vector<std::string>> names =
        detect_native_atem_calls("<atem:invoke name=\"read_file\"></atem:invoke>");
    ASSERT_TRUE(names.has_value());
    EXPECT_EQ(*names, std::vector<std::string>{"read_file"});
}

TEST(MuseD5, AtemInReasoningBodyIsIgnored) {
    EXPECT_FALSE(detect_native_atem_calls(
                     "<|start|>assistant to=self<|message|>"
                     "<atem:invoke name=\"read_file\"></atem:invoke><|eom|>")
                     .has_value());
}

TEST(MuseD5, AtemInFinalAnswerIsIgnored) {
    EXPECT_FALSE(detect_native_atem_calls(
                     "<|start|>assistant to=user<|message|>"
                     "<atem:invoke name=\"read_file\"></atem:invoke><|eot|>")
                     .has_value());
}

TEST(MuseD5, IncompleteInvokeIsText) {
    EXPECT_FALSE(detect_native_atem_calls("<atem:invoke name=\"read_file\">").has_value());
}

TEST(MuseD5, DetectionDisabledByFlag) {
    const std::string body = sse_body({content_frame(kToolChannelTurn), finish_frame("stop")});
    auto transport = std::make_shared<FakeTransport>(body);

    LLMProviderConfig config;
    config.provider    = "openai-compatible";
    config.base_url    = "http://127.0.0.1:9/v1";
    config.model       = "test-model";
    config.api_key     = std::string{"test-key"};
    config.api_key_env = "YMH_TEST_API_KEY";
    OpenAICompatibleProvider provider(config, openai_compatible_capabilities(), transport);

    const RunResult result = run(provider, content_request({}));
    EXPECT_EQ(result.response.outcome, StreamOutcome::Completed);
    EXPECT_TRUE(result.response.tool_calls.empty());
    EXPECT_EQ(result.response.finish, FinishReason::Stop);
}

TEST(MuseAdapter, LeakedJsonInStreamedContentIsLifted) {
    const std::string leaked =
        "<tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"a\"}}</tool_call>";
    const std::string body = sse_body(
        {content_frame(leaked.substr(0, 20)), content_frame(leaked.substr(20)),
         finish_frame("stop")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const RunResult result = run(provider, content_request({tool_schema("read_file")}));
    EXPECT_EQ(result.response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(result.response.tool_calls.size(), 1u);
    EXPECT_EQ(result.response.tool_calls[0].name, "read_file");
    EXPECT_EQ(result.response.tool_calls[0].arguments, nlohmann::json({{"path", "a"}}));
    EXPECT_EQ(result.response.finish, FinishReason::ToolCalls);
    EXPECT_GE(count_tool_events(result.events), 1u);
}

TEST(MuseAdapter, TwoLeakedBlocksBothExecute) {
    const std::string body = sse_body(
        {content_frame("<tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"a\"}}"
                       "</tool_call><tool_call>{\"name\":\"read_file\",\"arguments\":{\"path\":\"b\"}}"
                       "</tool_call>"),
         finish_frame("stop")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const RunResult result = run(provider, content_request({tool_schema("read_file")}));
    EXPECT_EQ(result.response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(result.response.tool_calls.size(), 2u);
    EXPECT_EQ(result.response.tool_calls[0].id, "leaked_0");
    EXPECT_EQ(result.response.tool_calls[1].id, "leaked_1");
}

TEST(MuseAdapter, OfferedNameUnparseableArgsIsError) {
    const std::string body = sse_body(
        {content_frame("<tool_call>{\"name\":\"read_file\",\"arguments\":\"{bad\"}</tool_call>"),
         finish_frame("stop")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const RunResult result = run(provider, content_request({tool_schema("read_file")}));
    EXPECT_EQ(result.response.outcome, StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, LLMErrorCode::MalformedToolCall);
}

TEST(MuseAdapter, NativeAtemInStreamedContentErrors) {
    const std::string body = sse_body({content_frame(kToolChannelTurn), finish_frame("stop")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const RunResult result = run(provider, content_request({}));
    EXPECT_EQ(result.response.outcome, StreamOutcome::Failed);
    EXPECT_EQ(result.response.error.code, LLMErrorCode::MalformedToolCall);
}

TEST(MuseAdapter, NativeAtemEmitsNoToolCallEvents) {
    const std::string body = sse_body({content_frame(kToolChannelTurn), finish_frame("stop")});
    auto transport = std::make_shared<FakeTransport>(body);
    OpenAICompatibleProvider provider(muse_provider_config(),
                                      openai_compatible_capabilities(), transport);

    const RunResult result = run(provider, content_request({}));
    EXPECT_EQ(count_tool_events(result.events), 0u);
}

TEST(MuseAdapter, OfferedNameCheckUsesRequestTools) {
    const std::string offered = sse_body(
        {content_frame("<tool_call>{\"name\":\"read_file\",\"arguments\":{}}</tool_call>"),
         finish_frame("stop")});
    auto offered_transport = std::make_shared<FakeTransport>(offered);
    OpenAICompatibleProvider offered_provider(muse_provider_config(),
                                              openai_compatible_capabilities(),
                                              offered_transport);
    const RunResult offered_result = run(offered_provider, content_request({tool_schema("read_file")}));
    EXPECT_EQ(offered_result.response.outcome, StreamOutcome::Completed);
    ASSERT_EQ(offered_result.response.tool_calls.size(), 1u);

    const std::string unoffered = sse_body(
        {content_frame("<tool_call>{\"name\":\"other_tool\",\"arguments\":{}}</tool_call>"),
         finish_frame("stop")});
    auto unoffered_transport = std::make_shared<FakeTransport>(unoffered);
    OpenAICompatibleProvider unoffered_provider(muse_provider_config(),
                                                openai_compatible_capabilities(),
                                                unoffered_transport);
    const RunResult unoffered_result =
        run(unoffered_provider, content_request({tool_schema("read_file")}));
    EXPECT_EQ(unoffered_result.response.outcome, StreamOutcome::Failed);
    EXPECT_EQ(unoffered_result.response.error.code, LLMErrorCode::MalformedToolCall);
}
