/**
 * @file math_agent_main.cpp
 * @brief Math Agent - 数学计算专业 Agent
 * 
 * 基于 a2a-cpp/examples/multi_agent_demo/dynamic_math_agent.cpp
 * 集成 MCP 工具支持
 * 
 * Requirements: 12.2, 12.3, 12.4
 * Task 19.3: 集成 MCP 到 Math Agent
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

#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>
#include <chrono>

using namespace a2a;
using json = nlohmann::json;
using namespace agent_rpc::mcp;

/**
 * @brief Math Agent - 数学计算专家
 * 
 * 支持 MCP 工具集成，可使用计算器等工具增强数学计算能力
 */
class MathAgent {
public:
    /**
     * @brief 构造 Math Agent 并初始化外部依赖
     * @param agent_id Agent 唯一标识
     * @param listen_address 当前 Agent 对外监听地址
     * @param registry_url 服务注册中心地址
     * @param api_key Qwen 模型访问密钥
     * @param redis_host Redis 主机地址
     * @param redis_port Redis 端口
     * @param mcp_config MCP 集成配置
     */
    MathAgent(const std::string& agent_id,
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
        
        // 初始化 MCP 集成。失败时不阻塞主流程，允许 Agent 退化为纯 LLM 模式。
        if (!mcp_integration_->initialize(mcp_config)) {
            std::cerr << "[MathAgent] MCP 初始化失败，将在无 MCP 模式下运行" << std::endl;
        } else if (mcp_integration_->isAvailable()) {
            // 启动时打印工具清单，便于排查“工具已配置但未生效”的问题。
            auto tools = mcp_integration_->getToolNames();
            std::ostringstream tool_list;
            tool_list << "[MathAgent] MCP 已启用，可用工具:";
            for (const auto& tool : tools) {
                tool_list << " " << tool;
            }
            std::cout << tool_list.str() << std::endl;
        }
        
        std::cout << "[MathAgent] 初始化完成" << std::endl;
    }
    
    ~MathAgent() {
        if (mcp_integration_) {
            mcp_integration_->shutdown();
        }
    }
    
    /**
     * @brief 启动 HTTP 服务并注册到服务中心
     * @param port HTTP 服务监听端口
     */
    void start(int port) {
        HttpServer server(port);
        
        // 普通请求走同步发送接口，适合一次性返回完整答案的场景。
        server.register_handler("/", [this](const std::string& body) {
            return this->handle_request(body);
        });
        
        // 流式请求复用同一路径，通过 method 区分是否按事件流返回。
        server.register_stream_handler("/", [this](const std::string& body,
            std::function<bool(const std::string&)> write_callback) {
            this->handle_stream_request(body, write_callback);
        });

        server.register_handler("/.well-known/agent-card.json", [this](const std::string&) {
            return this->get_agent_card();
        });
        
        server.listen();
        std::cout << "[MathAgent] 启动在端口 " << port << std::endl;
        
        // 注册到注册中心
        AgentRegistration registration;
        registration.id = agent_id_;
        registration.name = "Math Agent";
        registration.address = listen_address_;
        registration.tags = {"math", "calculator"};
        
        if (registry_client_.register_agent(registration)) {
            std::cout << "[MathAgent] 已注册到服务中心" << std::endl;
        } else {
            std::cerr << "[MathAgent] 注册失败" << std::endl;
        }
        
        server.serve();
    }

private:
    /**
     * @brief 处理同步消息请求
     * @param body JSON-RPC 请求体
     * @return JSON-RPC 响应体
     */
    std::string handle_request(const std::string& body) {
        try {
            // 同时保留原始 JSON 和强类型请求对象：
            // 前者便于读取 params，后者便于统一访问 method/id。
            auto request_json = json::parse(body);
            auto request = JsonRpcRequest::from_json(body);
            
            if (request.method() == "message/send") {
                auto params_json = request_json["params"];
                auto message = AgentMessage::from_json(params_json["message"].dump());
                
                std::string user_text;
                if (!message.parts().empty()) {
                    // 当前示例只消费首个文本分片，适配最常见的 text-only 输入。
                    auto text_part = dynamic_cast<TextPart*>(message.parts()[0].get());
                    if (text_part) {
                        user_text = text_part->text();
                    }
                }
                
                // 没有显式上下文时回退到 default，保证最小可运行能力。
                std::string context_id = message.context_id().value_or("default");
                
                std::cout << "[MathAgent] 收到数学问题: " << user_text << std::endl;
                
                // 保存用户消息
                save_message(context_id, message);
                
                // 使用 AI 解决数学问题
                std::string response_text = solve_math(user_text, context_id);
                
                // 响应继续复用同一个 context_id，便于上游按会话串联消息历史。
                auto response_msg = AgentMessage::create()
                    .with_role(MessageRole::Agent)
                    .with_context_id(context_id);
                response_msg.add_text_part(response_text);
                save_message(context_id, response_msg);
                
                auto response = JsonRpcResponse::create_success(request.id(), response_msg.to_json());
                return response.to_json();
            }
            
            return JsonRpcResponse::create_error(request.id(), ErrorCode::MethodNotFound, "Method not found").to_json();
            
        } catch (const std::exception& e) {
            std::cerr << "[MathAgent] 错误: " << e.what() << std::endl;
            return JsonRpcResponse::create_error("1", ErrorCode::InternalError, e.what()).to_json();
        }
    }

