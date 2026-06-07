/**
 * @file general_agent_main.cpp
 * @brief General Agent - 通用问答专用 Agent
 *
 * 职责：
 * - 处理未命中 math/code 的通用问题
 * - 仅依赖上下文直接回答，不初始化也不调用 MCP 工具
 */

#include "ai_orchestrator/react_agent_template.hpp"

#include <string>

using namespace agent_rpc::mcp;

/**
 * @brief 通用问题专用 Agent
 */
class GeneralAgent : public A2AAgentRuntime {
public:
    /**
     * @brief 构造 General Agent
     * @param agent_id Agent 唯一标识
     * @param listen_address 对外监听地址
     * @param registry_url 注册中心地址
     * @param api_key Qwen 访问密钥
     * @param redis_host Redis 主机
     * @param redis_port Redis 端口
     */
    GeneralAgent(const std::string& agent_id,
                 const std::string& listen_address,
                 const std::string& registry_url,
                 const std::string& api_key,
                 const std::string& redis_host,
                 int redis_port)
        : A2AAgentRuntime({
            agent_id,
            "General Agent",
            listen_address,
            registry_url,
            api_key,
            redis_host,
            redis_port,
            MCPAgentConfig(),
            {"general", "chat", "orchestrator-child"}
        }) {}

protected:
    /**
     * @brief 处理通用问题，不使用工具，直接生成文本回复
     */
    AgentResponsePayload handle_agent_request(const std::string& user_text,
                                            const std::string& context_id) override {
        AgentResponsePayload payload;

        std::vector<QwenMessage> messages = {
            QwenMessage{"system", build_general_system_prompt(), "", "", {}}
        };

        // 走分层上下文，保留历史背景，避免每轮独立丢失偏好。
        const auto history_messages = build_history_messages(context_id, 10);
        messages.insert(messages.end(), history_messages.begin(), history_messages.end());

        // 当前用户问题已入库，若历史丢失则显式补一条 user 消息，保证模型一定可见当前问题。
        if (messages.empty() || messages.back().role != "user" ||
            messages.back().content != user_text) {
            messages.push_back(QwenMessage{"user", user_text, "", "", {}});
        }

        payload.text = qwen_client().chat_completion(messages).content;
        if (payload.text.empty()) {
            payload.text = "抱歉，我暂时没法给出合适的回复，请稍后再试。";
        }

        return payload;
    }

    /**
     * @brief 构造通用 Agent 的系统提示词
     * @return system prompt
     */
    std::string build_general_system_prompt() const {
        return
            "你是一个严谨、友好、准确的中文通用助手。"
            "基于会话上下文回答用户的问题，不进行工具调用。"
            "回答要简洁、明确，并且尽量给出可执行的建议。";
    }

    /**
     * @brief 生成 General Agent 的 Agent Card
     * @return Agent Card JSON
     */
    react_json build_agent_card_json() const override {
        return {
            {"name", "General Agent"},
            {"description", "通用问答 Agent：处理 general 场景，不使用 MCP 工具"},
            {"version", "1.0.0"},
            {"capabilities", {
                {"streaming", true},
                {"push_notifications", false},
                {"task_management", true}
            }},
            {"skills", react_json::array({
                {
                    {"name", "通用问答"},
                    {"description", "处理日常对话、知识问答和执行建议"},
                    {"input_modes", react_json::array({"text"})},
                    {"output_modes", react_json::array({"text"})}
                }
            })},
            {"provider", {
                {"name", "Agent Communication RPC"},
                {"organization", "A2A Integration"}
            }}
        };
    }
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
    std::cerr << "  说明: General Agent 会始终禁用 MCP，不执行任何工具调用" << std::endl;
    std::cerr << std::endl;
    std::cerr << "示例: " << program << " general-1 5002 http://localhost:8500 sk-xxx --redis-host 127.0.0.1 --redis-port 6379" << std::endl;
}

/**
 * @brief 程序入口
 */
int main(int argc, char* argv[]) {
    if (argc < 5) {
        print_usage(argv[0]);
        return 1;
    }

    const std::string agent_id = argv[1];
    const int port = std::stoi(argv[2]);
    const std::string registry_url = argv[3];
    const std::string api_key = argv[4];

    std::string redis_host = "127.0.0.1";
    int redis_port = 6379;

    for (int i = 5; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--redis-host" && i + 1 < argc) {
            redis_host = argv[++i];
        } else if (arg == "--redis-port" && i + 1 < argc) {
            redis_port = std::stoi(argv[++i]);
        }
    }

    const std::string listen_address = "http://localhost:" + std::to_string(port);

    try {
        // 通用 Agent 显式禁用 MCP，确保所有 general 问题都由模型直接回答。
        GeneralAgent agent(agent_id, listen_address, registry_url, api_key,
                          redis_host, redis_port);
        agent.start(port);
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
