# 完整部署指南服务启动地图

本文不是重复一遍部署命令，而是把 [`README.md`](../README.md) 中“完整部署指南（手动启动各组件）”涉及到的每个进程，在启动后到底做了什么、对应哪些函数、以及服务之间怎么互相连接，整理成一张运行时地图。

适用范围：

- [`README.md`](../README.md) 的手动启动步骤 1 到 6
- 多 Agent 示例：`Registry Server`、`Math Agent`、`Orchestrator`
- gRPC 前后端：`rpc_server`、`rpc_client`
- 被 `Math Agent` / `Orchestrator` 隐式拉起的 `mcp_server`

不在本文主范围内的内容：

- [`docs/deployment.md`](deployment.md) 里的 systemd / Nginx 生产化部署细节
- 测试程序、教程示例程序

## 1. 启动清单总览

| 步骤 | 进程/服务 | 是否手动启动 | 作用 | 入口 |
|---|---|---|---|---|
| 1 | `redis-server` | 是 | 保存任务和对话历史 | 外部服务，代码侧通过 [`a2a/src/examples/redis_task_store.cpp`](../a2a/src/examples/redis_task_store.cpp) 连接 |
| 2 | `ai_registry_server` | 是 | Agent 注册、发现、心跳清理 | [`examples/ai_orchestrator/registry_server_main.cpp`](../examples/ai_orchestrator/registry_server_main.cpp) |
| 3 | `ai_math_agent` | 是 | 数学问题处理，调用 MCP 工具 | [`examples/ai_orchestrator/math_agent_main.cpp`](../examples/ai_orchestrator/math_agent_main.cpp) |
| 4 | `ai_orchestrator` | 是 | 意图识别、路由到专业 Agent、通用问答 | [`examples/ai_orchestrator/orchestrator_main.cpp`](../examples/ai_orchestrator/orchestrator_main.cpp) |
| 5 | `rpc_server` | 是 | gRPC 入口，把 RPC 请求桥接到 A2A/HTTP | [`server/src/main.cpp`](../server/src/main.cpp) |
| 6 | `rpc_client` | 是 | 命令行交互入口，连 gRPC 服务 | [`client/src/main.cpp`](../client/src/main.cpp) |
| 隐式 | `mcp_server` | 否 | 被 Agent 子进程拉起，提供工具列表与工具调用 | [`mcp_server_integrated/src/main.cpp`](../mcp_server_integrated/src/main.cpp) |

有一个很重要的部署事实：

- [`README.md`](../README.md) 的手动部署步骤并没有单独启动 `mcp_server`。
- `Math Agent` 和 `Orchestrator` 会各自通过 `MCPClient::startMCPServer()` `fork + execv` 拉起一个本地 `mcp_server` 子进程。
- 所以完整系统里通常会出现两个 `mcp_server` 进程，而不是一个。
- 下面这份文档先只给源码文件路径加链接，函数名先保持纯文本。

## 2. 总体连接图

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

## 3. 各服务启动后做的事情与对应函数

### 3.1 Redis

`redis-server` 不是仓库内实现的服务，但它是 `Math Agent` 和 `Orchestrator` 的运行前提。

### 启动后在代码里的体现

| 动作 | 对应函数 | 说明 |
|---|---|---|
| 建立 Redis 连接 | `a2a::RedisTaskStore::RedisTaskStore()` | `MathAgent` / `AIOrchestrator` 构造时立刻执行 |
| 断线重连 | `a2a::RedisTaskStore::ensure_connection()` | 每次执行命令前检查连接 |
| 保存任务元信息 | `a2a::RedisTaskStore::set_task()` | 首次进入某个 `context_id` 时创建任务 |
| 保存对话历史 | `a2a::RedisTaskStore::add_history_message()` | 用户消息、Agent 回复都会写入 Redis List |
| 读取最近历史 | `a2a::RedisTaskStore::get_history()` | `Math Agent` 和 `Orchestrator` 生成回复前会取最近历史 |

