/**
 * @file ops_agent_main.cpp
 * @brief Ops Agent - 基于通用 ReAct 模板的运维专业 Agent
 *
 * 职责：
 * - 理解服务器状态检查与故障排查类问题
 * - 自主决定是否调用 CPU、磁盘、网络、进程等运维工具
 * - 基于工具结果给出结论、证据和处理建议
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
 * @brief 判断工具名是否明显属于运维观测工具
 * @param tool_name 工具名称
 * @return true 表示命中
 */
bool is_ops_tool_name(const std::string& tool_name) {
    const std::string lowered = to_lower_copy(tool_name);
    return lowered.find("cpu") != std::string::npos ||
           lowered.find("disk") != std::string::npos ||
           lowered.find("network") != std::string::npos ||
           lowered.find("process") != std::string::npos ||
           lowered.find("system") != std::string::npos ||
           lowered.find("server") != std::string::npos ||
           lowered.find("overview") != std::string::npos;
}

}  // namespace

/**
 * @brief 基于通用模板实现的运维专业 Agent
 */
class OpsAgent : public ReActAgentTemplate {
public:
    /**
     * @brief 构造 Ops Agent
     * @param agent_id Agent 唯一标识
     * @param listen_address 对外监听地址
     * @param registry_url 服务注册中心地址
     * @param api_key Qwen 访问密钥
     * @param redis_host Redis 主机
     * @param redis_port Redis 端口
     * @param mcp_config MCP 集成配置
     */
    OpsAgent(const std::string& agent_id,
             const std::string& listen_address,
             const std::string& registry_url,
             const std::string& api_key,
             const std::string& redis_host,
             int redis_port,
             const MCPAgentConfig& mcp_config = MCPAgentConfig())
        : ReActAgentTemplate({
            agent_id,
            "Ops Agent",
            listen_address,
            registry_url,
            api_key,
            redis_host,
            redis_port,
            mcp_config,
            {"ops", "server", "monitoring", "react-agent"},
            "",
            ""
        }) {}

protected:
    /**
     * @brief 将运维问题改写成自包含的巡检或诊断任务
     * @param user_text 用户原始输入
     * @param context_id 会话上下文 ID
     * @return 改写后的查询及流式状态事件
     */
    ReActPreparation prepare_query(const std::string& user_text,
                                   const std::string& context_id) override {
        const std::string history_text = build_history_text(context_id, 6);
        std::string system_prompt =
            "你是 Ops Agent 的请求改写器。"
            "请把当前用户输入改写成适合服务器巡检或故障排查的单条自包含任务。\n"
            "要求：\n"
            "1. 保留服务器对象、指标维度、阈值、时间范围和异常现象。\n"
            "2. 删除寒暄和与运维无关的描述。\n"
            "3. 如果用户只说“服务器很卡”“帮我看下状态”，改写成明确的巡检任务，包含 CPU、磁盘、网络和进程排查意图。\n"
            "4. 如果当前输入依赖最近上下文，请结合历史补全省略条件。\n"
            "5. 只输出改写后的结果，不要解释。";

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
            std::cerr << "[Ops Agent] 运维请求改写失败: " << e.what() << std::endl;
        }

