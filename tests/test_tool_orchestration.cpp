#include "tool/ToolRegistry.h"
#include "httplib.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>

namespace {

using vx::mcp::ToolExecutionContext;
using vx::mcp::ToolGateway;
using vx::mcp::ToolRegistry;
using json = nlohmann::json;

class ToolOrchestrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
            ("dag-tool-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / session_id_ / "workspace");
        registry_.set_sessions_root(root_);
        std::string error;
        ASSERT_TRUE(registry_.Initialize(DAG_TEST_PLUGIN_DIR, DAG_TEST_TOOL_CONFIG, &error)) << error;
        gateway_ = std::make_unique<ToolGateway>(registry_);
    }

    void TearDown() override {
        gateway_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    ToolExecutionContext context(std::string actor, std::string operation) const {
        ToolExecutionContext value;
        value.session_id = session_id_;
        value.operation_id = std::move(operation);
        value.mode_id = "dag_team";
        value.actor_kind = actor;
        value.actor_id = actor + "_test";
        return value;
    }

    json call(const ToolExecutionContext& context,
              const std::string& name,
              json arguments = json::object()) {
        return gateway_->Call(context, name, arguments);
    }

    ToolRegistry registry_;
    std::unique_ptr<ToolGateway> gateway_;
    std::filesystem::path root_;
    const std::string session_id_ = "fedcba9876543210fedcba9876543210";
};

class LocalWebServer {
public:
    ~LocalWebServer() {
        server.stop();
        if (thread.joinable()) thread.join();
    }

    void start() {
        port = server.bind_to_any_port("127.0.0.1");
        if (port <= 0) throw std::runtime_error("cannot bind local web test server");
        thread = std::thread([this] { server.listen_after_bind(); });
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port) + path;
    }

    httplib::Server server;
    int port = -1;
    std::thread thread;
};

TEST_F(ToolOrchestrationTest, ModeAndTrustedActorProduceExactPluginCandidates) {
    auto plugins = [&](const std::string& actor) {
        std::set<std::string> result;
        for (const auto& tool : registry_.ListTools(context(actor, "list"))) {
            result.insert(tool.plugin_id);
        }
        return result;
    };
    EXPECT_EQ(plugins("designer"), (std::set<std::string>{"team_design", "workspace_fs", "web_research"}));
    EXPECT_EQ(plugins("planner"), (std::set<std::string>{"dag_control", "workspace_fs", "web_research"}));
    EXPECT_EQ(plugins("executor"), (std::set<std::string>{"workspace_fs", "web_research"}));
    EXPECT_TRUE(plugins("Designer").empty());
}

TEST_F(ToolOrchestrationTest, GatewayRepeatsOwnershipAndSchemaChecks) {
    const auto forged = call(context("executor", "forged"), "team_design__list_team");
    EXPECT_TRUE(forged["isError"]);
    EXPECT_EQ(forged["structuredContent"]["error"]["code"], "TOOL_NOT_GRANTED");

    const auto invalid = call(context("executor", "invalid"), "workspace_fs__read_file",
                              {{"path", "x"}, {"unexpected", true}});
    EXPECT_TRUE(invalid["isError"]);
    EXPECT_EQ(invalid["structuredContent"]["error"]["code"], "INVALID_ARGUMENTS");
}

TEST_F(ToolOrchestrationTest, FilesystemRejectsTraversalSymlinksAndWorkspaceRootDeletion) {
    const auto actor = context("executor", "filesystem_safety");
    for (const auto& unsafe : {"../outside", "/tmp/outside", "C:\\outside"}) {
        EXPECT_TRUE(call(actor, "workspace_fs__read_file", {{"path", unsafe}})["isError"]);
    }
    EXPECT_TRUE(call(actor, "workspace_fs__delete_directory",
                     {{"path", "."}, {"recursive", true}})["isError"]);

    const auto outside = root_ / "outside.txt";
    std::ofstream(outside) << "secret";
    std::error_code error;
    std::filesystem::create_symlink(outside, root_ / session_id_ / "workspace" / "link.txt", error);
    if (!error) {
        EXPECT_TRUE(call(actor, "workspace_fs__read_file", {{"path", "link.txt"}})["isError"]);
    }
}

