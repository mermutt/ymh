#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "test_support.hpp"

#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

std::string config_text(bool allow_shell) {
    std::string text = R"({
  "llm": { "provider": "fake", "model": "fake-model", "api_key": "test" },
  "permissions": { "default": "ask", "read": "allow", "write": "ask", "shell": "ask" }
})";
    if (allow_shell) {
        text = R"({
  "llm": { "provider": "fake", "model": "fake-model", "api_key": "test" },
  "permissions": {
    "default": "ask", "read": "allow", "write": "ask", "shell": "ask",
    "rules": [ { "tool": "shell", "effect": "allow", "id": "shell.allow" } ]
  }
})";
    }
    return text;
}

void write_config(const fs::path& path, const std::string& text) {
    {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(stream.good());
        stream << text;
        ASSERT_TRUE(stream.good());
    }
    // `llm.api_key` may only live in a private file.
    std::error_code error;
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, error);
    ASSERT_FALSE(error);
}

PermissionRequest outside_shell(const fs::path& root) {
    PermissionRequest request;
    request.tool      = "shell";
    request.root      = root;
    request.roots     = {root};
    request.arguments = nlohmann::json{{"command", "cat /elsewhere/file.txt"}};
    request.sandbox   = SandboxMode::WorkspaceWrite;
    return request;
}

struct RuntimeFixture {
    ymh_test::TempWorkspace workspace;
    fs::path                config_dir;
    fs::path                config_path;

    RuntimeFixture() {
        config_dir = workspace.root / "config";
        fs::create_directories(config_dir);
        config_path = config_dir / "config.jsonc";
        write_config(config_path, config_text(false));
        fs::create_directories(workspace.root / ".ymh");
    }

    std::unique_ptr<WorkspaceRuntime> create() {
        ConfigPaths paths;
        paths.global    = config_path;
        paths.workspace = workspace_config_path(workspace.root);

        WorkspaceRuntimeOptions options;
        options.config      = load_config(paths);
        options.root        = workspace.root;
        options.config_path = config_path;
        options.boot_id     = BootId{"test-boot"};
        options.provider_factory = [](const LLMProviderConfig&) -> std::unique_ptr<LLMProvider> {
            return std::make_unique<FakeLLM>(FakeScript{});
        };
        auto created = WorkspaceRuntime::create(std::move(options));
        EXPECT_TRUE(created.has_value());
        return created.has_value() ? std::move(*created) : nullptr;
    }
};

TEST(WorkspaceRuntimeReload, PermissionEditAppliesWithoutRestart) {
    RuntimeFixture fixture;
    std::unique_ptr<WorkspaceRuntime> runtime = fixture.create();
    ASSERT_NE(runtime, nullptr);

    const PermissionRequest request = outside_shell(fixture.workspace.root);
    ASSERT_EQ(runtime->policy().evaluate(request), PolicyVerdict::Ask);

    write_config(fixture.config_path, config_text(true));
    EXPECT_TRUE(runtime->reload_permissions());
    EXPECT_EQ(runtime->policy().evaluate(request), PolicyVerdict::Allow);

    // Unchanged files are a no-op.
    EXPECT_FALSE(runtime->reload_permissions());
}

TEST(WorkspaceRuntimeReload, InvalidEditKeepsTheLastGoodPolicy) {
    RuntimeFixture fixture;
    std::unique_ptr<WorkspaceRuntime> runtime = fixture.create();
    ASSERT_NE(runtime, nullptr);

    const PermissionRequest request = outside_shell(fixture.workspace.root);
    write_config(fixture.config_path, config_text(true));
    ASSERT_TRUE(runtime->reload_permissions());
    ASSERT_EQ(runtime->policy().evaluate(request), PolicyVerdict::Allow);

    write_config(fixture.config_path, "{ this is not jsonc");
    EXPECT_FALSE(runtime->reload_permissions());
    EXPECT_EQ(runtime->policy().evaluate(request), PolicyVerdict::Allow);
}

} // namespace
