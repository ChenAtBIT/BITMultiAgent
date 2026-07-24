#include "ai_orchestrator/dag_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace ai_orchestrator::dag;

namespace {

ModelResponse text_response(std::string content) {
    return {std::move(content), {}, "stop"};
}

json wait_for_terminal(DagRuntime& runtime, const std::string& run_id) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto snapshot = runtime.snapshot(run_id);
        if (snapshot.is_object() && snapshot.contains("events")) {
            for (const auto& event : snapshot["events"]) {
                const auto type = event.value("type", "");
                if (type == "run_done" || type == "run_stalled") return snapshot;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return runtime.snapshot(run_id);
}

std::shared_ptr<LambdaChatModel> test_model(std::atomic<bool>* saw_retry = nullptr) {
    return std::make_shared<LambdaChatModel>(
        [saw_retry](const std::vector<ChatMessage>& messages, double, const json&) {
            const std::string system = messages.empty() ? "" : messages.front().content;
            const std::string user = messages.size() < 2 ? "" : messages.back().content;
            if (system.find("multi-agent orchestrator") != std::string::npos) {
                return text_response("[{\"agent_id\":\"researcher\",\"subtask\":\"Research the task\",\"depends_on\":[]},"
                                     "{\"agent_id\":\"writer\",\"subtask\":\"Write the final report\",\"depends_on\":[\"researcher\"]}]");
            }
            if (saw_retry && user.find("edited version") != std::string::npos) saw_retry->store(true);
            return text_response("{\"thought\":\"complete\",\"action\":\"final\",\"observation\":\"checked\",\"answer\":\"deterministic answer\"}");
        });
}

bool has_tool_schema(const json& tools, const std::string& name) {
    if (!tools.is_array()) return false;
    return std::any_of(tools.begin(), tools.end(), [&](const json& tool) {
        return tool.value("function", json::object()).value("name", "") == name;
    });
}

}  // namespace

TEST(DagPlannerTest, DefensivePlanParsingFiltersUnknownAndDuplicateAgents) {
    const auto agents = default_agents();
    const auto plan = parse_plan(
        "prefix [{\"agent_id\":\"ghost\",\"subtask\":\"bad\",\"depends_on\":[]},"
        "{\"agent_id\":\"researcher\",\"subtask\":\"research\",\"depends_on\":[\"ghost\"]},"
        "{\"agent_id\":\"researcher\",\"subtask\":\"duplicate\",\"depends_on\":[]} ] suffix",
        agents, "task");
    ASSERT_EQ(plan.size(), 1U);
    EXPECT_EQ(plan.front().agent_id, "researcher");
    EXPECT_TRUE(plan.front().depends_on.empty());
}

TEST(DagPlannerTest, InvalidPlanDoesNotCreateDefaultParallelDag) {
    const auto plan = parse_plan("not json", default_agents(), "invalid task");
    EXPECT_TRUE(plan.empty());
}

TEST(DagRuntimeTest, RunsDependencyDagAndInjectsSkills) {
    DagRuntime runtime(test_model());
    RunOptions options;
    options.user_input = "research and write a report";
    options.agents = {default_agents()[0], default_agents()[4]};
    options.max_agent_rounds = 2;
    const auto run_id = runtime.create_run(options);
    const auto snapshot = wait_for_terminal(runtime, run_id);

    ASSERT_FALSE(snapshot.is_null());
    EXPECT_EQ(snapshot["statuses"]["researcher"], "done");
    EXPECT_EQ(snapshot["statuses"]["writer"], "done");
    EXPECT_EQ(snapshot["outputs"]["writer"], "deterministic answer");

    bool has_skill_event = false;
    bool has_plan_event = false;
    for (const auto& event : snapshot["events"]) {
        has_skill_event |= event.value("type", "") == "skills_activated";
        has_plan_event |= event.value("type", "") == "plan_created";
    }
    EXPECT_TRUE(has_skill_event);
    EXPECT_TRUE(has_plan_event);
}

TEST(DagRuntimeTest, RejectsInvalidOrDuplicateManualAgentIds) {
    DagRuntime runtime(test_model());
    RunOptions options;
    options.user_input = "invalid agents";
    options.agents = {{"duplicate", "A", "Role A", "A", {}},
                      {"duplicate", "B", "Role B", "B", {}}};
    EXPECT_THROW(runtime.create_run(options), std::invalid_argument);
    options.agents = {{"../escape", "A", "Role A", "A", {}}};
    EXPECT_THROW(runtime.create_run(options), std::invalid_argument);
}

TEST(DagRuntimeTest, CyclicPlanStallsWithoutDeadlock) {
    auto model = std::make_shared<LambdaChatModel>(
        [](const std::vector<ChatMessage>& messages, double, const json&) {
            if (!messages.empty() && messages.front().content.find("multi-agent orchestrator") != std::string::npos) {
                return text_response("[{\"agent_id\":\"researcher\",\"subtask\":\"a\",\"depends_on\":[\"writer\"]},"
                                     "{\"agent_id\":\"writer\",\"subtask\":\"b\",\"depends_on\":[\"researcher\"]}]");
            }
            return text_response("{\"action\":\"final\",\"answer\":\"unused\"}");
        });
    DagRuntime runtime(model);
    RunOptions options;
    options.user_input = "cycle";
    options.agents = {default_agents()[0], default_agents()[4]};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));
    bool stalled = false;
    bool invalid_plan = false;
    for (const auto& event : snapshot["events"]) {
        stalled |= event.value("type", "") == "run_stalled";
        if (event.value("type", "") == "plan_created") invalid_plan = !event["payload"].value("valid", true);
    }
    EXPECT_TRUE(stalled);
    EXPECT_TRUE(invalid_plan);
}

