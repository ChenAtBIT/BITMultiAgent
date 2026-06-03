# MCP-Server 篇新手笔记

这份笔记不是讲抽象概念，而是专门围绕这个仓库，带你把下面三件事连起来：

1. `gRPC` 是怎么收请求的。
2. `MCP Server` 是怎么列工具、调工具的。
3. 你怎么自己加一个 MCP 工具插件，并通过 gRPC 间接调到它。

---

## 1. 先把脑图搭起来

这个仓库里，最适合新手先记住的主链路是：

```text
gRPC Client
  -> gRPC Server
    -> MCPAgentIntegration
      -> MCPToolManager
        -> MCPClient
          -> mcp_server (STDIO 子进程)
            -> PluginAPI
              -> 某个插件的 HandleRequest()
```

如果你先记住这条线，后面看代码就不容易乱。

---

## 2. 你应该按什么顺序看代码

### 第一层：先看“业务入口”

先看这些文件，理解“请求从哪里来”：

- `examples/mcp_grpc_tutorial/grpc_mcp_tutorial_client.cpp`
- `examples/mcp_grpc_tutorial/grpc_mcp_tutorial_server.cpp`
- `proto/ai_query.proto`

为什么先看它们：

- `proto/ai_query.proto` 告诉你 gRPC 的请求和响应长什么样。
- `grpc_mcp_tutorial_client.cpp` 告诉你客户端怎么发 `Query()` / `QueryStream()`。
- `grpc_mcp_tutorial_server.cpp` 告诉你服务端收到请求后，什么时候去调 MCP。

### 第二层：看“Agent 如何用 MCP”

再看：

- `mcp/include/agent_rpc/mcp/mcp_agent_integration.h`
- `mcp/src/mcp_agent_integration.cpp`

重点函数：

- `initialize()`
- `getAvailableTools()`
- `getToolInputSchema()`
- `callTool()`

这一层是“给业务代码用的舒服封装”。  
你可以把它理解为：业务层一般不直接自己拼 JSON-RPC，而是先调它。

### 第三层：看“真正发请求的人”

再看：

- `mcp/include/agent_rpc/mcp/mcp_client.h`
- `mcp/src/mcp_client.cpp`

重点函数：

- `connect()`
- `startMCPServer()`
- `listTools()`
- `callTool()`
- `buildJSONRPCRequest()`
- `parseJSONRPCResponse()`

这里非常关键，因为你会看到：

- 默认不是 HTTP，而是 `STDIO`。
- `MCPClient` 会 `fork + execv` 启动 `mcp_server`。
- 然后通过管道给它写一行 JSON。

### 第四层：看“Server 怎么把请求分给插件”

再看：

- `mcp_server_integrated/src/main.cpp`
- `mcp_server_integrated/src/server/Server.cpp`
- `mcp_server_integrated/src/interface/PluginAPI.h`
- `mcp_server_integrated/src/loader/PluginsLoader.cpp`

重点理解：

- `OverrideCallback("tools/list", ...)`
- `OverrideCallback("tools/call", ...)`
- `PluginsLoader::LoadPlugin()`
- `PluginAPI`

这一步看懂后，你就会明白：

- `mcp_server` 只是“插件宿主 + 方法分发器”。
- 真正干活的是插件里的 `HandleRequest()`。

### 第五层：最后看插件本体

建议顺序：

1. `mcp_server_integrated/plugins/calculator/Calculator.cpp`
2. `mcp_server_integrated/plugins/tutorial_echo/TutorialEcho.cpp`
3. `mcp_server_integrated/plugins/notification/Notification.cpp`

原因：

- `calculator` 最直观。
- `tutorial_echo` 是最适合仿写的最小插件。
- `notification` 再帮助你理解 MCP 的通知能力。

---

## 3. 这次我给你新增了什么教程代码

### A. 直连 MCP 的最小示例

文件：

- `examples/mcp_grpc_tutorial/mcp_tutorial_direct.cpp`

作用：

- 不经过 gRPC。
- 直接用 `MCPClient` 连本地 `mcp_server`。
- 测试 `tools/list`、`schema <tool>`、`call <tool> <json>`。

这一步最适合先练“工具发现”和“工具调用”。

### B. gRPC 包住 MCP 的教程服务端

文件：

- `examples/mcp_grpc_tutorial/grpc_mcp_tutorial_server.cpp`

作用：

- 对外提供 gRPC 的 `AIQueryService`。
- 内部用 `MCPAgentIntegration` 调 MCP 工具。
- 你可以把它看成“gRPC 到 MCP 的桥”。

支持这些命令：

```text
/help
/tools
/schema <tool>
/call <tool> <json>
calc <expression>
echo <text>
```

### C. gRPC 教程客户端

文件：

- `examples/mcp_grpc_tutorial/grpc_mcp_tutorial_client.cpp`

作用：

- 连 `grpc_mcp_tutorial_server`
- 发同步或流式请求
- 帮你观察 gRPC 层是怎么把命令传给 MCP 的