### 谁会连接 Redis

- [`examples/ai_orchestrator/math_agent_main.cpp`](../examples/ai_orchestrator/math_agent_main.cpp) 里的 `MathAgent::MathAgent()`
- [`examples/ai_orchestrator/orchestrator_main.cpp`](../examples/ai_orchestrator/orchestrator_main.cpp) 里的 `AIOrchestrator::AIOrchestrator()`

### Redis 在运行时的作用

- `Math Agent` 在 `solve_math()` 里通过 `get_history()` 读取上下文。
- `Orchestrator` 在 `handle_general_query()` 里通过 `get_history()` 读取上下文。
- 两者都通过 `save_message()` 间接调用 `set_task()` 和 `add_history_message()`。

### 3.2 Registry Server

入口：[`examples/ai_orchestrator/registry_server_main.cpp`](../examples/ai_orchestrator/registry_server_main.cpp) 里的 `main()`

### 启动后的主流程

| 启动动作 | 对应函数 | 说明 |
|---|---|---|
| 创建注册中心实例 | `RegistryServer::RegistryServer()` | 内部持有 `AgentRegistry` |
| 注册 HTTP 路由 | `RegistryServer::start()` | 挂载 `/v1/agent/register`、`/deregister`、`/heartbeat`、`/find`、`/agents` |
| 开始监听端口 | `HttpServer::listen()` | 端口就绪后才继续 |
| 启动健康检查线程 | `RegistryServer::start()` 里的 `health_thread` | 每 10 秒调用一次 `AgentRegistry::check_health()` |
| 进入服务循环 | `HttpServer::serve()` | 持续接收 Agent 的注册/发现请求 |

### 各 HTTP 接口对应函数

| 接口 | 处理函数 | 最终操作 |
|---|---|---|
| `/v1/agent/register` | `RegistryServer::handle_register()` | `AgentRegistry::register_agent()` |
| `/v1/agent/deregister` | `RegistryServer::handle_deregister()` | `AgentRegistry::deregister_agent()` |
| `/v1/agent/heartbeat` | `RegistryServer::handle_heartbeat()` | `AgentRegistry::heartbeat()` |
| `/v1/agent/find` | `RegistryServer::handle_find()` | `AgentRegistry::find_agents_by_tag()` |
| `/v1/agents` | `RegistryServer::handle_list()` | `AgentRegistry::get_all_agents()` |

### 它和其他服务的连接关系

- `Math Agent` 启动后会向它注册自己。
- `Orchestrator` 启动后会向它注册自己。
- `RegistryClient::start_heartbeat()` 会每 10 秒向它发送一次 `/v1/agent/heartbeat`。
- `Orchestrator` 在路由数学问题时，通过 `RegistryClient::select_agent_by_tag("math")` 先查注册中心，再决定调用哪个 Math Agent。

### 3.3 Math Agent

入口：[`examples/ai_orchestrator/math_agent_main.cpp`](../examples/ai_orchestrator/math_agent_main.cpp) 里的 `main()`

结果：Math Agent 父进程、mcp_server 子进程；启动一个心跳线程与注册中心保持连接。

### 启动后的初始化阶段