TEST(DagRuntimeTest, RetryCarriesEditedOutputAndFeedback) {
    std::atomic<bool> saw_retry{false};
    DagRuntime runtime(test_model(&saw_retry));
    RunOptions options;
    options.user_input = "write a report";
    options.agents = {default_agents()[4]};
    const auto run_id = runtime.create_run(options);
    (void)wait_for_terminal(runtime, run_id);
    ASSERT_TRUE(runtime.retry_agent(run_id, "writer", "edited version", "make it concise"));

    for (int attempt = 0; attempt < 100 && !saw_retry.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(saw_retry.load());
}

TEST(DagRuntimeTest, RunSnapshotAndRetryAreOwnedBySession) {
    DagRuntime runtime(test_model());
    RunOptions options;
    options.session_id = "11111111111111111111111111111111";
    options.user_input = "session-owned run";
    options.agents = {default_agents()[4]};
    const auto run_id = runtime.create_run(options);
    (void)wait_for_terminal(runtime, run_id);
    EXPECT_TRUE(runtime.snapshot("22222222222222222222222222222222", run_id).is_null());
    EXPECT_FALSE(runtime.retry_agent(run_id, "22222222222222222222222222222222",
                                     "writer", "edited", "feedback"));
    EXPECT_FALSE(runtime.snapshot(options.session_id, run_id).is_null());
}

TEST(DagRuntimeTest, InjectsAgentPrivateMaterialsOnlyIntoThatAgentPrompt) {
    std::atomic<bool> saw_private{false};
    auto model = std::make_shared<LambdaChatModel>(
        [&saw_private](const std::vector<ChatMessage>& messages, double, const json&) {
            const std::string system = messages.empty() ? "" : messages.front().content;
            const std::string user = messages.size() < 2 ? "" : messages.back().content;
            if (system.find("multi-agent orchestrator") != std::string::npos) {
                return text_response("[{\"agent_id\":\"writer\",\"subtask\":\"Write\",\"depends_on\":[]}]");
            }
            if (user.find("private material") != std::string::npos) saw_private.store(true);
            return text_response("{\"action\":\"final\",\"answer\":\"ok\"}");
        });
    DagRuntime runtime(model);
    RunOptions options;
    options.user_input = "write";
    auto writer = default_agents()[4];
    writer.private_materials = {"private material"};
    options.agents = {writer};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));
    EXPECT_EQ(snapshot["statuses"]["writer"], "done");
    EXPECT_TRUE(saw_private.load());
}

