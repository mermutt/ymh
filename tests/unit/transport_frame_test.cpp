#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ymh/transport/frame_codec.hpp"
#include "ymh/transport/json_rpc.hpp"
#include "ymh/transport/protocol.hpp"

namespace {

using namespace ymh;

constexpr std::size_t kMax = 8u * 1024u * 1024u;

int protocol_error_code(const std::function<void()>& action) {
    try {
        action();
    } catch (const protocol::ProtocolError& error) {
        return error.code;
    }
    return 0;
}

bool protocol_error_is_fatal(const std::function<void()>& action) {
    try {
        action();
    } catch (const protocol::ProtocolError& error) {
        return error.fatal;
    }
    return false;
}

TEST(FrameCodec, EncodeUsesBigEndianLengthPrefix) {
    const std::string frame = protocol::FrameCodec::encode("{}", kMax);
    ASSERT_EQ(frame.size(), 6u);
    EXPECT_EQ(static_cast<unsigned char>(frame[0]), 0u);
    EXPECT_EQ(static_cast<unsigned char>(frame[1]), 0u);
    EXPECT_EQ(static_cast<unsigned char>(frame[2]), 0u);
    EXPECT_EQ(static_cast<unsigned char>(frame[3]), 2u);
    EXPECT_EQ(frame.substr(4), "{}");
}

TEST(FrameCodec, EncodeRejectsOversizedBody) {
    const std::string body(16, 'x');
    EXPECT_EQ(protocol_error_code([&] { protocol::FrameCodec::encode(body, 8); }),
              protocol::code_value(protocol::AppCode::PayloadTooLarge));
}

TEST(FrameCodec, DecodeSingleAndMultipleFrames) {
    std::string buffer = protocol::FrameCodec::encode(R"({"a":1})", kMax);
    buffer += protocol::FrameCodec::encode(R"({"b":2})", kMax);
    const std::vector<std::string> frames = protocol::FrameCodec::decode(buffer, kMax);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0], R"({"a":1})");
    EXPECT_EQ(frames[1], R"({"b":2})");
    EXPECT_TRUE(buffer.empty());
}

TEST(FrameCodec, DecodeLeavesPartialTail) {
    std::string buffer = protocol::FrameCodec::encode("hello", kMax);
    const std::string partial = buffer.substr(0, buffer.size() - 2);
    std::string working = partial;
    EXPECT_TRUE(protocol::FrameCodec::decode(working, kMax).empty());
    EXPECT_EQ(working, partial);

    std::string frame;
    EXPECT_EQ(protocol::FrameCodec::decode_step(working, kMax, frame),
              protocol::DecodeStatus::NeedMore);
    working.append(buffer.substr(partial.size()));
    EXPECT_EQ(protocol::FrameCodec::decode_step(working, kMax, frame),
              protocol::DecodeStatus::Ok);
    EXPECT_EQ(frame, "hello");
}

TEST(FrameCodec, ZeroLengthIsFatalInvalidRequest) {
    std::string buffer("\x00\x00\x00\x00", 4);
    EXPECT_EQ(protocol_error_code([&] { protocol::FrameCodec::decode(buffer, kMax); }),
              protocol::code_value(protocol::RpcCode::InvalidRequest));
    EXPECT_TRUE(protocol_error_is_fatal([&] { protocol::FrameCodec::decode(buffer, kMax); }));
}

TEST(FrameCodec, OversizedLengthIsFatalFrameTooLarge) {
    std::string buffer;
    const std::uint32_t length = 1024;
    buffer.push_back(static_cast<char>((length >> 24u) & 0xFFu));
    buffer.push_back(static_cast<char>((length >> 16u) & 0xFFu));
    buffer.push_back(static_cast<char>((length >> 8u) & 0xFFu));
    buffer.push_back(static_cast<char>(length & 0xFFu));
    buffer.append(length, 'x');
    EXPECT_EQ(protocol_error_code([&] { protocol::FrameCodec::decode(buffer, 16); }),
              protocol::code_value(protocol::AppCode::FrameTooLarge));
    EXPECT_TRUE(protocol_error_is_fatal([&] { protocol::FrameCodec::decode(buffer, 16); }));
}

