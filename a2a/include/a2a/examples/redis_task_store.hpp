#pragma once

#include <a2a/server/task_store.hpp>
#include <hiredis/hiredis.h>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <stdexcept>

namespace a2a {

/**
 * @brief Redis-based TaskStore implementation for distributed deployment
 * 
 * This implementation uses Redis to store tasks and history messages,
 * allowing multiple agents in different processes/machines to share the same storage.
 */
class RedisTaskStore : public ITaskStore {
public:
    /**
     * @brief Construct with Redis connection parameters
     * @param host Redis host (default: localhost)
     * @param port Redis port (default: 6379)
     * @param key_namespace Redis 键命名空间，默认兼容旧版 a2a:task / a2a:history
     */
    explicit RedisTaskStore(const std::string& host = "127.0.0.1",
                            int port = 6379,
                            std::string key_namespace = "a2a");
    
    ~RedisTaskStore();
    
    // Disable copy, enable move
    RedisTaskStore(const RedisTaskStore&) = delete;
    RedisTaskStore& operator=(const RedisTaskStore&) = delete;
    RedisTaskStore(RedisTaskStore&&) noexcept = default;
    RedisTaskStore& operator=(RedisTaskStore&&) noexcept = default;
    
    // ITaskStore interface implementation
    std::optional<AgentTask> get_task(const std::string& task_id) override;
    void set_task(const AgentTask& task) override;
    bool task_exists(const std::string& task_id) override;
    bool delete_task(const std::string& task_id) override;
    void update_status(const std::string& task_id,
                      TaskState status,
                      const std::string& message = "") override;
    void add_artifact(const std::string& task_id,
                     const Artifact& artifact) override;
    void add_history_message(const std::string& task_id,
                            const AgentMessage& message) override;
    std::vector<AgentMessage> get_history(const std::string& context_id,
                                          int max_length = 0) override;

private:
    /**
     * @brief 规范化命名空间，避免出现尾部多余分隔符
     */
    static std::string normalize_key_namespace(std::string key_namespace);

    /**
     * @brief 规范化键组件，避免不同层级直接拼接原始上下文
     */
    static std::string sanitize_key_component(const std::string& raw_value);

    /**
     * @brief Get Redis key for task
     */
    std::string task_key(const std::string& task_id) const;

    /**
     * @brief Get Redis key for history
     */
    std::string history_key(const std::string& context_id) const;

    /**
     * @brief Execute Redis command and check for errors
     */
    redisReply* execute_command(const char* format, ...);
    
    /**
     * @brief Reconnect to Redis if connection is lost
     */
    void ensure_connection();
    
    redisContext* context_;
    std::string host_;
    int port_;
    std::string key_namespace_;
    mutable std::mutex mutex_;
};

} // namespace a2a
