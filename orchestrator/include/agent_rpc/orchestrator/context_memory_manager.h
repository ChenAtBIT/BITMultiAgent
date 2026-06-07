#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace agent_rpc {
namespace orchestrator {

/**
 * @brief 上下文记忆中的单条消息
 */
struct ContextMemoryMessage {
    std::string role;
    std::string content;
};

/**
 * @brief 长期记忆条目
 */
struct LongTermMemoryItem {
    std::string content;
    std::string category;
    std::string source;
    std::string created_at;
    std::string updated_at;
    int importance = 5;
    int hit_count = 1;
};

/**
 * @brief 单个 context_id 的记忆快照
 */
struct ContextMemorySnapshot {
    std::string context_id;
    std::string sanitized_context_id;
    std::string short_term_summary;
    std::size_t consolidated_message_count = 0;
    std::vector<LongTermMemoryItem> long_term_memories;
};

/**
 * @brief 分层记忆管理器配置
 */
struct ContextMemoryConfig {
    std::size_t max_tokens = 8000;
    double working_memory_ratio = 0.5;
    double short_term_ratio = 0.25;
    double long_term_ratio = 0.25;
    std::size_t keep_recent_messages = 8;
    std::size_t min_messages_to_compress = 6;
    std::size_t compression_source_budget_tokens = 6000;
    std::size_t long_term_extract_min_tokens = 220;
    std::size_t max_long_term_items = 40;
    std::string memory_dir = "build/runtime/context_memory";
};

/**
 * @brief 分层记忆管理器运行统计
 */
struct ContextMemoryStats {
    std::size_t total_compressions = 0;
    std::size_t total_long_term_extractions = 0;
    std::size_t total_llm_failures = 0;
};

/**
 * @brief 分层记忆上下文管理器
 *
 * 该类不直接依赖具体 LLM 客户端，而是通过 LlmCompressor 回调完成摘要压缩
 * 和长期记忆提取，便于复用和单元测试。
 */
class ContextMemoryManager {
public:
    using LlmCompressor = std::function<std::string(
        const std::string& system_prompt,
        const std::string& user_prompt)>;

    /**
     * @brief 构造分层记忆管理器
     * @param config 记忆管理配置
     */
    explicit ContextMemoryManager(ContextMemoryConfig config = {});

    /**
     * @brief 设置用于摘要压缩和长期记忆提取的 LLM 回调
     * @param compressor LLM 调用函数
     */
    void set_llm_compressor(LlmCompressor compressor);

    /**
     * @brief 观察完整历史，并在超出工作窗口时触发压缩
     * @param context_id 会话上下文 ID
     * @param history 当前上下文的完整历史消息
     */
    void observe_conversation(const std::string& context_id,
                              const std::vector<ContextMemoryMessage>& history);

    /**
     * @brief 构造可注入 LLM 的分层上下文消息
     * @param context_id 会话上下文 ID
     * @param recent_history 当前上下文的历史消息
     * @return 长期记忆、短期摘要和工作记忆组成的消息列表
     */
    std::vector<ContextMemoryMessage> build_context_messages(
        const std::string& context_id,
        const std::vector<ContextMemoryMessage>& recent_history) const;

    /**
     * @brief 读取指定上下文的记忆快照
     * @param context_id 会话上下文 ID
     * @return 记忆快照
     */
    ContextMemorySnapshot load_snapshot(const std::string& context_id) const;

    /**
     * @brief 获取运行统计
     * @return 统计信息
     */
    ContextMemoryStats stats() const;

    /**
     * @brief 获取 context_id 对应的长期记忆文件路径
     * @param context_id 会话上下文 ID
     * @return 记忆文件路径
     */
    std::string memory_file_path(const std::string& context_id) const;

    /**
     * @brief 将 context_id 转换为安全文件名片段
     * @param context_id 原始上下文 ID
     * @return 安全文件名片段
     */
    static std::string sanitize_context_id(const std::string& context_id);

    /**
     * @brief 估算文本 token 数
     * @param text 输入文本
     * @return 近似 token 数
     */
    static std::size_t estimate_tokens(const std::string& text);

private:
    struct MemoryState;

    std::size_t working_budget() const;
    std::size_t short_term_budget() const;
    std::size_t long_term_budget() const;

    MemoryState load_state_unlocked(const std::string& context_id) const;
    void save_state_unlocked(const MemoryState& state) const;
    bool compress_short_term_unlocked(MemoryState& state,
                                      const std::vector<ContextMemoryMessage>& messages);
    void extract_long_term_unlocked(MemoryState& state);
    std::string summarize_messages_unlocked(const MemoryState& state,
                                            const std::vector<ContextMemoryMessage>& messages);
    std::vector<LongTermMemoryItem> extract_memories_from_summary_unlocked(
        const MemoryState& state);
    std::string format_messages_for_prompt(
        const std::vector<ContextMemoryMessage>& messages) const;
    std::string format_long_term_memories(const MemoryState& state) const;
    void merge_long_term_memories(MemoryState& state,
                                  const std::vector<LongTermMemoryItem>& memories) const;

    ContextMemoryConfig config_;
    LlmCompressor llm_compressor_;
    mutable ContextMemoryStats stats_;
    mutable std::mutex mutex_;
};

}  // namespace orchestrator
}  // namespace agent_rpc
