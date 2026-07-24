#include "agent_rpc/mcp/mcp_agent_integration.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>

namespace {

using agent_rpc::mcp::MCPAgentConfig;
using agent_rpc::mcp::MCPAgentIntegration;
using agent_rpc::mcp::ToolExecutionContext;
using agent_rpc::mcp::ToolInfo;
using json = nlohmann::json;

TEST(MCPAgentIntegrationTest, DisabledRuntimeIsExplicitlyUnavailable) {
    MCPAgentIntegration integration;
    MCPAgentConfig config;
    config.enable_mcp = false;
    ASSERT_TRUE(integration.initialize(config));
    EXPECT_TRUE(integration.isInitialized());
    EXPECT_FALSE(integration.isAvailable());
    EXPECT_EQ(integration.getStatusDescription(), "MCP disabled");

    ToolExecutionContext context;
    EXPECT_TRUE(integration.getAvailableTools(context).empty());
    const auto result = integration.callTool(context, "workspace_fs__read_file", "{}");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.empty());
}

TEST(MCPAgentIntegrationTest, EnabledRuntimeRequiresServerPath) {
    MCPAgentIntegration integration;
    MCPAgentConfig config;
    config.enable_mcp = true;
    EXPECT_FALSE(integration.initialize(config));
    EXPECT_FALSE(integration.isInitialized());
    EXPECT_FALSE(integration.isAvailable());
}

TEST(MCPAgentIntegrationTest, FunctionSchemaSerializationUsesModelSafeNames) {
    ToolInfo tool;
    tool.name = "workspace_fs__read_file";
    tool.tool_id = "workspace_fs.read_file";
    tool.plugin_id = "workspace_fs";
    tool.description = "Read a workspace file";
    tool.input_schema = R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})";
    const auto serialized = json::parse(MCPAgentIntegration::toFunctionCallingFormat({tool}));
    ASSERT_EQ(serialized.size(), 1U);
    EXPECT_EQ(serialized[0]["type"], "function");
    EXPECT_EQ(serialized[0]["function"]["name"], tool.name);
    EXPECT_EQ(serialized[0]["function"]["parameters"]["required"][0], "path");
}

#if defined(DAG_TEST_MCP_SERVER) && defined(DAG_TEST_PLUGIN_DIR) && defined(DAG_TEST_TOOL_CONFIG)

class MCPProcessIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
            ("dag-mcp-integration-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / session_id_ / "workspace");

        MCPAgentConfig config;
        config.enable_mcp = true;
        config.mcp_server_path = DAG_TEST_MCP_SERVER;
        config.mcp_args = {"-p", DAG_TEST_PLUGIN_DIR,
                           "-c", DAG_TEST_TOOL_CONFIG,
                           "--sessions-root", root_.string(),
                           "-l", (root_ / "logs").string()};
        config.max_retry_count = 0;
        ASSERT_TRUE(integration_.initialize(config));
    }

    void TearDown() override {
        integration_.shutdown();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    ToolExecutionContext context(const std::string& actor_kind,
                                 const std::string& operation) const {
        ToolExecutionContext value;
        value.session_id = session_id_;
        value.operation_id = operation;
        value.mode_id = "dag_team";
        value.actor_kind = actor_kind;
        value.actor_id = actor_kind + "_test";
        return value;
    }

    static bool has_plugin(const std::vector<ToolInfo>& tools, const std::string& plugin) {
        return std::any_of(tools.begin(), tools.end(), [&](const ToolInfo& tool) {
            return tool.plugin_id == plugin;
        });
    }

    MCPAgentIntegration integration_;
    std::filesystem::path root_;
    const std::string session_id_ = "0123456789abcdef0123456789abcdef";
};

TEST_F(MCPProcessIntegrationTest, TrustedActorMappingControlsTheCompleteToolView) {
    const auto designer = integration_.getAvailableTools(context("designer", "list_designer"));
    const auto planner = integration_.getAvailableTools(context("planner", "list_planner"));
    const auto executor = integration_.getAvailableTools(context("executor", "list_executor"));

    EXPECT_TRUE(has_plugin(designer, "team_design"));
    EXPECT_FALSE(has_plugin(designer, "dag_control"));
    EXPECT_TRUE(has_plugin(planner, "dag_control"));
    EXPECT_FALSE(has_plugin(planner, "team_design"));
    EXPECT_TRUE(has_plugin(executor, "workspace_fs"));
    EXPECT_TRUE(has_plugin(executor, "web_research"));
    EXPECT_FALSE(has_plugin(executor, "team_design"));
    EXPECT_FALSE(has_plugin(executor, "dag_control"));
}

TEST_F(MCPProcessIntegrationTest, ConcurrentCallsAreMatchedByRequestId) {
    auto write = [&](std::string operation, std::string path, std::string content) {
        return integration_.callTool(
            context("executor", operation), "workspace_fs__write_file",
            json({{"path", path}, {"content", content}}).dump());
    };
    auto first = std::async(std::launch::async, write, "write_first", "first.txt", "alpha");
    auto second = std::async(std::launch::async, write, "write_second", "second.txt", "beta");
    EXPECT_TRUE(first.get().success);
    EXPECT_TRUE(second.get().success);

    const auto read_first = integration_.callTool(
        context("executor", "read_first"), "workspace_fs__read_file",
        R"({"path":"first.txt"})");
    const auto read_second = integration_.callTool(
        context("executor", "read_second"), "workspace_fs__read_file",
        R"({"path":"second.txt"})");
    ASSERT_TRUE(read_first.success) << read_first.error;
    ASSERT_TRUE(read_second.success) << read_second.error;
    EXPECT_EQ(json::parse(read_first.result)["structuredContent"]["result"]["content"], "alpha");
    EXPECT_EQ(json::parse(read_second.result)["structuredContent"]["result"]["content"], "beta");
}

TEST_F(MCPProcessIntegrationTest, NonCandidateToolCannotBeCalled) {
    const auto result = integration_.callTool(
        context("executor", "forged_call"), "team_design__list_team", "{}");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("not available"), std::string::npos);
}

TEST_F(MCPProcessIntegrationTest, MalformedArgumentsNeverBecomeAnEmptyObjectCall) {
    const auto result = integration_.callTool(
        context("executor", "bad_arguments"), "workspace_fs__stat_path", "not-json");
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error.find("valid JSON object"), std::string::npos);
}

#endif

}  // namespace