TEST(DagRuntimeTest, WritesCompleteReactExchangeToPerAgentLog) {
    const auto log_directory = std::filesystem::temp_directory_path() / "agent_dag_log_test";
    std::error_code ignored;
    std::filesystem::remove_all(log_directory, ignored);

    DagRuntime runtime(test_model(), log_directory);
    RunOptions options;
    options.user_input = "write a report";
    options.agents = {default_agents()[4]};
    options.max_agent_rounds = 2;
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));
    ASSERT_EQ(snapshot["statuses"]["writer"], "done");

    std::ifstream input(log_directory / "agent_writer.log");
    ASSERT_TRUE(input.good());
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\"phase\": \"react\""), std::string::npos);
    EXPECT_NE(content.find("当前 Agent 子任务"), std::string::npos);
    EXPECT_NE(content.find("deterministic answer"), std::string::npos);

    std::filesystem::remove_all(log_directory, ignored);
}

TEST(DagRuntimeTest, InvalidUtf8InPromptCannotFailAgentExecution) {
    const auto log_directory = std::filesystem::temp_directory_path() / "agent_dag_invalid_utf8_log_test";
    std::error_code ignored;
    std::filesystem::remove_all(log_directory, ignored);

    DagRuntime runtime(test_model(), log_directory);
    RunOptions options;
    options.user_input = "write a report";
    options.user_input.push_back(static_cast<char>(0xFF));
    options.agents = {default_agents()[4]};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));

    EXPECT_EQ(snapshot["statuses"]["writer"], "done");
    std::ifstream input(log_directory / "agent_writer.log");
    ASSERT_TRUE(input.good());
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\\xFF"), std::string::npos);

    std::filesystem::remove_all(log_directory, ignored);
}

TEST(DagRuntimeTest, LongChineseMemoryIsNotCutInsideUtf8Character) {
    std::atomic<bool> second_round_prompt_is_valid{false};
    auto model = std::make_shared<LambdaChatModel>(
        [&second_round_prompt_is_valid](const std::vector<ChatMessage>& messages, double, const json&) {
            const std::string system = messages.empty() ? "" : messages.front().content;
            const std::string user = messages.size() < 2 ? "" : messages.back().content;
            if (system.find("multi-agent orchestrator") != std::string::npos) {
                return text_response("[{\"agent_id\":\"writer\",\"subtask\":\"Write\",\"depends_on\":[]}]");
            }
            if (user.find("当前轮次：2/2") != std::string::npos) {
                try {
                    // This mirrors the UTF-8 validation performed when the
                    // real Qwen model builds its JSON request body.
                    const json request = {{"content", user}};
                    (void)request;
                    second_round_prompt_is_valid.store(true);
                } catch (...) {
                    second_round_prompt_is_valid.store(false);
                }
                return text_response("{\"action\":\"final\",\"answer\":\"done\"}");
            }
            std::string long_observation;
            for (int index = 0; index < 1200; ++index) long_observation += "中文内容";
            return text_response(json({{"action", "reason"}, {"observation", long_observation}, {"answer", ""}}).dump());
        });

    DagRuntime runtime(model);
    RunOptions options;
    options.user_input = "write a report";
    options.agents = {default_agents()[4]};
    options.max_agent_rounds = 2;
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));

    EXPECT_EQ(snapshot["statuses"]["writer"], "done");
    EXPECT_TRUE(second_round_prompt_is_valid.load());
}

