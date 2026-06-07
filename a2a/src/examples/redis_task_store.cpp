#include "redis_task_store.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <cstdarg>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

using json = nlohmann::json;

namespace a2a {

RedisTaskStore::RedisTaskStore(const std::string& host,
                               int port,
                               std::string key_namespace)
    : context_(nullptr)
    , host_(host)
    , port_(port)
    , key_namespace_(normalize_key_namespace(std::move(key_namespace))) {
    std::cout << "[RedisTaskStore] 连接到 Redis " << host << ":" << port << std::endl;
    std::cout << "[RedisTaskStore] 使用命名空间: " << key_namespace_ << std::endl;
    
    context_ = redisConnect(host.c_str(), port);
    
    if (context_ == nullptr || context_->err) {
        if (context_) {
            std::string error = context_->errstr;
            redisFree(context_);
            throw std::runtime_error("Redis 连接失败: " + error);
        } else {
            throw std::runtime_error("Redis 连接失败: 无法分配 context");
        }
    }
    
    std::cout << "[RedisTaskStore] 连接成功" << std::endl;
}

RedisTaskStore::~RedisTaskStore() {
    if (context_) {
        redisFree(context_);
        std::cout << "[RedisTaskStore] 断开连接" << std::endl;
    }
}

std::string RedisTaskStore::normalize_key_namespace(std::string key_namespace) {
    if (key_namespace.empty()) {
        return "a2a";
    }

    // 去掉尾部多余的冒号，避免后续拼接产生双分隔符。
    while (!key_namespace.empty() && key_namespace.back() == ':') {
        key_namespace.pop_back();
    }

    return key_namespace.empty() ? "a2a" : key_namespace;
}

std::string RedisTaskStore::sanitize_key_component(const std::string& raw_value) {
    std::string sanitized;
    sanitized.reserve(raw_value.size());

    bool changed = false;
    for (unsigned char ch : raw_value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            sanitized.push_back(static_cast<char>(ch));
            continue;
        }

        // 将分隔符和空白统一替换为下划线，避免命名空间层级被原始值打断。
        sanitized.push_back('_');
        changed = true;
    }

    if (sanitized.empty()) {
        sanitized = "default";
        changed = true;
    }

    const auto hash_value = static_cast<unsigned long long>(
        std::hash<std::string>{}(raw_value));
    std::ostringstream suffix_builder;
    suffix_builder << std::hex << hash_value;
    const std::string hash_suffix = suffix_builder.str();
    std::string suffix;

    if (changed) {
        // 当原始值被替换过时追加短哈希，避免 lab/user 与 lab_user 撞键。
        suffix = "_h" + hash_suffix.substr(0, std::min<std::size_t>(8, hash_suffix.size()));
    }

    constexpr std::size_t kMaxComponentLength = 96;
    if (sanitized.size() > kMaxComponentLength && suffix.empty()) {
        // 对超长但未替换过的 key 也追加短哈希，避免截断后撞键。
        suffix = "_h" + hash_suffix.substr(0, std::min<std::size_t>(8, hash_suffix.size()));
    }

    if (sanitized.size() + suffix.size() > kMaxComponentLength) {
        const std::size_t keep_length =
            kMaxComponentLength > suffix.size() ? kMaxComponentLength - suffix.size() : 0;
        sanitized = sanitized.substr(0, keep_length);
    }

    return sanitized + suffix;
}

std::string RedisTaskStore::task_key(const std::string& task_id) const {
    return key_namespace_ + ":task:" + sanitize_key_component(task_id);
}

std::string RedisTaskStore::history_key(const std::string& context_id) const {
    return key_namespace_ + ":history:" + sanitize_key_component(context_id);
}

void RedisTaskStore::ensure_connection() {
    if (context_ && !context_->err) {
        return;
    }
    
    std::cout << "[RedisTaskStore] 重新连接..." << std::endl;
    
    if (context_) {
        redisFree(context_);
    }
    
    context_ = redisConnect(host_.c_str(), port_);
    
    if (context_ == nullptr || context_->err) {
        throw std::runtime_error("Redis 重连失败");
    }
}

redisReply* RedisTaskStore::execute_command(const char* format, ...) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    ensure_connection();
    
    va_list args;
    va_start(args, format);
    redisReply* reply = static_cast<redisReply*>(redisvCommand(context_, format, args));
    va_end(args);
    
    if (reply == nullptr) {
        throw std::runtime_error("Redis 命令执行失败");
    }
    
    if (reply->type == REDIS_REPLY_ERROR) {
        std::string error = reply->str;
        freeReplyObject(reply);
        throw std::runtime_error("Redis 错误: " + error);
    }
    
    return reply;
}