TEST(FrameCodec, InvalidUtf8IsFatalParseError) {
    const std::string body{"\xC3\x28", 2};
    std::string for_code = protocol::FrameCodec::encode(body, kMax);
    EXPECT_EQ(protocol_error_code([&] { protocol::FrameCodec::decode(for_code, kMax); }),
              protocol::code_value(protocol::RpcCode::ParseError));
    std::string for_fatal = protocol::FrameCodec::encode(body, kMax);
    EXPECT_TRUE(protocol_error_is_fatal([&] { protocol::FrameCodec::decode(for_fatal, kMax); }));
}

TEST(JsonRpc, ParsesRequestAndNotification) {
    const protocol::Message request = protocol::parse_message(
        R"({"jsonrpc":"2.0","id":7,"method":"host.ping","params":{}})");
    ASSERT_TRUE(std::holds_alternative<protocol::Request>(request));
    const auto& parsed = std::get<protocol::Request>(request);
    EXPECT_EQ(parsed.method, "host.ping");
    EXPECT_TRUE(parsed.id.is_integer());
    EXPECT_EQ(std::get<std::int64_t>(parsed.id.value), 7);

    const protocol::Message notification = protocol::parse_message(
        R"({"jsonrpc":"2.0","method":"host.detach"})");
    ASSERT_TRUE(std::holds_alternative<protocol::Notification>(notification));
    EXPECT_EQ(std::get<protocol::Notification>(notification).method, "host.detach");
}

TEST(JsonRpc, ParsesResponseAndErrorWithNullId) {
    const protocol::Message response = protocol::parse_message(
        R"({"jsonrpc":"2.0","id":"abc","result":{"ok":true}})");
    ASSERT_TRUE(std::holds_alternative<protocol::Response>(response));
    EXPECT_TRUE(std::get<protocol::Response>(response).id.is_string());

    const protocol::Message error = protocol::parse_message(
        R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700,"message":"bad"}})");
    ASSERT_TRUE(std::holds_alternative<protocol::ErrorResponse>(error));
    const auto& parsed = std::get<protocol::ErrorResponse>(error);
    EXPECT_TRUE(parsed.id.is_null());
    EXPECT_EQ(parsed.code, protocol::code_value(protocol::RpcCode::ParseError));
}

TEST(JsonRpc, RejectsBatchAndNonObject) {
    EXPECT_EQ(protocol_error_code([] { static_cast<void>(protocol::parse_message("[]")); }),
              protocol::code_value(protocol::RpcCode::InvalidRequest));
    EXPECT_EQ(protocol_error_code([] { static_cast<void>(protocol::parse_message("42")); }),
              protocol::code_value(protocol::RpcCode::InvalidRequest));
}

TEST(JsonRpc, MalformedJsonIsRecoverableParseError) {
    EXPECT_EQ(protocol_error_code([] { static_cast<void>(protocol::parse_message("{not json")); }),
              protocol::code_value(protocol::RpcCode::ParseError));
    EXPECT_FALSE(protocol_error_is_fatal(
        [] { static_cast<void>(protocol::parse_message("{not json")); }));
}

TEST(JsonRpc, TrailingBytesAreFatalInvalidRequest) {
    EXPECT_EQ(
        protocol_error_code([] { static_cast<void>(protocol::parse_message(R"({"jsonrpc":"2.0"}x)")); }),
        protocol::code_value(protocol::RpcCode::InvalidRequest));
    EXPECT_TRUE(protocol_error_is_fatal(
        [] { static_cast<void>(protocol::parse_message(R"({"jsonrpc":"2.0"}x)")); }));
}

TEST(JsonRpc, EncodesErrorWithNullId) {
    const nlohmann::json json =
        protocol::encode(protocol::ErrorResponse{protocol::RequestId{}, -32700, "bad", {}});
    EXPECT_TRUE(json.at("id").is_null());
    EXPECT_EQ(json.at("error").at("code").get<int>(), -32700);
    EXPECT_EQ(json.at("jsonrpc").get<std::string>(), "2.0");
}

TEST(JsonRpc, RequestIdRoundTrips) {
    for (const protocol::RequestId& id : {protocol::RequestId{},
                                          protocol::RequestId{std::int64_t{42}},
                                          protocol::RequestId{std::string{"x"}}}) {
        nlohmann::json json;
        protocol::to_json(json, id);
        protocol::RequestId parsed;
        protocol::from_json(json, parsed);
        EXPECT_EQ(protocol::request_id_key(parsed), protocol::request_id_key(id));
    }
}

} // namespace