| 启动动作 | 对应函数 | 说明 |
|---|---|---|
| 解析 MCP / RAG 参数 | `parseMCPConfigFromArgs()`、`parseMCPConfigFromEnv()` | 识别 `--enable-mcp`、`--mcp-server`、`--enable-rag` 等参数 |
| 创建 Redis 任务存储 | `MathAgent::MathAgent()` -> `RedisTaskStore::RedisTaskStore()` | 建立到 Redis 的连接 |
| 创建 Qwen 客户端 | `MathAgent::MathAgent()` -> `QwenClient` 构造 | 后续真正调用发生在请求期 |
| 创建 Registry 客户端 | `MathAgent::MathAgent()` -> `RegistryClient` 构造 | 用于注册和心跳 |
| 初始化 MCP 集成 | `MathAgent::MathAgent()` -> `MCPAgentIntegration::initialize()` | 连接工具系统 |
| 连接 MCP Server | `MCPAgentIntegration::connectToMCPServer()` | 内部创建 `MCPClient` 和 `MCPToolManager` |
| 拉起本地 `mcp_server` 子进程 | `MCPClient::connect()` -> `MCPClient::startMCPServer()` | 使用 `fork + execv`，STDIO 管道通信 |
| 拉取工具列表并缓存 | `MCPToolManager::initialize()` -> `refreshTools()` -> `MCPClient::listTools()` | 缓存工具元信息 |
| 初始化 RAG | `MCPAgentIntegration::initializeRAG()` | 创建 `ToolRetriever` |
| 构建向量索引 | `ToolRetriever::initialize()`、`ToolRetriever::indexTools()` | 对工具描述做 embedding 和索引 |

### 为什么手动命令里没写 `--mcp-args -p ...` 也能找到插件

因为 [`mcp/src/mcp_client.cpp`](../mcp/src/mcp_client.cpp) 里的 `buildEffectiveServerArgs()` 会在没有显式 `-p/--plugins` 时，自动推断：

- `mcp_server` 所在目录的同级 `plugins/`
- 对于 `build/mcp_server_integrated/mcp_server`，推断出的目录就是 `build/mcp_server_integrated/plugins`

所以 `Math Agent` 在手动部署命令里只传 `--mcp-server` 也能拉起并发现工具。

### 启动监听后的动作

| 启动动作 | 对应函数 | 说明 |
|---|---|---|
| 注册普通请求处理器 | `MathAgent::start()` -> `server.register_handler("/")` | 处理 `message/send` |
| 注册流式处理器 | `MathAgent::start()` -> `server.register_stream_handler("/")` | 处理 `message/stream` |
| 暴露 Agent Card | `MathAgent::start()` -> `get_agent_card()` | A2A 标准能力描述 |
| 启动 HTTP 监听 | `HttpServer::listen()`、`HttpServer::serve()` | 对外提供 A2A HTTP 服务 |
| 注册到注册中心 | `MathAgent::start()` -> `RegistryClient::register_agent()` | 注册成功后自动启动心跳线程 |
| 后台发送心跳 | `RegistryClient::start_heartbeat()` | 每 10 秒一次 |

### 收到请求后做什么

| 运行动作 | 对应函数 | 说明 |
|---|---|---|
| 处理同步 A2A 消息 | `MathAgent::handle_request()` | 解析 `message/send` |
| 处理流式 A2A 消息 | `MathAgent::handle_stream_request()` | 发出 `stream_start/chunk/stream_end` |
| 保存上下文 | `MathAgent::save_message()` | 间接写 Redis |
| 读取历史上下文 | `MathAgent::solve_math()` -> `RedisTaskStore::get_history()` | 取最近 5 条历史 |
| 优先尝试工具计算 | `MathAgent::tryMCPCalculation()` | 先走 RAG 工具检索，再调 `calculator` |
| 调 MCP 工具 | `MCPAgentIntegration::callTool()` -> `MCPToolManager::executeTool()` -> `MCPClient::callTool()` | 通过 `tools/call` 请求 `mcp_server` |
| 调用大模型组织最终答案 | `QwenClient::chat()` | 把工具结果拼进 prompt，再调用 Qwen |

### Math Agent 的上下游

- 上游调用者：`Orchestrator`
- 服务发现：`ai_registry_server`
- 状态存储：`redis-server`
- 工具系统：本地 `mcp_server` 子进程
- 外部大模型：Qwen API

### 3.4 Orchestrator