### D. 最小自定义插件

文件：

- `mcp_server_integrated/plugins/tutorial_echo/TutorialEcho.cpp`

这个插件提供两个工具：

- `echo_text`
- `repeat_text`

它的目的不是复杂功能，而是帮你看清楚：

- 工具定义写在哪
- JSON Schema 怎么写
- `HandleRequest()` 怎么读参数
- 返回值必须是什么形状

---

## 4. 先编译哪些东西

你需要编译两部分：

### 4.1 编译主项目

```bash
# 等价于 mkdir -p build && cd build && cmake ..
cmake -S . -B build
# 等价于 cd build && make -j$(nproc)
cmake --build build -j$(nproc)
```

### 4.2 编译 MCP Server 和插件

```bash
cmake -S . -B build
cmake --build build --target mcp_server -j$(nproc)
cmake --build build --target mcp_tutorial_direct -j$(nproc)
```

编译完成后，重点看这些文件是否存在：

```bash
./build/examples/mcp_grpc_tutorial/mcp_tutorial_direct
./build/examples/mcp_grpc_tutorial/grpc_mcp_tutorial_server
./build/examples/mcp_grpc_tutorial/grpc_mcp_tutorial_client
./build/mcp_server_integrated/mcp_server
./build/mcp_server_integrated/plugins/tutorial_echo/libtutorial_echo.so
```

---

## 5. 第一个渐进练习：先别碰 gRPC，只直连 MCP

### 5.1 启动直连示例

```bash
./build/examples/mcp_grpc_tutorial/mcp_tutorial_direct \
  ./build/mcp_server_integrated/mcp_server \
  ./build/mcp_server_integrated/plugins
```

### 5.2 你应该按这个顺序试

先输入：

```text
list
```

你应该看到类似：

```text
- calculator
- add
- subtract
- multiply
- divide
- power
- sqrt
- factorial
- sleep
- echo_text
- repeat_text
```

再输入：

```text
schema calculator
```

它会显示这个工具需要什么参数。

然后试一次真实调用：

```text
call calculator {"expression":"1+7"}
```

再试你新增的教程插件：

```text
call echo_text {"text":"hello mcp","prefix":"[demo] "}
```

再试一个带整数参数的工具：

```text
call repeat_text {"text":"go","count":3}
```

### 5.3 这一关你在学什么

你其实在学 3 件事：

1. `tools/list` 返回的是“工具目录”。
2. `tools/call` 需要你自己给出工具名和 JSON 参数。
3. 参数格式不是随便写的，必须符合工具的 `inputSchema`。

---

## 6. 第二个渐进练习：再看 gRPC 怎么包住 MCP

### 6.1 先启动教程服务端

```bash
./build/examples/mcp_grpc_tutorial/grpc_mcp_tutorial_server \
  ./build/mcp_server_integrated/mcp_server \
  50071 \
  ./build/mcp_server_integrated/plugins
```

### 6.2 再启动教程客户端

```bash
./build/examples/mcp_grpc_tutorial/grpc_mcp_tutorial_client localhost:50071
```

### 6.3 先跑这几组命令

先看帮助：

```text
/help
```

再列出 MCP 工具：

```text
/tools
```

看工具参数：

```text
/schema calculator
/schema echo_text
```

调用数学工具：

```text
calc 1+7
/call calculator {"expression":"12*(3+4)"}
```

调用你自己加的教程工具：

```text
echo hello grpc mcp
/call echo_text {"text":"hello","prefix":"[from grpc] "}
```

### 6.4 你现在应该怎么理解 gRPC 和 MCP 的关系

这里最重要的是分清两层：

```text
非本地 gRPC Client -> AIQueryService(Query/QueryStream) -> TutorialMCPService -> MCPAgentIntegration -> MCPClient -> JSON-RPC -> mcp_server -> plugin

gRPC Client
  -> gRPC transport：HTTP/2 + Protobuf + TCP/TLS
  -> 桥接层：grpc_mcp_tutorial_server / AIQueryService
  -> MCPClient
  -> MCP transport：Streamable HTTP / STDIO
  -> mcp_server
```



#### gRPC 层

- 客户端调的是 `AIQueryService::Query()`
- 服务端收的是 `AIQueryRequest`
- 这是“应用之间通信”的协议

#### MCP 层

- 服务端内部再去调 `MCPAgentIntegration::callTool()`
- 之后变成 `tools/call`
- 最后落到某个插件的 `HandleRequest()`

也就是说：

> gRPC 是“你的应用怎么和服务端讲话”，MCP 是“服务端内部怎么和工具系统讲话”。

---

## 7. 第三个渐进练习：你怎么自己加一个 MCP 工具

这一步最重要。

### 7.1 你必须遵守的规矩

写 MCP 插件时，最容易踩坑的是这些规则：

#### 规则 1：必须导出 `CreatePlugin` 和 `DestroyPlugin`

