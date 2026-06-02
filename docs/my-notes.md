
# 面试题目
## 本地 Client 是怎么与 MCP-Server STDIO通信的？
1. Client 先创建两个无名管道A、B，并 fork MCP-Server 作为一个子进程，
2. 将 Server 的键盘输入 STDIN_FILENO 重定向到管道 B 的写入端（dup2(pipeB[1], STDIN_FILENO)），
3. 将 Server 的标准输出 STDOUT_FILENO 重定向到管道 A 的读取端（dup2(pipeA[0], STDOUT_FILENO)）。
4. 这样，MCP-Server 的输入来自管道 B，输出写入管道 A。Client 则通过管道 A 的读取端读取 MCP-Server 的输出，通过管道 B 的写入端发送请求给 MCP-Server。
5. Client 启动了一个后台非阻塞读线程，持续从管道 A 读取 MCP-Server 的输出

## 本地 Client 与 MCP-Server 之间的请求与响应格式
请求与响应遵循 JSON-RPC 格式，包含以下字段：
```json
{
  "jsonrpc": "2.0",
  "id": "unique_request_id_12345",
  "method": "method_name",
  "params": {
    // method-specific parameters
  }
}
```
MCP-Server 处理请求后，返回一个 JSON-RPC 响应字符串，包含以下字段：
```json
{
  "jsonrpc": "2.0",
  "id": "unique_request_id_12345",
  "result": {
    "content": ...// method-specific result data
  }
}
```
method_name 可以是预定义的方法，如 "tools/list"、"tools/call"，也可以是用户自定义的方法.

## 非本地 Client 是怎么与 MCP-Server 通信的？

```text
rpc_client
    |
    | gRPC
    v
rpc_server
    |
    | A2A over HTTP/JSON-RPC
    v
ai_orchestrator ------------------------> ai_registry_server
    |                                         ^
    | HTTP/JSON-RPC                           |
    v                                         | register/find/heartbeat
ai_math_agent --------------------------------+
    |
    | STDIO(JSON-RPC over pipes)
    v
mcp_server
    |
    | plugin dispatch
    v
calculator / sleep / weather / ...

ai_orchestrator 也有自己独立拉起的 mcp_server 子进程
ai_math_agent    也会连接 redis-server
ai_orchestrator  也会连接 redis-server
```

可以把整个系统理解成四条并行链路：

1. 用户查询链路：`rpc_client -> rpc_server -> ai_orchestrator -> ai_math_agent`
2. 服务发现链路：`ai_math_agent / ai_orchestrator -> ai_registry_server`
3. 上下文持久化链路：`ai_math_agent / ai_orchestrator -> redis-server`
4. 工具调用链路：`ai_math_agent / ai_orchestrator -> mcp_server -> plugin`

## plugins 与 tools 的关系
- plugins 是这些 tools 的打包、注册、加载单位
- tools 是插件的抽象接口，定义了插件应该提供哪些功能和参数。
- MCP-Server 通过 tools 定义的接口调用 plugins 提供的功能，实现工具的注册和调用。
- 1 个 plugin -> N 个 tools