入口：[`examples/ai_orchestrator/orchestrator_main.cpp`](../examples/ai_orchestrator/orchestrator_main.cpp) 里的 `main()`

### 启动后的初始化阶段

它和 `Math Agent` 的初始化骨架几乎一样，只是职责不同。

| 启动动作 | 对应函数 | 说明 |
|---|---|---|
| 解析 MCP / RAG 参数 | `parseMCPConfigFromArgs()`、`parseMCPConfigFromEnv()` | 与 Math Agent 共用 |
| 连接 Redis | `AIOrchestrator::AIOrchestrator()` -> `RedisTaskStore::RedisTaskStore()` | 用于上下文历史 |
| 创建 Qwen 客户端 | `AIOrchestrator::AIOrchestrator()` -> `QwenClient` 构造 | 用于意图识别与通用回答 |
| 创建 Registry 客户端 | `AIOrchestrator::AIOrchestrator()` -> `RegistryClient` 构造 | 用于注册和发现专业 Agent |
| 初始化 MCP 集成 | `MCPAgentIntegration::initialize()` | 会拉起自己的 `mcp_server` 子进程 |
| 拉工具列表 / 建 RAG 索引 | `MCPToolManager::initialize()`、`MCPAgentIntegration::initializeRAG()` | 供通用问题时辅助调用工具 |

### 启动监听后的动作

| 启动动作 | 对应函数 | 说明 |
|---|---|---|
| 注册普通请求处理器 | `AIOrchestrator::start()` -> `server.register_handler("/")` | 处理 `message/send` |
| 注册流式处理器 | `AIOrchestrator::start()` -> `server.register_stream_handler("/")` | 处理 `message/stream` |
| 暴露 Agent Card | `AIOrchestrator::start()` -> `get_agent_card()` | A2A 标准元信息 |
| 启动 HTTP 监听 | `HttpServer::listen()`、`HttpServer::serve()` | 对外作为 A2A 协调器 |
| 注册到注册中心 | `AIOrchestrator::start()` -> `RegistryClient::register_agent()` | 注册成功后自动启用心跳线程 |

### 收到请求后做什么

| 运行动作 | 对应函数 | 说明 |
|---|---|---|
| 处理同步请求 | `AIOrchestrator::handle_request()` | 同步链路入口 |
| 处理流式请求 | `AIOrchestrator::handle_stream_request()` | 会先发 `intent` 事件，再分块返回结果 |
| 保存上下文 | `AIOrchestrator::save_message()` | 写入 Redis |
| 意图识别 | `AIOrchestrator::analyze_intent()` | 通过 `QwenClient::chat()` 判断 `math/code/general` |
| 按标签选择下游 Agent | `AIOrchestrator::call_agent_by_tag()` -> `RegistryClient::select_agent_by_tag()` | 通过注册中心找 `math` 或 `code` Agent |
| 调用下游 Agent | `SimpleHttpClient::post()` | 向目标 Agent 发 A2A JSON-RPC 请求 |
| 通用问答时用工具增强 | `AIOrchestrator::tryMCPTools()` | 遍历可用工具做辅助查询 |
| 通用回答 | `AIOrchestrator::handle_general_query()` -> `QwenClient::chat()` | 不命中专业 Agent 时兜底 |

### Orchestrator 的上下游

- 上游调用者：默认是 `rpc_server`
- 下游专业 Agent：`Math Agent`，未来也可以有 `Code Agent`
- 服务发现：`ai_registry_server`
- 状态存储：`redis-server`
- 工具系统：自己拉起的 `mcp_server` 子进程
- 外部大模型：Qwen API

### 3.5 MCP Server（隐式子进程）

入口：[`mcp_server_integrated/src/main.cpp`](../mcp_server_integrated/src/main.cpp) 里的 `main()`

### 它是怎么被拉起的

