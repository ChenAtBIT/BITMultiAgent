#pragma once

#include "http_server.hpp"
#include "qwen_client.hpp"
#include "redis_task_store.hpp"
#include "registry_client.hpp"

#include <a2a/core/error_code.hpp>
#include <a2a/core/jsonrpc_request.hpp>
#include <a2a/core/jsonrpc_response.hpp>
#include <a2a/models/agent_message.hpp>
#include <a2a/models/agent_task.hpp>
#include <a2a/models/message_part.hpp>
#include <a2a/models/task_status.hpp>

#include <agent_rpc/mcp/mcp_agent_integration.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using react_json = nlohmann::json;

/**
 * @brief 通用 Agent 运行时配置
 */
struct AgentRuntimeConfig {
    std::string agent_id;
    std::string agent_name;
    std::string listen_address;
    std::string registry_url;
    std::string api_key;
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    agent_rpc::mcp::MCPAgentConfig mcp_config;
    std::vector<std::string> registration_tags;
};

/**
 * @brief Agent 单次响应结果
 *
 * text 是最终返回给调用方的自然语言文本；
 * stream_events 是在流式模式下正文前额外发送的状态事件。
 */
struct AgentResponsePayload {
    std::string text;
    std::vector<react_json> stream_events;
};

/**
 * @brief ReAct 处理前的查询准备结果
 */
struct ReActPreparation {
    std::string model_query;
    std::vector<react_json> stream_events;
};

/**
 * @brief 单轮 ReAct 推理可见的工具上下文
 *
 * tool_definitions 是暴露给 LLM 的候选工具定义；
 * visible_tool_names 用于校验后续 tool_call 是否仍在候选集合内。
 */
struct ReActToolContext {
    std::vector<QwenToolDefinition> tool_definitions;
    std::vector<std::string> visible_tool_names;
};

/**
 * @brief 通用 A2A Agent 运行时基类
 *
 * 负责：
 * - HTTP/A2A 同步与流式请求处理
 * - Redis 历史消息持久化
 * - 服务注册中心注册
 * - MCP 生命周期管理
 */
class A2AAgentRuntime {
public:
    explicit A2AAgentRuntime(const AgentRuntimeConfig& config)
        : config_(config)
        , task_store_(std::make_shared<a2a::RedisTaskStore>(config.redis_host, config.redis_port))
        , qwen_client_(config.api_key)
        , registry_client_(config.registry_url)
        , mcp_integration_(std::make_unique<agent_rpc::mcp::MCPAgentIntegration>()) {
        if (!mcp_integration_->initialize(config_.mcp_config)) {
            std::cerr << log_prefix() << " MCP 初始化失败，将在无 MCP 模式下运行" << std::endl;
        } else if (mcp_integration_->isAvailable()) {
            auto tools = mcp_integration_->getToolNames();
            std::cout << log_prefix() << " MCP 已启用，可用工具:";
            for (const auto& tool : tools) {
                std::cout << " " << tool;
            }
            std::cout << std::endl;
        }

        std::cout << log_prefix() << " 初始化完成" << std::endl;
    }

    virtual ~A2AAgentRuntime() {
        if (mcp_integration_) {
            mcp_integration_->shutdown();
        }
    }

    /**
     * @brief 启动 Agent 的 HTTP 服务并注册到服务中心
     * @param port HTTP 服务监听端口
     */
    void start(int port) {
        HttpServer server(port);

        server.register_handler("/", [this](const std::string& body) {
            return this->handle_request(body);
        });
        server.register_stream_handler("/", [this](const std::string& body,
            std::function<bool(const std::string&)> write_callback) {
            this->handle_stream_request(body, write_callback);
        });
        server.register_handler("/.well-known/agent-card.json", [this](const std::string&) {
            return this->build_agent_card_json().dump();
        });

        server.listen();
        std::cout << log_prefix() << " 启动在端口 " << port << std::endl;

        AgentRegistration registration;
        registration.id = config_.agent_id;
        registration.name = config_.agent_name;
        registration.address = config_.listen_address;
        registration.tags = config_.registration_tags;

        if (registry_client_.register_agent(registration)) {
            std::cout << log_prefix() << " 已注册到服务中心" << std::endl;
        } else {
            std::cerr << log_prefix() << " 注册失败" << std::endl;
        }

        server.serve();
    }

protected:
    /**
     * @brief 由子类实现核心业务处理
     * @param user_text 用户输入文本
     * @param context_id 会话上下文 ID
     * @return 业务处理结果
     */
    virtual AgentResponsePayload handle_agent_request(const std::string& user_text,
                                                      const std::string& context_id) = 0;

