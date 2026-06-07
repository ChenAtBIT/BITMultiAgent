#include <agent_rpc/orchestrator/context_memory_manager.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using agent_rpc::orchestrator::ContextMemoryConfig;
using agent_rpc::orchestrator::ContextMemoryManager;
using agent_rpc::orchestrator::ContextMemoryMessage;

namespace {

/**
 * @brief 生成项目内测试记忆目录
 * @param name 测试目录名
 * @return 测试目录路径
 */
std::string make_test_memory_dir(const std::string& name) {
    const std::filesystem::path dir =
        std::filesystem::path(AGENT_RPC_PROJECT_SOURCE_DIR) /
        "build" / "runtime" / "tests" / "context_memory_manager" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir.string();
}

/**
 * @brief 创建适合触发压缩的测试历史
 * @param count 消息数量
 * @return 测试消息列表
 */
std::vector<ContextMemoryMessage> make_history(std::size_t count) {
    std::vector<ContextMemoryMessage> history;
    history.reserve(count);

    for (std::size_t index = 0; index < count; ++index) {
        const bool is_user = (index % 2 == 0);
        history.push_back({
            is_user ? "user" : "assistant",
            is_user
                ? "用户说明自己的研究方向是 Multi Agent 科研协作系统，并偏好 C++ 实现。"
                : "助手确认会在本项目内编译验证，并保留中文关键注释。"
        });
    }

    return history;
}

/**
 * @brief 创建小预算记忆配置，便于单测快速触发压缩
 * @param dir 记忆目录
 * @return 测试配置
 */
ContextMemoryConfig make_config(const std::string& dir) {
    ContextMemoryConfig config;
    config.memory_dir = dir;
    config.max_tokens = 240;
    config.keep_recent_messages = 2;
    config.min_messages_to_compress = 2;
    config.long_term_extract_min_tokens = 1;
    config.compression_source_budget_tokens = 200;
    return config;
}

}  // namespace

TEST(ContextMemoryManagerTest, SanitizesContextIdForFileNames) {
    EXPECT_EQ("lab_ctx_01",
              ContextMemoryManager::sanitize_context_id("lab/ctx 01"));
    const std::string malicious =
        ContextMemoryManager::sanitize_context_id("../../");
    EXPECT_EQ(std::string::npos, malicious.find("."));
    EXPECT_EQ(std::string::npos, malicious.find("/"));
    EXPECT_FALSE(malicious.empty());

    ContextMemoryManager manager(make_config(make_test_memory_dir("sanitize")));
    const std::string path = manager.memory_file_path("../ctx/alpha");
    EXPECT_EQ(std::string::npos, path.find(".."));
    EXPECT_NE(std::string::npos, path.find("ctx_alpha.json"));
}

TEST(ContextMemoryManagerTest, CompressesWithLlmAndPersistsByContextId) {
    ContextMemoryManager manager(
        make_config(make_test_memory_dir("persist")));

    int llm_calls = 0;
    manager.set_llm_compressor(
        [&llm_calls](const std::string& system_prompt,
                     const std::string&) {
            llm_calls++;
            if (system_prompt.find("长期记忆提取器") != std::string::npos) {
                return std::string(
                    "{\"memories\":[{\"content\":\"用户研究方向是 Multi Agent 科研协作系统\","
                    "\"category\":\"research_direction\",\"importance\":9}]}");
            }

            return std::string(
                "用户研究方向是 Multi Agent 科研协作系统；偏好 C++ 实现，"
                "并要求在本项目内编译验证。");
        });

    manager.observe_conversation("lab/user", make_history(6));
    const auto snapshot = manager.load_snapshot("lab/user");

    EXPECT_GE(llm_calls, 2);
    EXPECT_EQ(4U, snapshot.consolidated_message_count);
    EXPECT_NE(std::string::npos,
              snapshot.short_term_summary.find("Multi Agent"));
    ASSERT_EQ(1U, snapshot.long_term_memories.size());
    EXPECT_EQ("research_direction",
              snapshot.long_term_memories.front().category);
    EXPECT_EQ(9, snapshot.long_term_memories.front().importance);
    EXPECT_TRUE(std::filesystem::exists(manager.memory_file_path("lab/user")));
    EXPECT_FALSE(std::filesystem::exists(manager.memory_file_path("other-user")));
}

TEST(ContextMemoryManagerTest, LlmFailureDoesNotAdvanceCompressionCursor) {
    ContextMemoryManager manager(
        make_config(make_test_memory_dir("failure")));

    manager.set_llm_compressor(
        [](const std::string&, const std::string&) -> std::string {
            throw std::runtime_error("mock llm failure");
        });

    manager.observe_conversation("ctx-fail", make_history(6));

    const auto snapshot = manager.load_snapshot("ctx-fail");
    EXPECT_EQ(0U, snapshot.consolidated_message_count);
    EXPECT_TRUE(snapshot.short_term_summary.empty());
    EXPECT_TRUE(snapshot.long_term_memories.empty());
    EXPECT_EQ(1U, manager.stats().total_llm_failures);
}

TEST(ContextMemoryManagerTest, BuildContextAvoidsDuplicatingCompressedMessages) {
    ContextMemoryManager manager(
        make_config(make_test_memory_dir("context")));

    manager.set_llm_compressor(
        [](const std::string& system_prompt,
           const std::string&) {
            if (system_prompt.find("长期记忆提取器") != std::string::npos) {
                return std::string(
                    "{\"memories\":[{\"content\":\"用户偏好中文关键注释和项目内编译\","
                    "\"category\":\"preference\",\"importance\":8}]}");
            }
            return std::string("用户偏好中文关键注释和项目内编译。");
        });

    const auto history = make_history(6);
    manager.observe_conversation("ctx-context", history);
    const auto context =
        manager.build_context_messages("ctx-context", history);

    ASSERT_GE(context.size(), 4U);
    EXPECT_EQ("system", context[0].role);
    EXPECT_NE(std::string::npos, context[0].content.find("长期核心记忆"));
    EXPECT_EQ("system", context[1].role);
    EXPECT_NE(std::string::npos, context[1].content.find("近期会话摘要"));

    // 6 条历史中前 4 条已经压缩，工作记忆只保留最后 2 条。
    EXPECT_EQ("user", context[2].role);
    EXPECT_EQ("assistant", context[3].role);
    EXPECT_EQ(4U, context.size());
}