TEST(DagRuntimeTest, ForcesToolFreeFinalizationAfterLastNonFinalRound) {
    std::atomic<int> react_calls{0};
    std::atomic<int> finalization_calls{0};
    std::atomic<bool> finalization_has_memory{false};
    std::atomic<bool> finalization_has_no_tools{false};
    auto model = std::make_shared<LambdaChatModel>(
        [&](const std::vector<ChatMessage>& messages, double, const json& schemas) {
            const std::string system = messages.empty() ? "" : messages.front().content;
            const std::string user = messages.size() < 2 ? "" : messages.back().content;
            if (system.find("multi-agent orchestrator") != std::string::npos) {
                return text_response(
                    "[{\"agent_id\":\"writer\",\"subtask\":\"Write\",\"depends_on\":[]}]");
            }
            if (system.find("强制收尾阶段") != std::string::npos) {
                ++finalization_calls;
                finalization_has_no_tools.store(schemas.empty());
                finalization_has_memory.store(
                    user.find("当前 Agent 子任务") != std::string::npos &&
                    user.find("collected evidence") != std::string::npos);
                return text_response(
                    R"({"thought":"summarize","action":"final","observation":"enough","answer":"forced summary"})");
            }
            ++react_calls;
            return text_response(
                R"({"thought":"continue","action":"reason","observation":"collected evidence","answer":""})");
        });

    DagRuntime runtime(model);
    RunOptions options;
    options.user_input = "write a report";
    options.agents = {default_agents()[4]};
    options.max_agent_rounds = 2;
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));

    EXPECT_EQ(react_calls.load(), 2);
    EXPECT_EQ(finalization_calls.load(), 1);
    EXPECT_TRUE(finalization_has_no_tools.load());
    EXPECT_TRUE(finalization_has_memory.load());
    EXPECT_EQ(snapshot["statuses"]["writer"], "done");
    EXPECT_EQ(snapshot["outputs"]["writer"], "forced summary");
    EXPECT_TRUE(std::any_of(snapshot["events"].begin(), snapshot["events"].end(), [](const json& event) {
        return event.value("type", "") == "finalization_forced";
    }));
}

TEST(DagRuntimeTest, RejectsEmptyForcedFinalizationInsteadOfReturningRawMemory) {
    auto model = std::make_shared<LambdaChatModel>(
        [](const std::vector<ChatMessage>& messages, double, const json&) {
            const std::string system = messages.empty() ? "" : messages.front().content;
            if (system.find("multi-agent orchestrator") != std::string::npos) {
                return text_response(
                    "[{\"agent_id\":\"writer\",\"subtask\":\"Write\",\"depends_on\":[]}]");
            }
            if (system.find("强制收尾阶段") != std::string::npos) {
                return text_response(R"({"action":"final","answer":""})");
            }
            return text_response(
                R"({"action":"reason","observation":"unfinished memory","answer":""})");
        });

    DagRuntime runtime(model);
    RunOptions options;
    options.user_input = "write a report";
    options.agents = {default_agents()[4]};
    options.max_agent_rounds = 1;
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));

    EXPECT_EQ(snapshot["statuses"]["writer"], "error");
    EXPECT_NE(snapshot["errors"]["writer"].get<std::string>().find("forced finalization"),
              std::string::npos);
    EXPECT_FALSE(snapshot["outputs"].contains("writer"));
}

#if defined(DAG_TEST_MCP_SERVER) && defined(DAG_TEST_PLUGIN_DIR) && defined(DAG_TEST_TOOL_CONFIG)

class DagToolLoopTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
            ("dag-runtime-tool-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / session_id_ / "workspace");
        std::filesystem::create_directories(root_ / session_id_ / "artifacts");
        std::filesystem::create_directories(root_ / session_id_ / "audit");
        agent_rpc::mcp::MCPAgentConfig config;
        config.enable_mcp = true;
        config.mcp_server_path = DAG_TEST_MCP_SERVER;
        config.mcp_args = {"-p", DAG_TEST_PLUGIN_DIR,
                           "-c", DAG_TEST_TOOL_CONFIG,
                           "--sessions-root", root_.string(),
                           "-l", (root_ / "logs").string()};
        config.max_retry_count = 0;
        tools_ = std::make_shared<agent_rpc::mcp::MCPAgentIntegration>();
        ASSERT_TRUE(tools_->initialize(config));
    }

    void TearDown() override {
        tools_->shutdown();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::shared_ptr<agent_rpc::mcp::MCPAgentIntegration> tools_;
    std::filesystem::path root_;
    const std::string session_id_ = "abcdef0123456789abcdef0123456789";
};