    /**
     * @brief 由子类提供 Agent Card
     * @return Agent Card JSON
     */
    virtual react_json build_agent_card_json() const = 0;

    /**
     * @brief 获取最近若干轮历史，并转换为 Qwen 消息格式
     * @param context_id 会话上下文 ID
     * @param limit 最多读取的消息数
     * @return 转换后的消息列表
     */
    std::vector<QwenMessage> build_history_messages(const std::string& context_id,
                                                    size_t limit) const {
        std::vector<QwenMessage> messages;
        const auto history = task_store_->get_history(context_id, static_cast<int>(limit));
        messages.reserve(history.size());

        for (const auto& message : history) {
            QwenMessage qwen_message;
            qwen_message.role =
                (message.role() == a2a::MessageRole::User) ? "user" : "assistant";
            qwen_message.content = extract_text_from_message(message);
            messages.push_back(std::move(qwen_message));
        }

        return messages;
    }

    /**
     * @brief 将最近若干轮历史拼成可读文本
     * @param context_id 会话上下文 ID
     * @param limit 最多读取的消息数
     * @return 文本形式的历史记录
     */
    std::string build_history_text(const std::string& context_id, size_t limit) const {
        const auto history = task_store_->get_history(context_id, static_cast<int>(limit));
        std::string history_text;

        for (const auto& message : history) {
            const std::string role =
                (message.role() == a2a::MessageRole::User) ? "user" : "assistant";
            history_text += role + ": " + extract_text_from_message(message) + "\n";
        }

        return history_text;
    }

    /**
     * @brief 保存消息到 Redis 历史存储
     * @param context_id 会话上下文 ID
     * @param message 待保存消息
     */
    void save_message(const std::string& context_id, const a2a::AgentMessage& message) {
        if (!task_store_->task_exists(context_id)) {
            auto task = a2a::AgentTask::create()
                .with_id(context_id)
                .with_context_id(context_id)
                .with_status(a2a::TaskState::Running);
            task_store_->set_task(task);
        }
        task_store_->add_history_message(context_id, message);
    }

    /**
     * @brief 访问 Qwen 客户端
     */
    QwenClient& qwen_client() { return qwen_client_; }

    /**
     * @brief 访问 MCP 集成器
     */
    agent_rpc::mcp::MCPAgentIntegration* mcp_integration() const {
        return mcp_integration_.get();
    }

    /**
     * @brief 获取 Agent 名称对应的日志前缀
     */
    std::string log_prefix() const {
        return "[" + config_.agent_name + "]";
    }

    /**
     * @brief 去掉首尾空白字符
     * @param text 原始文本
     * @return 去空白后的文本
     */
    static std::string trim(const std::string& text) {
        size_t start = 0;
        while (start < text.size() &&
               std::isspace(static_cast<unsigned char>(text[start]))) {
            start++;
        }

        size_t end = text.size();
        while (end > start &&
               std::isspace(static_cast<unsigned char>(text[end - 1]))) {
            end--;
        }

        return text.substr(start, end - start);
    }

private:
    /**
     * @brief 从消息对象中提取首个文本分片
     * @param message A2A 消息
     * @return 文本内容
     */
    static std::string extract_text_from_message(const a2a::AgentMessage& message) {
        if (message.parts().empty()) {
            return "";
        }

        auto* text_part = dynamic_cast<a2a::TextPart*>(message.parts()[0].get());
        if (!text_part) {
            return "";
        }

        return text_part->text();
    }

