/**
 * @file minutes_agent_main.cpp
 * @brief Minutes Agent - 基于通用 ReAct 模板的会议纪要 Agent
 *
 * 职责：
 * - 根据用户给出的会议文件路径读取会议记录
 * - 总结输出 Markdown 纪要
 * - 调用 MCP 文件工具完成纪要写入
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
 * @brief 判断工具名是否明显属于纪要文件处理工具
 * @param tool_name 工具名称
 * @return true 表示命中
 */
bool is_minutes_tool_name(const std::string& tool_name) {
    const std::string lowered = to_lower_copy(tool_name);
    if (lowered == "read_text_file" ||
        lowered == "write_text_file" ||
        lowered == "derive_markdown_output_path") {
        return true;
    }

    return lowered.find("meeting") != std::string::npos ||
           lowered.find("minutes") != std::string::npos ||
           lowered.find("markdown") != std::string::npos ||
           (lowered.find("file") != std::string::npos &&
            (lowered.find("read") != std::string::npos ||
             lowered.find("write") != std::string::npos));
}

}  // namespace

/**
 * @brief 基于通用模板实现的会议纪要 Agent
 */
class MinutesAgent : public ReActAgentTemplate {
public:
    /**
     * @brief 构造 Minutes Agent
     * @param agent_id Agent 唯一标识
     * @param listen_address 对外监听地址
     * @param registry_url 服务注册中心地址
     * @param api_key Qwen 访问密钥
     * @param redis_host Redis 主机
     * @param redis_port Redis 端口
     * @param mcp_config MCP 集成配置
     */
    MinutesAgent(const std::string& agent_id,
                 const std::string& listen_address,
                 const std::string& registry_url,
                 const std::string& api_key,
                 const std::string& redis_host,
                 int redis_port,
                 const MCPAgentConfig& mcp_config = MCPAgentConfig())
        : ReActAgentTemplate({
            agent_id,
            "Minutes Agent",
            listen_address,
            registry_url,
            api_key,
            redis_host,
            redis_port,
            mcp_config,
            {"minutes", "meeting", "summary", "react-agent"},
            "",
            ""
        }) {}

protected:
    /**
     * @brief 将纪要请求改写成自包含的执行任务
     * @param user_text 用户原始输入
     * @param context_id 会话上下文 ID
     * @return 改写后的查询及流式状态事件
     */
    ReActPreparation prepare_query(const std::string& user_text,
                                   const std::string& context_id) override {
        const std::string history_text = build_history_text(context_id, 6);
        std::string system_prompt =
            "你是 Minutes Agent 的任务整理器。"
            "请把当前用户输入改写成一条可执行的会议纪要生成任务。\n"
            "要求：\n"
            "1. 保留会议源文件路径、输出路径、语言、格式要求和重点关注项。\n"
            "2. 如果用户没有明确输出路径，请在任务里保留“需要自动推导 Markdown 输出路径”的要求。\n"
            "3. 如果用户补充了上下文中的会议背景，请一起保留。\n"
            "4. 只输出改写后的任务，不要解释。";

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
            std::cerr << "[Minutes Agent] 纪要请求改写失败: " << e.what() << std::endl;
        }