TEST_F(DagToolLoopTest, DesignerRetriesFreshDraftThenRequiresCommit) {
    std::atomic<int> calls{0};
    auto model = std::make_shared<LambdaChatModel>(
        [&](const std::vector<ChatMessage>&, double, const json& schemas) -> ModelResponse {
            if (!has_tool_schema(schemas, "team_design__add_agent")) return text_response("{}");
            const int call = ++calls;
            if (call == 1) return text_response("plain text is not a commit");
            if (call == 2) {
                return {"", {
                    {"add_a", "team_design__add_agent", R"({"id":"a","name":"A","role":"Analyze"})"},
                    {"add_b", "team_design__add_agent", R"({"id":"b","name":"B","role":"Build"})"},
                    {"add_c", "team_design__add_agent", R"({"id":"c","name":"C","role":"Compose"})"}
                }, "tool_calls"};
            }
            return {"", {{"commit", "team_design__commit_team", "{}"}}, "tool_calls"};
        });
    const auto chat_logs = root_ / "chat_logs";
    DagRuntime runtime(model, tools_, chat_logs);
    const auto agents = runtime.draft_agents(session_id_, "design a team", 3, {});
    EXPECT_EQ(agents.size(), 3U);
    EXPECT_EQ(calls.load(), 3);
    std::ifstream input(chat_logs / "agent_designer.log");
    ASSERT_TRUE(input.good());
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\"phase\": \"agent_design\""), std::string::npos);
    EXPECT_NE(content.find("\"model_iteration\": 2"), std::string::npos);
    EXPECT_NE(content.find("team_design__commit_team"), std::string::npos);
}

TEST_F(DagToolLoopTest, PlannerRetriesThenCommitsAndExecutorFinishes) {
    std::atomic<int> planner_calls{0};
    auto model = std::make_shared<LambdaChatModel>(
        [&](const std::vector<ChatMessage>&, double, const json& schemas) -> ModelResponse {
            if (has_tool_schema(schemas, "dag_control__add_node")) {
                const int call = ++planner_calls;
                if (call == 1) return text_response("not committed");
                if (call == 2) {
                    return {"", {{"add", "dag_control__add_node",
                        R"({"agent_id":"a","subtask":"Do A","depends_on":[]})"}}, "tool_calls"};
                }
                return {"", {{"commit", "dag_control__commit_plan", "{}"}}, "tool_calls"};
            }
            return text_response(R"({"thought":"done","action":"final","observation":"ok","answer":"finished"})");
        });
    const auto chat_logs = root_ / "chat_logs";
    DagRuntime runtime(model, tools_, chat_logs);
    RunOptions options;
    options.session_id = session_id_;
    options.user_input = "run a plan";
    options.agents = {{"a", "A", "Do A", "A", {}},
                      {"b", "B", "Do B", "B", {}},
                      {"c", "C", "Do C", "C", {}}};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));
    EXPECT_EQ(planner_calls.load(), 3);
    EXPECT_EQ(snapshot["statuses"]["a"], "done");
    EXPECT_EQ(snapshot["outputs"]["a"], "finished");
    std::ifstream input(chat_logs / "planner.log");
    ASSERT_TRUE(input.good());
    const std::string content((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\"phase\": \"planning\""), std::string::npos);
    EXPECT_NE(content.find("dag_control__commit_plan"), std::string::npos);
}

TEST_F(DagToolLoopTest, PlannerSecondFailureStallsWithoutDefaultDag) {
    auto model = std::make_shared<LambdaChatModel>(
        [](const std::vector<ChatMessage>&, double, const json&) {
            return text_response("never committed");
        });
    DagRuntime runtime(model, tools_);
    RunOptions options;
    options.session_id = session_id_;
    options.user_input = "must stall";
    options.agents = {{"a", "A", "Do A", "A", {}},
                      {"b", "B", "Do B", "B", {}},
                      {"c", "C", "Do C", "C", {}}};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));
    EXPECT_TRUE(snapshot["plan"].empty());
    EXPECT_TRUE(std::any_of(snapshot["events"].begin(), snapshot["events"].end(), [](const json& event) {
        return event.value("type", "") == "run_stalled";
    }));
}