std::optional<AgentTask> RedisTaskStore::get_task(const std::string& task_id) {
    try {
        auto reply = execute_command("GET %s", task_key(task_id).c_str());
        
        if (reply->type == REDIS_REPLY_NIL) {
            freeReplyObject(reply);
            return std::nullopt;
        }
        
        std::string json_str(reply->str, reply->len);
        freeReplyObject(reply);
        
        // 反序列化 Task
        return AgentTask::from_json(json_str);
        
    } catch (const std::exception& e) {
        std::cerr << "[RedisTaskStore] get_task 错误: " << e.what() << std::endl;
        return std::nullopt;
    }
}

void RedisTaskStore::set_task(const AgentTask& task) {
    try {
        std::string json_str = task.to_json();
        auto reply = execute_command("SET %s %s", 
                                    task_key(task.id()).c_str(),
                                    json_str.c_str());
        freeReplyObject(reply);
        
        std::cout << "[RedisTaskStore] 保存任务: " << task.id() << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "[RedisTaskStore] set_task 错误: " << e.what() << std::endl;
    }
}

bool RedisTaskStore::task_exists(const std::string& task_id) {
    try {
        auto reply = execute_command("EXISTS %s", task_key(task_id).c_str());
        bool exists = (reply->integer == 1);
        freeReplyObject(reply);
        return exists;
        
    } catch (const std::exception& e) {
        std::cerr << "[RedisTaskStore] task_exists 错误: " << e.what() << std::endl;
        return false;
    }
}

bool RedisTaskStore::delete_task(const std::string& task_id) {
    try {
        auto reply = execute_command("DEL %s", task_key(task_id).c_str());
        bool deleted = (reply->integer > 0);
        freeReplyObject(reply);
        return deleted;
        
    } catch (const std::exception& e) {
        std::cerr << "[RedisTaskStore] delete_task 错误: " << e.what() << std::endl;
        return false;
    }
}

void RedisTaskStore::update_status(const std::string& task_id,
                                   TaskState status,
                                   const std::string& message) {
    try {
        auto task = get_task(task_id);
        if (task.has_value()) {
            AgentTaskStatus new_status(status);
            if (!message.empty()) {
                new_status.set_message(message);
            }
            task->set_status(new_status);
            set_task(*task);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[RedisTaskStore] update_status 错误: " << e.what() << std::endl;
    }
}

void RedisTaskStore::add_artifact(const std::string& task_id,
                                  const Artifact& artifact) {
    try {
        auto task = get_task(task_id);
        if (task.has_value()) {
            task->add_artifact(artifact);
            set_task(*task);
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[RedisTaskStore] add_artifact 错误: " << e.what() << std::endl;
    }
}

void RedisTaskStore::add_history_message(const std::string& task_id,
                                        const AgentMessage& message) {
    try {
        std::string json_str = message.to_json();
        // 提前计算历史消息实际写入的 Redis 键名，便于命令复用和日志排查。
        const std::string history_redis_key = history_key(task_id);
        
        // 使用 Redis List 存储历史消息
        auto reply = execute_command("RPUSH %s %s",
                                    history_redis_key.c_str(),
                                    json_str.c_str());
        freeReplyObject(reply);
        
        std::cout << "[RedisTaskStore] 添加历史消息到: " << task_id 
                  << " (键名: " << history_redis_key << ")"
                  << " (角色: " << (message.role() == MessageRole::User ? "User" : "Agent") << ")"
                  << std::endl;
        
        // 可选：限制历史长度（保留最近 1000 条）
        reply = execute_command("LTRIM %s -1000 -1", history_redis_key.c_str());
        freeReplyObject(reply);
        
    } catch (const std::exception& e) {
        std::cerr << "[RedisTaskStore] add_history_message 错误: " << e.what() << std::endl;
    }
}

std::vector<AgentMessage> RedisTaskStore::get_history(const std::string& context_id,
                                                      int max_length) {
    std::vector<AgentMessage> history;
    
    try {
        redisReply* reply;
        
        if (max_length <= 0) {
            // 获取所有历史
            reply = execute_command("LRANGE %s 0 -1", history_key(context_id).c_str());
        } else {
            // 获取最近 N 条
            reply = execute_command("LRANGE %s -%d -1", 
                                  history_key(context_id).c_str(),
                                  max_length);
        }
        
        if (reply->type == REDIS_REPLY_ARRAY) {
            std::cout << "[RedisTaskStore] 获取历史: " << context_id 
                      << " (数量: " << reply->elements << ")" << std::endl;
            
            for (size_t i = 0; i < reply->elements; ++i) {
                if (reply->element[i]->type == REDIS_REPLY_STRING) {
                    std::string json_str(reply->element[i]->str, reply->element[i]->len);
                    try {
                        auto message = AgentMessage::from_json(json_str);
                        history.push_back(message);
                    } catch (const std::exception& e) {
                        std::cerr << "[RedisTaskStore] 反序列化消息失败: " << e.what() << std::endl;
                    }
                }
            }
        }
        
        freeReplyObject(reply);
        
    } catch (const std::exception& e) {
        std::cerr << "[RedisTaskStore] get_history 错误: " << e.what() << std::endl;
    }
    
    return history;
}

} // namespace a2a
