#include "agent_rpc/mcp/mcp_agent_integration.h"
#include "ai_query.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using agent_rpc::mcp::MCPAgentConfig;
using agent_rpc::mcp::MCPAgentIntegration;
using agent_rpc::mcp::ToolCallResult;
using agent_rpc::mcp::ToolInfo;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;

namespace {

// 这个文件在整套教程里的角色：
// - 暴露已经存在的 AIQuery gRPC 服务接口。
// - 把收到的文本命令翻译成 MCP 工具调用。
// - 再把 MCP 的结果包装成普通 gRPC 响应或流式响应返回出去。
//
// 换句话说，这个文件就是 “gRPC 世界” 和 “MCP 世界” 之间的适配层。

// 用全局状态配合信号处理，便于在示例程序里用 Ctrl+C 干净地停掉 gRPC 服务。
std::atomic<bool> g_running{true};
std::unique_ptr<Server> g_server;

// 给 "/schema ..."、"/call ..." 这类命令共用的去空白辅助函数。
std::string trim(const std::string& text) {
    const std::string whitespace = " \t\r\n";
    const auto begin = text.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return "";
    }

    const auto end = text.find_last_not_of(whitespace);
    return text.substr(begin, end - begin + 1);
}

// 把 MCP 工具列表整理成一段可读文本，方便直接塞进 gRPC 返回值里。
std::string joinToolNames(const std::vector<ToolInfo>& tools) {
    std::ostringstream oss;
    for (const auto& tool : tools) {
        oss << "- " << tool.name << ": " << tool.description << "\n";
    }
    return trim(oss.str());
}

// MCP 集成层返回的是 JSON 字符串。
// 和直连版教程客户端一样，这里把最常见的 "content" 数组提取成纯文本，
// 这样 gRPC 调用方看到的是更容易理解的答案，而不是一整段协议 JSON。
std::string formatToolResponse(const ToolCallResult& result) {
    if (!result.success) {
        return "[工具报错] " + result.error;
    }

    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(result.result, root)) {
        return result.result;
    }

    std::ostringstream oss;
    if (root.isMember("content") && root["content"].isArray()) {
        for (const auto& item : root["content"]) {
            if (item.isMember("text")) {
                oss << item["text"].asString() << "\n";
            }
        }
    } else {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        oss << Json::writeString(builder, root);
    }

    return trim(oss.str());
}

// 收到 Ctrl+C 或终止信号时，同时停止外层循环和 gRPC 服务本身。
void signalHandler(int) {
    g_running = false;
    if (g_server) {
        g_server->Shutdown();
    }
}

// 一个很小的结果包装类型。
// 这样命令解析阶段可以区分“业务上成功/失败”，而不必把教学用的命令错误
// 直接上升成 gRPC 传输层错误。
struct TutorialReply {
    bool ok = true;
    std::string text;
};

// 这个服务类就是教程里最核心的 “gRPC -> MCP” 桥接器：
// 1. gRPC 客户端发来一条文本命令。
// 2. 这个服务负责解释这条命令。
// 3. 服务再通过 MCPAgentIntegration 去调用真正的 MCP 工具。
// 4. 最终结果被包装成普通 gRPC 响应或流式事件返回给客户端。
class TutorialMCPService final : public agent_communication::AIQueryService::Service {
public:
    explicit TutorialMCPService(const MCPAgentConfig& config) {
        // initialize() 会启动/连接配置好的 MCP Server，并读取工具目录。
        // isAvailable() 则进一步确认子进程真的已经可以处理工具调用了。
        initialized_ = integration_.initialize(config);
        ready_ = initialized_ && integration_.isAvailable();
    }

    bool isReady() const {
        return ready_;
    }

    std::string statusText() const {
        return integration_.getStatusDescription();
    }

    Status Query(ServerContext*,
                 const agent_communication::AIQueryRequest* request,
                 agent_communication::AIQueryResponse* response) override {
        if (!ready_) {
            return Status(grpc::StatusCode::UNAVAILABLE, "MCP 教学服务尚未就绪");
        }

        // 记录一次完整处理耗时，让示例响应更接近真实 unary gRPC 接口，
        // 而不只是回一段答案文本。
        const auto started = std::chrono::steady_clock::now();
        TutorialReply reply = handleQuestion(request->question());
        const auto ended = std::chrono::steady_clock::now();

        response->set_request_id(request->request_id());
        response->set_context_id(request->context_id());
        response->set_answer(reply.text);
        response->set_agent_name("grpc-mcp-tutorial");
        response->set_processing_time_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(ended - started).count());