TEST_F(DagToolLoopTest, ExecutorReceivesToolFailureAndCorrectsItsAnswer) {
    std::atomic<int> planner_calls{0};
    std::atomic<int> executor_calls{0};
    std::atomic<bool> saw_native_tool_contract{false};
    auto model = std::make_shared<LambdaChatModel>(
        [&](const std::vector<ChatMessage>& messages, double, const json& schemas) -> ModelResponse {
            if (has_tool_schema(schemas, "dag_control__add_node")) {
                if (++planner_calls == 1) {
                    return {"", {{"add", "dag_control__add_node",
                        R"({"agent_id":"a","subtask":"Do A","depends_on":[]})"}}, "tool_calls"};
                }
                return {"", {{"commit", "dag_control__commit_plan", "{}"}}, "tool_calls"};
            }
            if (++executor_calls == 1) {
                saw_native_tool_contract.store(
                    !messages.empty() &&
                    messages.front().content.find("严禁返回 ReAct JSON") != std::string::npos);
                return {"", {{"missing", "workspace_fs__read_file",
                    R"({"path":"missing.txt"})"}}, "tool_calls"};
            }
            return text_response(R"({"thought":"corrected","action":"final","observation":"missing","answer":"recovered"})");
        });
    DagRuntime runtime(model, tools_);
    RunOptions options;
    options.session_id = session_id_;
    options.user_input = "recover from a tool error";
    options.agents = {{"a", "A", "Do A", "A", {}}};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));
    EXPECT_EQ(snapshot["statuses"]["a"], "done");
    EXPECT_EQ(snapshot["outputs"]["a"], "recovered");
    EXPECT_TRUE(saw_native_tool_contract.load());
    EXPECT_TRUE(std::any_of(snapshot["events"].begin(), snapshot["events"].end(), [](const json& event) {
        return event.value("type", "") == "tool_failed";
    }));
}

TEST_F(DagToolLoopTest, RejectsContentEncodedToolIntentWithoutCallingMcp) {
    std::atomic<int> planner_calls{0};
    std::atomic<int> executor_calls{0};
    auto model = std::make_shared<LambdaChatModel>(
        [&](const std::vector<ChatMessage>&, double, const json& schemas) -> ModelResponse {
            if (has_tool_schema(schemas, "dag_control__add_node")) {
                if (++planner_calls == 1) {
                    return {"", {{"add", "dag_control__add_node",
                        R"({"agent_id":"a","subtask":"Do A","depends_on":[]})"}}, "tool_calls"};
                }
                return {"", {{"commit", "dag_control__commit_plan", "{}"}}, "tool_calls"};
            }
            if (++executor_calls == 1) {
                return text_response(R"({"thought":"need context","action":"workspace_fs__read_file","arguments":{"path":"missing.txt"},"observation":"","answer":""})");
            }
            return text_response(R"({"thought":"recovered","action":"final","observation":"file missing","answer":"recovered"})");
        });
    DagRuntime runtime(model, tools_);
    RunOptions options;
    options.session_id = session_id_;
    options.user_input = "recover from a content encoded tool request";
    options.agents = {{"a", "A", "Do A", "A", {}}};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));
    EXPECT_EQ(snapshot["statuses"]["a"], "error");
    EXPECT_NE(snapshot["errors"]["a"].get<std::string>().find("structured tool_calls"),
              std::string::npos);
    EXPECT_FALSE(snapshot["outputs"].contains("a"));
    EXPECT_TRUE(std::any_of(snapshot["events"].begin(), snapshot["events"].end(), [](const json& event) {
        return event.value("type", "") == "tool_call_rejected" &&
               event.value("payload", json::object()).value("agent_id", "") == "a";
    }));
    EXPECT_FALSE(std::any_of(snapshot["events"].begin(), snapshot["events"].end(), [](const json& event) {
        return event.value("type", "") == "tool_started" &&
               event.value("payload", json::object()).value("agent_id", "") == "a";
    }));
}

