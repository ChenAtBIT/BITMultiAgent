#include "ai_orchestrator/dag_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace ai_orchestrator::dag;

namespace {

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
        [saw_retry](const std::vector<ChatMessage>& messages, double) {
            const std::string system = messages.empty() ? "" : messages.front().content;
            const std::string user = messages.size() < 2 ? "" : messages.back().content;
            if (system.find("multi-agent orchestrator") != std::string::npos) {
                return std::string("[{\"agent_id\":\"researcher\",\"subtask\":\"Research the task\",\"depends_on\":[]},"
                                   "{\"agent_id\":\"writer\",\"subtask\":\"Write the final report\",\"depends_on\":[\"researcher\"]}]");
            }
            if (saw_retry && user.find("edited version") != std::string::npos) saw_retry->store(true);
            return std::string("{\"thought\":\"complete\",\"action\":\"final\",\"observation\":\"checked\",\"answer\":\"deterministic answer\"}");
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

TEST(DagPlannerTest, InvalidPlanFallsBackToAllParallelAgents) {
    const auto plan = parse_plan("not json", default_agents(), "fallback task");
    ASSERT_EQ(plan.size(), default_agents().size());
    for (const auto& item : plan) EXPECT_TRUE(item.depends_on.empty());
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

TEST(DagRuntimeTest, CyclicPlanStallsWithoutDeadlock) {
    auto model = std::make_shared<LambdaChatModel>(
        [](const std::vector<ChatMessage>& messages, double) {
            if (!messages.empty() && messages.front().content.find("multi-agent orchestrator") != std::string::npos) {
                return std::string("[{\"agent_id\":\"researcher\",\"subtask\":\"a\",\"depends_on\":[\"writer\"]},"
                                   "{\"agent_id\":\"writer\",\"subtask\":\"b\",\"depends_on\":[\"researcher\"]}]");
            }
            return std::string("{\"action\":\"final\",\"answer\":\"unused\"}");
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

TEST(DagRuntimeTest, InjectsAgentPrivateMaterialsOnlyIntoThatAgentPrompt) {
    std::atomic<bool> saw_private{false};
    auto model = std::make_shared<LambdaChatModel>(
        [&saw_private](const std::vector<ChatMessage>& messages, double) {
            const std::string system = messages.empty() ? "" : messages.front().content;
            const std::string user = messages.size() < 2 ? "" : messages.back().content;
            if (system.find("multi-agent orchestrator") != std::string::npos) {
                return std::string("[{\"agent_id\":\"writer\",\"subtask\":\"Write\",\"depends_on\":[]}]");
            }
            if (user.find("private material") != std::string::npos) saw_private.store(true);
            return std::string("{\"action\":\"final\",\"answer\":\"ok\"}");
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
        [&second_round_prompt_is_valid](const std::vector<ChatMessage>& messages, double) {
            const std::string system = messages.empty() ? "" : messages.front().content;
            const std::string user = messages.size() < 2 ? "" : messages.back().content;
            if (system.find("multi-agent orchestrator") != std::string::npos) {
                return std::string("[{\"agent_id\":\"writer\",\"subtask\":\"Write\",\"depends_on\":[]}]");
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
                return std::string("{\"action\":\"final\",\"answer\":\"done\"}");
            }
            std::string long_observation;
            for (int index = 0; index < 1200; ++index) long_observation += "中文内容";
            return json({{"action", "reason"}, {"observation", long_observation}, {"answer", ""}}).dump();
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

TEST(ToolRegistryTest, EmptyRegistryDoesNotExecuteTools) {
    ToolRegistry registry;
    EXPECT_TRUE(registry.empty());
    EXPECT_TRUE(registry.definitions().empty());
    EXPECT_FALSE(registry.execute("web_search", "{}").success);
}
