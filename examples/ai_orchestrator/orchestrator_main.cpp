/**
 * @file orchestrator_main.cpp
 * @brief AI Orchestrator - 调度 AI Agent
 *
 * 基于 a2a-cpp/examples/multi_agent_demo/dynamic_orchestrator.cpp
 * 集成到 agent-communication RPC 框架
 * 当前职责仅为意图识别、上下文维护和下游 Agent 路由
 */

#include "redis_task_store.hpp"
#include "qwen_client.hpp"
#include "http_server.hpp"
#include "registry_client.hpp"

#include <a2a/models/agent_message.hpp>
#include <a2a/models/agent_task.hpp>
#include <a2a/models/task_status.hpp>
#include <a2a/models/message_part.hpp>
#include <a2a/core/jsonrpc_request.hpp>
#include <a2a/core/jsonrpc_response.hpp>
#include <a2a/core/error_code.hpp>

// MCP 集成
#include <agent_rpc/mcp/mcp_agent_integration.h>
#include <agent_rpc/orchestrator/context_memory_manager.h>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <chrono>
#include <vector>

using namespace a2a;
using json = nlohmann::json;
using namespace agent_rpc::mcp;

// 简单的 HTTP 客户端
class SimpleHttpClient {
public:
    /**
     * @brief libcurl 写回调
     * @param contents 当前回调返回的数据块
     * @param size 单个元素大小
     * @param nmemb 元素个数
     * @param userp 累积响应内容的目标缓冲区
     * @return 实际处理的字节数
     */
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
    /**
     * @brief 发送 HTTP POST 请求
     * @param url 下游 Agent 的 A2A 服务地址
     * @param body JSON-RPC 请求体
     * @return 下游 Agent 返回的原始响应字符串
     *
     * 这里只保留编排器示例所需的最小能力，用于同步转发请求到专业 Agent。
     */
    static std::string post(const std::string& url, const std::string& body) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";
        
        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        
        curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        
        return response;
    }
};

/**
 * @brief AI Orchestrator - 智能调度器
 * 
 * 功能:
 * - 接收用户问题
 * - 使用 AI 模型分析意图
 * - 路由到合适的专业 Agent
 * - 调用下游专用 Agent 的独立能力
 * - 返回处理结果
 */
class AIOrchestrator {
public:
    AIOrchestrator(const std::string& agent_id,
                   const std::string& listen_address,
                   const std::string& registry_url,
                   const std::string& api_key,
                   const std::string& redis_host,
                   int redis_port,
                   const MCPAgentConfig& mcp_config = MCPAgentConfig())
        : agent_id_(agent_id)
        , listen_address_(listen_address)
        , task_store_(std::make_shared<RedisTaskStore>(redis_host, redis_port))
        , qwen_client_(api_key)
        , context_memory_manager_()
        , registry_client_(registry_url) {
        context_memory_manager_.set_llm_compressor(
            [this](const std::string& system_prompt,
                   const std::string& user_prompt) {
                return qwen_client_.chat(system_prompt, user_prompt);
            });

        // Orchestrator 不再直接执行工具，仅做路由编排。
        (void)mcp_config;
        
        std::cout << "[Orchestrator] 初始化完成" << std::endl;
    }
    
    ~AIOrchestrator() {
    }
    
    void start(int port) {
        // 启动 HTTP 服务器
        HttpServer server(port);
        
        // A2A 协议端点 - 普通请求
        server.register_handler("/", [this](const std::string& body) {
            return this->handle_request(body);
        });

        // A2A 协议端点 - 流式请求
        server.register_stream_handler("/", [this](const std::string& body, 
            std::function<bool(const std::string&)> write_callback) {
            this->handle_stream_request(body, write_callback);
        });
        
        // Agent Card 端点 (A2A 协议标准)
        server.register_handler("/.well-known/agent-card.json", [this](const std::string&) {
            return this->get_agent_card();
        });
        
        server.listen();
        std::cout << "[Orchestrator] 启动在端口 " << port << std::endl;
        
        // 注册到注册中心
        AgentRegistration registration;
        registration.id = agent_id_;
        registration.name = "AI Orchestrator";
        registration.address = listen_address_;
        registration.tags = {"orchestrator", "coordinator"};
        
        if (registry_client_.register_agent(registration)) {
            std::cout << "[Orchestrator] 已注册到服务中心" << std::endl;
        } else {
            std::cerr << "[Orchestrator] 注册失败" << std::endl;
        }
        
        server.serve();
    }

private:
    /**
     * @brief 去掉字符串首尾空白字符
     * @param text 待清理文本
     * @return 去除首尾空白后的结果
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

    /**
     * @brief 生成仅对 ASCII 字母做小写化的副本
     * @param text 原始文本
     * @return 小写化后的文本
     */
    static std::string to_lower_copy(const std::string& text) {
        std::string lowered = text;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
        return lowered;
    }