| 动作 | 对应函数 | 说明 |
|---|---|---|
| Agent 连接 MCP | `MCPAgentIntegration::connectToMCPServer()` | 创建 `MCPClient` |
| 以 STDIO 模式连接 | `MCPClient::connect()` | 默认本地模式 |
| 真正拉起子进程 | `MCPClient::startMCPServer()` | `fork + execv`，并把 stdin/stdout 重定向到管道 |

### `mcp_server` 自己启动后做什么

| 启动动作 | 对应函数 | 说明 |
|---|---|---|
| 解析命令行参数 | [`mcp_server_integrated/src/main.cpp`](../mcp_server_integrated/src/main.cpp) 里的 `main()` | 决定日志目录、插件目录、SSE/STDIO 模式 |
| 初始化日志 | `main()` | 创建带时间戳的日志文件 |
| 扫描并加载插件 | `vx::mcp::PluginsLoader::LoadPlugins()` | 递归加载 `.so` |
| 初始化每个插件 | `vx::mcp::PluginsLoader::LoadPlugin()` | 调 `CreatePlugin()` 和 `Initialize()` |
| 注册 `tools/list` 回调 | `server->OverrideCallback("tools/list", ...)` | 把插件工具元信息暴露给客户端 |
| 注册 `tools/call` 回调 | `server->OverrideCallback("tools/call", ...)` | 把具体工具调用转发给对应插件 |
| 进入读写循环 | `vx::mcp::Server::Connect()` | 持续读取 JSON-RPC 请求并返回响应 |

### 它和其他服务的连接关系

- `Math Agent` 有自己的一份 `mcp_server`
- `Orchestrator` 也有自己的一份 `mcp_server`
- 两者都通过 `STDIN/STDOUT pipes` 和各自的 `mcp_server` 通信
- `mcp_server` 再把请求分发给 `calculator`、`sleep`、`weather` 等插件

### 3.6 RPC Server

入口：[`server/src/main.cpp`](../server/src/main.cpp) 里的 `main()`

proto 服务定义：
```
代理通信服务定义-AgentCommunicationService
健康检查服务-HealthService
AI查询服务-AIQueryService
```

### `RpcServer::setupServer()` 注册的 gRPC 服务一览