        auto* status = response->mutable_status();
        status->set_code(reply.ok ? 0 : 1);
        status->set_message(reply.ok ? "OK" : "COMMAND_ERROR");
        return Status::OK;
    }

    Status QueryStream(ServerContext* context,
                       const agent_communication::AIQueryRequest* request,
                       ServerWriter<agent_communication::AIStreamEvent>* writer) override {
        if (!ready_) {
            agent_communication::AIStreamEvent error_event;
            error_event.set_event_type("error");
            error_event.set_content("MCP 教学服务尚未就绪");
            writer->Write(error_event);
            return Status(grpc::StatusCode::UNAVAILABLE, "MCP 教学服务尚未就绪");
        }

        // 为了教学简单，流式接口和普通接口共用同一套命令解析逻辑，
        // 只是把最终文本拆成多个块发出去。
        // 这样你能清楚看到：同一个 MCP 工具调用，也可以包在流式 gRPC 里返回。
        TutorialReply reply = handleQuestion(request->question());

        agent_communication::AIStreamEvent start_event;
        start_event.set_event_type("status");
        start_event.set_task_state("processing");
        start_event.set_context_id(request->context_id());
        writer->Write(start_event);

        const std::string payload = reply.text;
        constexpr std::size_t chunk_size = 48;
        for (std::size_t pos = 0; pos < payload.size(); pos += chunk_size) {
            if (context->IsCancelled()) {
                return Status(grpc::StatusCode::CANCELLED, "客户端已取消请求");
            }

            // "partial" 事件用来演示“边生成边返回”这种流式体验。
            // 在真实 AI 系统里，这些块可能是逐步生成的；
            // 这里为了方便理解，只是把已经得到的完整文本切块发送。
            agent_communication::AIStreamEvent chunk_event;
            chunk_event.set_event_type("partial");
            chunk_event.set_context_id(request->context_id());
            chunk_event.set_content(payload.substr(pos, chunk_size));
            writer->Write(chunk_event);
        }

        // 最后的 "complete" 事件同时带上完整答案和结束状态。
        // 教程客户端那边会避免把 partial 已经拼好的内容再重复显示一遍。
        agent_communication::AIStreamEvent done_event;
        done_event.set_event_type("complete");
        done_event.set_task_state(reply.ok ? "completed" : "failed");
        done_event.set_context_id(request->context_id());
        done_event.set_content(payload);
        writer->Write(done_event);
        return Status::OK;
    }

    Status GetQueryStatus(ServerContext*,
                          const agent_communication::QueryStatusRequest*,
                          agent_communication::QueryStatusResponse* response) override {
        // 这个演示本身是无状态的，没有后台任务表可查。
        // 但还是把这个 RPC 实现出来，方便你看到 proto 里定义的完整服务面。
        auto* status = response->mutable_status();
        status->set_code(0);
        status->set_message("OK");
        response->set_task_state("stateless-demo");
        return Status::OK;
    }

