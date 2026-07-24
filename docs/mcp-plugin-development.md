# MCP Plugin V2 开发指南

本项目的 Plugin 是加载到 MCP Server 进程中的 C++ 动态库。Plugin 只实现具体工具能力；工具发现、mode/actor 过滤、权限判断、参数校验、Session 工作区推导、输出限制和审计由 Registry/Gateway 统一处理。

## 调用链

```text
tool_orchestration.json
  -> ToolRegistry 加载并校验 Manifest
  -> tools/list 返回当前可信 actor 的完整 ToolView
  -> 模型生成 model-safe name + arguments
  -> ToolGateway 二次校验归属和 Schema
  -> Plugin HandleToolCall(stable_id, arguments, trusted_context)
  -> 输出 Schema/大小校验和脱敏审计
```

首版 `PermissionEvaluator` 固定返回 `allow`，但 mode/actor 过滤不会被跳过。配置中写入 `ask` 或 `deny` 会导致启动失败。

## V2 ABI

ABI 定义位于 `mcp_server/src/interface/PluginAPI.h`：

```cpp
#define MCP_PLUGIN_API_VERSION 2

typedef struct {
    const char* id;            // 稳定 ID：plugin_id.tool_name
    const char* name;          // 模型名：plugin_id__tool_name
    const char* description;
    const char* inputSchema;
    const char* outputSchema;
} PluginTool;

typedef struct {
    int (*GetApiVersion)();
    const char* (*GetId)();
    const char* (*GetName)();
    const char* (*GetVersion)();
    int (*Initialize)();
    void (*Shutdown)();
    int (*GetToolCount)();
    const PluginTool* (*GetTool)(int index);
    char* (*HandleToolCall)(const char* toolId,
                            const char* arguments,
                            const char* context);
    void (*FreeResult)(char* result);
} PluginAPI;
```

动态库必须导出：

```cpp
extern "C" PLUGIN_API PluginAPI* CreatePlugin();
extern "C" PLUGIN_API void DestroyPlugin(PluginAPI* api);
```

Registry 会在启动时验证所有函数指针、API 版本、Plugin ID、语义化版本、工具描述、工具命名和输入/输出 Schema。任一核心 Plugin 不合法，MCP Server 直接退出。

## 命名规则

假设 Plugin ID 为 `example_tools`，工具局部名为 `echo`：

- 稳定内部 ID：`example_tools.echo`
- 注入模型的函数名：`example_tools__echo`
- 动态库文件：Linux 为 `example_tools.so`
- 配置映射使用 Plugin ID：`example_tools`

Plugin ID、Tool ID 和模型函数名必须全局唯一。不要在模型参数里加入 `session_id`、`actor_kind`、`workspace_root` 等可信字段。

## 最小实现

```cpp
#include "PluginAPI.h"
#include "PluginSupport.h"

#include <string>

using vx::plugin::json;

namespace {

constexpr const char* output_schema = R"({
  "type": "object",
  "properties": {"ok": {"type": "boolean"}},
  "required": ["ok"]
})";

PluginTool tools[] = {
    {
        "example_tools.echo",
        "example_tools__echo",
        "Return one short text value.",
        R"({
          "type": "object",
          "properties": {
            "text": {"type": "string", "minLength": 1, "maxLength": 1000}
          },
          "required": ["text"],
          "additionalProperties": false
        })",
        output_schema
    }
};

char* handle(const char* tool_id, const char* raw_arguments, const char* raw_context) {
    try {
        const json arguments = json::parse(raw_arguments ? raw_arguments : "{}");
        const json context = json::parse(raw_context ? raw_context : "{}");
        const std::string id = tool_id ? tool_id : "";

        if (id == "example_tools.echo") {
            return vx::plugin::copy_result(vx::plugin::success({
                {"text", arguments.at("text")},
                {"operation_id", context.at("operation_id")}
            }));
        }
        return vx::plugin::copy_result(
            vx::plugin::failure("UNKNOWN_TOOL", "unknown example_tools tool"));
    } catch (const std::exception& error) {
        return vx::plugin::copy_result(
            vx::plugin::failure("PLUGIN_ERROR", error.what()));
    }
}

int api_version() { return MCP_PLUGIN_API_VERSION; }
const char* plugin_id() { return "example_tools"; }
const char* plugin_name() { return "Example Tools"; }
const char* plugin_version() { return "1.0.0"; }
int initialize() { return 1; }
void shutdown() {}
int tool_count() { return static_cast<int>(sizeof(tools) / sizeof(tools[0])); }
const PluginTool* get_tool(int index) {
    return index >= 0 && index < tool_count() ? &tools[index] : nullptr;
}

}  // namespace

extern "C" PLUGIN_API PluginAPI* CreatePlugin() {
    return new PluginAPI{api_version, plugin_id, plugin_name, plugin_version,
                         initialize, shutdown, tool_count, get_tool,
                         handle, vx::plugin::free_result};
}

extern "C" PLUGIN_API void DestroyPlugin(PluginAPI* api) { delete api; }
```