    /**
     * @brief 以 UTF-8 安全方式切分字符串
     * @param text 原始文本
     * @param start 起始偏移
     * @param max_len 最大切片长度
     * @return 安全切出的片段
     */
    static std::string utf8_safe_chunk(const std::string& text,
                                       size_t start,
                                       size_t max_len) {
        if (start >= text.size()) {
            return "";
        }

        size_t end = std::min(start + max_len, text.size());
        while (end > start && end < text.size()) {
            const unsigned char current = static_cast<unsigned char>(text[end]);
            if ((current & 0xC0) == 0x80) {
                end--;
            } else {
                break;
            }
        }

        return text.substr(start, end - start);
    }

    /**
     * @brief 处理同步消息请求
     * @param body JSON-RPC 请求体
     * @return JSON-RPC 响应体
     */
    std::string handle_request(const std::string& body) {
        try {
            const auto request_json = react_json::parse(body);
            const auto request = a2a::JsonRpcRequest::from_json(body);

            if (request.method() != "message/send") {
                return a2a::JsonRpcResponse::create_error(
                    request.id(),
                    a2a::ErrorCode::MethodNotFound,
                    "Method not found").to_json();
            }

            const auto params_json = request_json["params"];
            const auto message = a2a::AgentMessage::from_json(params_json["message"].dump());
            const std::string user_text = extract_text_from_message(message); // [..]["params"]["message"]
            const std::string context_id = message.context_id().value_or("default");

            std::cout << log_prefix() << " 收到消息: " << user_text << std::endl;
            save_message(context_id, message);

            const AgentResponsePayload payload = handle_agent_request(user_text, context_id);

            auto response_msg = a2a::AgentMessage::create()
                .with_role(a2a::MessageRole::Agent)
                .with_context_id(context_id);
            response_msg.add_text_part(payload.text);
            save_message(context_id, response_msg);

            return a2a::JsonRpcResponse::create_success(
                request.id(),
                response_msg.to_json()).to_json();
        } catch (const std::exception& e) {
            std::cerr << log_prefix() << " 错误: " << e.what() << std::endl;
            return a2a::JsonRpcResponse::create_error(
                "1",
                a2a::ErrorCode::InternalError,
                e.what()).to_json();
        }
    }

    /**
     * @brief 处理流式消息请求
     * @param body JSON-RPC 请求体
     * @param write_callback SSE 写回调
     */
    void handle_stream_request(const std::string& body,
                               std::function<bool(const std::string&)> write_callback) {
        try {
            const auto request_json = react_json::parse(body);
            const auto request = a2a::JsonRpcRequest::from_json(body);

            if (request.method() != "message/stream") {
                react_json error_response = {
                    {"jsonrpc", "2.0"},
                    {"id", request.id()},
                    {"error", {
                        {"code", -32601},
                        {"message", "Method not found for streaming"}
                    }}
                };
                write_callback(error_response.dump());
                return;
            }

            const auto params_json = request_json["params"];
            const auto message = a2a::AgentMessage::from_json(params_json["message"].dump());
            const std::string user_text = extract_text_from_message(message);
            const std::string context_id = message.context_id().value_or("default");

            std::cout << log_prefix() << " 收到流式消息: " << user_text << std::endl;
            save_message(context_id, message);

            react_json start_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {
                    {"type", "stream_start"},
                    {"contextId", context_id}
                }}
            };
            write_callback(start_event.dump());

            const AgentResponsePayload payload = handle_agent_request(user_text, context_id);

            for (const auto& event_result : payload.stream_events) {
                react_json event = {
                    {"jsonrpc", "2.0"},
                    {"id", request.id()},
                    {"result", event_result}
                };
                if (!write_callback(event.dump())) {
                    return;
                }
            }