| 注册服务名 | RPC 方法 | 关键请求参数 | 作用 |
|---|---|---|---|
| `AIQueryService` | `Query(AIQueryRequest)` | `request_id`、`question`、`context_id`、`history_length`、`timeout_seconds`、`metadata`、`preference` | 当前主链路入口。接收一次完整 AI 问答请求，经 `A2AAdapter` 转发给 `ai_orchestrator`，再把最终答案组装成 `AIQueryResponse` 返回。 |
| `AIQueryService` | `QueryStream(AIQueryRequest)` | 同 `Query()` | 流式问答入口。把请求桥接到 A2A 流式接口，持续返回 `AIStreamEvent`，适合边生成边展示。 |
| `AIQueryService` | `GetQueryStatus(QueryStatusRequest)` | `task_id` 或 `context_id` | 设计上用于查询任务状态；但当前实现没有状态仓库，返回的是 `unavailable` / `UNIMPLEMENTED` 语义。 |
| `AgentCommunicationService` | `SendMessage(SendMessageRequest)` | `message`、`target_agent`、`timeout_seconds` | 向指定 Agent 的内存消息队列投递一条消息。 |
| `AgentCommunicationService` | `ReceiveMessage(ReceiveMessageRequest)` | `agent_id`、`max_messages`、`timeout_seconds` | 从指定 Agent 的内存消息队列拉取消息；当前实现主要按 `agent_id` 和 `max_messages` 工作。 |
| `AgentCommunicationService` | `BroadcastMessage(BroadcastMessageRequest)` | `message`、`target_agents`、`exclude_sender` | 向多个 Agent 广播消息；`target_agents` 为空时广播给全部已注册 Agent。当前实现未实际使用 `exclude_sender`。 |
| `AgentCommunicationService` | `GetAgents(GetAgentsRequest)` | `filter`、`limit`、`offset` | 返回当前 `rpc_server` 内存里维护的 Agent 列表，可按服务名子串过滤并分页。 |
| `AgentCommunicationService` | `RegisterAgent(RegisterAgentRequest)` | `agent_info(service_name/version/host/port/tags/metadata)`、`heartbeat_interval` | 向 `rpc_server` 的内存注册表登记一个 Agent，并生成 `agent_id=service_name-host-port`。当前实现未实际使用 `heartbeat_interval`。 |
| `AgentCommunicationService` | `UnregisterAgent(UnregisterAgentRequest)` | `agent_id`、`reason` | 从 `rpc_server` 的内存注册表移除指定 Agent。 |
| `AgentCommunicationService` | `Heartbeat(HeartbeatRequest)` | `agent_id`、`agent_info` | 刷新指定 Agent 的最后心跳时间，防止被离线清理线程移除。当前实现只使用 `agent_id`。 |
| `AgentCommunicationService` | `ListenMessages(ReceiveMessageRequest)` | `agent_id`、`timeout_seconds` | 服务端流式接口。持续监听指定 Agent 的消息队列，直到超时或客户端取消。 |
| `AgentCommunicationService` | `BatchSendMessages(stream SendMessageRequest)` | 流里的每个元素都包含 `message`、`target_agent`、`timeout_seconds` | 客户端流式批量投递消息，最终返回一次汇总结果。 |
| `AgentCommunicationService` | `RealTimeCommunication(stream Message)` | 流里的每个元素都是 `Message(id/type/content/timestamp/headers/payload)` | 双向流式实时通信接口；当前实现还是 echo back 占位逻辑，读到什么就回写什么。 |
| `HealthService` | `Check(HealthCheckRequest)` | `service` | 单次健康检查，返回 `SERVING` / `NOT_SERVING`；当前实现并未区分具体 `service` 名字。 |
| `HealthService` | `Watch(HealthCheckRequest)` | `service` | 流式健康检查，每 5 秒返回一次健康状态，直到客户端取消。 |

补充说明：

- 当前 `rpc_client` 走的主业务链路基本只会用到 `AIQueryService`。
- `AgentCommunicationService` 和 `HealthService` 更像是 `rpc_server` 暴露出来的通用基础能力，其中一部分方法目前还是内存态或占位实现。

结果：启动一个 gRPC Server（子线程），监听来自 `rpc_client` 的请求，把它们桥接成 A2A 请求发给 `Orchestrator`。

### 启动后的初始化阶段

| 启动动作 | 对应函数 | 说明 |
|---|---|---|
| 读取命令行和环境变量 | [`server/src/main.cpp`](../server/src/main.cpp) 里的 `main()` | 解析 `--port`、`--orchestrator`、`--registry` |
| 写入 A2A 配置 | `RpcServer::setA2AConfig()` | 把 Orchestrator URL 传给 `AIQueryService` |
| 初始化 RPC Server | `RpcServer::initialize()` | 总入口 |
| 初始化序列化器 | `common::MessageSerializer::initialize()` | 选择 `PROTOBUF_BINARY` |
| 初始化 AI 查询服务 | `AIQueryServiceImpl::initialize()` | 创建并初始化 `A2AAdapter` |
| 初始化 A2A 客户端 | `A2AAdapter::initialize()` | `a2a::A2AClient(config_.orchestrator_url)` |
| 注册 gRPC 服务 | `RpcServer::setupServer()` | 注册 `AIQueryService`、`AgentCommunicationService`、`HealthService` |
| 可选注册到服务注册中心 | `RpcServer::initializeServiceRegistry()` | 只有显式开启时才执行 |
| 启动 gRPC 服务线程 | `RpcServer::start()` | 后台线程 `server_->Wait()` |

### 它启动后并不会自动连 `ai_registry_server`