private:
    // 把教程里的人类可读命令，翻译成具体的 MCP 操作。
    // 如果你想抓住 “gRPC 请求到底在哪一步变成 MCP 工具调用”，
    // 这个函数就是最值得细看的地方。
    TutorialReply handleQuestion(const std::string& question) {
        if (question == "/help") {
            return {true,
                    "可用命令：\n"
                    "/help               查看帮助\n"
                    "/tools              查看 MCP 工具列表\n"
                    "/schema <tool>      查看工具 schema\n"
                    "/call <tool> <json> 直接调用工具\n"
                    "calc <expression>   快速调用 calculator\n"
                    "echo <text>         快速调用 echo_text\n"};
        }

        if (question == "/tools") {
            // 语义上等价于 MCP 的 "tools/list"，
            // 只是这里返回纯文本，避免 gRPC 客户端再去理解 MCP 协议细节。
            const auto tools = integration_.getAvailableTools();
            return {true, "当前可用 MCP 工具：\n" + joinToolNames(tools)};
        }

        if (question.rfind("/schema ", 0) == 0) {
            const std::string tool_name = trim(question.substr(8));
            // schema 很关键，因为它定义了后面 "/call" 时必须遵守的 JSON 参数契约。
            const std::string schema = integration_.getToolInputSchema(tool_name);
            if (schema.empty()) {
                return {false, "未找到工具：" + tool_name};
            }
            return {true, "工具 " + tool_name + " 的 schema：\n" + schema};
        }

        if (question.rfind("/call ", 0) == 0) {
            const std::string rest = trim(question.substr(6));
            const auto split = rest.find(' ');
            if (split == std::string::npos) {
                return {false, "用法：/call <tool> <json>"};
            }

            const std::string tool_name = trim(rest.substr(0, split));
            const std::string json_args = trim(rest.substr(split + 1));
            // 这是桥接层最“原样透传”的形式：
            // gRPC 收到一条文本命令后，直接把“工具名 + JSON 参数”
            // 转交给 MCP 集成层。
            ToolCallResult result = integration_.callTool(tool_name, json_args);
            return {result.success, formatToolResponse(result)};
        }

        if (question.rfind("calc ", 0) == 0) {
            // 这是一个教学上的便捷命令：
            // 用户只要输入 "calc 1+7"，桥接层就帮他组装出
            // calculator 工具真正需要的 JSON 参数对象。
            Json::Value args;
            args["expression"] = question.substr(5);
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            ToolCallResult result = integration_.callTool(
                "calculator",
                Json::writeString(builder, args));
            return {result.success, formatToolResponse(result)};
        }

        if (question.rfind("echo ", 0) == 0) {
            // 这里演示的是“调用自定义插件工具”，而不只是内置工具。
            // 固定加上 "[grpc] " 前缀，能帮助你在联调时一眼看出
            // 这条结果是经过 gRPC 桥接层返回的。
            Json::Value args;
            args["text"] = question.substr(5);
            args["prefix"] = "[grpc] ";
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            ToolCallResult result = integration_.callTool(
                "echo_text",
                Json::writeString(builder, args));
            return {result.success, formatToolResponse(result)};
        }

        return {false,
                "无法识别的教学命令。\n"
                "可以试试：/help、/tools、calc 1+7、echo hello，"
                "或者 /call calculator {\"expression\":\"1+7\"}。"};
    }

    MCPAgentIntegration integration_;
    bool initialized_ = false;
    bool ready_ = false;
};

void printUsage(const char* program) {
    std::cout << "用法：\n"
              << "  " << program << " <mcp_server_path> [grpc_port] [plugins_dir]\n\n"
              << "示例：\n"
              << "  " << program << " ./build/mcp_server/mcp_server\n"
              << "  " << program << " ./build/mcp_server/mcp_server 50071 ./build/mcp_server/plugins\n"
              << std::endl;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string mcp_server_path = argv[1];
    const std::string grpc_port = argc >= 3 ? argv[2] : "50071";

    MCPAgentConfig config;
    // 这组配置告诉桥接层：底层要真的拉起/连接一个 MCP Server，
    // 然后把工具调用继续转发给它。
    config.enable_mcp = true;
    config.mcp_server_path = mcp_server_path;
    config.tool_call_timeout_ms = 30000;

    if (argc >= 4) {
        // 把可选插件目录继续透传给底层 MCP Server，
        // 这样教程里的 tutorial_echo 这类自定义工具才能被发现。
        config.mcp_args.push_back("-p");
        config.mcp_args.push_back(argv[3]);
    }

    TutorialMCPService service(config);
    if (!service.isReady()) {
        std::cerr << "启动教学 MCP 服务失败：" << service.statusText() << std::endl;
        return 1;
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    ServerBuilder builder;
    // 这里只绑定一个本地演示用的明文 gRPC 服务。
    // 目标是先学清协议流转，所以故意不引入证书和 TLS 这些额外复杂度。
    builder.AddListeningPort("0.0.0.0:" + grpc_port, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    g_server = builder.BuildAndStart();

    if (!g_server) {
        std::cerr << "启动 gRPC 服务失败，端口：" << grpc_port << std::endl;
        return 1;
    }

    std::cout << "==========================================" << std::endl;
    std::cout << "gRPC -> MCP 教学服务已启动" << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "gRPC 地址：localhost:" << grpc_port << std::endl;
    std::cout << "MCP 状态：  " << service.statusText() << std::endl;
    std::cout << "\n可以在教学客户端里试试这些命令：" << std::endl;
    std::cout << "  /help" << std::endl;
    std::cout << "  /tools" << std::endl;
    std::cout << "  calc 1+7" << std::endl;
    std::cout << "  /call calculator {\"expression\":\"2^10\"}" << std::endl;
    std::cout << "  echo hello grpc mcp" << std::endl;
    std::cout << "==========================================" << std::endl;

    while (g_running) {
        // 主线程这里只是等待退出信号。
        // 真正处理请求的工作，在 BuildAndStart() 之后已经交给 gRPC 内部线程池了。
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (g_server) {
        g_server->Shutdown();
    }
    return 0;
}