            constexpr size_t kChunkSize = 50;
            for (size_t pos = 0; pos < payload.text.size();) {
                const std::string chunk = utf8_safe_chunk(payload.text, pos, kChunkSize);
                if (chunk.empty()) {
                    break;
                }

                pos += chunk.size();

                react_json chunk_event = {
                    {"jsonrpc", "2.0"},
                    {"id", request.id()},
                    {"result", {
                        {"type", "chunk"},
                        {"content", chunk}
                    }}
                };
                if (!write_callback(chunk_event.dump())) {
                    return;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            auto response_msg = a2a::AgentMessage::create()
                .with_role(a2a::MessageRole::Agent)
                .with_context_id(context_id);
            response_msg.add_text_part(payload.text);
            save_message(context_id, response_msg);

            react_json end_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {
                    {"type", "stream_end"},
                    {"message", response_msg.to_json()}
                }}
            };
            write_callback(end_event.dump());
        } catch (const std::exception& e) {
            std::cerr << log_prefix() << " 流式处理错误: " << e.what() << std::endl;
            react_json error_event = {
                {"jsonrpc", "2.0"},
                {"id", "1"},
                {"error", {
                    {"code", -32603},
                    {"message", e.what()}
                }}
            };
            write_callback(error_event.dump());
        }
    }

    AgentRuntimeConfig config_;
    std::shared_ptr<a2a::RedisTaskStore> task_store_;
    QwenClient qwen_client_;
    RegistryClient registry_client_;
    std::unique_ptr<agent_rpc::mcp::MCPAgentIntegration> mcp_integration_;
};

/**
 * @brief 带 ReAct 工具调用能力的通用 Agent 模板
 *
 * 子类只需覆写系统提示词、查询预处理和工具筛选逻辑，
 * 即可复用统一的多轮对话与工具调用回环。
 */
class ReActAgentTemplate : public A2AAgentRuntime {
public:
    explicit ReActAgentTemplate(const AgentRuntimeConfig& config)
        : A2AAgentRuntime(config) {}

protected:
    /**
     * @brief 默认的 ReAct 查询预处理
     * @param user_text 用户原始输入
     * @param context_id 会话上下文 ID
     * @return 默认直接把原始输入交给模型
     */
    virtual ReActPreparation prepare_query(const std::string& user_text,
                                           const std::string& /*context_id*/) {
        return {user_text, {}};
    }

    /**
     * @brief 由子类构造领域系统提示词
     * @param prepared_query 预处理后的查询
     * @param context_id 会话上下文 ID
     * @return 发给模型的 system prompt
     */
    virtual std::string build_react_system_prompt(const std::string& prepared_query,
                                                  const std::string& context_id) const = 0;

    /**
     * @brief 构造当前查询可见的工具列表
     * @param prepared_query 预处理后的查询
     * @param context_id 会话上下文 ID
     * @return 提供给模型的工具定义列表
     */
    virtual std::vector<QwenToolDefinition> build_tool_definitions(
        const std::string& prepared_query,
        const std::string& /*context_id*/) {
        std::vector<QwenToolDefinition> tool_definitions;
        auto* integration = mcp_integration();
        if (!integration || !integration->isAvailable()) {
            return tool_definitions;
        }

        // 使用 RAG-MCP 先为当前请求检索候选工具，但是否调用、调用哪个仍交给 LLM 决定。
        const auto relevant_tools = integration->getRelevantTools(prepared_query, rag_tool_top_k());
        tool_definitions.reserve(relevant_tools.size());
        for (const auto& tool : relevant_tools) {
            tool_definitions.push_back({
                tool.name,
                tool.description,
                tool.input_schema
            });
        }

        return tool_definitions;
    }

    /**
     * @brief 构造本轮可见工具上下文
     * @param prepared_query 预处理后的查询
     * @param context_id 会话上下文 ID
     * @return 候选工具定义及名称集合
     */
    virtual ReActToolContext build_tool_context(const std::string& prepared_query,
                                                const std::string& context_id) {
        ReActToolContext tool_context;
        tool_context.tool_definitions = build_tool_definitions(prepared_query, context_id);
        tool_context.visible_tool_names.reserve(tool_context.tool_definitions.size());

        for (const auto& tool : tool_context.tool_definitions) {
            if (!tool.name.empty()) {
                tool_context.visible_tool_names.push_back(tool.name);
            }
        }

        return tool_context;
    }

    /**
     * @brief 构造额外的工具选择提示词
     * @param tool_context 当前轮次的候选工具上下文
     * @param prepared_query 预处理后的查询
     * @param context_id 会话上下文 ID
     * @return 发给模型的补充 system prompt
     */
    virtual std::string build_tool_selection_prompt(const ReActToolContext& tool_context,
                                                    const std::string& /*prepared_query*/,
                                                    const std::string& /*context_id*/) const {
        if (tool_context.tool_definitions.empty()) {
            return "";
        }

        std::string prompt =
            "当前提供给你的 tools 已经过检索过滤，只代表本轮任务的候选工具集合。"
            "是否需要调用工具、以及具体调用哪个工具，由你自行判断。"
            "如果现有上下文已经足够，请直接回答。"
            "不要编造未出现在 tools 列表中的工具。"
            "\n候选工具：";

        for (const auto& tool : tool_context.tool_definitions) {
            prompt += "\n- " + tool.name;
            if (!tool.description.empty()) {
                prompt += "：";
                prompt += tool.description;
            }
        }

        return prompt;
    }