默认手动命令是：

```bash
./build/server/rpc_server
```

这种启动方式下：

- `rpc_server` 只知道 `Orchestrator URL`
- 它通过 `A2AAdapter` 直接访问 `Orchestrator URL` 的 `http://localhost:5000`
- 它不会使用 `ai_registry_server`

只有显式传 `--enable-registry` / `--registry` 时，`RpcServer::initializeServiceRegistry()` 才会启用自己的服务注册。

### 收到 gRPC 请求后做什么

| 运行动作 | 对应函数 | 说明 |
|---|---|---|
| 接收同步 AI 查询 | `AIQueryServiceImpl::Query()` | `AIQueryRequest -> AIQueryResponse` |
| 接收流式 AI 查询 | `AIQueryServiceImpl::QueryStream()` | `AIStreamEvent` 流 |
| RPC 请求转 A2A | `A2AAdapter::processQuery()` / `processQueryStreaming()` | 桥接核心 |
| 构造 A2A 消息 | `RequestAdapter::convertToA2A()` | 生成 `MessageSendParams` |
| 发送到 Orchestrator | `a2a::A2AClient::send_message()` / `send_message_streaming()` | HTTP/A2A 调用 |
| A2A 响应转回 gRPC | `ResponseAdapter::convertFromA2A()` / `buildStreamEvent()` | 组装 RPC 响应 |

### RPC Server 的上下游

- 上游：`rpc_client` 或任何 gRPC 调用方
- 下游：`ai_orchestrator`
- 默认不依赖：`ai_registry_server`

### 3.7 RPC Client（严格来说不是服务，而是交互入口）

入口：[`client/src/main.cpp`](../client/src/main.cpp) 里的 `main()`

结果：一个大循环的 REPL，用户输入问题，发送 gRPC 请求到 `rpc_server`，显示结果。

### 启动后的动作

| 启动动作 | 对应函数 | 说明 |
|---|---|---|
| 读取参数 | [`client/src/main.cpp`](../client/src/main.cpp) 里的 `main()` | 支持直连地址或注册中心发现 |
| 初始化 RPC 客户端 | `RpcClient::initialize()` | 初始化序列化器、熔断器、负载均衡器 |
| 连接目标 `rpc_server` | `RpcClient::connect()` / `connectViaRegistry()` | 选出服务端地址 |
| 建立 gRPC Channel | `RpcClient::connectToEndpoint()` -> `setupChannel()` | 建立普通 gRPC stub |
| 建立 AI Query stub | `RpcClient::connectToEndpoint()` -> `AIQueryClient::connect()` | 建立 `AIQueryService` 的 stub |
| 进入 REPL | [`client/src/main.cpp`](../client/src/main.cpp) 里的 `main()` | 循环读取用户输入 |

### 发送查询时的函数链

| 模式 | 函数链 |
|---|---|
| 同步 | `client main -> RpcClient::aiQuery() -> AIQueryClient::query() -> AIQueryService::Query()` |
| 流式 | `client main -> RpcClient::aiQueryStream() -> AIQueryClient::queryStream() -> AIQueryService::QueryStream()` |

## 4. 几条关键连接链路

### 4.1 用户查询主链路

以“`1+7`”为例：

