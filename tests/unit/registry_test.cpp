#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/tools/tool.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;

nlohmann::json echo_schema() {
    return nlohmann::json{{"type", "object"},
                          {"properties", {{"text", {{"type", "string"}}}}},
                          {"required", nlohmann::json::array({"text"})},
                          {"additionalProperties", false}};
}

class EchoTool final : public Tool {
public:
    ToolSchema schema() const override {
        return ToolSchema{ToolName{"echo"}, ToolVersion{1, 0}, "echo", echo_schema(),
                          false};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        ToolResult result;
        result.id = context.callId();
        result.name = "echo";
        result.output = arguments.value.value("text", std::string{});
        return Task<ToolResult>(std::move(result));
    }
};

class ThrowTool final : public Tool {
public:
    ToolSchema schema() const override {
        return ToolSchema{ToolName{"throws"}, ToolVersion{1, 0}, "throws",
                          nlohmann::json{{"type", "object"},
                                         {"properties", nlohmann::json::object()},
                                         {"required", nlohmann::json::array()},
                                         {"additionalProperties", false}},
                          false};
    }

    Task<ToolResult> execute(const ToolContext&, const ToolArguments&) override {
        throw ToolError{ToolErrorCode::NotFound, "missing"};
    }
};

class BoomTool final : public Tool {
public:
    ToolSchema schema() const override {
        return ToolSchema{ToolName{"boom"}, ToolVersion{1, 0}, "boom",
                          nlohmann::json{{"type", "object"},
                                         {"properties", nlohmann::json::object()},
                                         {"required", nlohmann::json::array()},
                                         {"additionalProperties", false}},
                          false};
    }

    Task<ToolResult> execute(const ToolContext&, const ToolArguments&) override {
        throw std::runtime_error{"boom"};
    }
};

class NamedTool final : public Tool {
public:
    NamedTool(std::string name, ToolVersion version)
        : name_(std::move(name)), version_(version) {}

    ToolSchema schema() const override {
        return ToolSchema{ToolName{name_}, version_, "named",
                          nlohmann::json{{"type", "object"},
                                         {"properties", nlohmann::json::object()},
                                         {"required", nlohmann::json::array()},
                                         {"additionalProperties", false}},
                          false};
    }

    Task<ToolResult> execute(const ToolContext&, const ToolArguments&) override {
        return Task<ToolResult>(ToolResult{});
    }

private:
    std::string name_;
    ToolVersion version_;
};

payload::ToolCall make_call(const std::string& name, nlohmann::json arguments) {
    payload::ToolCall call;
    call.id = "call-1";
    call.name = name;
    call.arguments = std::move(arguments);
    return call;
}

TEST(ToolName, Grammar) {
    EXPECT_TRUE(is_valid_tool_name("read_file"));
    EXPECT_TRUE(is_valid_tool_name("git_status"));
    EXPECT_TRUE(is_valid_tool_name("mcp.github.search"));
    EXPECT_FALSE(is_valid_tool_name(""));
    EXPECT_FALSE(is_valid_tool_name("Read"));
    EXPECT_FALSE(is_valid_tool_name("_read"));
    EXPECT_FALSE(is_valid_tool_name("read..file"));
    EXPECT_FALSE(is_valid_tool_name("read."));
    EXPECT_FALSE(is_valid_tool_name("read-file"));
}

TEST(SchemaValidate, RequiredAndTypesAndAdditionalProperties) {
    const nlohmann::json schema = echo_schema();

    EXPECT_TRUE(schema_validate(schema, nlohmann::json{{"text", "hi"}}));
    EXPECT_FALSE(schema_validate(schema, nlohmann::json::object()));
    EXPECT_FALSE(schema_validate(schema, nlohmann::json{{"text", 1}}));
    EXPECT_FALSE(schema_validate(schema, nlohmann::json{{"text", "hi"}, {"extra", 1}}));
    EXPECT_FALSE(schema_validate(schema, nlohmann::json::array()));
}

TEST(ToolRegistry, AddFindContainsAndSortedNames) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    keeper.add(std::make_unique<NamedTool>("zeta", ToolVersion{1, 0}));
    keeper.add(std::make_unique<NamedTool>("alpha", ToolVersion{1, 0}));

    EXPECT_EQ(registry.size(), 2u);
    EXPECT_TRUE(registry.contains(ToolName{"alpha"}));
    EXPECT_FALSE(registry.contains(ToolName{"missing"}));
    EXPECT_NE(registry.find(ToolName{"zeta"}), nullptr);

    const std::vector<ToolName> names = registry.names();
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0].value, "alpha");
    EXPECT_EQ(names[1].value, "zeta");

    const std::vector<ToolSchema> schemas = registry.schemas();
    ASSERT_EQ(schemas.size(), 2u);
    EXPECT_EQ(schemas[0].name.value, "alpha");
}