    /**
     * @brief 执行模型返回的工具调用
     * @param tool_call 模型请求的工具调用
     * @param prepared_query 预处理后的查询
     * @param context_id 会话上下文 ID
     * @return 需要回填给模型的工具输出文本
     */
    virtual std::string execute_tool_call(const QwenToolCall& tool_call,
                                          const std::string& /*prepared_query*/,
                                          const std::string& /*context_id*/) {
        auto* integration = mcp_integration();
        if (!integration || !integration->isAvailable()) {
            return build_tool_error_payload(tool_call.name, "MCP is not available");
        }
        if (tool_call.name.empty()) {
            return build_tool_error_payload("<empty>", "Tool name is empty");
        }

        const std::string arguments_json = trim(tool_call.arguments_json);
        if (arguments_json.empty()) {
            return build_tool_error_payload(tool_call.name, "Tool arguments are empty");
        }

        try {
            const react_json parsed_arguments = react_json::parse(arguments_json);
            (void)parsed_arguments;
        } catch (const std::exception& e) {
            return build_tool_error_payload(
                tool_call.name,
                "Tool arguments are not valid JSON: " + std::string(e.what()));
        }

        std::cout << log_prefix() << " 模型请求工具: "
                  << tool_call.name
                  << " args=" << arguments_json << std::endl;

        const auto tool_result = integration->callTool(tool_call.name, arguments_json);
        if (!tool_result.success) {
            return build_tool_error_payload(tool_call.name, tool_result.error);
        }

        if (tool_result.result.empty()) {
            react_json payload = {
                {"tool", tool_call.name},
                {"success", true},
                {"result", ""}
            };
            return payload.dump();
        }

        return tool_result.result;
    }

    /**
     * @brief RAG-MCP 检索默认返回的候选工具数量
     */
    virtual int rag_tool_top_k() const {
        return 5;
    }

    /**
     * @brief 历史消息窗口大小
     */
    virtual size_t history_message_limit() const {
        return 0;
    }

    /**
     * @brief 单次请求中允许的最大工具调用轮数
     */
    virtual int max_tool_rounds() const {
        return 5;
    }

    /**
     * @brief 统一执行 ReAct 工具调用回环
     * @param user_text 用户原始输入
     * @param context_id 会话上下文 ID
     * @return 最终响应结果
     */
    AgentResponsePayload handle_agent_request(const std::string& user_text,
                                              const std::string& context_id) override {
        ReActPreparation preparation = prepare_query(user_text, context_id);
        if (preparation.model_query.empty()) {
            preparation.model_query = user_text;
        }

        AgentResponsePayload payload;
        payload.stream_events = preparation.stream_events;
        payload.text = run_react_loop(user_text, preparation.model_query, context_id);
        return payload;
    }

private:
    /**
     * @brief 构造统一的工具错误 JSON 文本
     * @param tool_name 工具名称
     * @param error_message 错误描述
     * @return JSON 字符串
     */
    static std::string build_tool_error_payload(const std::string& tool_name,
                                                const std::string& error_message) {
        react_json payload = {
            {"tool", tool_name},
            {"success", false},
            {"error", error_message}
        };
        return payload.dump();
    }

    /**
     * @brief 判断工具是否在本轮候选集合内
     * @param tool_name 工具名称
     * @param visible_tool_names 当前轮次可见工具名集合
     * @return true 表示模型请求的工具仍在候选范围内
     */
    static bool is_tool_visible(const std::string& tool_name,
                                const std::vector<std::string>& visible_tool_names) {
        return std::find(visible_tool_names.begin(),
                         visible_tool_names.end(),
                         tool_name) != visible_tool_names.end();
    }