    /**
     * @brief 判断字符串是否以前缀开头
     * @param text 待检查文本
     * @param prefix 前缀
     * @return true 表示命中
     */
    static bool starts_with(const std::string& text, const std::string& prefix) {
        return text.size() >= prefix.size() &&
               text.compare(0, prefix.size(), prefix) == 0;
    }

    /**
     * @brief 判断字符串是否以后缀结尾
     * @param text 待检查文本
     * @param suffix 后缀
     * @return true 表示命中
     */
    static bool ends_with(const std::string& text, const std::string& suffix) {
        return text.size() >= suffix.size() &&
               text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    /**
     * @brief 替换字符串中的全部目标片段
     * @param text 待修改文本
     * @param from 待替换片段
     * @param to 替换内容
     */
    static void replace_all(std::string& text, const std::string& from, const std::string& to) {
        if (from.empty()) {
            return;
        }

        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    /**
     * @brief 检查文本是否包含任一关键词
     * @param text 待检查文本
     * @param keywords 关键词列表
     * @return true 表示至少命中一个关键词
     */
    static bool contains_any(const std::string& text,
                             const std::vector<std::string>& keywords) {
        for (const auto& keyword : keywords) {
            if (!keyword.empty() && text.find(keyword) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 统一常见数学符号写法
     * @param text 用户输入
     * @return 做过基础符号归一化的文本
     */
    static std::string normalize_math_symbols(const std::string& text) {
        std::string normalized = text;

        replace_all(normalized, "（", "(");
        replace_all(normalized, "）", ")");
        replace_all(normalized, "【", "[");
        replace_all(normalized, "】", "]");
        replace_all(normalized, "＋", "+");
        replace_all(normalized, "－", "-");
        replace_all(normalized, "—", "-");
        replace_all(normalized, "–", "-");
        replace_all(normalized, "×", "*");
        replace_all(normalized, "÷", "/");
        replace_all(normalized, "＝", "=");
        replace_all(normalized, "％", "%");
        replace_all(normalized, "，", ",");
        replace_all(normalized, "：", ":");
        replace_all(normalized, "；", ";");
        replace_all(normalized, "？", "?");
        replace_all(normalized, "。", ".");
        replace_all(normalized, "\t", " ");

        return normalized;
    }

    /**
     * @brief 判断是否存在明显的编程上下文
     * @param text 用户输入
     * @return true 表示更像代码类问题
     */
    static bool is_likely_code_query(const std::string& text) {
        const std::string normalized = normalize_math_symbols(text);
        const std::string lower_text = to_lower_copy(normalized);

        const std::vector<std::string> code_keywords = {
            "代码", "编程", "脚本", "报错", "编译", "调试", "接口", "sdk",
            "python", "java", "javascript", "typescript", "c++", "c#", "golang",
            "rust", "sql", "html", "css", "json", "yaml", "docker", "api",
            "bug", "debug", "#include", "println", "printf", "cout", "def ",
            "class ", "public ", "private ", "select "
        };

        if (contains_any(lower_text, code_keywords)) {
            return true;
        }

        if (normalized.find("```") != std::string::npos ||
            normalized.find("#include") != std::string::npos ||
            normalized.find("::") != std::string::npos) {
            return true;
        }

        return false;
    }

    /**
     * @brief 判断字符是否属于可直接求值的表达式字符
     * @param ch 单个字符
     * @return true 表示可接受
     */
    static bool is_expression_char(unsigned char ch) {
        return std::isdigit(ch) ||
               std::isalpha(ch) ||
               std::isspace(ch) ||
               ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
               ch == '^' || ch == '%' || ch == '=' || ch == '.' ||
               ch == ',' || ch == '(' || ch == ')' || ch == '[' ||
               ch == ']' || ch == '{' || ch == '}' || ch == ':' ||
               ch == '!' || ch == '<' || ch == '>' || ch == '|';
    }

    /**
     * @brief 判断文本是否更像日期而不是数学表达式
     * @param text 待判断文本
     * @return true 表示命中常见日期格式
     */
    static bool is_date_like_text(const std::string& text) {
        const std::string candidate = trim(text);
        for (char separator : {'-', '/'}) {
            if (std::count(candidate.begin(), candidate.end(), separator) != 2) {
                continue;
            }

            std::vector<std::string> parts;
            std::stringstream stream(candidate);
            std::string part;
            while (std::getline(stream, part, separator)) {
                parts.push_back(trim(part));
            }

            if (parts.size() != 3) {
                continue;
            }

            bool all_numeric = true;
            for (const auto& item : parts) {
                if (item.empty() || item.size() > 4) {
                    all_numeric = false;
                    break;
                }
                for (unsigned char ch : item) {
                    if (!std::isdigit(ch)) {
                        all_numeric = false;
                        break;
                    }
                }
                if (!all_numeric) {
                    break;
                }
            }

            if (!all_numeric) {
                continue;
            }

            // 年份通常会是 4 位，这里优先排除 yyyy-mm-dd 和 mm-dd-yyyy 这类日期写法。
            if (parts[0].size() == 4 || parts[2].size() == 4) {
                return true;
            }
        }

        return false;
    }

    /**
     * @brief 尝试从用户输入里提取可直接交给计算器的表达式
     * @param text 用户输入
     * @return 提取成功时返回表达式，否则返回空字符串
     */
    static std::string extract_direct_math_expression(const std::string& text) {
        std::string candidate = trim(normalize_math_symbols(text));
        const std::string original_candidate = candidate;
        if (candidate.empty()) {
            return "";
        }

        const std::vector<std::string> prefixes = {
            "请帮我计算", "帮我计算", "请帮我算一下", "帮我算一下",
            "请帮我算下", "帮我算下", "请计算", "计算一下", "计算下",
            "计算", "算一下", "算下", "算", "求值", "求一下", "求",
            "请问", "麻烦你", "麻烦", "帮我", "可以帮我"
        };
        const std::vector<std::string> suffixes = {
            "等于多少", "是多少", "结果是多少", "结果是几", "等于几",
            "结果", "对吗", "可以吗", "?", ".", "!"
        };

        bool changed = false;
        bool stripped_prefix = true;
        while (stripped_prefix) {
            stripped_prefix = false;
            for (const auto& prefix : prefixes) {
                if (starts_with(candidate, prefix)) {
                    candidate = trim(candidate.substr(prefix.size()));
                    stripped_prefix = true;
                    changed = true;
                    break;
                }
            }
        }

        replace_all(candidate, "加上", "+");
        replace_all(candidate, "加", "+");
        replace_all(candidate, "减去", "-");
        replace_all(candidate, "减", "-");
        replace_all(candidate, "乘以", "*");
        replace_all(candidate, "乘上", "*");
        replace_all(candidate, "乘", "*");
        replace_all(candidate, "除以", "/");
        replace_all(candidate, "除", "/");

        bool stripped_suffix = true;
        while (stripped_suffix) {
            stripped_suffix = false;
            candidate = trim(candidate);
            for (const auto& suffix : suffixes) {
                if (ends_with(candidate, suffix)) {
                    candidate = trim(candidate.substr(0, candidate.size() - suffix.size()));
                    stripped_suffix = true;
                    changed = true;
                    break;
                }
            }
        }

        if (starts_with(candidate, ":")) {
            candidate = trim(candidate.substr(1));
            changed = true;
        }

        if (candidate.empty() || candidate.find("```") != std::string::npos) {
            return "";
        }

        if (is_date_like_text(candidate)) {
            return "";
        }

        size_t digit_count = 0;
        size_t alpha_count = 0;
        size_t operator_count = 0;
        for (unsigned char ch : candidate) {
            if (ch >= 128) {
                return "";
            }
            if (!is_expression_char(ch)) {
                return "";
            }
            if (std::isdigit(ch)) {
                digit_count++;
            }
            if (std::isalpha(ch)) {
                alpha_count++;
            }
            if (ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
                ch == '^' || ch == '%' || ch == '=' || ch == '(' ||
                ch == ')' || ch == '[' || ch == ']') {
                operator_count++;
            }
        }

        const std::string lower_candidate = to_lower_copy(candidate);
        const bool contains_math_function = contains_any(lower_candidate, {
            "sqrt", "sin", "cos", "tan", "log", "ln", "pi"
        });
        const bool looks_like_expression = operator_count > 0 || contains_math_function;
        const bool math_request_wrapped = changed || candidate != original_candidate;

        if (digit_count == 0 && alpha_count == 0) {
            return "";
        }
        if (!looks_like_expression && !math_request_wrapped) {
            return "";
        }

        return trim(candidate);
    }

    /**
     * @brief 基于规则判断是否明显属于数学问题
     * @param text 用户输入
     * @return true 表示高概率应路由到 Math Agent
     */
    static bool is_likely_math_query(const std::string& text) {
        if (is_likely_code_query(text)) {
            return false;
        }

        if (!extract_direct_math_expression(text).empty()) {
            return true;
        }

        const std::string normalized = normalize_math_symbols(text);
        const std::string lower_text = to_lower_copy(normalized);
        const std::vector<std::string> math_keywords = {
            "数学", "计算", "算一下", "算下", "求值", "求解", "方程", "代数",
            "几何", "概率", "统计", "导数", "积分", "极限", "矩阵", "函数值",
            "根号", "平方根", "均值", "方差", "math", "calculate", "calc",
            "compute", "equation", "solve", "derivative", "integral", "matrix",
            "probability", "mean", "variance"
        };
        if (contains_any(lower_text, math_keywords)) {
            return true;
        }

        size_t digit_count = 0;
        size_t operator_count = 0;
        for (unsigned char ch : normalized) {
            if (std::isdigit(ch)) {
                digit_count++;
            }
            if (ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
                ch == '^' || ch == '%' || ch == '=' || ch == '(' ||
                ch == ')' || ch == '[' || ch == ']') {
                operator_count++;
            }
        }

        return digit_count > 0 &&
               operator_count > 0 &&
               !is_date_like_text(trim(normalized));
    }

    /**
     * @brief 提取模型输出中的首条有效结果
     * @param text 模型原始回复
     * @return 单行、去前缀后的文本
     */
    static std::string sanitize_single_line_output(const std::string& text) {
        std::string sanitized = trim(text);
        if (sanitized.empty()) {
            return "";
        }

        replace_all(sanitized, "```", "");

        std::istringstream stream(sanitized);
        std::string line;
        while (std::getline(stream, line)) {
            line = trim(line);
            if (!line.empty()) {
                sanitized = line;
                break;
            }
        }

        const std::vector<std::string> prefixes = {
            "改写后:", "改写后：", "输出:", "输出：", "结果:", "结果：",
            "规范化后:", "规范化后：", "math:", "Math:"
        };
        for (const auto& prefix : prefixes) {
            if (starts_with(sanitized, prefix)) {
                sanitized = trim(sanitized.substr(prefix.size()));
                break;
            }
        }

        if (!sanitized.empty() &&
            (sanitized.front() == '"' || sanitized.front() == '\'' || sanitized.front() == '`')) {
            sanitized.erase(sanitized.begin());
        }
        if (!sanitized.empty() &&
            (sanitized.back() == '"' || sanitized.back() == '\'' || sanitized.back() == '`')) {
            sanitized.pop_back();
        }

        return trim(sanitized);
    }

    /**
     * @brief 将 A2A 消息转换为记忆管理器消息
     * @param message A2A 消息
     * @return 记忆管理器消息
     */
    static agent_rpc::orchestrator::ContextMemoryMessage to_memory_message(
        const AgentMessage& message) {
        std::string role = "assistant";
        if (message.role() == MessageRole::User) {
            role = "user";
        } else if (message.role() == MessageRole::System) {
            role = "system";
        }

        std::string text;
        if (!message.parts().empty()) {
            auto text_part = dynamic_cast<TextPart*>(message.parts()[0].get());
            if (text_part) {
                text = text_part->text();
            }
        }

        return {role, text};
    }

    /**
     * @brief 读取 Redis 历史并转换为记忆管理器格式
     * @param context_id 会话上下文 ID
     * @param max_length 最大读取数量，0 表示全部
     * @return 转换后的消息列表
     */
    std::vector<agent_rpc::orchestrator::ContextMemoryMessage>
    load_context_memory_history(const std::string& context_id, int max_length) {
        auto history = task_store_->get_history(context_id, max_length);
        std::vector<agent_rpc::orchestrator::ContextMemoryMessage> messages;
        messages.reserve(history.size());

        for (const auto& message : history) {
            messages.push_back(to_memory_message(message));
        }

        return messages;
    }

    /**
     * @brief 构造分层后的上下文消息
     * @param context_id 会话上下文 ID
     * @param limit 最近原始历史读取数量，0 表示全部
     * @return 长期记忆、短期摘要和工作记忆组成的消息列表
     */
    std::vector<agent_rpc::orchestrator::ContextMemoryMessage>
    build_managed_context_messages(const std::string& context_id, size_t limit) {
        auto raw_history =
            load_context_memory_history(context_id, static_cast<int>(limit));
        return context_memory_manager_.build_context_messages(context_id, raw_history);
    }

    /**
     * @brief 读取最近几条历史消息并拼成可读文本
     * @param context_id 会话上下文
     * @param limit 最多读取的消息数
     * @return 历史文本
     */
    std::string build_history_text(const std::string& context_id, size_t limit) {
        auto history = build_managed_context_messages(context_id, limit);
        std::string history_text;
        for (const auto& msg : history) {
            history_text += msg.role + ": " + msg.content + "\n";
        }
        return history_text;
    }

    /**
     * @brief 处理普通的 message/send 请求
     * @param body JSON-RPC 请求体
     * @return JSON-RPC 响应体
     *
     * 这个入口负责完整的同步链路：解析消息、保存上下文、识别意图、
     * 转发到专业 Agent 或通用模型，然后把最终回复回写到历史记录。
     */
    std::string handle_request(const std::string& body) {
        try {
            auto request_json = json::parse(body);
            auto request = JsonRpcRequest::from_json(body);
            
            if (request.method() == "message/send") {
                auto params_json = request_json["params"];
                auto message = AgentMessage::from_json(params_json["message"].dump());
                
                // 获取文本内容
                std::string user_text;
                if (!message.parts().empty()) {
                    auto text_part = dynamic_cast<TextPart*>(message.parts()[0].get());
                    if (text_part) {
                        user_text = text_part->text();
                    }
                }
                
                // context_id 同时承担会话标识和任务存储主键，确保多轮对话能复用历史。
                std::string context_id = message.context_id().value_or("default");
                
                std::cout << "[Orchestrator] 收到消息: " << user_text << std::endl;
                
                // 保存用户消息并刷新分层记忆。
                save_message(context_id, message);
                
                // 识别意图
                std::string intent = analyze_intent(user_text);
                std::cout << "[Orchestrator] 识别意图: " << intent << std::endl;
                
                std::string response_text;
                
                // 编排器本身不直接处理专业问题，而是先做路由决策，再委托给下游 Agent。
                if (intent == "math") {
                    // 数学请求改写和工具调用都由 Math Agent 自主完成，编排器只负责路由。
                    response_text = call_math_agent(user_text, context_id);
                } else if (intent == "code") {
                    // 动态查找 Code Agent
                    response_text = call_code_agent(user_text, context_id);
                } else {
                    // 通用对话由专用 General Agent 处理。
                    response_text = call_general_agent(user_text, context_id);
                }
                
                // 无论回复来自哪个下游 Agent，都统一写回 orchestrator 的会话历史。
                auto response_msg = AgentMessage::create()
                    .with_role(MessageRole::Agent)
                    .with_context_id(context_id);
                response_msg.add_text_part(response_text);
                save_message(context_id, response_msg);
                
                // 返回响应
                auto response = JsonRpcResponse::create_success(request.id(), response_msg.to_json());
                return response.to_json();
            }
            
            return JsonRpcResponse::create_error(request.id(), ErrorCode::MethodNotFound, "Method not found").to_json();
            
        } catch (const std::exception& e) {
            std::cerr << "[Orchestrator] 错误: " << e.what() << std::endl;
            return JsonRpcResponse::create_error("1", ErrorCode::InternalError, e.what()).to_json();
        }
    }
    
    /**
     * @brief 处理流式请求 (message/stream)
     * @param body JSON-RPC 请求体
     * @param write_callback 向客户端持续写入事件的回调
     *
     * 事件顺序固定为：
     * 1. stream_start
     * 2. intent
     * 3. 多个 chunk
     * 4. stream_end
     * 
     * 这样客户端既能尽早拿到编排状态，也能逐步消费最终回答内容。
     */
    void handle_stream_request(const std::string& body, 
                               std::function<bool(const std::string&)> write_callback) {
        try {
            auto request_json = json::parse(body);
            auto request = JsonRpcRequest::from_json(body);
            
            if (request.method() != "message/stream") {
                // 非流式方法，返回错误
                json error_response = {
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
            
            auto params_json = request_json["params"];
            auto message = AgentMessage::from_json(params_json["message"].dump());
            
            // 获取文本内容
            std::string user_text;
            if (!message.parts().empty()) {
                auto text_part = dynamic_cast<TextPart*>(message.parts()[0].get());
                if (text_part) {
                    user_text = text_part->text();
                }
            }
            
            // 流式请求与普通请求复用同一份上下文，避免历史记录被拆成两个会话。
            std::string context_id = message.context_id().value_or("default");
            
            std::cout << "[Orchestrator] 收到流式消息: " << user_text << std::endl;
            
            // 保存用户消息并刷新分层记忆。
            save_message(context_id, message);
            
            // 先发送开始事件，让客户端知道当前请求已进入流式处理阶段。
            json start_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {
                    {"type", "stream_start"},
                    {"contextId", context_id}
                }}
            };
            write_callback(start_event.dump());
            
            // 识别意图
            std::string intent = analyze_intent(user_text);
            std::cout << "[Orchestrator] 识别意图: " << intent << std::endl;
            
            // 在正文输出前先暴露意图识别结果，便于前端展示“正在路由到哪个 Agent”。
            json intent_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {
                    {"type", "intent"},
                    {"intent", intent}
                }}
            };
            write_callback(intent_event.dump());
            
            // 处理查询并流式返回
            std::string response_text;
            if (intent == "math") {
                // 数学请求改写和工具调用都由 Math Agent 自主完成，编排器只负责路由。
                response_text = call_math_agent(user_text, context_id);
            } else if (intent == "code") {
                response_text = call_code_agent(user_text, context_id);
            } else {
                response_text = call_general_agent(user_text, context_id);
            }
            
            // UTF-8 安全分块，避免中文等多字节字符被切成非法片段。
            auto utf8_safe_chunk = [](const std::string& text, size_t start, size_t max_len) -> std::string {
                if (start >= text.length()) return "";
                
                size_t end = std::min(start + max_len, text.length());
                
                // 确保不在 UTF-8 多字节字符中间切断
                while (end > start && end < text.length()) {
                    unsigned char c = static_cast<unsigned char>(text[end]);
                    // 如果是 UTF-8 后续字节 (10xxxxxx)，向前移动
                    if ((c & 0xC0) == 0x80) {
                        end--;
                    } else {
                        break;
                    }
                }
                
                return text.substr(start, end - start);
            };
            
            // 分块大小保持较小，便于前端更快看到增量输出，同时又不会产生过多事件。
            const size_t chunk_size = 50;
            size_t pos = 0;
            while (pos < response_text.length()) {
                std::string chunk = utf8_safe_chunk(response_text, pos, chunk_size);
                if (chunk.empty()) break;
                
                pos += chunk.length();
                
                json chunk_event = {
                    {"jsonrpc", "2.0"},
                    {"id", request.id()},
                    {"result", {
                        {"type", "chunk"},
                        {"content", chunk}
                    }}
                };
                
                // 回调返回 false 通常意味着客户端已断开，此时应尽快停止后续发送。
                if (!write_callback(chunk_event.dump())) {
                    std::cerr << "[Orchestrator] 流式写入失败" << std::endl;
                    return;
                }
                
                // 小延迟模拟流式效果
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            // 只在完整结果生成后入库，避免历史记录中混入不完整的 chunk 片段。
            auto response_msg = AgentMessage::create()
                .with_role(MessageRole::Agent)
                .with_context_id(context_id);
            response_msg.add_text_part(response_text);
            save_message(context_id, response_msg);
            
            // 发送完成事件
            json complete_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {
                    {"type", "stream_end"},
                    {"message", response_msg.to_json()}
                }}
            };
            write_callback(complete_event.dump());
            
            std::cout << "[Orchestrator] 流式响应完成" << std::endl;
            
        } catch (const std::exception& e) {
            std::cerr << "[Orchestrator] 流式处理错误: " << e.what() << std::endl;
            json error_event = {
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

    /**
     * @brief 使用大模型做轻量意图分类
     * @param text 用户原始输入
     * @return math、code 或 general
     *
     * 这里对模型输出做包含判断，而不是要求完全等于类别名，
     * 是为了兼容模型偶尔返回解释文本的情况。
     */
    std::string analyze_intent(const std::string& text) {
        if (is_likely_code_query(text)) {
            return "code";
        }

        if (is_likely_math_query(text)) {
            return "math";
        }

        const std::string system_prompt =
            "你是 AI Orchestrator 的意图分类器。"
            "你只能从 math、code、general 三个标签中选择一个，并且只输出标签本身。\n"
            "分类规则：\n"
            "1. math：数学计算、表达式求值、方程、几何、概率统计、微积分，以及像“1+1”、“计算1+1”、“(3+5)*2”、“解 x^2-4=0”这类输入。\n"
            "2. code：编程、代码解释、报错排查、脚本、SQL、接口、开发工具，以及在明确编程语境中讨论表达式。\n"
            "3. general：其他普通对话。\n"
            "如果同时出现数学表达式和明显编程语境，例如“C++里 1+1 的结果”或“写代码计算 1+1”，优先返回 code。\n"
            "输出要求：只能输出一个小写标签，不要解释，不要标点。";

        try {
            std::string result = to_lower_copy(
                sanitize_single_line_output(qwen_client_.chat(system_prompt, text)));

            if (result.find("math") != std::string::npos) {
                return "math";
            }
            if (result.find("code") != std::string::npos) {
                return "code";
            }
            if (result.find("general") != std::string::npos) {
                return "general";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Orchestrator] 意图识别模型调用失败: " << e.what() << std::endl;
        }

        return "general";
    }

    std::string call_math_agent(const std::string& query, const std::string& context_id) {
        return call_agent_by_tag("math", query, context_id);
    }
    
    std::string call_code_agent(const std::string& query, const std::string& context_id) {
        return call_agent_by_tag("code", query, context_id);
    }

    std::string call_general_agent(const std::string& query,
                                  const std::string& context_id) {
        return call_agent_by_tag("general", query, context_id);
    }
    
    /**
     * @brief 按标签查找并调用下游 Agent
     * @param tag 目标 Agent 标签，例如 math 或 code
     * @param query 用户问题
     * @param context_id 当前会话上下文
     * @return 下游 Agent 的文本响应，失败时返回降级提示
     *
     * 通过注册中心按标签发现服务，而不是硬编码地址，
     * 这样部署时可以灵活替换或扩容专业 Agent。
     */
    std::string call_agent_by_tag(const std::string& tag, 
                                  const std::string& query, 
                                  const std::string& context_id) {
        try {
            // 从注册中心查找 Agent
            std::string agent_url = registry_client_.select_agent_by_tag(tag);
            
            std::cout << "[Orchestrator] 调用 " << tag << " Agent: " << agent_url << std::endl;
            
            // 透传当前 query 和 context_id；专业 Agent 会自行决定是否改写请求或调用工具。
            json request = {
                {"jsonrpc", "2.0"},
                {"id", "1"},
                {"method", "message/send"},
                {"params", {
                    {"message", {
                        {"role", "user"},
                        {"contextId", context_id},
                        {"parts", {{{"kind", "text"}, {"text", query}}}}
                    }},
                    {"historyLength", 5}
                }}
            };
            
            // 编排器与专业 Agent 之间仍然走统一的 A2A JSON-RPC 协议。
            std::string response_body = SimpleHttpClient::post(agent_url, request.dump());
            auto response_json = json::parse(response_body);
            
            if (response_json.contains("result") &&
                response_json["result"].contains("parts") &&
                !response_json["result"]["parts"].empty()) {
                return response_json["result"]["parts"][0]["text"].get<std::string>();
            }
            
            // 返回结构不符合预期时，不抛出内部实现细节，直接给出可理解的错误信息。
            return "无法解析响应";
            
        } catch (const std::exception& e) {
            std::cerr << "[Orchestrator] 调用 " << tag << " Agent 失败: " << e.what() << std::endl;
            // 下游服务不可用时，直接返回可读降级提示。
            return "抱歉，" + tag + " 服务暂时不可用，请稍后再试。";
        }
    }
    
    /**
     * @brief 保存消息到任务存储
     * @param context_id 会话上下文 ID
     * @param message 待保存的消息
     *
     * 首次看到某个 context_id 时，会先补建运行中的任务，再追加历史消息，
     * 这样普通请求和流式请求都可以通过同一套存储接口读取上下文。
     */
    void save_message(const std::string& context_id, const AgentMessage& message) {
        if (!task_store_->task_exists(context_id)) {
            auto task = AgentTask::create()
                .with_id(context_id)
                .with_context_id(context_id)
                .with_status(TaskState::Running);
            task_store_->set_task(task);
        }
        task_store_->add_history_message(context_id, message);
        refresh_context_memory(context_id);
    }

    /**
     * @brief 刷新指定上下文的分层记忆
     * @param context_id 会话上下文 ID
     */
    void refresh_context_memory(const std::string& context_id) {
        auto history = load_context_memory_history(context_id, 0);
        context_memory_manager_.observe_conversation(context_id, history);
    }
    
    std::string get_agent_card() {
        json card = {
            {"name", "AI Orchestrator Agent"},
            {"description", "智能协调器，负责意图识别和任务分发"},
            {"version", "1.0.0"},
            {"capabilities", {
                {"streaming", true},
                {"push_notifications", false},
                {"task_management", true}
            }},
            {"skills", json::array({
                {
                    {"name", "意图识别"},
                    {"description", "识别用户意图并路由到相应的专业 Agent"},
                    {"input_modes", json::array({"text"})},
                    {"output_modes", json::array({"text"})}
                },
                {
                    {"name", "任务协调"},
                    {"description", "协调多个 Agent 完成复杂任务"},
                    {"input_modes", json::array({"text"})},
                    {"output_modes", json::array({"text"})}
                }
            })},
            {"provider", {
                {"name", "Agent Communication RPC"},
                {"organization", "A2A Integration"}
            }}
        };
        return card.dump();
    }
    
    std::string agent_id_;
    std::string listen_address_;
    std::shared_ptr<RedisTaskStore> task_store_;
    QwenClient qwen_client_;
    agent_rpc::orchestrator::ContextMemoryManager context_memory_manager_;
    RegistryClient registry_client_;
};

void print_usage(const char* program) {
    std::cerr << "用法: " << program << " <agent_id> <port> <registry_url> <api_key> [options]" << std::endl;
    std::cerr << "选项:" << std::endl;
    std::cerr << "  --redis-host <host>     Redis 主机 (默认: 127.0.0.1)" << std::endl;
    std::cerr << "  --redis-port <port>     Redis 端口 (默认: 6379)" << std::endl;
    std::cerr << "  --mcp-server <path>     兼容保留参数，当前 Orchestrator 不直接使用" << std::endl;
    std::cerr << "  --mcp-args <args>       兼容保留参数，当前 Orchestrator 不直接使用" << std::endl;
    std::cerr << "  --enable-mcp            兼容保留参数，当前 Orchestrator 不直接使用" << std::endl;
    std::cerr << std::endl;
    std::cerr << "示例: " << program << " orch-1 5000 http://localhost:8500 sk-xxx --redis-host 127.0.0.1 --redis-port 6379" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        print_usage(argv[0]);
        return 1;
    }
    
    std::string agent_id = argv[1];
    int port = std::stoi(argv[2]);
    std::string registry_url = argv[3];
    std::string api_key = argv[4];
    
    // 默认值
    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;
    
    // 优先解析命令行中的 MCP 配置，便于显式覆盖默认环境变量。
    MCPAgentConfig mcp_config = parseMCPConfigFromArgs(argc, argv);
    
    // 命令行未启用 MCP 时，再回退到环境变量配置，兼容容器化部署场景。
    if (!mcp_config.enable_mcp) {
        MCPAgentConfig env_config = parseMCPConfigFromEnv();
        if (env_config.enable_mcp) {
            mcp_config = env_config;
        }
    }
    
    // 其余参数由 orchestrator 自己消费，例如任务存储依赖的 Redis 连接信息。
    for (int i = 5; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--redis-host" && i + 1 < argc) {
            redis_host = argv[++i];
        } else if (arg == "--redis-port" && i + 1 < argc) {
            redis_port = std::stoi(argv[++i]);
        }
    }
    
    std::string listen_address = "http://localhost:" + std::to_string(port);
    
    try {
        AIOrchestrator orchestrator(agent_id, listen_address, registry_url, api_key, 
                                   redis_host, redis_port, mcp_config);
        orchestrator.start(port);
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