因为 `PluginsLoader` 会用动态加载的方式找这两个符号。

#### 规则 2：必须返回 `PluginAPI`

`mcp_server` 不认识你的 C++ 类，只认这一组函数指针：

- `GetName`
- `GetVersion`
- `GetType`
- `Initialize`
- `HandleRequest`
- `Shutdown`
- `GetToolCount`
- `GetTool`

#### 规则 3：工具清单必须通过 `PluginTool[]` 提供

比如：

```cpp
static PluginTool tools[] = {
    {
        "echo_text",
        "Echo a text with an optional prefix.",
        "{\"type\":\"object\", ... }"
    }
};
```

这里的第三个字段就是 JSON Schema。

#### 规则 4：`HandleRequest()` 里要自己解析 JSON

你会收到整条请求，比如：

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "echo_text",
    "arguments": {
      "text": "hello"
    }
  }
}
```

你通常会这么读：

```cpp
auto request = json::parse(req);
std::string tool_name = request["params"]["name"].get<std::string>();
auto args = request["params"]["arguments"];
```

#### 规则 5：返回值必须是 JSON 字符串

而且推荐遵循这个结构：

```json
{
  "content": [
    {
      "type": "text",
      "text": "your result"
    }
  ],
  "isError": false
}
```

#### 规则 6：返回的 `char*` 要自己分配，并且用 `new[]`

这是这个仓库里一个很关键的实现细节。  
因为 `mcp_server_integrated/src/main.cpp` 在收到插件返回值后会：

```cpp
delete[] res_ptr;
```

所以你要这样返回：

```cpp
std::string result = response.dump();
char* buffer = new char[result.length() + 1];
strcpy(buffer, result.c_str());
return buffer;
```

#### 规则 7：`Initialize()` 在这个仓库里要返回非 0 表示成功

注意这和很多人习惯的 “0 表示成功” 不一样。  
因为加载器里写的是：

```cpp
if (!entry.instance->Initialize()) {
    // 失败
}
```

所以这里：

- 返回 `1` = 成功
- 返回 `0` = 失败

这是新手最容易踩的坑之一。

---

## 8. 你新增的教程插件是怎么写的

插件文件：

- `mcp_server_integrated/plugins/tutorial_echo/TutorialEcho.cpp`

它提供两个工具：

### `echo_text`

参数：

```json
{
  "text": "hello",
  "prefix": "[demo] "
}
```

### `repeat_text`

参数：

```json
{
  "text": "go",
  "count": 3
}
```

### 为什么这个插件适合你照着抄

因为它刚好覆盖了你最需要学的 4 个点：

1. `string` 参数怎么收
2. `integer` 参数怎么收
3. 怎么做参数合法性保护
4. 怎么拼出 MCP 期望的返回 JSON

---

## 9. 你怎么验证“我加的新工具真的生效了”

顺序一定要这样：

### 第一步：重新编译 `mcp_server_integrated`

```bash
cmake -S . -B build
cmake --build build --target mcp_server -j$(nproc)
```

### 第二步：先用直连 MCP 的示例确认它已经出现在工具列表

```text
list
```

如果你看不到 `echo_text` / `repeat_text`，说明插件还没被正确加载。

### 第三步：直接调用它

```text
call echo_text {"text":"plugin works","prefix":"[ok] "}
call repeat_text {"text":"go","count":3}
```

### 第四步：再通过 gRPC 间接调用它

```text
echo hello grpc mcp
/call echo_text {"text":"hello","prefix":"[from grpc] "}
```

如果这一步也通了，就说明链路已经完整打通：

```text
gRPC Client -> gRPC Server -> MCP -> Plugin
```

---

## 10. 新手最容易混淆的 4 个点

### 混淆 1：gRPC 客户端不是 MCP 客户端

它们职责不同：

- gRPC 客户端：调 `AIQueryService`
- MCP 客户端：调 `mcp_server`

### 混淆 2：工具名和插件名不是一回事

比如：

- 插件名可能叫 `tutorial-echo-tools`
- 工具名是 `echo_text`

`tools/call` 里传的是工具名，不是插件名。

### 混淆 3：工具 Schema 只是“声明”，真正执行逻辑还要你自己写

Schema 负责告诉外界“需要什么参数”。  
但参数怎么处理、结果怎么算，还是你在 `HandleRequest()` 里写。

### 混淆 4：`MCPAgentIntegration` 和 `MCPClient` 不是同一层

- `MCPClient` 更底层，负责 transport 和 JSON-RPC
- `MCPAgentIntegration` 更高层，负责缓存、重试、工具列表等

所以：

- 学机制时，先看 `MCPClient`
- 写业务时，优先用 `MCPAgentIntegration`

---


## 11. 一句话记住这套系统

> gRPC 负责“把请求送进来”，MCP 负责“把工具调起来”，插件负责“把具体事情做完”。

只要你一直沿着这条线读代码，就不会迷路。