    /**
     * @brief 把工具名列表拼接成可读文本
     * @param tool_names 工具名列表
     * @return 逗号分隔的工具名文本
     */
    static std::string join_tool_names(const std::vector<std::string>& tool_names) {
        if (tool_names.empty()) {
            return "<none>";
        }

        std::string joined;
        for (size_t index = 0; index < tool_names.size(); ++index) {
            if (index > 0) {
                joined += ", ";
            }
            joined += tool_names[index];
        }
        return joined;
    }

    /**
     * @brief 执行多轮 ReAct 推理与工具调用
     * @param user_text 用户原始输入
     * @param prepared_query 预处理后的查询
     * @param context_id 会话上下文 ID
     * @return 最终自然语言回答
     */
    std::string run_react_loop(const std::string& /*user_text*/,
                               const std::string& prepared_query,
                               const std::string& context_id) {
        // 将当前 prepared_query 通过 RAG-MCP 检索候选工具
        const ReActToolContext tool_context =
            build_tool_context(prepared_query, context_id);
        // 上下文管理器
        std::vector<QwenMessage> messages = {
            QwenMessage{"system", build_react_system_prompt(prepared_query, context_id), "", "", {}}
        };
        const std::string tool_selection_prompt =
            build_tool_selection_prompt(tool_context, prepared_query, context_id);
        if (!tool_selection_prompt.empty()) {
            // 显式告诉模型：检索层只负责过滤候选工具，具体调用决策仍由模型完成。
            messages.push_back(QwenMessage{"system", tool_selection_prompt, "", "", {}});
        }

        auto history_messages = build_history_messages(context_id, history_message_limit());
        if (!history_messages.empty()) {
            // 当前用户消息已在运行时层入库，这里把最后一条 user 消息替换为预处理后的查询。
            if (history_messages.back().role == "user") {
                history_messages.back().content = prepared_query;
            }
            messages.insert(messages.end(), history_messages.begin(), history_messages.end());
        } else {
            messages.push_back(QwenMessage{"user", prepared_query, "", "", {}});
        }

        // 当历史最后一条不是当前用户消息时，再显式补一条，避免上下文不完整。
        if (messages.back().role != "user" || messages.back().content != prepared_query) {
            std::cout << "[Error]: 当前 messages 上下文管理器里的最后一条消息不是最新消息，当前最后一条消息=" << messages.back().role << ":" << messages.back().content << std::endl;
            messages.push_back(QwenMessage{"user", prepared_query, "", "", {}});
        }

        const auto& tools = tool_context.tool_definitions;
        if (tools.empty()) {
            return qwen_client().chat_completion(messages).content;
        }

        std::cout << log_prefix() << " 当前 RAG-MCP 候选工具: "
                  << join_tool_names(tool_context.visible_tool_names) << std::endl;

        for (int round = 0; round < max_tool_rounds(); ++round) {
            const QwenChatResult round_result = qwen_client().chat_completion(messages, tools);

            if (round_result.tool_calls.empty()) {
                if (!round_result.content.empty()) {
                    return round_result.content;
                }
                break;
            }

            // 把 assistant 的 tool_calls 显式回填，让模型知道自己上轮做过的工具决策。
            messages.push_back(QwenMessage{
                "assistant",
                round_result.content,
                "",
                "",
                round_result.tool_calls
            });

            for (const auto& tool_call : round_result.tool_calls) {
                std::string tool_output;
                if (!is_tool_visible(tool_call.name, tool_context.visible_tool_names)) {
                    // 如果模型越过候选集合请求其他工具，这里直接拦住并把错误反馈给下一轮推理。
                    tool_output = build_tool_error_payload(
                        tool_call.name,
                        "Tool is not in the RAG-filtered candidate set. Allowed tools: " +
                            join_tool_names(tool_context.visible_tool_names));
                } else {
                    tool_output = execute_tool_call(tool_call, prepared_query, context_id);
                }

                // 每个 tool 输出都按标准 role=tool 回填给下一轮模型。
                messages.push_back(QwenMessage{
                    "tool",
                    tool_output,
                    "",
                    tool_call.id,
                    {}
                });
            }
        }

        return "抱歉，我暂时无法完成这次工具调用请求。";
    }
};
