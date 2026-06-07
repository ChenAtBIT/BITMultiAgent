/**
 * @file math_agent_main.cpp
 * @brief Math Agent - 基于通用 ReAct 模板的数学专业 Agent
 *
 * 职责：
 * - 自主改写数学请求
 * - 自主决定是否调用数学工具
 * - 自主整合工具结果完成最终回答
 */

#include "ai_orchestrator/react_agent_template.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

using namespace agent_rpc::mcp;

namespace {

/**
 * @brief 生成小写副本
 * @param text 原始文本
 * @return 小写化后的文本
 */
std::string to_lower_copy(const std::string& text) {
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return lowered;
}

/**
 * @brief 判断工具名是否明显属于数学工具
 * @param tool_name 工具名称
 * @return true 表示命中
 */
bool is_math_tool_name(const std::string& tool_name) {
    const std::string lowered = to_lower_copy(tool_name);
    return lowered.find("calc") != std::string::npos ||
           lowered.find("math") != std::string::npos ||
           lowered.find("equation") != std::string::npos ||
           lowered.find("algebra") != std::string::npos;
}

}  // namespace

/**
 * @brief 基于通用模板实现的数学专业 Agent
 */
class MathAgent : public ReActAgentTemplate {
public:
    /**
     * @brief 构造 Math Agent
     * @param agent_id Agent 唯一标识
     * @param listen_address 对外监听地址
     * @param registry_url 服务注册中心地址
     * @param api_key Qwen 访问密钥
     * @param redis_host Redis 主机
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
        : ReActAgentTemplate({
            agent_id,
            "Math Agent",
            listen_address,
            registry_url,
            api_key,
            redis_host,
            redis_port,
            mcp_config,
            {"math", "calculator", "react-agent"},
            "",
            ""
        }) {}

protected:
    /**
     * @brief 对数学请求做自包含改写
     * @param user_text 用户原始输入
     * @param context_id 会话上下文 ID
     * @return 改写后的查询及流式状态事件
     */
    ReActPreparation prepare_query(const std::string& user_text,
                                   const std::string& context_id) override {
        const std::string history_text = build_history_text(context_id, 6);
        std::string system_prompt =
            "你是 Math Agent 的数学请求改写器。"
            "请把当前用户输入改写成适合求解与工具调用的单条数学任务。\n"
            "要求：\n"
            "1. 保留全部数学条件、数字、变量、单位和约束。\n"
            "2. 删除寒暄和与数学无关的内容。\n"
            "3. 如果本质上是直接计算题，只输出可直接求值的表达式。\n"
            "4. 如果是方程、化简、求导、积分、几何、概率统计等问题，输出一条自包含的数学任务。\n"
            "5. 如果当前输入依赖最近上下文，请结合历史补全省略条件。\n"
            "6. 只输出改写后的结果，不要解释。";

        if (!history_text.empty()) {
            system_prompt += "\n\n最近对话:\n" + history_text;
        }

        try {
            const std::string rewritten = trim(qwen_client().chat(system_prompt, user_text));
            if (!rewritten.empty() && rewritten != user_text) {
                return {
                    rewritten,
                    {react_json{
                        {"type", "rewrite"},
                        {"query", rewritten}
                    }}
                };
            }
        } catch (const std::exception& e) {
            std::cerr << "[Math Agent] 数学请求改写失败: " << e.what() << std::endl;
        }

        return {user_text, {}};
    }

    /**
     * @brief 构造数学领域的 ReAct 系统提示词
     * @param prepared_query 预处理后的数学查询
     * @param context_id 会话上下文 ID
     * @return system prompt
     */
    std::string build_react_system_prompt(const std::string& prepared_query,
                                          const std::string& /*context_id*/) const override {
        return
            "你是一个专业的数学 Agent，负责独立完成数学推理、计算和求解。"
            "当问题需要精确计算、表达式求值、方程求解或外部数学能力时，请主动调用可用工具。"
            "如果工具已经返回结果，请基于工具结果继续解释，不要重复请求相同工具。"
            "如果工具调用失败，请根据已知信息说明失败原因并尽量继续回答。"
            "最终回答请使用中文，给出清晰的解题步骤和结论。"
            "\n当前任务：" + prepared_query;
    }

    /**
     * @brief 为数学问题筛选更聚焦的工具集合
     * @param prepared_query 预处理后的数学查询
     * @param context_id 会话上下文 ID
     * @return 提供给模型的数学工具列表
     */
    std::vector<QwenToolDefinition> build_tool_definitions(
        const std::string& prepared_query,
        const std::string& context_id) override {
        auto tool_definitions =
            ReActAgentTemplate::build_tool_definitions(prepared_query, context_id);

        if (tool_definitions.empty()) {
            return tool_definitions;
        }

        std::vector<QwenToolDefinition> math_tools;
        math_tools.reserve(tool_definitions.size());

        for (const auto& tool : tool_definitions) {
            if (is_math_tool_name(tool.name)) {
                math_tools.push_back(tool);
            }
        }

        // 如果筛完为空，说明当前 MCP 没有明显数学命名的工具，回退到相关工具全集。
        if (!math_tools.empty()) {
            return math_tools;
        }

        return tool_definitions;
    }

    /**
     * @brief 提供 Math Agent 的 Agent Card
     * @return Agent Card JSON
     */
    react_json build_agent_card_json() const override {
        return {
            {"name", "Math Agent"},
            {"description", "基于通用 ReAct 模板的数学计算与求解 Agent"},
            {"version", "2.0.0"},
            {"capabilities", {
                {"streaming", true},
                {"push_notifications", false},
                {"task_management", true}
            }},
            {"skills", react_json::array({
                {
                    {"name", "数学问题改写"},
                    {"description", "将多轮对话中的数学请求改写成可求解的自包含任务"},
                    {"input_modes", react_json::array({"text"})},
                    {"output_modes", react_json::array({"text"})}
                },
                {
                    {"name", "ReAct 工具调用"},
                    {"description", "自主决定是否调用数学工具，并整合工具结果完成回答"},
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
    std::cerr << "  --mcp-server <path>     MCP Server 可执行文件路径" << std::endl;
    std::cerr << "  --mcp-args <args>       MCP Server 启动参数 (逗号分隔)" << std::endl;
    std::cerr << "  --enable-mcp            启用 MCP" << std::endl;
    std::cerr << std::endl;
    std::cerr << "示例: " << program << " math-1 5001 http://localhost:8500 sk-xxx --enable-mcp --mcp-server /path/to/mcp_server" << std::endl;
}

/**
 * @brief 程序入口
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @return 程序退出码
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

    MCPAgentConfig mcp_config = parseMCPConfigFromArgs(argc, argv);
    if (!mcp_config.enable_mcp) {
        MCPAgentConfig env_config = parseMCPConfigFromEnv();
        if (env_config.enable_mcp) {
            mcp_config = env_config;
        }
    }

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
        MathAgent agent(agent_id, listen_address, registry_url, api_key,
                        redis_host, redis_port, mcp_config);
        agent.start(port);
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
