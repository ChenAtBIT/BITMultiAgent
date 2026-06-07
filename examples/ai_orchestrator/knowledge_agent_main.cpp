/**
 * @file knowledge_agent_main.cpp
 * @brief Knowledge Agent - 基于通用 ReAct 模板的知识库 Agent
 *
 * 职责：
 * - 根据用户提供的文件路径，调用知识库工具完成文档切片、向量化与 sqlite-vec 入库
 * - 根据用户问题检索知识库中的相似片段，并基于检索证据作答
 * - 仅使用知识库相关工具，避免干扰其他 Agent 的职责边界
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
 * @brief 判断工具名是否明显属于知识库工具
 * @param tool_name 工具名称
 * @return true 表示命中
 */
bool is_knowledge_tool_name(const std::string& tool_name) {
    const std::string lowered = to_lower_copy(tool_name);
    return lowered.find("knowledge") != std::string::npos ||
           lowered.find("sqlite") != std::string::npos ||
           lowered.find("vector") != std::string::npos ||
           lowered.find("ingest") != std::string::npos ||
           lowered.find("search") != std::string::npos ||
           lowered.find("retrieve") != std::string::npos;
}

}  // namespace

/**
 * @brief 基于通用模板实现的知识库 Agent
 */
class KnowledgeAgent : public ReActAgentTemplate {
public:
    /**
     * @brief 构造 Knowledge Agent
     * @param agent_id Agent 唯一标识
     * @param listen_address 对外监听地址
     * @param registry_url 服务注册中心地址
     * @param api_key Qwen 访问密钥
     * @param redis_host Redis 主机
     * @param redis_port Redis 端口
     * @param mcp_config MCP 集成配置
     */
    KnowledgeAgent(const std::string& agent_id,
                   const std::string& listen_address,
                   const std::string& registry_url,
                   const std::string& api_key,
                   const std::string& redis_host,
                   int redis_port,
                   const MCPAgentConfig& mcp_config = MCPAgentConfig())
        : ReActAgentTemplate({
            agent_id,
            "Knowledge Agent",
            listen_address,
            registry_url,
            api_key,
            redis_host,
            redis_port,
            mcp_config,
            {"knowledge", "documents", "rag", "react-agent"},
            "",
            ""
        }) {}

protected:
    /**
     * @brief 将知识库请求改写成自包含任务
     * @param user_text 用户原始输入
     * @param context_id 会话上下文 ID
     * @return 改写后的查询及流式状态事件
     */
    ReActPreparation prepare_query(const std::string& user_text,
                                   const std::string& context_id) override {
        const std::string history_text = build_history_text(context_id, 6);
        std::string system_prompt =
            "你是 Knowledge Agent 的任务整理器。"
            "请把当前用户输入改写成一条可执行的知识库任务。\n"
            "要求：\n"
            "1. 保留文件路径、集合名、知识库问答目标、筛选条件和输出要求。\n"
            "2. 如果用户要求“导入/向量化/入库”，在任务中明确是写入 sqlite-vec 知识库。\n"
            "3. 如果用户要求“根据知识库回答”，在任务中明确需要先检索，再基于检索结果回答。\n"
            "4. 如果当前输入依赖最近上下文，请结合历史补全缺失条件。\n"
            "5. 只输出改写后的任务，不要解释。";

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
            std::cerr << "[Knowledge Agent] 请求改写失败: " << e.what() << std::endl;
        }