        return {user_text, {}};
    }

    /**
     * @brief 构造会议纪要领域的 ReAct 系统提示词
     * @param prepared_query 预处理后的纪要任务
     * @param context_id 会话上下文 ID
     * @return system prompt
     */
    std::string build_react_system_prompt(const std::string& prepared_query,
                                          const std::string& /*context_id*/) const override {
        return
            "你是一个专业的会议纪要 Agent，负责根据本地会议文件生成中文 Markdown 纪要。"
            "当任务里包含会议文件路径时，应优先读取文件内容，再总结并写出纪要文件。"
            "请严格遵守以下规则："
            "\n1. 先调用 read_text_file 读取会议记录；如果返回 truncated=true，继续分段读取，直到掌握完整关键信息。"
            "\n2. 如果用户没有给出输出路径，先调用 derive_markdown_output_path 推导默认输出路径。"
            "\n3. 纪要正文必须是 Markdown，优先包含：标题、会议基本信息、核心议题、关键结论、待办事项、风险与后续跟进。"
            "\n4. 不要编造未出现在会议文件中的日期、负责人、决议或截止时间；无法确认时请明确标注“未明确”。"
            "\n5. 在得到完整 Markdown 后，调用 write_text_file 写入目标文件。"
            "\n6. 最终回答使用中文，简要告知纪要已保存到哪个路径，并提炼 3 到 5 条摘要；不要把整份 Markdown 全文重复输出。"
            "\n7. 如果读取或写入失败，请清楚说明失败原因，不要谎称已生成。"
            "\n当前任务：" + prepared_query;
    }

    /**
     * @brief 为纪要任务筛选文件读写工具
     * @param prepared_query 预处理后的纪要任务
     * @param context_id 会话上下文 ID
     * @return 提供给模型的纪要工具列表
     */
    std::vector<QwenToolDefinition> build_tool_definitions(
        const std::string& prepared_query,
        const std::string& context_id) override {
        auto tool_definitions =
            ReActAgentTemplate::build_tool_definitions(prepared_query, context_id);

        std::vector<QwenToolDefinition> minutes_tools;
        minutes_tools.reserve(tool_definitions.size());

        for (const auto& tool : tool_definitions) {
            if (is_minutes_tool_name(tool.name)) {
                minutes_tools.push_back(tool);
            }
        }

        if (!minutes_tools.empty()) {
            return minutes_tools;
        }

        auto* integration = mcp_integration();
        if (!integration || !integration->isAvailable()) {
            return tool_definitions;
        }

        const auto available_tools = integration->getAvailableTools();
        for (const auto& tool : available_tools) {
            if (is_minutes_tool_name(tool.name)) {
                minutes_tools.push_back({tool.name, tool.description, tool.input_schema});
            }
        }

        if (!minutes_tools.empty()) {
            return minutes_tools;
        }

        return tool_definitions;
    }

    /**
     * @brief 补充纪要工具使用提示
     * @param tool_context 当前轮次候选工具
     * @param prepared_query 预处理后的任务
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
            "\n纪要执行提示："
            "\n1. 如果任务里有会议文件路径，必须先读文件，再生成纪要。"
            "\n2. 长文件要根据 next_start_line 继续读取，直到关键议题、决策和行动项足够完整。"
            "\n3. 如果用户没给输出路径，先推导默认 Markdown 路径，再写文件。"
            "\n4. 只有在 write_text_file 成功后，才可以告知用户“已经生成并保存”。";
        return prompt;
    }

    /**
     * @brief RAG-MCP 检索默认返回的候选工具数量
     * @return 候选工具数量
     */
    int rag_tool_top_k() const override {
        return 4;
    }

    /**
     * @brief 纪要任务通常需要保留最近几轮补充说明
     * @return 历史消息窗口大小
     */
    size_t history_message_limit() const override {
        return 8;
    }

    /**
     * @brief 长会议文件可能需要多轮读取和一次写文件
     * @return 单次请求允许的最大工具调用轮数
     */
    int max_tool_rounds() const override {
        return 8;
    }

    /**
     * @brief 提供 Minutes Agent 的 Agent Card
     * @return Agent Card JSON
     */
    react_json build_agent_card_json() const override {
        return {
            {"name", "Minutes Agent"},
            {"description", "Minutes Agent：负责读取会议文件并生成 Markdown 纪要"},
            {"version", "1.0.0"},
            {"capabilities", {
                {"streaming", true},
                {"push_notifications", false},
                {"task_management", true}
            }},
            {"skills", react_json::array({
                {
                    {"name", "会议纪要生成"},
                    {"description", "根据会议记录文件生成结构化的中文 Markdown 纪要"},
                    {"input_modes", react_json::array({"text"})},
                    {"output_modes", react_json::array({"text"})}
                },
                {
                    {"name", "行动项提炼"},
                    {"description", "识别会议结论、待办事项、负责人和后续跟进内容"},
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
    std::cerr << "示例: " << program << " minutes-1 5004 http://localhost:8500 sk-xxx --enable-mcp --mcp-server /path/to/mcp_server" << std::endl;
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
        MinutesAgent agent(agent_id, listen_address, registry_url, api_key,
                           redis_host, redis_port, mcp_config);
        agent.start(port);
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