TEST_F(DagToolLoopTest, RejectsPseudoToolBlockWithoutCallingMcp) {
    std::atomic<int> planner_calls{0};
    std::atomic<int> executor_calls{0};
    auto model = std::make_shared<LambdaChatModel>(
        [&](const std::vector<ChatMessage>&, double, const json& schemas) -> ModelResponse {
            if (has_tool_schema(schemas, "dag_control__add_node")) {
                if (++planner_calls == 1) {
                    return {"", {{"add", "dag_control__add_node",
                        R"({"agent_id":"a","subtask":"Do A","depends_on":[]})"}}, "tool_calls"};
                }
                return {"", {{"commit", "dag_control__commit_plan", "{}"}}, "tool_calls"};
            }
            if (++executor_calls == 1) {
                return text_response(
                    "{\"thought\":\"need context\",\"action\":\"workspace_fs__stat_path\"}\n"
                    "</tool_call>\n"
                    "{\"name\":\"workspace_fs__stat_path\",\"arguments\":{\"path\":\".\"}}\n"
                    "</tool_call>");
            }
            return text_response(
                R"({"thought":"done","action":"final","observation":"workspace checked","answer":"checked"})");
        });
    DagRuntime runtime(model, tools_);
    RunOptions options;
    options.session_id = session_id_;
    options.user_input = "recover a pseudo tool block";
    options.agents = {{"a", "A", "Do A", "A", {}}};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));

    EXPECT_EQ(snapshot["statuses"]["a"], "error");
    EXPECT_NE(snapshot["errors"]["a"].get<std::string>().find("structured tool_calls"),
              std::string::npos);
    EXPECT_FALSE(snapshot["outputs"].contains("a"));
    EXPECT_TRUE(std::any_of(snapshot["events"].begin(), snapshot["events"].end(), [](const json& event) {
        return event.value("type", "") == "tool_call_rejected" &&
               event.value("payload", json::object()).value("agent_id", "") == "a";
    }));
    EXPECT_FALSE(std::any_of(snapshot["events"].begin(), snapshot["events"].end(), [](const json& event) {
        return event.value("type", "") == "tool_started" &&
               event.value("payload", json::object()).value("agent_id", "") == "a";
    }));
}

TEST_F(DagToolLoopTest, ExecutorToolLoopStopsAfterEightIterations) {
    std::atomic<int> planner_calls{0};
    std::atomic<int> executor_calls{0};
    auto model = std::make_shared<LambdaChatModel>(
        [&](const std::vector<ChatMessage>&, double, const json& schemas) -> ModelResponse {
            if (has_tool_schema(schemas, "dag_control__add_node")) {
                if (++planner_calls == 1) {
                    return {"", {{"add", "dag_control__add_node",
                        R"({"agent_id":"a","subtask":"Loop","depends_on":[]})"}}, "tool_calls"};
                }
                return {"", {{"commit", "dag_control__commit_plan", "{}"}}, "tool_calls"};
            }
            const int call = ++executor_calls;
            return {"", {{"loop_" + std::to_string(call), "workspace_fs__stat_path",
                           R"({"path":"."})"}}, "tool_calls"};
        });
    DagRuntime runtime(model, tools_);
    RunOptions options;
    options.session_id = session_id_;
    options.user_input = "bound the tool loop";
    options.agents = {{"a", "A", "Do A", "A", {}}};
    const auto snapshot = wait_for_terminal(runtime, runtime.create_run(options));
    EXPECT_EQ(executor_calls.load(), 8);
    EXPECT_EQ(snapshot["statuses"]["a"], "error");
    EXPECT_NE(snapshot["errors"]["a"].get<std::string>().find("8 iterations"), std::string::npos);
}

#endif