        return {user_text, {}};
    }

    /**
     * @brief 构造知识库任务的 ReAct 系统提示词
     * @param prepared_query 预处理后的任务
     * @param context_id 会话上下文 ID
     * @return system prompt
     */
    std::string build_react_system_prompt(const std::string& prepared_query,
                                          const std::string& /*context_id*/) const override {
        return
            "你是一个专业的 Knowledge Agent，负责把本地文档向量化存入 sqlite-vec，"
            "并基于知识库检索结果回答问题。"
            "请严格遵守以下规则："
            "\n1. 如果用户给出文件路径并要求导入、入库、向量化、建立知识库，优先调用 ingest_knowledge_file。"
            "\n2. 如果用户要求查看知识库规模、文档数、chunk 数或最近更新时间，优先调用 get_knowledge_stats。"
            "\n3. 如果用户要求给文档打标签、补充元数据，优先调用 update_knowledge_metadata。"
            "\n4. 如果用户要求按标签、分类、元数据范围检索，必须先调用 search_by_metadata，再基于返回片段回答。"
            "\n5. 如果用户要求根据知识库回答、检索文档、查询资料，必须先调用 search_knowledge_base，再基于返回片段回答。"
            "\n6. 不要编造知识库中不存在的事实；当检索结果不足时，要明确说明“当前知识库未检索到足够证据”。"
            "\n7. 回答知识库问答时，优先引用 source_path 和 chunk_index 作为证据。"
            "\n8. 如果用户既要求导入文档，又要求立刻问答，可以先入库，再检索，再回答。"
            "\n9. 最终回答请使用中文；入库任务需要说明写入了多少个切片、用了哪个集合；问答任务需要给出简明结论和证据来源。"
            "\n当前任务：" + prepared_query;
    }

    /**
     * @brief 为知识库任务筛选工具集合
     * @param prepared_query 预处理后的任务
     * @param context_id 会话上下文 ID
     * @return 提供给模型的知识库工具列表
     */
    std::vector<QwenToolDefinition> build_tool_definitions(
        const std::string& prepared_query,
        const std::string& context_id) override {
        auto tool_definitions =
            ReActAgentTemplate::build_tool_definitions(prepared_query, context_id);

        std::vector<QwenToolDefinition> knowledge_tools;
        knowledge_tools.reserve(tool_definitions.size());

        for (const auto& tool : tool_definitions) {
            if (is_knowledge_tool_name(tool.name)) {
                knowledge_tools.push_back(tool);
            }
        }

        if (!knowledge_tools.empty()) {
            return knowledge_tools;
        }

        auto* integration = mcp_integration();
        if (!integration || !integration->isAvailable()) {
            return tool_definitions;
        }

        const auto available_tools = integration->getAvailableTools();
        for (const auto& tool : available_tools) {
            if (is_knowledge_tool_name(tool.name)) {
                knowledge_tools.push_back({tool.name, tool.description, tool.input_schema});
            }
        }

        if (!knowledge_tools.empty()) {
            return knowledge_tools;
        }

        return tool_definitions;
    }

    /**
     * @brief 补充知识库工具使用提示
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
            "\n知识库执行提示："
            "\n1. 入库任务优先 ingest_knowledge_file，不要自己假设切片数量或入库结果。"
            "\n2. 统计任务优先 get_knowledge_stats。"
            "\n3. 打标签或补元数据任务优先 update_knowledge_metadata。"
            "\n4. 带标签过滤的问答任务先 search_by_metadata，再回答。"
            "\n5. 普通知识库问答任务先 search_knowledge_base，再回答。"
            "\n6. 当 search_knowledge_base 或 search_by_metadata 返回 match_count=0 时，不要臆造答案，应明确说明未命中。"
            "\n7. 当回答使用了检索结果时，请点出 source_path 和 chunk_index 作为证据。";
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
     * @brief 知识库任务需要保留最近几轮上下文
     * @return 历史消息窗口大小
     */
    size_t history_message_limit() const override {
        return 8;
    }

    /**
     * @brief 一次请求可能涉及入库后再检索
     * @return 单次请求允许的最大工具调用轮数
     */
    int max_tool_rounds() const override {
        return 6;
    }

    /**
     * @brief 提供 Knowledge Agent 的 Agent Card
     * @return Agent Card JSON
     */
    react_json build_agent_card_json() const override {
        return {
            {"name", "Knowledge Agent"},
            {"description", "Knowledge Agent：负责文档向量化入库与基于 sqlite-vec 的知识库问答"},
            {"version", "1.0.0"},
            {"capabilities", {
                {"streaming", true},
                {"push_notifications", false},
                {"task_management", true}
            }},
            {"skills", react_json::array({
                {
                    {"name", "文档向量化入库"},
                    {"description", "根据文件路径把本地文档切片、向量化并写入 sqlite-vec"},
                    {"input_modes", react_json::array({"text"})},
                    {"output_modes", react_json::array({"text"})}
                },
                {
                    {"name", "知识库问答"},
                    {"description", "先检索知识库相关片段，再基于检索证据回答问题"},
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
    std::cerr << "示例: " << program << " knowledge-1 5005 http://localhost:8500 sk-xxx --enable-mcp --mcp-server /path/to/mcp_server" << std::endl;
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
        KnowledgeAgent agent(agent_id, listen_address, registry_url, api_key,
                             redis_host, redis_port, mcp_config);
        agent.start(port);
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
