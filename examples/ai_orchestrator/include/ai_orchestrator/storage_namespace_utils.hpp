#pragma once

#include <agent_rpc/orchestrator/context_memory_manager.h>

#include <filesystem>
#include <string>

namespace ai_orchestrator {
namespace storage {

/**
 * @brief 规范化存储组件标识
 * @param raw_value 原始标识
 * @return 可安全用于 Redis 命名空间和目录名的字符串
 */
inline std::string sanitize_storage_component(const std::string& raw_value) {
    return agent_rpc::orchestrator::ContextMemoryManager::sanitize_context_id(raw_value);
}

/**
 * @brief 生成 Orchestrator 的 Redis 命名空间
 * @return Orchestrator 专用命名空间
 */
inline std::string build_orchestrator_redis_namespace() {
    return "a2a:v2:orch";
}

/**
 * @brief 生成子 Agent 的 Redis 命名空间
 * @param agent_id Agent 标识
 * @return Agent 专用命名空间
 */
inline std::string build_agent_redis_namespace(const std::string& agent_id) {
    return "a2a:v2:agent:" + sanitize_storage_component(agent_id);
}

/**
 * @brief 拼接某个运行时主体的记忆目录
 * @param owner_kind 主体类别，例如 orchestrator 或 agents
 * @param owner_id 主体标识
 * @param root_dir 记忆根目录
 * @return 该主体的专属记忆目录
 */
inline std::string build_memory_dir(const std::string& owner_kind,
                                    const std::string& owner_id,
                                    const std::string& root_dir =
                                        "build/runtime/context_memory") {
    const std::filesystem::path base_dir(root_dir);
    const std::filesystem::path full_path =
        base_dir / owner_kind / sanitize_storage_component(owner_id);
    return full_path.string();
}

/**
 * @brief 构造 Orchestrator 使用的分层记忆配置
 * @param agent_id Orchestrator 标识
 * @return 指向 Orchestrator 独立目录的配置
 */
inline agent_rpc::orchestrator::ContextMemoryConfig
make_orchestrator_memory_config(const std::string& agent_id) {
    agent_rpc::orchestrator::ContextMemoryConfig config;
    config.memory_dir = build_memory_dir("orchestrator", agent_id);
    return config;
}

/**
 * @brief 构造子 Agent 使用的分层记忆配置
 * @param agent_id Agent 标识
 * @return 指向 Agent 独立目录的配置
 */
inline agent_rpc::orchestrator::ContextMemoryConfig
make_agent_memory_config(const std::string& agent_id) {
    agent_rpc::orchestrator::ContextMemoryConfig config;
    config.memory_dir = build_memory_dir("agents", agent_id);
    return config;
}

}  // namespace storage
}  // namespace ai_orchestrator