        return {user_text, {}};
    }

    /**
     * @brief 构造运维领域的 ReAct 系统提示词
     * @param prepared_query 预处理后的运维查询
     * @param context_id 会话上下文 ID
     * @return system prompt
     */
    std::string build_react_system_prompt(const std::string& prepared_query,
                                          const std::string& /*context_id*/) const override {
        return
            "你是一个专业的实验室服务器运维 Agent，负责检查服务器状态并给出诊断建议。"
            "当用户询问 CPU、磁盘、网络、进程、服务器卡顿、负载异常等问题时，"
            "应优先调用工具获取事实数据，再基于结果作答。"
            "不要编造指标，不要假设工具已经返回不存在的数据。"
            "如果问题是“服务器很卡/状态如何”，通常应先查看整体概览，再按需要补查 CPU、磁盘、网络和 top 进程。"
            "如果工具调用失败，请说明失败点，并基于已知信息给出保守建议。"
            "最终回答请使用中文，优先包含“结论、证据、建议”三部分。"
            "\n当前任务：" + prepared_query;
    }

    /**
     * @brief 为运维问题筛选更聚焦的工具集合
     * @param prepared_query 预处理后的运维查询
     * @param context_id 会话上下文 ID
     * @return 提供给模型的运维工具列表
     */
    std::vector<QwenToolDefinition> build_tool_definitions(
        const std::string& prepared_query,
        const std::string& context_id) override {
        auto tool_definitions =
            ReActAgentTemplate::build_tool_definitions(prepared_query, context_id);

        std::vector<QwenToolDefinition> ops_tools;
        ops_tools.reserve(tool_definitions.size());

        for (const auto& tool : tool_definitions) {
            if (is_ops_tool_name(tool.name)) {
                ops_tools.push_back(tool);
            }
        }

        if (!ops_tools.empty()) {
            return ops_tools;
        }

        auto* integration = mcp_integration();
        if (!integration || !integration->isAvailable()) {
            return tool_definitions;
        }

        // 当 RAG 返回结果不稳定时，回退到全部已知运维工具，确保诊断链路可继续执行。
        const auto available_tools = integration->getAvailableTools();
        for (const auto& tool : available_tools) {
            if (is_ops_tool_name(tool.name)) {
                ops_tools.push_back({tool.name, tool.description, tool.input_schema});
            }
        }

        if (!ops_tools.empty()) {
            return ops_tools;
        }

        return tool_definitions;
    }

    /**
     * @brief 补充运维工具选择提示
     * @param tool_context 当前轮次候选工具
     * @param prepared_query 预处理后的查询
     * @param context_id 会话上下文 ID
     * @return 补充的 system prompt
     */
    std::string build_tool_selection_prompt(const ReActToolContext& tool_context,
                                            const std::string& prepared_query,
                                            const std::string& context_id) const override {
        std::string prompt = ReActAgentTemplate::build_tool_selection_prompt(
            tool_context, prepared_query, context_id);
        if (prompt.empty()) {
            return prompt;
        }

        prompt +=
            "\n运维决策提示："
            "\n1. 如果是“状态如何/帮我巡检”，优先使用 overview 类工具。"
            "\n2. 如果是“很卡/很慢/高负载”，优先检查 CPU，再结合 top 进程。"
            "\n3. 如果涉及空间不足、写入失败、日志打满，优先检查磁盘。"
            "\n4. 如果涉及连不上、丢包、网络慢，优先检查网络。";
        return prompt;
    }

    /**
     * @brief RAG-MCP 检索默认返回的候选工具数量
     * @return 候选工具数量
     */
    int rag_tool_top_k() const override {
        return 6;
    }

    /**
     * @brief 运维问题通常需要保留最近几轮上下文
     * @return 历史消息窗口大小
     */
    size_t history_message_limit() const override {
        return 10;
    }

    /**
     * @brief 运维诊断可能需要多轮补充检查
     * @return 单次请求允许的最大工具调用轮数
     */
    int max_tool_rounds() const override {
        return 6;
    }

    /**
     * @brief 提供 Ops Agent 的 Agent Card
     * @return Agent Card JSON
     */
    react_json build_agent_card_json() const override {
        return {
            {"name", "Ops Agent"},
            {"description", "Ops Agent：负责实验室服务器状态巡检与故障排查"},
            {"version", "1.0.0"},
            {"capabilities", {
                {"streaming", true},
                {"push_notifications", false},
                {"task_management", true}
            }},
            {"skills", react_json::array({
                {
                    {"name", "服务器状态巡检"},
                    {"description", "检查 CPU、磁盘、网络和关键进程状态"},
                    {"input_modes", react_json::array({"text"})},
                    {"output_modes", react_json::array({"text"})}
                },
                {
                    {"name", "异常诊断建议"},
                    {"description", "基于观测结果给出卡顿、空间不足、网络异常等问题的处理建议"},
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
    std::cerr << "示例: " << program << " ops-1 5003 http://localhost:8500 sk-xxx --enable-mcp --mcp-server /path/to/mcp_server" << std::endl;
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
        OpsAgent agent(agent_id, listen_address, registry_url, api_key,
                       redis_host, redis_port, mcp_config);
        agent.start(port);
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