TEST_F(ToolOrchestrationTest, FilesystemWritesAtomicallyAndKeepsOperationsInSessionWorkspace) {
    const auto actor = context("executor", "filesystem_write");
    ASSERT_FALSE(call(actor, "workspace_fs__write_file",
                      {{"path", "report.txt"}, {"content", "one"}})["isError"]);
    ASSERT_FALSE(call(actor, "workspace_fs__write_file",
                      {{"path", "report.txt"}, {"content", "two"}, {"overwrite", true}})["isError"]);
    const auto read = call(actor, "workspace_fs__read_file", {{"path", "report.txt"}});
    ASSERT_FALSE(read["isError"]);
    EXPECT_EQ(read["structuredContent"]["result"]["content"], "two");
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / session_id_ / "workspace" / "report.txt"));
}

TEST_F(ToolOrchestrationTest, AuditRecordsMetadataWithoutFileContent) {
    const auto actor = context("executor", "audit_write");
    ASSERT_FALSE(call(actor, "workspace_fs__write_file",
                      {{"path", "audit.txt"}, {"content", "secret-file-body"}})["isError"]);
    std::ifstream input(root_ / session_id_ / "audit" / "tools.jsonl");
    ASSERT_TRUE(input.good());
    const std::string audit((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    EXPECT_NE(audit.find("workspace_fs.write_file"), std::string::npos);
    EXPECT_NE(audit.find("audit.txt"), std::string::npos);
    EXPECT_EQ(audit.find("secret-file-body"), std::string::npos);
}

TEST_F(ToolOrchestrationTest, AuditRedactsUrlUserinfoAndQuery) {
    const auto actor = context("executor", "audit_url");
    EXPECT_TRUE(call(actor, "web_research__fetch_webpage",
                     {{"url", "http://api-key@example.com/path?token=secret"}})["isError"]);
    std::ifstream input(root_ / session_id_ / "audit" / "tools.jsonl");
    ASSERT_TRUE(input.good());
    const std::string audit((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    EXPECT_NE(audit.find("http://example.com/path"), std::string::npos);
    EXPECT_EQ(audit.find("api-key"), std::string::npos);
    EXPECT_EQ(audit.find("token=secret"), std::string::npos);
}

TEST_F(ToolOrchestrationTest, DesignerDraftIsOperationScopedAndRequiresValidCommit) {
    auto designer = context("designer", "design_one");
    designer.trusted_data = {{"max_agents", 3}};
    for (const auto& id : {"researcher", "analyst", "writer"}) {
        ASSERT_FALSE(call(designer, "team_design__add_agent",
                          {{"id", id}, {"name", id}, {"role", "Do work"}})["isError"]);
    }
    const auto committed = call(designer, "team_design__commit_team");
    ASSERT_FALSE(committed["isError"]);
    EXPECT_TRUE(committed["structuredContent"]["result"]["committed"]);

    auto other_operation = context("designer", "design_two");
    other_operation.trusted_data = {{"max_agents", 3}};
    EXPECT_TRUE(call(other_operation, "team_design__commit_team")["isError"]);
}

TEST_F(ToolOrchestrationTest, PlannerRejectsCyclesAndCommitsAValidDag) {
    auto planner = context("planner", "plan_cycle");
    planner.trusted_data = {{"agent_ids", {"a", "b"}}};
    ASSERT_FALSE(call(planner, "dag_control__add_node",
                      {{"agent_id", "a"}, {"subtask", "A"}, {"depends_on", {"b"}}})["isError"]);
    ASSERT_FALSE(call(planner, "dag_control__add_node",
                      {{"agent_id", "b"}, {"subtask", "B"}, {"depends_on", {"a"}}})["isError"]);
    const auto cyclic = call(planner, "dag_control__commit_plan");
    EXPECT_TRUE(cyclic["isError"]);
    EXPECT_EQ(cyclic["structuredContent"]["error"]["code"], "CYCLIC_PLAN");

    auto valid = context("planner", "plan_valid");
    valid.trusted_data = planner.trusted_data;
    ASSERT_FALSE(call(valid, "dag_control__add_node",
                      {{"agent_id", "a"}, {"subtask", "A"}, {"depends_on", json::array()}})["isError"]);
    ASSERT_FALSE(call(valid, "dag_control__add_node",
                      {{"agent_id", "b"}, {"subtask", "B"}, {"depends_on", {"a"}}})["isError"]);
    const auto committed = call(valid, "dag_control__commit_plan");
    ASSERT_FALSE(committed["isError"]);
    EXPECT_TRUE(committed["structuredContent"]["result"]["committed"]);
}

TEST_F(ToolOrchestrationTest, WebFetchSupportsLocalRedirectHtmlAndCharset) {
    LocalWebServer web;
    web.server.Get("/page", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(
            "<html><head><title>T &amp; T</title><script>secret-script</script></head>"
            "<body>你好 &amp; 世界</body></html>", "text/html; charset=utf-8");
    });
    web.server.Get("/redirect", [](const httplib::Request&, httplib::Response& response) {
        response.set_redirect("/page", 302);
    });
    web.server.Get("/latin1", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(std::string("caf\xe9", 4), "text/plain; charset=iso-8859-1");
    });
    web.start();

    const auto actor = context("executor", "web_local");
    const auto fetched = call(actor, "web_research__fetch_webpage",
                              {{"url", web.url("/redirect")}, {"max_chars", 100}});
    ASSERT_FALSE(fetched["isError"]) << fetched.dump();
    const auto result = fetched["structuredContent"]["result"];
    EXPECT_EQ(result["title"], "T & T");
    EXPECT_EQ(result["content_trust"], "untrusted_web_content");
    EXPECT_NE(result["content"].get<std::string>().find("你好 & 世界"), std::string::npos);
    EXPECT_EQ(result["content"].get<std::string>().find("secret-script"), std::string::npos);

    const auto latin1 = call(actor, "web_research__fetch_webpage", {{"url", web.url("/latin1")}});
    ASSERT_FALSE(latin1["isError"]) << latin1.dump();
    EXPECT_EQ(latin1["structuredContent"]["result"]["content"], "café");
}

TEST_F(ToolOrchestrationTest, BingFixtureParsesAndDecodesRedirectWithoutNetwork) {
    LocalWebServer web;
    web.server.Get("/search", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(
            R"(<html><li class="b_algo"><h2><a href="https://www.bing.com/ck/a?u=a1aHR0cHM6Ly9leGFtcGxlLmNvbS9wYXRo">Example &amp; title</a></h2><p>Useful &amp; fixed snippet</p></li></html>)",
            "text/html; charset=utf-8");
    });
    web.start();
    ASSERT_EQ(::setenv("BING_SEARCH_URL", web.url("/search").c_str(), 1), 0);
    const auto searched = call(context("executor", "web_search"),
                               "web_research__web_search", {{"query", "fixture"}});
    ::unsetenv("BING_SEARCH_URL");
    ASSERT_FALSE(searched["isError"]) << searched.dump();
    const auto results = searched["structuredContent"]["result"]["results"];
    ASSERT_EQ(results.size(), 1U);
    EXPECT_EQ(results[0]["url"], "https://example.com/path");
    EXPECT_EQ(results[0]["title"], "Example & title");
    EXPECT_EQ(results[0]["snippet"], "Useful & fixed snippet");
}

TEST_F(ToolOrchestrationTest, WebFetchReturnsStructuredErrorsForStatusTimeoutAndLimit) {
    LocalWebServer web;
    web.server.Get("/denied", [](const httplib::Request&, httplib::Response& response) {
        response.status = 403;
    });
    web.server.Get("/slow", [](const httplib::Request&, httplib::Response& response) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        response.set_content("late", "text/plain");
    });
    web.server.Get("/large", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(std::string(2 * 1024 * 1024 + 1, 'x'), "text/plain");
    });
    web.start();
    const auto actor = context("executor", "web_errors");
    EXPECT_TRUE(call(actor, "web_research__fetch_webpage", {{"url", web.url("/denied")}})["isError"]);
    EXPECT_TRUE(call(actor, "web_research__fetch_webpage",
                     {{"url", web.url("/slow")}, {"timeout_ms", 1000}})["isError"]);
    EXPECT_TRUE(call(actor, "web_research__fetch_webpage", {{"url", web.url("/large")}})["isError"]);
}

