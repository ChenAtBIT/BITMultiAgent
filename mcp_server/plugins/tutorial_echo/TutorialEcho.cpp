#include "PluginAPI.h"
#include "json.hpp"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

namespace {

// 这个文件在整套教程里的角色：
// - 实现一个尽量小、但真正可用的自定义 MCP 工具插件。
// - 展示工具元数据和 JSON Schema 是怎么注册给宿主的。
// - 展示工具请求是怎么被解析并转换成 MCP 风格响应的。

// 这组元数据决定了宿主如何发现这个插件暴露了哪些工具。
// 每个条目都会出现在 "tools/list" 结果里，而 inputSchema
// 就是调用方执行 "tools/call" 时必须遵守的参数契约。
static PluginTool tools[] = {
    {
        "echo_text",
        "回显一段文本，并可选在前面加上前缀。适合用来学习自定义 MCP 工具的工作方式。",
        R"({
            "type": "object",
            "properties": {
                "text": {
                    "type": "string",
                    "description": "要回显的主要文本。"
                },
                "prefix": {
                    "type": "string",
                    "description": "可选前缀，会加在主要文本前面。"
                }
            },
            "required": ["text"]
        })"
    },
    {
        "repeat_text",
        "把一段文本重复多次。适合用来测试整数参数的传递和校验。",
        R"({
            "type": "object",
            "properties": {
                "text": {
                    "type": "string",
                    "description": "要重复的文本。"
                },
                "count": {
                    "type": "integer",
                    "minimum": 1,
                    "maximum": 10,
                    "description": "文本需要重复多少次。"
                }
            },
            "required": ["text", "count"]
        })"
    }
};

// 插件 API 要求 HandleRequest() 返回一个堆上分配的 C 字符串。
// 宿主后面会用 delete[] 释放这块内存，所以这里也必须用同样的方式分配，
// 再把 JSON 响应序列化进去。
char* makeResponse(const json& payload) {
    const std::string result = payload.dump();
    char* buffer = new char[result.size() + 1];
#ifdef _WIN32
    strcpy_s(buffer, result.size() + 1, result.c_str());
#else
    std::strcpy(buffer, result.c_str());
#endif
    return buffer;
}

// MCP 工具结果通常会包在 "content" 数组里。
// 这里也沿用同样的结构，便于不同 MCP Client 复用统一返回格式
// 都可以用同一套格式化逻辑直接显示结果。
json makeTextResult(const std::string& text) {
    json response;
    response["content"] = json::array();
    response["content"].push_back({
        {"type", "text"},
        {"text", text}
    });
    response["isError"] = false;
    return response;
}

// 错误响应故意和成功响应保持同样的外层结构，
// 这样客户端只需要看 "isError" 和文本内容就能统一处理。
json makeErrorResult(const std::string& text) {
    json response;
    response["content"] = json::array();
    response["content"].push_back({
        {"type", "text"},
        {"text", "错误：" + text}
    });
    response["isError"] = true;
    return response;
}

// 最简单的自定义工具逻辑：
// 读取必填字段 text，可选读取 prefix，最后拼出返回字符串。
std::string handleEchoText(const json& arguments) {
    const std::string text = arguments.at("text").get<std::string>();
    const std::string prefix = arguments.value("prefix", "");
    return prefix + text;
}

// 一个稍微复杂一点的例子，用来演示 schema 校验之外的业务校验。
// schema 只说明了 count 是整数；这里再额外补一层运行时范围检查，
// 让学习者看到“通用参数校验”和“具体业务规则”分别该写在哪。
std::string handleRepeatText(const json& arguments) {
    const std::string text = arguments.at("text").get<std::string>();
    const int count = arguments.at("count").get<int>();

    if (count < 1 || count > 10) {
        throw std::runtime_error("count 必须在 1 到 10 之间");
    }

    std::ostringstream oss;
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            oss << " | ";
        }
        oss << text;
    }
    return oss.str();
}

}  // namespace

const char* GetNameImpl() {
    // 插件加载时，服务端日志里会显示这个名字。
    return "tutorial-echo-tools";
}

const char* GetVersionImpl() {
    return "1.0.0";
}

PluginType GetTypeImpl() {
    // 这个教程插件提供的是 tools，不是 prompts 或 resources。
    return PLUGIN_TYPE_TOOLS;
}

int InitializeImpl() {
    // 这个宿主接口有个需要特别注意的小约定：返回非 0 才表示初始化成功。
    // 所以这里返回 1，告诉插件加载器“我已经准备好了”。
    return 1;
}

char* HandleRequestImpl(const char* req) {
    try {
        // 宿主传进来的是一个类似 JSON-RPC 的请求对象。
        // 对工具调用来说，我们最关心两部分：
        // - params.name：到底要执行哪个工具
        // - params.arguments：这个工具的 JSON 参数对象
        const json request = json::parse(req);
        const std::string tool_name = request.at("params").at("name").get<std::string>();
        const json arguments = request.at("params").value("arguments", json::object());

        if (tool_name == "echo_text") {
            return makeResponse(makeTextResult(handleEchoText(arguments)));
        }

        if (tool_name == "repeat_text") {
            return makeResponse(makeTextResult(handleRepeatText(arguments)));
        }

        return makeResponse(makeErrorResult("未知工具：" + tool_name));
    } catch (const std::exception& ex) {
        return makeResponse(makeErrorResult(ex.what()));
    }
}

void ShutdownImpl() {
    // 这个教程插件没有持久化资源或后台线程，因此无需额外清理。
}

int GetToolCountImpl() {
    return static_cast<int>(sizeof(tools) / sizeof(tools[0]));
}

const PluginTool* GetToolImpl(int index) {
    // 宿主在做工具发现时，会按下标一个个枚举工具。
    if (index < 0 || index >= GetToolCountImpl()) {
        return nullptr;
    }
    return &tools[index];
}

// 这是导出给宿主的函数表。
// 插件加载器会先通过 CreatePlugin() 拿到这张表，
// 然后在插件整个生命周期里都通过这些函数指针和插件交互。
static PluginAPI plugin = {
    GetNameImpl,
    GetVersionImpl,
    GetTypeImpl,
    InitializeImpl,
    HandleRequestImpl,
    ShutdownImpl,
    GetToolCountImpl,
    GetToolImpl,
    nullptr,
    nullptr,
    nullptr,
    nullptr
};

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    // 这是插件加载器要求必须导出的符号之一。
    return &plugin;
}

extern "C" PLUGIN_API void DestroyPlugin(PluginAPI*) {
    // 这里没有动态创建插件对象，函数表本身又是静态存储，所以无需销毁。
}
