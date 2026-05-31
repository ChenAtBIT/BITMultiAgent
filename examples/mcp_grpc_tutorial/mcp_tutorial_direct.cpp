#include "agent_rpc/mcp/mcp_client.h"

#include <json/json.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using agent_rpc::mcp::MCPClient;
using agent_rpc::mcp::MCPResponse;
using agent_rpc::mcp::MCPTool;

namespace {

// 这个文件在整套教程里的角色：
// - 通过 STDIO 启动并连接本地 MCP Server。
// - 用 list/schema 先观察服务端到底暴露了哪些工具。
// - 直接调用工具，先不引入任何 gRPC 包装层。
//
// 如果你是第一次顺着教程看代码，建议先从这个文件开始，
// 因为它展示的是最原始、最直接的 MCP 交互方式。

// 去掉首尾空白字符，避免命令行里多打的空格或换行影响解析。
std::string trim(const std::string& text) {
    const std::string whitespace = " \t\r\n";
    const auto begin = text.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return "";
    }

    const auto end = text.find_last_not_of(whitespace);
    return text.substr(begin, end - begin + 1);
}

// MCP 工具调用返回的是 JSON 字符串。
// 这里把最常见的 "content -> text" 结构提取成纯文本显示，
// 这样新手在命令行里更容易看懂；如果某个工具返回了别的结构，
// 就保留原始 JSON，方便继续排查和学习。
std::string formatToolPayload(const std::string& payload) {
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(payload, root)) {
        return payload;
    }

    std::ostringstream oss;

    if (root.isMember("isError")) {
        oss << (root["isError"].asBool() ? "[工具报错]" : "[工具成功]") << "\n";
    }

    if (root.isMember("content") && root["content"].isArray()) {
        for (const auto& item : root["content"]) {
            if (item.isMember("text")) {
                oss << item["text"].asString() << "\n";
            }
        }
    } else {
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        oss << Json::writeString(builder, root) << "\n";
    }

    return trim(oss.str());
}

// 把可用命令直接写在程序里，方便新手一边运行一边对照练习。
void printHelp(const char* program) {
    std::cout << "MCP 直连教学客户端\n\n"
              << "用法：\n"
              << "  " << program << " <mcp_server_path> [plugins_dir]\n\n"
              << "交互命令：\n"
              << "  help                查看帮助\n"
              << "  list                查看工具列表\n"
              << "  schema <tool>       查看工具参数 schema\n"
              << "  call <tool> <json>  调用工具\n"
              << "  demo                运行教学演示\n"
              << "  quit                退出\n\n"
              << "示例：\n"
              << "  call calculator {\"expression\":\"1+7\"}\n"
              << "  call echo_text {\"text\":\"hello mcp\",\"prefix\":\"[demo] \"}\n"
              << std::endl;
}

// 大多数人第一次学 MCP，最适合先看 "tools/list"。
// 这里同时打印工具名和描述，帮助你把协议返回结果和服务端实际能力对应起来。
void printTools(const std::vector<MCPTool>& tools) {
    std::cout << "\n当前可用工具（" << tools.size() << " 个）：" << std::endl;
    for (const auto& tool : tools) {
        std::cout << "  - " << tool.name << ": " << tool.description << std::endl;
    }
    std::cout << std::endl;
}

// 给 demo 和 schema 命令共用的小工具函数：按名字查找工具。
const MCPTool* findTool(const std::vector<MCPTool>& tools, const std::string& name) {
    for (const auto& tool : tools) {
        if (tool.name == name) {
            return &tool;
        }
    }
    return nullptr;
}

