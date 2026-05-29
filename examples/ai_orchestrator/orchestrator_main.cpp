/**
 * @file orchestrator_main.cpp
 * @brief AI Orchestrator - 调度 AI Agent
 * 
 * 基于 a2a-cpp/examples/multi_agent_demo/dynamic_orchestrator.cpp
 * 集成到 agent-communication RPC 框架
 * 集成 MCP 工具支持
 * 
 * Requirements: 12.2, 12.3, 12.4
 * Task 19.4: 集成 MCP 到 Orchestrator Agent
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

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

using namespace a2a;
using json = nlohmann::json;
using namespace agent_rpc::mcp;

// 简单的 HTTP 客户端
class SimpleHttpClient {
public:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
    
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
 * - 使用 MCP 工具增强能力
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
        , registry_client_(registry_url)
        , mcp_integration_(std::make_unique<MCPAgentIntegration>()) {
        
        // 初始化 MCP 集成
        if (!mcp_integration_->initialize(mcp_config)) {
            std::cerr << "[Orchestrator] MCP 初始化失败，将在无 MCP 模式下运行" << std::endl;
        } else if (mcp_integration_->isAvailable()) {
            auto tools = mcp_integration_->getToolNames();
            std::cout << "[Orchestrator] MCP 已启用，可用工具: ";
            for (const auto& tool : tools) {
                std::cout << tool << " ";
            }
            std::cout << std::endl;
        }
        
        std::cout << "[Orchestrator] 初始化完成" << std::endl;
    }
    
    ~AIOrchestrator() {
        if (mcp_integration_) {
            mcp_integration_->shutdown();
        }
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
        
        std::cout << "[Orchestrator] 启动在端口 " << port << std::endl;
        
        // 在后台线程中启动服务器
        std::thread server_thread([&server]() {
            server.start();
        });
        
        // 等待服务器启动
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
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
        
        server_thread.join();
    }

private:
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
                
                std::string context_id = message.context_id().value_or("default");
                
                std::cout << "[Orchestrator] 收到消息: " << user_text << std::endl;
                
                // 保存用户消息
                save_message(context_id, message);
                
                // 识别意图
                std::string intent = analyze_intent(user_text);
                std::cout << "[Orchestrator] 识别意图: " << intent << std::endl;
                
                std::string response_text;
                
                if (intent == "math") {
                    // 动态查找 Math Agent
                    response_text = call_math_agent(user_text, context_id);
                } else if (intent == "code") {
                    // 动态查找 Code Agent
                    response_text = call_code_agent(user_text, context_id);
                } else {
                    // 通用对话
                    response_text = handle_general_query(user_text, context_id);
                }
                
                // 保存 Agent 响应
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
     * 
     * 支持 A2A 协议的流式消息传输
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
            
            std::string context_id = message.context_id().value_or("default");
            
            std::cout << "[Orchestrator] 收到流式消息: " << user_text << std::endl;
            
            // 保存用户消息
            save_message(context_id, message);
            
            // 发送开始事件
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
            
            // 发送意图识别事件
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
                response_text = call_math_agent(user_text, context_id);
            } else if (intent == "code") {
                response_text = call_code_agent(user_text, context_id);
            } else {
                response_text = handle_general_query(user_text, context_id);
            }
            
            // UTF-8 安全的分块函数
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
            
            // 流式输出：UTF-8 安全分块
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
                
                if (!write_callback(chunk_event.dump())) {
                    std::cerr << "[Orchestrator] 流式写入失败" << std::endl;
                    return;
                }
                
                // 小延迟模拟流式效果
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            // 保存 Agent 响应
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

    std::string analyze_intent(const std::string& text) {
        std::string prompt = "判断以下用户输入属于哪个类别，只回答类别名称：\n"
                            "- math: 数学计算、方程求解\n"
                            "- code: 编程、代码相关\n"
                            "- general: 其他对话\n\n"
                            "用户输入: " + text;
        
        std::string result = qwen_client_.chat("", prompt);
        
        if (result.find("math") != std::string::npos) {
            return "math";
        }
        if (result.find("code") != std::string::npos) {
            return "code";
        }
        return "general";
    }
    
    std::string call_math_agent(const std::string& query, const std::string& context_id) {
        return call_agent_by_tag("math", query, context_id);
    }
    
    std::string call_code_agent(const std::string& query, const std::string& context_id) {
        return call_agent_by_tag("code", query, context_id);
    }
    
    std::string call_agent_by_tag(const std::string& tag, 
                                   const std::string& query, 
                                   const std::string& context_id) {
        try {
            // 从注册中心查找 Agent
            std::string agent_url = registry_client_.select_agent_by_tag(tag);
            
            std::cout << "[Orchestrator] 调用 " << tag << " Agent: " << agent_url << std::endl;
            
            // 构造请求
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
            
            // 发送请求
            std::string response_body = SimpleHttpClient::post(agent_url, request.dump());
            auto response_json = json::parse(response_body);
            
            if (response_json.contains("result") &&
                response_json["result"].contains("parts") &&
                !response_json["result"]["parts"].empty()) {
                return response_json["result"]["parts"][0]["text"].get<std::string>();
            }
            
            return "无法解析响应";
            
        } catch (const std::exception& e) {
            std::cerr << "[Orchestrator] 调用 " << tag << " Agent 失败: " << e.what() << std::endl;
            return "抱歉，" + tag + " 服务暂时不可用，使用通用模型回答";
        }
    }
    
    std::string handle_general_query(const std::string& query, const std::string& context_id) {
        auto history = task_store_->get_history(context_id, 5);
        std::string history_text;
        for (const auto& msg : history) {
            std::string role_str = to_string(msg.role());
            std::string text;
            if (!msg.parts().empty()) {
                auto text_part = dynamic_cast<TextPart*>(msg.parts()[0].get());
                if (text_part) {
                    text = text_part->text();
                }
            }
            history_text += role_str + ": " + text + "\n";
        }
        
        // 尝试使用 MCP 工具增强回答
        std::string tool_context = tryMCPTools(query);
        if (!tool_context.empty()) {
            history_text += "\n工具辅助信息:\n" + tool_context;
        }
        
        return qwen_client_.chat(history_text, query);
    }
    
    /**
     * @brief 尝试使用 MCP 工具获取辅助信息
     */
    std::string tryMCPTools(const std::string& query) {
        if (!mcp_integration_ || !mcp_integration_->isAvailable()) {
            return "";
        }
        
        std::string result;
        auto tools = mcp_integration_->getAvailableTools();
        
        // 根据查询内容选择合适的工具
        for (const auto& tool : tools) {
            // 简单的关键词匹配来决定是否使用工具
            bool should_use = false;
            
            if (tool.name.find("search") != std::string::npos ||
                tool.name.find("query") != std::string::npos) {
                // 搜索类工具
                should_use = true;
            } else if (tool.name.find("time") != std::string::npos ||
                       tool.name.find("date") != std::string::npos) {
                // 时间类工具
                if (query.find("时间") != std::string::npos ||
                    query.find("日期") != std::string::npos ||
                    query.find("time") != std::string::npos) {
                    should_use = true;
                }
            }
            
            if (should_use) {
                json args;
                args["query"] = query;
                
                std::cout << "[Orchestrator] 调用 MCP 工具: " << tool.name << std::endl;
                
                auto tool_result = mcp_integration_->callTool(tool.name, args.dump());
                if (tool_result.success) {
                    result += "[" + tool.name + "]: " + tool_result.result + "\n";
                }
            }
        }
        
        return result;
    }
    
    void save_message(const std::string& context_id, const AgentMessage& message) {
        if (!task_store_->task_exists(context_id)) {
            auto task = AgentTask::create()
                .with_id(context_id)
                .with_context_id(context_id)
                .with_status(TaskState::Running);
            task_store_->set_task(task);
        }
        task_store_->add_history_message(context_id, message);
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
    RegistryClient registry_client_;
    std::unique_ptr<MCPAgentIntegration> mcp_integration_;
};