    /**
     * @brief 处理流式请求 (message/stream)
     * @param body JSON-RPC 请求体
     * @param write_callback 用于逐段写回事件的回调
     */
    void handle_stream_request(const std::string& body,
                               std::function<bool(const std::string&)> write_callback) {
        try {
            auto request_json = json::parse(body);
            auto request = JsonRpcRequest::from_json(body);
            
            if (request.method() != "message/stream") {
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
            
            std::string user_text;
            if (!message.parts().empty()) {
                // 与同步接口保持一致，只抽取首个文本分片作为问题内容。
                auto text_part = dynamic_cast<TextPart*>(message.parts()[0].get());
                if (text_part) {
                    user_text = text_part->text();
                }
            }
            
            std::string context_id = message.context_id().value_or("default");
            
            std::cout << "[MathAgent] 收到流式数学问题: " << user_text << std::endl;
            
            // 保存用户消息
            save_message(context_id, message);
            
            // 先发送 stream_start，让调用方可以建立本次流式会话状态。
            json start_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {
                    {"type", "stream_start"},
                    {"contextId", context_id}
                }}
            };
            write_callback(start_event.dump());
            
            // 解决数学问题
            std::string response_text = solve_math(user_text, context_id);
            
            // 这里按固定字符长度切块，目的是模拟增量输出，而不是 token 级流式推送。
            const size_t chunk_size = 50;
            for (size_t i = 0; i < response_text.length(); i += chunk_size) {
                std::string chunk = response_text.substr(i, chunk_size);
                
                json chunk_event = {
                    {"jsonrpc", "2.0"},
                    {"id", request.id()},
                    {"result", {
                        {"type", "chunk"},
                        {"content", chunk}
                    }}
                };
                
                // 客户端断开时尽快停止发送，避免继续做无效写操作。
                if (!write_callback(chunk_event.dump())) {
                    return;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            // 保存响应
            auto response_msg = AgentMessage::create()
                .with_role(MessageRole::Agent)
                .with_context_id(context_id);
            response_msg.add_text_part(response_text);
            save_message(context_id, response_msg);
            
            // 最终事件携带完整消息对象，方便兼容依赖最终落盘消息的调用方。
            json complete_event = {
                {"jsonrpc", "2.0"},
                {"id", request.id()},
                {"result", {
                    {"type", "stream_end"},
                    {"message", response_msg.to_json()}
                }}
            };
            write_callback(complete_event.dump());
            
        } catch (const std::exception& e) {
            std::cerr << "[MathAgent] 流式处理错误: " << e.what() << std::endl;
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
     * @brief 结合历史对话和可选工具结果生成数学问题答案
     * @param question 用户当前问题
     * @param context_id 会话上下文标识
     * @return 大模型生成的最终回答
     */
    std::string solve_math(const std::string& question, const std::string& context_id) {
        // 获取最近几轮历史，帮助模型理解多轮追问、补充条件等上下文。
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
        
        // 先尝试走外部计算工具，获取更稳定的数值结果，再交给模型组织答案。
        std::string tool_result;
        if (mcp_integration_ && mcp_integration_->isAvailable()) {
            tool_result = tryMCPCalculation(question);
        }
        
        std::string system_prompt = "你是一个专业的数学助手。请解答用户的数学问题，给出详细的解题步骤。"
                                   "如果是计算题，请给出准确的计算结果。";
        
        // 工具结果作为“参考信息”注入提示词，而不是直接替代最终自然语言回答。
        if (!tool_result.empty()) {
            system_prompt += "\n\n工具计算结果参考:\n" + tool_result;
        }
        
        return qwen_client_.chat(system_prompt + "\n\n历史对话:\n" + history_text, question);
    }
    
    /**
     * @brief 尝试使用 MCP 工具进行数学计算
     * @param question 用户问题或待计算表达式
     * @return 工具计算结果；调用失败时返回空字符串
     * 
     * 使用 RAG 智能检索相关工具，优先使用 calculator 工具
     */
    std::string tryMCPCalculation(const std::string& question) {
        if (!mcp_integration_ || !mcp_integration_->isAvailable()) {
            return "";
        }
        
        // 优先通过 RAG 缩小候选工具集合，避免对所有工具逐个试调用。
        std::vector<ToolInfo> relevant_tools;
        if (mcp_integration_->isRAGEnabled()) {
            std::cout << "[MathAgent] 使用 RAG 检索相关工具..." << std::endl;
            // 增加 top_k 到 5，获取更多候选
            relevant_tools = mcp_integration_->getRelevantTools(question, 5);
            std::cout << "[MathAgent] RAG 检索到 " << relevant_tools.size() << " 个相关工具: ";
            for (const auto& tool : relevant_tools) {
                std::cout << tool.name << " ";
            }
            std::cout << std::endl;
        } else {
            // RAG 未启用，回退到检查特定工具
            std::cout << "[MathAgent] RAG 未启用，使用默认工具检查" << std::endl;
            if (mcp_integration_->hasToolAvailable("calculator")) {
                ToolInfo calc_tool;
                calc_tool.name = "calculator";
                relevant_tools.push_back(calc_tool);
            }
        }
        
        // 优先尝试具备数学表达式求值能力的工具别名。
        for (const auto& tool : relevant_tools) {
            if (tool.name == "calculator" || tool.name == "calculate" || tool.name == "math") {
                json args;
                // 直接把用户问题作为 expression 传入。
                // 如果问题不是纯表达式，工具可能失败，随后由大模型继续兜底回答。
                args["expression"] = question;
                
                std::cout << "[MathAgent] 调用 MCP 工具: " << tool.name << std::endl;
                
                auto result = mcp_integration_->callTool(tool.name, args.dump());
                if (result.success) {
                    std::cout << "[MathAgent] MCP 工具返回: " << result.result << std::endl;
                    return result.result;
                } else {
                    std::cerr << "[MathAgent] MCP 工具调用失败: " << result.error << std::endl;
                }
            }
        }
        
        // RAG 结果可能受召回质量影响，这里再做一次显式工具名兜底。
        if (mcp_integration_->hasToolAvailable("calculator")) {
            json args;
            args["expression"] = question;
            
            std::cout << "[MathAgent] 回退使用 calculator 工具" << std::endl;
            
            auto result = mcp_integration_->callTool("calculator", args.dump());
            if (result.success) {
                std::cout << "[MathAgent] MCP 工具返回: " << result.result << std::endl;
                return result.result;
            }
        }
        
        return "";
    }
    
    /**
     * @brief 按上下文保存消息历史
     * @param context_id 会话上下文标识
     * @param message 待保存消息
     */
    void save_message(const std::string& context_id, const AgentMessage& message) {
        if (!task_store_->task_exists(context_id)) {
            // 首次出现的上下文先补建任务记录，保证历史消息有归属任务。
            auto task = AgentTask::create()
                .with_id(context_id)
                .with_context_id(context_id)
                .with_status(TaskState::Running);
            task_store_->set_task(task);
        }
        task_store_->add_history_message(context_id, message);
    }
    
    /**
     * @brief 生成 Agent Card 描述信息
     * @return Agent Card 的 JSON 字符串
     */
    std::string get_agent_card() {
        json card = {
            {"name", "Math Agent"},
            {"description", "专业数学计算助手，擅长各类数学问题求解"},
            {"version", "1.0.0"},
            {"capabilities", {
                {"streaming", false},
                {"push_notifications", false},
                {"task_management", true}
            }},
            {"skills", json::array({
                {
                    {"name", "数学计算"},
                    {"description", "执行各类数学计算和方程求解"},
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

/**
 * @brief 打印命令行帮助信息
 * @param program 当前程序名
 */
void print_usage(const char* program) {
    std::cerr << "用法: " << program << " <agent_id> <port> <registry_url> <api_key> [options]" << std::endl;
    std::cerr << "选项:" << std::endl;
    std::cerr << "  --redis-host <host>     Redis 主机 (默认: 127.0.0.1)" << std::endl;
    std::cerr << "  --redis-port <port>     Redis 端口 (默认: 6379)" << std::endl;
    std::cerr << "  --mcp-server <path>     MCP Server 可执行文件路径" << std::endl;
    std::cerr << "  --mcp-args <args>       MCP Server 启动参数 (逗号分隔)" << std::endl;
    std::cerr << "  --enable-mcp            启用 MCP" << std::endl;
    std::cerr << std::endl;
    std::cerr << "示例: " << program << " math-1 5001 http://localhost:8500 sk-xxx --enable-mcp --mcp-server /path/to/mcp_server" << std::endl;
}

/**
 * @brief 程序入口，解析配置并启动 Math Agent
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 程序退出码
 */
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
    
    // 命令行未启用 MCP 时，再尝试从环境变量读取，便于部署场景统一注入配置。
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
    
    // 注册中心需要可访问地址，因此这里提前组装 Agent 的对外监听地址。
    std::string listen_address = "http://localhost:" + std::to_string(port);
    
    try {
        MathAgent agent(agent_id, listen_address, registry_url, api_key, 
                       redis_host, redis_port, mcp_config);
        agent.start(port);
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