// 按教程节奏跑一组演示调用，故意覆盖三类场景：
// 1. 带结构化参数的内置工具（calculator）。
// 2. 带一点副作用/等待行为的工具（sleep）。
// 3. 你自己新增的自定义插件工具（echo_text）。
//
// 这和笔记里的学习顺序是一致的：先看协议，再调现成工具，
// 最后确认自定义插件也真的被加载进来了。
bool runDemo(MCPClient& client, const std::vector<MCPTool>& tools) {
    bool ran_any = false;

    if (findTool(tools, "calculator")) {
        std::cout << "\n[演示] 正在调用 calculator..." << std::endl;
        MCPResponse response = client.callTool("calculator", "{\"expression\":\"12*(3+4)\"}");
        std::cout << formatToolPayload(response.result) << "\n" << std::endl;
        ran_any = true;
    }

    if (findTool(tools, "sleep")) {
        std::cout << "[演示] 正在调用 sleep..." << std::endl;
        MCPResponse response = client.callTool("sleep", "{\"milliseconds\":50}");
        std::cout << formatToolPayload(response.result) << "\n" << std::endl;
        ran_any = true;
    }

    if (findTool(tools, "echo_text")) {
        std::cout << "[演示] 正在调用 echo_text..." << std::endl;
        MCPResponse response = client.callTool(
            "echo_text",
            "{\"text\":\"new plugin works\",\"prefix\":\"[tutorial] \"}");
        std::cout << formatToolPayload(response.result) << "\n" << std::endl;
        ran_any = true;
    }

    if (!ran_any) {
        std::cout << "没有找到教学演示需要的工具，请先构建 mcp_server_integrated。" << std::endl;
    }

    return ran_any;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp(argv[0]);
        return 1;
    }

    const std::string server_path = argv[1];
    std::vector<std::string> args;
    if (argc >= 3) {
        // 这个仓库里的集成版 MCP Server 支持 "-p <dir>" 来指定插件目录。
        // 这里把参数透传下去，就能在教程里测试自定义插件，
        // 而不用把插件路径硬编码死。
        args.push_back("-p");
        args.push_back(argv[2]);
    }

    MCPClient client;
    // 这里的 connect 会拉起一个 MCP Server 子进程，并通过 STDIO 跟它通信。
    // 对新手最重要的理解是：这一层还没有 gRPC，
    // 只有“客户端 <-> MCP Server”的直接对话。
    if (!client.connect(server_path, args)) {
        std::cerr << "连接 MCP Server 失败：" << server_path << std::endl;
        return 1;
    }

    std::cout << "已通过 STDIO 连接到 MCP Server。\n";
    std::cout << "提示：建议先输入 'list'，再输入 'demo'。\n" << std::endl;

    std::vector<MCPTool> tools = client.listTools();
    printTools(tools);

    std::string line;
    while (true) {
        std::cout << "mcp-direct> ";
        if (!std::getline(std::cin, line)) {
            std::cout << std::endl;
            break;
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line == "quit" || line == "exit") {
            break;
        }

        if (line == "help") {
            printHelp(argv[0]);
            continue;
        }

        if (line == "list") {
            // 每次都重新拉一次工具列表，这样新加载的插件或更新后的 schema
            // 都能马上反映在练习里。
            tools = client.listTools();
            printTools(tools);
            continue;
        }

        if (line == "demo") {
            // 先重新获取工具列表，确保 demo 只调用当前真的存在的工具。
            tools = client.listTools();
            runDemo(client, tools);
            continue;
        }

        if (line.rfind("schema ", 0) == 0) {
            const std::string tool_name = trim(line.substr(7));
            if (tool_name.empty()) {
                std::cout << "用法：schema <tool>" << std::endl;
                continue;
            }

            tools = client.listTools();
            const MCPTool* tool = findTool(tools, tool_name);
            if (!tool) {
                std::cout << "未找到工具：" << tool_name << std::endl;
                continue;
            }

            // schema 就是调用方必须遵守的参数契约。
            // 先把它打印出来，新手在真正执行 tools/call 之前
            // 就能知道 JSON 里应该传哪些字段。
            std::cout << "\n工具 " << tool_name << " 的 schema：\n"
                      << tool->input_schema << std::endl;
            continue;
        }

        if (line.rfind("call ", 0) == 0) {
            const std::string rest = trim(line.substr(5));
            const auto first_space = rest.find(' ');
            if (first_space == std::string::npos) {
                std::cout << "用法：call <tool> <json>" << std::endl;
                continue;
            }

            const std::string tool_name = trim(rest.substr(0, first_space));
            const std::string json_args = trim(rest.substr(first_space + 1));

            // 这里就是最直接的 MCP "tools/call"：
            // 给出工具名，再给一个符合 schema 的 JSON 参数对象。
            MCPResponse response = client.callTool(tool_name, json_args);
            if (response.is_error) {
                std::cout << "[客户端错误] " << response.error << std::endl;
            } else {
                std::cout << "\n" << formatToolPayload(response.result) << "\n" << std::endl;
            }
            continue;
        }

        std::cout << "未知命令。请输入 'help' 查看帮助。" << std::endl;
    }

    client.disconnect();
    return 0;
}