void print_usage(const char* program) {
    std::cerr << "用法: " << program << " <agent_id> <port> <registry_url> <api_key> [options]" << std::endl;
    std::cerr << "选项:" << std::endl;
    std::cerr << "  --redis-host <host>     Redis 主机 (默认: 127.0.0.1)" << std::endl;
    std::cerr << "  --redis-port <port>     Redis 端口 (默认: 6379)" << std::endl;
    std::cerr << "  --mcp-server <path>     MCP Server 可执行文件路径" << std::endl;
    std::cerr << "  --mcp-args <args>       MCP Server 启动参数 (逗号分隔)" << std::endl;
    std::cerr << "  --enable-mcp            启用 MCP" << std::endl;
    std::cerr << std::endl;
    std::cerr << "示例: " << program << " orch-1 5000 http://localhost:8500 sk-xxx --enable-mcp --mcp-server /path/to/mcp_server" << std::endl;
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
    
    // 解析 MCP 配置
    MCPAgentConfig mcp_config = parseMCPConfigFromArgs(argc, argv);
    
    // 也尝试从环境变量获取 MCP 配置
    if (!mcp_config.enable_mcp) {
        MCPAgentConfig env_config = parseMCPConfigFromEnv();
        if (env_config.enable_mcp) {
            mcp_config = env_config;
        }
    }
    
    // 解析其他命令行参数
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
