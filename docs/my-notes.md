
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
非本地 gRPC Client -> AIQueryService(Query/QueryStream) -> 本地 TutorialMCPService -> MCPAgentIntegration -> MCPClient -> JSON-RPC -> mcp_server -> plugin

gRPC Client
  -> 非本地 gRPC transport：HTTP/2 + Protobuf + TCP/TLS
  -> 本地桥接层：grpc_mcp_tutorial_server / AIQueryService
  -> MCPClient
  -> MCP transport：Streamable HTTP / STDIO
  -> mcp_server
```