`HandleToolCall` 返回的内存属于 Plugin。必须提供匹配的 `FreeResult`，宿主不会使用 `free()` 或 `delete[]` 猜测分配方式。

## 返回约定

成功：

```json
{
  "ok": true,
  "result": {"value": "..."}
}
```

失败：

```json
{
  "ok": false,
  "error": {
    "code": "DOMAIN_ERROR",
    "message": "human-readable message"
  }
}
```

可以直接使用 `PluginSupport.h` 中的 `success`、`failure`、`copy_result` 和 `free_result`。Plugin 应捕获异常并返回确定的结构化错误，不得返回未初始化数据。

Gateway 将限制原始结果为 2 MiB并按 `outputSchema` 再校验。大正文工具应设置更低的领域上限，例如 `web_research` 的网页响应上限为 2 MiB、文本输出上限为 100000 个 UTF-8 字符。

## 可信执行上下文

Plugin 收到的 `context` 由宿主构造：

```json
{
  "session_id": "32位十六进制ID",
  "operation_id": "当前设计、计划或节点操作ID",
  "mode_id": "dag_team",
  "actor_kind": "designer | planner | executor",
  "actor_id": "可信Actor ID",
  "workspace_root": "/server/derived/session/workspace",
  "trusted_data": {}
}
```

模型只能提供 Tool name 和 `arguments`，不能提供或覆盖这些字段。`workspace_root` 始终由 MCP Server 使用 `session_id` 推导。

需要草稿隔离的 Plugin 可以使用：

```cpp
const std::string key = vx::plugin::state_key(context);
// key = session_id + ":" + operation_id
```

`team_design` 和 `dag_control` 都按这个键隔离状态。若 Plugin 保存进程内状态，必须用互斥锁保护，因为 MCP Server 会通过固定 worker pool 并发调用不同工具。

## 文件工具规则

涉及 Session 文件时，优先扩展现有 `workspace_fs`，不要在其他 Plugin 重复实现路径解析。安全要求包括：

- 参数只接受相对路径；
- 拒绝绝对路径、`..`、空字节和根目录操作；
- 不跟随任意路径组件中的软链接；
- 覆盖写使用同目录临时文件和原子重命名；
- 检查单文件和 Session 总配额；
- 递归删除必须由显式参数触发。

不要接受模型传入的宿主路径，也不要自行拼接项目根目录。

## CMake

创建 `mcp_server/plugins/example_tools/CMakeLists.txt`：

```cmake
add_library(example_tools SHARED ExampleTools.cpp)
set_target_properties(example_tools PROPERTIES
    PREFIX ""
    OUTPUT_NAME "example_tools"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/mcp_server/plugins"
)
target_include_directories(example_tools PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src/interface
)
```

然后在 `mcp_server/CMakeLists.txt` 中加入：

```cmake
add_subdirectory(plugins/example_tools)
```

## 配置 Actor 可见性

在 `config/tool_orchestration.json` 的目标 Actor 列表中加入 Plugin ID。例如仅允许 Planner 使用：

```json
{
  "schema_version": 1,
  "modes": {
    "dag_team": {
      "actors": {
        "designer": ["team_design", "workspace_fs", "web_research"],
        "planner": ["dag_control", "example_tools", "workspace_fs", "web_research"],
        "executor": ["workspace_fs", "web_research"]
      }
    }
  },
  "permission": "allow"
}
```

候选工具严格来自这份映射。不要增加语义检索、RAG 或 Top-K 选择层。

## 测试清单

新增 Plugin 至少应覆盖：

- Manifest、版本、Tool ID/name 和 Schema 可被 Registry 加载；
- 未授权 actor 在 `tools/list` 中看不到并且 `tools/call` 无法执行；
- 缺字段、错误类型、额外字段和领域非法参数返回结构化错误；
- 并发调用不会串状态；
- 输出满足 Schema 和大小限制；
- 审计不包含正文或密钥；
- 若涉及文件，覆盖遍历、软链接、配额、原子覆盖和递归操作；
- 若涉及网络，使用本地 fixture 覆盖重定向、状态码、超时、编码和大小上限。

构建和运行：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target example_tools mcp_server -j$(nproc)
ctest --test-dir build --output-on-failure
```