1. `rpc_client` 在 [`client/src/main.cpp`](../client/src/main.cpp) 收到用户输入。
2. `RpcClient::aiQuery()` 调 `AIQueryClient::query()` 发起 gRPC 请求。
3. `rpc_server` 的 `AIQueryServiceImpl::Query()` 收到请求。
4. `A2AAdapter::processQuery()` 调 `RequestAdapter::convertToA2A()` 组装 A2A 请求。
5. `a2a::A2AClient::send_message()` 把请求发给 `ai_orchestrator`。
6. `AIOrchestrator::handle_request()` 收到请求。
7. `AIOrchestrator::analyze_intent()` 判断这是 `math`。
8. `AIOrchestrator::call_agent_by_tag("math", ...)` 调 `RegistryClient::select_agent_by_tag("math")` 找到 `Math Agent` 地址。
9. `SimpleHttpClient::post()` 把请求转发给 `ai_math_agent`。
10. `MathAgent::handle_request()` 收到问题。
11. `MathAgent::tryMCPCalculation()` 调 `MCPAgentIntegration::callTool("calculator", ...)`。
12. `mcp_server` 的 `tools/call` 回调把请求路由到 `calculator` 插件。
13. 工具结果回到 `MathAgent::solve_math()`，再由 `QwenClient::chat()` 组织最终回答。
14. 响应按原路返回：`Math Agent -> Orchestrator -> rpc_server -> rpc_client`。

### 4.2 服务发现与心跳链路

1. `MathAgent::start()` 调 `RegistryClient::register_agent()`。
2. `RegistryServer::handle_register()` 调 `AgentRegistry::register_agent()`。
3. 注册成功后，`RegistryClient::start_heartbeat()` 启动后台线程。
4. 心跳线程每 10 秒调用一次 `/v1/agent/heartbeat`。
5. `RegistryServer` 的健康检查线程每 10 秒调用一次 `AgentRegistry::check_health()`。
6. 如果某个 Agent 超过 `heartbeat_timeout` 没有续命，就会被注册中心移除。

`Orchestrator` 的注册和心跳流程完全一样。

### 4.3 Redis 上下文链路

1. `Math Agent` / `Orchestrator` 构造时创建 `RedisTaskStore` 连接 Redis。
2. 收到请求后先在 `save_message()` 里写用户消息。
3. 回复前在 `get_history()` 里读取最近历史。
4. 生成结果后再次在 `save_message()` 里写 Agent 回复。

所以 Redis 里保存的是：

- 任务元信息
- 某个 `context_id` 下的消息历史

### 4.4 MCP 工具调用链路

1. `Math Agent` 或 `Orchestrator` 的 `MCPAgentIntegration::initialize()` 创建 `MCPClient`。
2. `MCPClient::startMCPServer()` 拉起本地 `mcp_server` 子进程。
3. `MCPToolManager::refreshTools()` 先走 `tools/list` 拉取工具元信息。
4. 业务侧要调工具时，执行 `MCPAgentIntegration::callTool()`。
5. `MCPClient::callTool()` 发送 `tools/call` JSON-RPC 请求。
6. `mcp_server` 的 `OverrideCallback("tools/call", ...)` 找到对应插件。
7. 插件 `HandleRequest()` 执行具体逻辑并返回结果。

## 5. 部署时最容易误解的几点

1. `rpc_server` 默认不依赖 `ai_registry_server`。它默认只直连 `Orchestrator`。
2. `ai_registry_server` 管的是 A2A Agent，不是 gRPC 客户端。
3. `mcp_server` 不是手动部署步骤里单独起的守护进程，而是 Agent 拉起的子进程。
4. `Math Agent` 和 `Orchestrator` 各有一份 `mcp_server`，各自维护工具缓存和 RAG 索引。
5. `Redis` 不是只有 `Math Agent` 用，`Orchestrator` 也会写入和读取上下文。
6. `RAG` 的启动成本发生在 Agent 初始化阶段。启用后会在 `initializeRAG()` / `ToolRetriever::indexTools()` 中做工具索引构建。

## 6. 一句话总结

这套系统真正的职责分层是：

- `rpc_client/rpc_server` 负责 gRPC 接入
- `ai_orchestrator` 负责意图识别和路由
- `ai_registry_server` 负责服务发现和心跳
- `ai_math_agent` 负责数学任务处理
- `redis-server` 负责上下文持久化
- `mcp_server + plugins` 负责工具执行

把这几个角色分开看，完整部署指南里的启动顺序、日志输出和请求流向就会非常清晰。