TEST(ToolRegistryConfigurationTest, RejectsUnsupportedPermissionBeforeLoadingPlugins) {
    const auto root = std::filesystem::temp_directory_path() /
        ("dag-config-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto config = root / "tools.json";
    std::ofstream(config) << R"({"schema_version":1,"modes":{"dag_team":{"actors":{"executor":["workspace_fs"]}}},"permission":"ask"})";
    ToolRegistry registry;
    std::string error;
    EXPECT_FALSE(registry.Initialize(DAG_TEST_PLUGIN_DIR, config, &error));
    EXPECT_NE(error.find("permission=allow"), std::string::npos);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ToolRegistryConfigurationTest, RejectsDuplicateAndUnknownPluginMappings) {
    const auto root = std::filesystem::temp_directory_path() /
        ("dag-config-mapping-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto config = root / "tools.json";
    std::ofstream(config) << R"({"schema_version":1,"modes":{"dag_team":{"actors":{"executor":["workspace_fs","workspace_fs"]}}},"permission":"allow"})";
    ToolRegistry duplicate;
    std::string error;
    EXPECT_FALSE(duplicate.Initialize(DAG_TEST_PLUGIN_DIR, config, &error));
    EXPECT_NE(error.find("duplicate plugin id"), std::string::npos);

    std::ofstream(config, std::ios::trunc) << R"({"schema_version":1,"modes":{"dag_team":{"actors":{"executor":["missing_plugin"]}}},"permission":"allow"})";
    ToolRegistry unknown;
    error.clear();
    EXPECT_FALSE(unknown.Initialize(DAG_TEST_PLUGIN_DIR, config, &error));
    EXPECT_NE(error.find("failed to load"), std::string::npos);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

#ifdef DAG_TEST_INVALID_PLUGIN_DIR
TEST(ToolRegistryManifestTest, RejectsDuplicateToolsInvalidSchemaAndDuplicatePluginId) {
    const auto root = std::filesystem::temp_directory_path() /
        ("dag-manifest-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto config = root / "tools.json";
    auto write_config = [&](const json& plugins) {
        std::ofstream(config, std::ios::trunc) << json({
            {"schema_version", 1},
            {"modes", {{"dag_team", {{"actors", {{"executor", plugins}}}}}}},
            {"permission", "allow"}
        }).dump();
    };

    std::string error;
    write_config(json::array({"duplicate_tool"}));
    ToolRegistry duplicate_tool;
    EXPECT_FALSE(duplicate_tool.Initialize(DAG_TEST_INVALID_PLUGIN_DIR, config, &error));
    EXPECT_NE(error.find("duplicate tool"), std::string::npos);

    error.clear();
    write_config(json::array({"invalid_schema"}));
    ToolRegistry invalid_schema;
    EXPECT_FALSE(invalid_schema.Initialize(DAG_TEST_INVALID_PLUGIN_DIR, config, &error));
    EXPECT_NE(error.find("invalid tool descriptor"), std::string::npos);

    error.clear();
    write_config(json::array({"duplicate_plugin", "duplicate_alias"}));
    ToolRegistry duplicate_plugin;
    EXPECT_FALSE(duplicate_plugin.Initialize(DAG_TEST_INVALID_PLUGIN_DIR, config, &error));
    EXPECT_NE(error.find("duplicate plugin"), std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

TEST(ToolGatewayContextTest, RejectsRoleAndSessionForgeryShapes) {
    ToolExecutionContext parsed;
    std::string error;
    EXPECT_FALSE(ToolGateway::ParseContext(
        {{"context", {{"session_id", "../bad"}, {"operation_id", "op"},
                       {"mode_id", "dag_team"}, {"actor_kind", "executor"},
                       {"actor_id", "executor"}}}}, &parsed, &error));
    EXPECT_FALSE(error.empty());
    error.clear();
    EXPECT_FALSE(ToolGateway::ParseContext(
        {{"context", {{"session_id", 42}, {"operation_id", "op"},
                       {"mode_id", "dag_team"}, {"actor_kind", "executor"},
                       {"actor_id", "executor"}, {"trusted_data", "forged"}}}},
        &parsed, &error));
    EXPECT_NE(error.find("must be a string"), std::string::npos);
}

}  // namespace