TEST(ToolRegistry, DuplicateVersionAndNameAreRejected) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    keeper.add(std::make_unique<NamedTool>("echo", ToolVersion{1, 0}));

    try {
        keeper.add(std::make_unique<NamedTool>("echo", ToolVersion{1, 0}));
        FAIL() << "expected DuplicateVersion";
    } catch (const ToolRegistryError& error) {
        EXPECT_EQ(error.code(), ToolRegistryErrorCode::DuplicateVersion);
    }

    try {
        keeper.add(std::make_unique<NamedTool>("echo", ToolVersion{2, 0}));
        FAIL() << "expected DuplicateName";
    } catch (const ToolRegistryError& error) {
        EXPECT_EQ(error.code(), ToolRegistryErrorCode::DuplicateName);
    }
}

TEST(ToolRegistry, InvalidNameIsRejected) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    try {
        keeper.add(std::make_unique<NamedTool>("Bad-Name", ToolVersion{1, 0}));
        FAIL() << "expected InvalidName";
    } catch (const ToolRegistryError& error) {
        EXPECT_EQ(error.code(), ToolRegistryErrorCode::InvalidName);
    }
}

TEST(ToolRegistry, InvalidSchemaIsRejected) {
    class BadSchemaTool final : public Tool {
    public:
        ToolSchema schema() const override {
            return ToolSchema{ToolName{"bad"}, ToolVersion{1, 0}, "bad",
                              nlohmann::json{{"type", "object"},
                                             {"properties", nlohmann::json::object()}},
                              false};
        }
        Task<ToolResult> execute(const ToolContext&, const ToolArguments&) override {
            return Task<ToolResult>(ToolResult{});
        }
    };

    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    try {
        keeper.add(std::make_unique<BadSchemaTool>());
        FAIL() << "expected InvalidSchema";
    } catch (const ToolRegistryError& error) {
        EXPECT_EQ(error.code(), ToolRegistryErrorCode::InvalidSchema);
    }
}

TEST(ToolRegistry, FreezeRejectsFurtherRegistration) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    keeper.add(std::make_unique<NamedTool>("echo", ToolVersion{1, 0}));
    const std::uint64_t generation = registry.generation();
    registry.freeze();

    EXPECT_TRUE(registry.frozen());
    EXPECT_EQ(registry.generation(), generation);
    try {
        keeper.add(std::make_unique<NamedTool>("other", ToolVersion{1, 0}));
        FAIL() << "expected RegistryFrozen";
    } catch (const ToolRegistryError& error) {
        EXPECT_EQ(error.code(), ToolRegistryErrorCode::RegistryFrozen);
    }
}

TEST(ToolRegistry, GenerationBumpsOnAddAndRegistrationDrop) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    const std::uint64_t start = registry.generation();
    {
        auto registration = registry.add(std::make_unique<NamedTool>("echo", ToolVersion{1, 0}));
        EXPECT_TRUE(registration.held());
        EXPECT_GT(registry.generation(), start);
        EXPECT_TRUE(registry.contains(ToolName{"echo"}));
    }
    EXPECT_FALSE(registry.contains(ToolName{"echo"}));
}

TEST(ToolRegistry, ExecuteUnknownTool) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    ymh::test::ToolEnv env("registry_unknown");

    const ToolResult result =
        registry.execute(make_call("missing", nlohmann::json::object()), env.context())
            .get();

    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "UnknownTool");
}

TEST(ToolRegistry, ExecuteInvalidArguments) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    keeper.add(std::make_unique<EchoTool>());
    ymh::test::ToolEnv env("registry_invalid");

    const ToolResult result =
        registry.execute(make_call("echo", nlohmann::json::object()), env.context())
            .get();

    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "InvalidArguments");
}

TEST(ToolRegistry, ExecuteHappyPath) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    keeper.add(std::make_unique<EchoTool>());
    ymh::test::ToolEnv env("registry_happy");

    const ToolResult result = registry
                                  .execute(make_call("echo", nlohmann::json{{"text", "hi"}}),
                                           env.context())
                                  .get();

    EXPECT_EQ(result.outcome, payload::ToolOutcome::Ok);
    EXPECT_EQ(result.output, "hi");
    EXPECT_EQ(result.id, "call-1");
    EXPECT_EQ(result.name, "echo");
}

TEST(ToolRegistry, ToolErrorMapsToDurableErrorCode) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    keeper.add(std::make_unique<ThrowTool>());
    ymh::test::ToolEnv env("registry_toolerror");

    const ToolResult result =
        registry.execute(make_call("throws", nlohmann::json::object()), env.context())
            .get();

    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "NotFound");
}

TEST(ToolRegistry, UnexpectedExceptionMapsToInternal) {
    ToolRegistry registry;
    ymh::test::RegistrationKeeper keeper(registry);
    keeper.add(std::make_unique<BoomTool>());
    ymh::test::ToolEnv env("registry_boom");

    const ToolResult result =
        registry.execute(make_call("boom", nlohmann::json::object()), env.context())
            .get();

    EXPECT_EQ(result.outcome, payload::ToolOutcome::Error);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "Internal");
}

} // namespace
